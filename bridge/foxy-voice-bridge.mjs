#!/usr/bin/env node
/**
 * Foxy 语音桥 —— 给 xiaozhi-esp32-server 用的 OpenAI 兼容 /v1/chat/completions
 *
 * 拓扑：
 *   xiaozhi-server (LLM type=openai) --HTTP--> 本文件 (:8010)
 *        --ACP over stdio--> DeepSeek Harness (profile acp) --deepseek-flash--> 文本
 *
 * 为什么这么绕：本机没有 DEEPSEEK_API_KEY，凭据在 DSH 里（deepseek-account 设备授权）。
 * ACP 复用现成凭据，代价是回复不流式、冷启动慢，因此本进程常驻并做预热。
 *
 * 零依赖，Node >= 20。启动见 CoreS3-Foxy-实施方案.md §4.4。
 */

import http from 'node:http'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { spawn } from 'node:child_process'
import { randomUUID } from 'node:crypto'

// 同步写文件，避免 PowerShell 重定向的缓冲让我们看不到实时日志
function trace(msg) {
  try {
    fs.appendFileSync(path.join(process.cwd(), 'bridge-trace.log'), `${new Date().toISOString()} ${msg}\n`)
  } catch {
    /* ignore */
  }
}

const CFG = {
  port: Number(process.env.PORT || 8010),
  exe: process.env.DSH_EXE || 'F:\\kunfox\\deepseek\\DeepSeek Harness.exe',
  cli: process.env.DSH_CLI || '',
  // 注意：不要读 DSH_PROFILE —— 桌面版进程里它常被设成 desktop，会覆盖 acp
  profile: process.env.FOXY_DSH_PROFILE || 'acp',
  dshHome: process.env.DSH_HOME || 'C:\\Users\\MECHREV\\.dsh',
  model: process.env.DSH_MODEL || 'deepseek-flash',
  cwd: process.env.BRIDGE_WORKSPACE || path.join(os.homedir(), '.dsh', 'tmp', 'foxy-voice'),
  personaPath: process.env.VOICE_PERSONA || path.join(process.cwd(), 'voice-persona.md'),
  memoryPath:
    process.env.VOICE_MEMORY ||
    'C:\\Users\\MECHREV\\.dsh\\foxy\\memory\\entries.jsonl',
  memoryTurns: Number(process.env.VOICE_MEMORY_TURNS || 12),
  promptTimeoutMs: Number(process.env.PROMPT_TIMEOUT_MS || 45000),
  warmup: process.env.WARMUP !== '0'
}

const FALLBACK_PERSONA = `你是 Foxy，和 kunfox 一起生活的狐狸，用嘴说话，不是打字。
每次 1-3 句、合计不超过 60 字；短句、口语。不用列表、不用 markdown、不念表情符号。
不确定就反问一句，别编。`

const log = (...a) => console.log(new Date().toISOString(), ...a)

/* ---------------------------------- ACP ---------------------------------- */

class AcpSession {
  constructor () {
    this.child = null
    this.buffer = ''
    this.pending = new Map()
    this.nextId = 1
    this.sessionId = null
    this.ready = null
    this.assistant = ''
    this.busy = Promise.resolve()
    this.turns = 0
  }

  start () {
    if (this.ready) return this.ready
    fs.mkdirSync(CFG.cwd, { recursive: true })
    const args = CFG.cli
      ? ['--expose-internals', CFG.cli, '--profile', CFG.profile]
      : ['--profile', CFG.profile]
    log('spawn', CFG.exe, args.join(' '))
    this.child = spawn(CFG.exe, args, {
      cwd: CFG.cwd,
      env: {
        ...process.env,
        ELECTRON_RUN_AS_NODE: '1',
        DSH_HOME: CFG.dshHome
      },
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true
    })

    this.child.stdout.setEncoding('utf8')
    this.child.stdout.on('data', (chunk) => this.onData(chunk))
    this.child.stderr.setEncoding('utf8')
    this.child.stderr.on('data', (chunk) => {
      const text = String(chunk).trim()
      if (text) log('[dsh:stderr]', text.slice(0, 400))
    })
    this.child.on('exit', (code) => {
      log('dsh exited', code)
      this.child = null
      this.ready = null
      this.sessionId = null
      for (const [, entry] of this.pending) entry.rej(new Error('dsh exited'))
      this.pending.clear()
    })

    this.ready = (async () => {
      await this.request('initialize', { protocolVersion: 1, clientCapabilities: {} })
      const session = await this.request('session/new', { cwd: CFG.cwd, mcpServers: [] })
      this.sessionId = session.sessionId
      const value = this.resolveModel(session, CFG.model)
      if (value) {
        await this.request('session/set_config_option', {
          sessionId: this.sessionId,
          configId: 'model',
          value
        })
      }
      log('acp ready, session', this.sessionId, 'model', value)
      return this.sessionId
    })().catch((error) => {
      log('acp init failed:', error.message)
      this.ready = null
      throw error
    })
    return this.ready
  }

