# Foxy-OS

把 M5Stack CoreS3 做成「Foxy」的随身语音终端：屏幕、麦克风、喇叭、Wi-Fi 都在板子上，脑子在电脑上。

```
CoreS3 ──Wi-Fi──▶ 本地 xiaozhi-server ──HTTP──▶ Foxy 桥 ──ACP──▶ DSH（人格 / 记忆 / 工具）
   ▲                   (VAD/ASR/TTS)              :8010
   └────────── Opus 音频 / WebSocket ─────────────┘
```

## 现在有什么

- **固件**：基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.6.0 的 CoreS3 定制版
  - 待命常连：回完话不挂断，服务器随时可以推文本让它开口（不用再点屏幕）
  - 息屏待机：空闲 60 秒关背光，联网与唤醒词继续工作，不关机
  - 底部控制条：对话 / 静音 / 音量− / 音量+ / 信息（触摸点位实测可用）
  - SPI 80MHz + 40 行刷新缓冲（整屏重绘约 45–60ms）
- **服务器补丁**（打在 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 上）
  - 每 30 秒 ping 保活，设备不再被静默断开
  - `POST /debug/say`：网页打字直接让板子念
  - `POST /debug/tool`：直接调用设备的 MCP 工具（音量、亮度、拍照）
  - 本地指令拦截：说「音量调到三十」「太吵了」不再经过大模型
- **桥**：`bridge/foxy-voice-bridge.mjs`，零依赖，把 OpenAI 兼容接口转成 DSH 的 ACP 会话，附带网页文字台与音量条

## 仓库结构

```
overlay/              # 直接覆盖到固件源码树上的文件
patches/              # 对上游的 patch（固件 + 服务器）
server/               # 服务器侧新增/覆盖文件
bridge/               # Foxy 语音桥（Node，零依赖）
scripts/              # 引导与构建脚本
docs/                 # 设计笔记、踩坑记录、路线图
```

## 从零开始

```powershell
# 1) 拉上游源码并打补丁（约 3 分钟，走 gh-proxy 镜像）
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1

# 2) 安装 ESP-IDF（只需一次，约 3GB）
#    参考 scripts\install-idf.ps1 里的步骤，或用官方安装器装 release/v6.1

# 3) 编译
powershell -ExecutionPolicy Bypass -File scripts\build.ps1

# 4) 只烧 app 分区（不要烧整包，会覆盖 NVS 里的 Wi-Fi 与 OTA 地址）
powershell -ExecutionPolicy Bypass -File scripts\flash-app.ps1
```

首次烧录整包（`merged-binary.bin` 到 `0x0`）会清掉 NVS，之后必须重新写配置或用网页配网。

## 设备档案

| 项 | 值 |
|---|---|
| 板卡 | M5Stack CoreS3（SKU `m5stack-core-s3`，ESP32-S3 16MB/8MB） |
| 串口 | COM7（ESP32-S3 原生 USB-Serial/JTAG，无需额外驱动） |
| MAC | 7c:4f:ad:b0:4e:18 |
| 屏幕 | 320×240 IPS，ILI9342，SPI3 @ 80MHz，触摸 FT6336 |
| 音频 | ES7210 三麦（TDM）+ AW88298 功放，16kHz 上行 / 24kHz 下行 Opus |
| 分区 | nvs 0x9000/16K、ota_0 0x20000/4M、ota_1 0x3f0000/4M、assets 8M |

## 已知限制

- CoreS3 没有播放参考通道，**设备端 AEC 不可用**，只能半双工（说完才放）
- M5Stack 出厂的摄像头/指南针演示页是另一套固件，与本固件分区冲突，不能共存
- LVGL 9 在这套主题字体下，给标签显式设字体会崩（`InstrFetchProhibited`），一律继承主题字体
- 全屏滑动约 20fps，做不出视频里那种丝滑；切页用短动画更稳

## 路线图

- [x] 语音对话闭环（自建服务器 + Foxy 桥）
- [x] 打字说话、音量指令、息屏待机
- [x] 多页 UI（首页 / 应用 / 设置 / 对话 / 音乐 / 计算器 / 触摸测试，共 9 页）
- [x] 消息提醒通道（首页消息卡，服务器 `POST /debug/notify` 推送）
- [x] 音乐播放（服务器 `cores3-server\music` 里的 mp3 流式播放，设备端解码）
- [ ] 相机预览、小说、闹钟、文件与 SSH
