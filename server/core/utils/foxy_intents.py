"""Foxy 的设备意图：音乐控制、拍照、闹钟。

大模型（Foxy 桥）不会去调用设备端工具，所以"放首歌""拍张照""定个闹钟"
这类话在交给 LLM 之前先在这里拦一道，命中就直接操作设备。
返回 True 表示已经处理完，不再走 LLM。
"""

import asyncio
import json
import re
import time

from core.utils.device_commands import _speak, resolve_tool_name
from core.providers.tools.device_mcp.mcp_handler import call_mcp_tool

TAG = __name__

_MUSIC_STOP = re.compile(r"(停止播放|关掉音乐|别放了|安静|不要放了|停止音乐)")
_MUSIC_NEXT = re.compile(r"(下一首|换一首|下一曲|切歌|换首歌)")
_MUSIC_PREV = re.compile(r"(上一首|前一首|上一曲|回到上一首)")
_MUSIC_PAUSE = re.compile(r"(暂停|先停一下|停一下|暂停播放)")
_MUSIC_PLAY = re.compile(
    r"(播放|放首|放一首|放首歌|放点音乐|来点音乐|来首歌|听歌|唱一首|放音乐)"
)

_CAMERA = re.compile(r"(拍张照|拍个照|拍一张|拍照|照相|看看现在|摄像头看看|看看我在哪)")

_ALARM = re.compile(r"(闹钟|提醒我|定时|计时)")
_MINUTES = re.compile(r"(\d{1,3})\s*(分钟|分|minutes?)")
_SECONDS = re.compile(r"(\d{1,3})\s*(秒|seconds?)")

_MUSIC_ACTIONS = (
    (_MUSIC_STOP, "stop"),
    (_MUSIC_NEXT, "next"),
    (_MUSIC_PREV, "prev"),
    (_MUSIC_PAUSE, "pause"),
)


async def _send_device(conn, payload: dict) -> None:
    websocket = getattr(conn, "websocket", None)
    if websocket is None:
        return
    await websocket.send(json.dumps(payload, ensure_ascii=False))


async def _handle_music(conn, text: str) -> bool:
    if _MUSIC_PLAY.search(text) and not _MUSIC_STOP.search(text):
        # "放首歌"：如果有正在播的，就继续；否则从第一首开始
        await _send_device(conn, {"type": "music", "action": "play", "index": -1})
        conn.logger.bind(tag=TAG).info(f"音乐指令: {text!r} -> play")
        await _speak(conn, "好，放点音乐。")
        return True

    for pattern, action in _MUSIC_ACTIONS:
        if pattern.search(text):
            await _send_device(conn, {"type": "music", "action": action, "index": -1})
            conn.logger.bind(tag=TAG).info(f"音乐指令: {text!r} -> {action}")
            replies = {
                "stop": "好，关掉。",
                "next": "换一首。",
                "prev": "上一首。",
                "pause": "先暂停。",
            }
            await _speak(conn, replies.get(action, "好。"))
            return True
    return False


async def _handle_camera(conn, text: str) -> bool:
    if not _CAMERA.search(text):
        return False
    try:
        tool = resolve_tool_name(conn, "self.camera.take_photo")
        if not tool:
            await _speak(conn, "摄像头还没准备好。")
            return True
        result = await call_mcp_tool(conn, conn.mcp_client, tool, "{}")
        conn.logger.bind(tag=TAG).info(f"拍照: 结果 {result}")
        await _speak(conn, "拍好了。")
    except Exception as e:
        conn.logger.bind(tag=TAG).error(f"拍照失败: {e}")
        await _speak(conn, "拍照没成功。")
    return True


async def _alarm_worker(conn, seconds: int, label: str) -> None:
    await asyncio.sleep(seconds)
    try:
        await _send_device(conn, {"type": "foxy", "message": f"闹钟：{label}"})
    except Exception:
        pass
    try:
        await _speak(conn, f"到点了，{label}。")
    except Exception:
        pass


async def _handle_alarm(conn, text: str) -> bool:
    if not _ALARM.search(text):
        return False
    seconds = 0
    minutes_match = _MINUTES.search(text)
    seconds_match = _SECONDS.search(text)
    if minutes_match:
        seconds = int(minutes_match.group(1)) * 60
    elif seconds_match:
        seconds = int(seconds_match.group(1))
    if seconds <= 0:
        # "定个闹钟" 没说时间：先反问一句，别瞎定
        await _speak(conn, "几分钟后？你说个时间。")
        return True
    if seconds > 24 * 3600:
        await _speak(conn, "太久了，我记不住。")
        return True
    label = text.strip()
    asyncio.create_task(_alarm_worker(conn, seconds, label))
    conn.logger.bind(tag=TAG).info(f"闹钟: {seconds}s label={label!r}")
    minutes = seconds // 60
    if minutes > 0:
        await _speak(conn, f"好，{minutes}分钟后叫你。")
    else:
        await _speak(conn, f"好，{seconds}秒后叫你。")
    return True


async def handle_intent(conn, text: str) -> bool:
    """音乐 / 拍照 / 闹钟，命中就处理掉。"""
    if not text:
        return False
    if await _handle_music(conn, text):
        return True
    if await _handle_alarm(conn, text):
        return True
    if await _handle_camera(conn, text):
        return True
    return False