  resolveModel (session, wanted) {
    const entry = ((session && session.configOptions) || []).find((i) => i.id === 'model')
    const options = []
    for (const group of (entry && entry.options) || []) {
      if (group && Array.isArray(group.options)) options.push(...group.options)
      else if (group) options.push(group)
    }
    if (!options.length) return ''
    const raw = String(wanted || '').trim()
    const lower = raw.toLowerCase()
    let match = options.find((o) => String(o.name || '').toLowerCase() === lower)
    if (!match) {
      match = options.find((o) => {
        try {
          const pair = JSON.parse(o.value)
          return Array.isArray(pair) && String(pair[1]).toLowerCase() === lower
        } catch {
          return false
        }
      })
    }
    if (!match) match = options.find((o) => String(o.name || '').toLowerCase().includes(lower))
    return match ? match.value : ''
  }

  send (message) {
    if (!this.child || this.child.stdin.destroyed) return
    this.child.stdin.write(JSON.stringify(message) + '\n')
  }

  request (method, params) {
    const id = this.nextId++
    return new Promise((res, rej) => {
      this.pending.set(id, { res, rej })
      this.send({ jsonrpc: '2.0', id, method, params })
    })
  }

  onData (chunk) {
    this.buffer += chunk
    let index
    while ((index = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, index).trim()
      this.buffer = this.buffer.slice(index + 1)
      if (line) this.onLine(line)
    }
  }

  onLine (line) {
    let message
    try {
      message = JSON.parse(line)
    } catch {
      return
    }
    // 服务端 → 客户端请求：语音场景一律不给权限
    if (message.id != null && message.method) {
      const optionId = message.method === 'session/request_permission' ? 'reject-once' : null
      if (optionId) {
        this.send({ jsonrpc: '2.0', id: message.id, result: { outcome: { outcome: 'selected', optionId } } })
      } else {
        this.send({
          jsonrpc: '2.0',
          id: message.id,
          error: { code: -32601, message: 'unsupported client method' }
        })
      }
      return
    }
    if (message.id != null && this.pending.has(message.id)) {
      const entry = this.pending.get(message.id)
      this.pending.delete(message.id)
      if (message.error) entry.rej(new Error(message.error.message || JSON.stringify(message.error)))
      else entry.res(message.result)
      return
    }
    if (message.method === 'session/update') {
      const update = message.params && message.params.update
      if (update && update.sessionUpdate === 'agent_message_chunk') {
        if (update.content && update.content.type === 'text') this.assistant += update.content.text || ''
      }
    }
  }

  /** 串行一次问答；同一时刻只允许一个 prompt（语音场景天然单路）。 */
  ask (promptText) {
    const run = async () => {
      await this.start()
      this.assistant = ''
      const timer = new Promise((_, rej) =>
        setTimeout(() => rej(new Error('prompt timeout')), CFG.promptTimeoutMs)
      )
      try {
        await Promise.race([
          this.request('session/prompt', {
            sessionId: this.sessionId,
            prompt: [{ type: 'text', text: promptText }]
          }),
          timer
        ])
      } finally {
        this.turns++
      }
      return this.assistant.trim()
    }
    this.busy = this.busy.then(run, run)
    return this.busy
  }

  status () {
    return { connected: Boolean(this.child), session: this.sessionId, turns: this.turns }
  }
}

/* ------------------------------- 人格与记忆 ------------------------------- */

function loadPersona () {
  try {
    const text = fs.readFileSync(CFG.personaPath, 'utf8').trim()
    return text || FALLBACK_PERSONA
  } catch {
    return FALLBACK_PERSONA
  }
}

function readMemoryLines () {
  try {
    return fs
      .readFileSync(CFG.memoryPath, 'utf8')
      .split('\n')
      .filter((line) => line.trim())
      .map((line) => {
        try {
          return JSON.parse(line)
        } catch {
          return null
        }
      })
      .filter(Boolean)
  } catch {
    return []
  }
}

