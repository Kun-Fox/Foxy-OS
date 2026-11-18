"""设备端快捷指令：音量 / 亮度。

大模型（Foxy 桥）不会去调用设备端 MCP 工具，所以"把音量调低"这种话
单纯交给 LLM 是不会生效的。这里在交给 LLM 之前先做一次本地拦截：
命中音量/亮度指令就直接调用设备 MCP 工具，并让板子用 TTS 回一句确认。

返回 True 表示已经处理完（不再走 LLM）。
"""

import json
import re
import uuid
from typing import Optional

from core.providers.tts.dto.dto import ContentType, TTSMessageDTO, SentenceType
from core.providers.tools.device_mcp.mcp_handler import call_mcp_tool

TAG = __name__

_CN_DIGITS = {
    "零": 0,
    "〇": 0,
    "一": 1,
    "二": 2,
    "两": 2,
    "三": 3,
    "四": 4,
    "五": 5,
    "六": 6,
    "七": 7,
    "八": 8,
    "九": 9,
}
_CN_UNITS = {"十": 10, "百": 100}

VOLUME_WORDS = re.compile(r"(音量|声音|嗓门|响度|喇叭)")
BRIGHT_WORDS = re.compile(r"(亮度|屏幕亮|屏幕亮度|背光|屏幕暗)")

UP_WORDS = re.compile(r"(大一点|大点|大些|调大|开大|增大|提高|调高|高一点|听不清|听不见|太轻|太小|有点小|有些小|有点轻)")
DOWN_WORDS = re.compile(r"(小一点|小点|小些|调小|关小|降低|减小|调低|低一点|太大|太响|太吵|刺耳|吵死)")
MAX_WORDS = re.compile(r"(最大|最响|调到最大)")
MIN_WORDS = re.compile(r"(最小|静音|闭嘴|调到最小|关了|关掉)")

_CN_CHAR = r"[零〇一二两三四五六七八九十百\d]"
_NUM_RE = re.compile(r"(\d{1,3})\s*(?:%|％|分|成)?")
_PERCENT_RE = re.compile(r"百分之\s*(" + _CN_CHAR + r"{1,4})")
_SIGN_RE = re.compile(r"(\d{1,3})\s*[%％]")
_ABS_RE = re.compile(r"(?:调到|调成|设置成|设为|设置为|改成)\s*(" + _CN_CHAR + r"{1,4})")

# device_id -> 最近一次设定值（固件默认 70）
_last_volume: dict[str, int] = {}
DEFAULT_VOLUME = 70


def _parse_chinese_number(text: str) -> Optional[int]:
    """把"三十""四十""一百"这类中文数字转成 int，失败返回 None。"""
    if not text:
        return None
    if _NUM_RE.search(text):
        return int(_NUM_RE.search(text).group(1))

    digits = [c for c in text if c in _CN_DIGITS]
    units = [c for c in text if c in _CN_UNITS]
    if not digits and not units:
        return None

    # 形如 十 / 十五 / 二十 / 二十三 / 一百
    total = 0
    current = 0
    for ch in text:
        if ch in _CN_DIGITS:
            current = _CN_DIGITS[ch]
        elif ch in _CN_UNITS:
            unit = _CN_UNITS[ch]
            if current == 0:
                current = 1
            total += current * unit
            current = 0
    total += current
    return total if total > 0 else None


def _target_volume(device_id: str, text: str) -> Optional[int]:
    """根据一句话算出目标音量（0-100），判断不出来返回 None。"""
    base = _last_volume.get(device_id, DEFAULT_VOLUME)

    # 1) 绝对数值：百分之六十 / 60% / 调到六十
    number = None
    for regex in (_PERCENT_RE, _SIGN_RE, _ABS_RE):
        match = regex.search(text)
        if match:
            number = _parse_chinese_number(match.group(1))
            if number is not None:
                break
    if number is not None:
        return max(0, min(100, number))

    # 2) 极值
    if MIN_WORDS.search(text):
        return 0
    if MAX_WORDS.search(text):
        return 100

    # 3) 相对：太吵/太大要先于"大一点"判断
    if DOWN_WORDS.search(text):
        return max(0, min(100, base - 10))
    if UP_WORDS.search(text):
        return max(0, min(100, base + 10))
    return None


def resolve_tool_name(conn, name: str) -> Optional[str]:
    """把设备报上来的原始工具名解析成 MCP 客户端内部的（已净化的）键。

    MCPClient 用 sanitize_tool_name() 把 "self.audio_speaker.set_volume" 存成
    "self_audio_speaker_set_volume"，所以直接拿原点号名字查 has_tool 会查不到。
    """
    mcp_client = getattr(conn, "mcp_client", None)
    if not mcp_client:
        return None
    if name in getattr(mcp_client, "tools", {}):
        return name
    for sanitized, original in getattr(mcp_client, "name_mapping", {}).items():
        if original == name:
            return sanitized
    return None


async def _speak(conn, text: str) -> None:
    """让板子念一句确认语（沿用服务器现成的 TTS 队列用法）。"""
    if not getattr(conn, "tts", None):
        return
    conn.sentence_id = uuid.uuid4().hex
    conn.tts.store_tts_text(conn.sentence_id, text)
    conn.tts.tts_text_queue.put(
        TTSMessageDTO(
            sentence_id=conn.sentence_id,
            sentence_type=SentenceType.FIRST,
            content_type=ContentType.ACTION,
        )
    )
    conn.tts.tts_one_sentence(conn, ContentType.TEXT, content_detail=text)
    conn.tts.tts_text_queue.put(
        TTSMessageDTO(
            sentence_id=conn.sentence_id,
            sentence_type=SentenceType.LAST,
            content_type=ContentType.ACTION,
        )
    )


async def handle_device_command(conn, text: str) -> bool:
    """命中音量/亮度指令则直接处理并返回 True。"""
    if not text or not getattr(conn, "mcp_client", None):
        return False

    device_id = getattr(conn, "device_id", "") or ""

    if VOLUME_WORDS.search(text):
        target = _target_volume(device_id, text)
        if target is None:
            return False
        try:
            tool = resolve_tool_name(conn, "self.audio_speaker.set_volume")
            if not tool:
                raise RuntimeError("设备没有上报音量工具")
            result = await call_mcp_tool(
                conn,
                conn.mcp_client,
                tool,
                json.dumps({"volume": target}),
            )
            conn.logger.bind(tag=TAG).info(f"音量指令: {text!r} -> {target}, 结果: {result}")
        except Exception as e:
            conn.logger.bind(tag=TAG).error(f"设置音量失败: {e}")
            await _speak(conn, "音量没调成功，我这边联系不上喇叭。")
            return True
        _last_volume[device_id] = target
        await _speak(conn, f"音量调到百分之{target}了。")
        return True

    if BRIGHT_WORDS.search(text):
        base_map = {"self.screen.set_brightness": None}
        try:
            # 亮度直接用 0-100 的相对值：亮一点 +20，暗一点 -20
            value = 100 if MAX_WORDS.search(text) else 10 if MIN_WORDS.search(text) else None
            if value is None:
                if UP_WORDS.search(text):
                    value = 80
                elif DOWN_WORDS.search(text):
                    value = 20
            if value is None:
                return False
            await call_mcp_tool(
                conn,
                conn.mcp_client,
                resolve_tool_name(conn, "self.screen.set_brightness") or "self.screen.set_brightness",
                json.dumps({"brightness": value}),
            )
            await _speak(conn, "屏幕亮度调好了。")
            return True
        except Exception as e:
            conn.logger.bind(tag=TAG).error(f"设置亮度失败: {e}")
            return False

    return False
