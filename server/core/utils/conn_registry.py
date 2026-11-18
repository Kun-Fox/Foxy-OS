"""在线设备连接注册表（仅本机调试用）

用途：让 HTTP 接口能把一句文字注入到「已连上来的真设备」会话里，
等价于设备自己发了一条 listen(detect, text)，这样就能不打字给板子、
只用键盘测试语音链路。
"""

from typing import Dict, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from core.connection import ConnectionHandler

_connections: Dict[str, "ConnectionHandler"] = {}


def register(conn: "ConnectionHandler") -> None:
    device_id = getattr(conn, "device_id", None)
    if device_id:
        _connections[device_id] = conn


def unregister(conn: "ConnectionHandler") -> None:
    device_id = getattr(conn, "device_id", None)
    if device_id and _connections.get(device_id) is conn:
        _connections.pop(device_id, None)


def get(device_id: Optional[str] = None) -> Optional["ConnectionHandler"]:
    if device_id:
        return _connections.get(device_id)
    if len(_connections) == 1:
        return next(iter(_connections.values()))
    return None


def all_devices() -> Dict[str, "ConnectionHandler"]:
    return dict(_connections)