function memoryContext () {
  const rows = readMemoryLines()
    .filter((row) => row.type === 'conversation' && row.status === 'active')
    .slice(-CFG.memoryTurns)
  if (!rows.length) return ''
  const lines = rows.map((row) => {
    const who = row.role === 'assistant' ? 'Foxy' : row.speaker || 'kunfox'
    return `- ${who}：${String(row.content || '').slice(0, 120)}`
  })
  return `[记忆片段]\n${lines.join('\n')}`
}

function appendMemory (role, content) {
  if (!content) return
  const now = Date.now()
  const row = {
    id: randomUUID(),
    presetId: 'foxy3',
    sessionId: `voice-${new Date(now).toISOString().slice(0, 10)}`,
    platform: 'cores3',
    sourceLabel: 'CoreS3',
    speaker: role === 'assistant' ? 'foxy' : 'kunfox',
    role,
    type: 'conversation',
    status: 'active',
    content,
    keywords: [],
    summary: '',
    importance: 0.5,
    createdAt: now,
    updatedAt: now
  }
  try {
    fs.mkdirSync(path.dirname(CFG.memoryPath), { recursive: true })
    fs.appendFileSync(CFG.memoryPath, JSON.stringify(row) + '\n', 'utf8')
  } catch (error) {
    log('memory append failed:', error.message)
  }
}

/* -------------------------------- 提示词拼装 ------------------------------- */

function buildPrompt (messages) {
  const persona = loadPersona()
  const memory = memoryContext()
  const history = messages
    .filter((m) => m && m.role !== 'system')
    .slice(-8)
    .map((m) => `${m.role === 'assistant' ? 'Foxy' : 'kunfox'}：${String(m.content || '').trim()}`)
    .filter((line) => line.length > 4)

  return [
    '[人格]',
    persona,
    memory,
    history.length > 1 ? `[最近对话]\n${history.slice(0, -1).join('\n')}` : '',
    '[现在]',
    `${history[history.length - 1] || 'kunfox：'}`,
    '',
    '请只输出你要说出口的话，不要解释、不要引号、不要括号动作。'
  ]
    .filter(Boolean)
    .join('\n')
}

function splitSentences (text) {
  const parts = String(text).match(/[^。！？!?\n]+[。！？!?]*/gu) || [text]
  return parts.map((p) => p.trim()).filter(Boolean)
}

/* --------------------------------- 文字台 --------------------------------- */

const CHAT_PAGE = `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foxy · 文字台</title>
<style>
  :root{--bg:#14110f;--panel:#1e1a17;--line:#332c26;--txt:#efe6dc;--dim:#a2948a;--accent:#e8843c}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--txt);font:15px/1.65 "Microsoft YaHei",system-ui,sans-serif;display:flex;flex-direction:column;height:100vh}
  header{padding:12px 18px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:10px}
  header b{font-size:16px} header span{color:var(--dim);font-size:12px}
  #dot{width:8px;height:8px;border-radius:50%;background:#555}
  #dot.on{background:#5ac26b}
  main{flex:1;overflow-y:auto;padding:18px;display:flex;flex-direction:column;gap:12px}
  .row{display:flex;gap:10px;max-width:860px;width:100%;margin:0 auto}
  .row.me{justify-content:flex-end}
  .who{width:44px;flex:none;color:var(--dim);font-size:12px;padding-top:8px;text-align:center}
  .bub{padding:9px 13px;border-radius:12px;background:var(--panel);border:1px solid var(--line);white-space:pre-wrap;word-break:break-word;max-width:78%}
  .me .bub{background:#2a2018;border-color:#4a382a}
  .meta{color:var(--dim);font-size:11px;margin-top:4px}
  footer{border-top:1px solid var(--line);padding:12px 18px;display:flex;gap:10px;align-items:flex-end}
  textarea{flex:1;resize:none;height:56px;background:var(--panel);color:var(--txt);border:1px solid var(--line);border-radius:10px;padding:10px 12px;font:inherit;outline:none}
  textarea:focus{border-color:var(--accent)}
  button{background:var(--accent);color:#1a1208;border:0;border-radius:10px;padding:0 18px;height:56px;font:inherit;font-weight:700;cursor:pointer}
  button:disabled{opacity:.45;cursor:default}
</style></head><body>
<header><b>Foxy · 文字台</b><span id="dot"></span><span id="stat">连接中…</span></header>
<main id="log"></main>
<footer>
  <textarea id="input" placeholder="打字给 Foxy，回车发送（Shift+回车换行）"></textarea>
  <button id="send">发送</button>
</footer>
<script>
const log=document.getElementById('log'),input=document.getElementById('input'),send=document.getElementById('send'),dot=document.getElementById('dot'),stat=document.getElementById('stat')
const history=[]
function bubble(who,text,meta){
  const row=document.createElement('div');row.className='row'+(who==='me'?' me':'')
  const w=document.createElement('div');w.className='who';w.textContent=who==='me'?'我':'Foxy'
  const b=document.createElement('div');b.className='bub';b.textContent=text
  if(meta){const m=document.createElement('div');m.className='meta';m.textContent=meta;b.appendChild(m)}
  row.appendChild(w);row.appendChild(b);log.appendChild(row);log.scrollTop=log.scrollHeight
  return b
}
async function health(){
  try{const r=await fetch('/health');const j=await r.json()
    dot.className=j.connected?'on':'';stat.textContent=j.connected?'Foxy 在线 · 回合 '+j.turns:'DSH 未连接'
  }catch{stat.textContent='桥没在跑'}
}
async function ask(text){
  history.push({role:'user',content:text})
  bubble('me',text)
  send.disabled=true;stat.textContent='Foxy 在想…'
  const t0=performance.now()
  try{
    const r=await fetch('/v1/chat/completions',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({model:'foxy-voice',messages:history.slice(-8)})})
    const j=await r.json()
    if(j.error)throw new Error(j.error.message)
    const out=j.choices[0].message.content
    history.push({role:'assistant',content:out})
    bubble('foxy',out,((performance.now()-t0)/1000).toFixed(1)+'s')
  }catch(e){bubble('foxy','（出错）'+e.message)}
  send.disabled=false;health()
  input.focus()
}
function submit(){const v=input.value.trim();if(!v||send.disabled)return;input.value='';ask(v)}
send.onclick=submit
input.addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();submit()}})
health();setInterval(health,10000);input.focus()
</script></body></html>`

/* --------------------------------- HTTP --------------------------------- */

const SERVER_BASE = process.env.VOICE_SERVER || 'http://127.0.0.1:8003'

const acp = new AcpSession()

const SAY_PAGE = `<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Foxy · 让板子说话</title>
<style>
  :root{--bg:#14110f;--panel:#1e1a17;--line:#332c26;--txt:#efe6dc;--dim:#a2948a;--accent:#e8843c}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--txt);font:15px/1.65 "Microsoft YaHei",system-ui,sans-serif;display:flex;flex-direction:column;height:100vh}
  header{padding:12px 18px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:10px}
  header b{font-size:16px} header span{color:var(--dim);font-size:12px}
  #dot{width:8px;height:8px;border-radius:50%;background:#555}
  #dot.on{background:#5ac26b}
  main{flex:1;overflow-y:auto;padding:18px;display:flex;flex-direction:column;gap:8px}
  .item{max-width:860px;width:100%;margin:0 auto;padding:9px 13px;border-radius:10px;background:var(--panel);border:1px solid var(--line);white-space:pre-wrap;word-break:break-word}
  .item .meta{color:var(--dim);font-size:11px;margin-top:4px}
  .item.err{border-color:#7a2f2f}
  .tip{max-width:860px;width:100%;margin:0 auto;color:var(--dim);font-size:12px;padding:4px 2px}
  footer{border-top:1px solid var(--line);padding:12px 18px;display:flex;gap:10px;align-items:flex-end}
  textarea{flex:1;resize:none;height:56px;background:var(--panel);color:var(--txt);border:1px solid var(--line);border-radius:10px;padding:10px 12px;font:inherit;outline:none}
  textarea:focus{border-color:var(--accent)}
  button{background:var(--accent);color:#1a1208;border:0;border-radius:10px;padding:0 18px;height:56px;font:inherit;font-weight:700;cursor:pointer}
  button:disabled{opacity:.45;cursor:default}
  #ctl{display:flex;align-items:center;gap:8px;padding:10px 18px;border-bottom:1px solid var(--line);color:var(--dim);font-size:13px}
  #ctl b{color:var(--txt);min-width:28px;text-align:center;font-size:15px}
  button.mini{height:32px;padding:0 12px;border-radius:8px;font-weight:600;font-size:13px;background:var(--panel);color:var(--txt);border:1px solid var(--line)}
  button.mini:active{background:var(--accent);color:#1a1208}
</style></head><body>
<header><b>让板子说话</b><span id="dot"></span><span id="stat">查询中…</span></header>
<div id="ctl">
  <span>音量</span>
  <button class="mini" id="vdown">−</button>
  <b id="volv">70</b>
  <button class="mini" id="vup">＋</button>
  <button class="mini" id="v30">30</button>
  <button class="mini" id="v50">50</button>
  <button class="mini" id="v70">70</button>
  <span id="vstat"></span>
</div>
<main id="log"><div class="tip">打一句话，回车，CoreS3 就会把它念出来（走本地服务器 → Foxy 桥 → TTS）。板子要先唤醒一次，保持在线。</div></main>
<footer>
  <textarea id="input" placeholder="例如：今天天气怎么样？（回车发送，Shift+回车换行）"></textarea>
  <button id="send">念出来</button>
</footer>
<script>
const log=document.getElementById('log'),input=document.getElementById('input'),send=document.getElementById('send'),dot=document.getElementById('dot'),stat=document.getElementById('stat')
function item(text,meta,err){
  const d=document.createElement('div');d.className='item'+(err?' err':'');d.textContent=text
  if(meta){const m=document.createElement('div');m.className='meta';m.textContent=meta;d.appendChild(m)}
  log.appendChild(d);log.scrollTop=log.scrollHeight
}
async function state(){
  try{
    const r=await fetch('/say-state');const j=await r.json()
    const n=(j.devices||[]).length
    dot.className=n?'on':'';stat.textContent=n?'板子在线':'板子不在线（唤醒一次）'
  }catch{stat.textContent='服务器没在跑'}
}
async function say(){
  const v=input.value.trim();if(!v||send.disabled)return
  input.value='';send.disabled=true
  try{
    const r=await fetch('/say',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:v})})
    const j=await r.json()
    if(!j.ok) throw new Error(j.error||'发送失败')
    item(v,new Date().toLocaleTimeString()+' 已发送')
  }catch(e){item('（没发出去）'+e.message,'',true)}
  send.disabled=false;state();input.focus()
}
send.onclick=say
input.addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();say()}})

let volume=70
const volv=document.getElementById('volv'),vstat=document.getElementById('vstat')
async function setVolume(v){
  v=Math.max(0,Math.min(100,Math.round(v)))
  vstat.textContent='设置中…'
  try{
    const r=await fetch('/say-tool',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:'self.audio_speaker.set_volume',arguments:{volume:v}})})
    const j=await r.json()
    if(!j.ok) throw new Error(j.error||'设置失败')
    volume=v;volv.textContent=v;vstat.textContent=''
    item('音量 → '+v,'直接调用设备工具')
  }catch(e){vstat.textContent='失败';item('（音量没设成）'+e.message,'',true)}
}
document.getElementById('vdown').onclick=()=>setVolume(volume-10)
document.getElementById('vup').onclick=()=>setVolume(volume+10)
document.getElementById('v30').onclick=()=>setVolume(30)
document.getElementById('v50').onclick=()=>setVolume(50)
document.getElementById('v70').onclick=()=>setVolume(70)

state();setInterval(state,3000);input.focus()
</script></body></html>`

const server = http.createServer((req, res) => {
  if (req.method === 'GET' && req.url.startsWith('/health')) {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    res.end(JSON.stringify({ ok: true, ...acp.status() }))
    return
  }
  if (req.method === 'GET' && (req.url === '/' || req.url.startsWith('/chat'))) {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' })
    res.end(CHAT_PAGE)
    return
  }
  if (req.method === 'GET' && req.url.startsWith('/say-state')) {
    relay(req, res, '/debug/state', null)
    return
  }
  if (req.method === 'GET' && (req.url === '/say' || req.url.startsWith('/say?'))) {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' })
    res.end(SAY_PAGE)
    return
  }
  if (req.method === 'POST' && req.url.startsWith('/say-tool')) {
    let body = ''
    req.on('data', (chunk) => (body += chunk))
    req.on('end', () => {
      let name = ''
      let args = {}
      try {
        const parsed = JSON.parse(body || '{}')
        name = String(parsed.name || '')
        args = parsed.arguments || {}
      } catch {
        /* ignore */
      }
      relay(req, res, '/debug/tool', JSON.stringify({ name, arguments: args }))
    })
    return
  }
  if (req.method === 'POST' && req.url.startsWith('/say')) {
    let body = ''
    req.on('data', (chunk) => (body += chunk))
    req.on('end', () => {
      let text = ''
      try {
        text = String(JSON.parse(body || '{}').text || '').trim()
      } catch {
        /* ignore */
      }
      relay(req, res, '/debug/say', JSON.stringify({ text }))
    })
    return
  }
  if (req.method !== 'POST' || !req.url.startsWith('/v1/chat/completions')) {
    res.writeHead(404).end('not found')
    return
  }

  let body = ''
  req.on('data', (chunk) => (body += chunk))
  req.on('end', async () => {
    let payload
    try {
      payload = JSON.parse(body || '{}')
    } catch {
      res.writeHead(400).end('bad json')
      return
    }
    const messages = Array.isArray(payload.messages) ? payload.messages : []
    const lastUser = [...messages].reverse().find((m) => m.role === 'user')
    trace(`POST /v1/chat/completions stream=${payload.stream === true} msgs=${messages.length} last="${String(lastUser?.content || '').slice(0, 30)}"`)
    if (!lastUser) {
      res.writeHead(400).end('no user message')
      return
    }
    req.on('aborted', () => trace('client aborted request'))
    res.on('close', () => trace(`response closed (writableEnded=${res.writableEnded})`))

    const started = Date.now()
    let text
    try {
      text = await acp.ask(buildPrompt(messages))
    } catch (error) {
      log('ask failed:', error.message)
      res.writeHead(500, { 'Content-Type': 'application/json' })
      res.end(JSON.stringify({ error: { message: error.message } }))
      return
    }
    if (!text) text = '……我刚才走神了，你再说一遍？'
    appendMemory('user', String(lastUser.content || ''))
    appendMemory('assistant', text)
    log(`turn ${acp.turns} ${Date.now() - started}ms → ${text.slice(0, 40)}`)

    const id = `chatcmpl-${Date.now()}`
    const created = Math.floor(Date.now() / 1000)

    if (!payload.stream) {
      res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end(
        JSON.stringify({
          id,
          object: 'chat.completion',
          created,
          model: payload.model || 'foxy-voice',
          choices: [{ index: 0, message: { role: 'assistant', content: text }, finish_reason: 'stop' }]
        })
      )
      return
    }

    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-cache',
      // 关键：不要让客户端复用连接。Node 默认 5 秒就关掉空闲 keep-alive，
      // 而 xiaozhi-server 的 httpx 连接池会把死连接留着，下一次请求直接报
      // "Connection error." 且根本到不了这里。显式 close 才能保证每轮重新握手。
      Connection: 'close'
    })
    const chunk = (content) =>
      `data: ${JSON.stringify({
        id,
        object: 'chat.completion.chunk',
        created,
        model: payload.model || 'foxy-voice',
        choices: [{ index: 0, delta: { content }, finish_reason: null }]
      })}\n\n`

    for (const sentence of splitSentences(text)) res.write(chunk(sentence))
    res.write(`data: ${JSON.stringify({ id, choices: [{ index: 0, delta: {}, finish_reason: 'stop' }] })}\n\n`)
    res.write('data: [DONE]\n\n')
    res.end()
  })
})

async function relay(req, res, path, jsonBody) {
  try {
    const init = jsonBody
      ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: jsonBody }
      : { method: 'GET' }
    const r = await fetch(SERVER_BASE + path, init)
    const text = await r.text()
    res.writeHead(r.status, { 'Content-Type': 'application/json; charset=utf-8' })
    res.end(text)
  } catch (error) {
    res.writeHead(502, { 'Content-Type': 'application/json; charset=utf-8' })
    res.end(JSON.stringify({ ok: false, error: `服务器没在跑: ${error.message}` }))
  }
}

server.keepAliveTimeout = 65000
server.headersTimeout = 66000

server.on('error', (error) => {
  log('http server error:', error.message)
  process.exitCode = 1
})

server.listen(CFG.port, '127.0.0.1', async () => {
  log(`foxy voice bridge on http://127.0.0.1:${CFG.port}`)
  log('persona:', CFG.personaPath, fs.existsSync(CFG.personaPath) ? '(loaded)' : '(fallback)')
  if (CFG.warmup) {
    try {
      await acp.ask('你好')
      log('warmup done')
    } catch (error) {
      log('warmup failed:', error.message)
    }
  }
})
