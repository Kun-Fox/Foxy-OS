import asyncio
import json
from aiohttp import web
from config.logger import setup_logging
from core.api.ota_handler import OTAHandler
from core.api.vision_handler import VisionHandler

TAG = __name__


class SimpleHttpServer:
    def __init__(self, config: dict):
        self.config = config
        self.logger = setup_logging()
        self.ota_handler = OTAHandler(config)
        self.vision_handler = VisionHandler(config)

    def _get_websocket_url(self, local_ip: str, port: int) -> str:
        """获取websocket地址

        Args:
            local_ip: 本地IP地址
            port: 端口号

        Returns:
            str: websocket地址
        """
        server_config = self.config["server"]
        websocket_config = server_config.get("websocket")

        if websocket_config and "你" not in websocket_config:
            return websocket_config
        else:
            return f"ws://{local_ip}:{port}/xiaozhi/v1/"

    async def handle_debug_say(self, request):
        """让在线设备"念"一句话：等价于设备自己发来 listen(detect, text)。

        用法: POST http://127.0.0.1:8003/debug/say {"text": "你好"}
        """
        import json as _json
        from core.utils import conn_registry
        from core.handle.textHandle import handleTextMessage

        try:
            data = await request.json()
        except Exception:
            data = {}
        text = (data.get("text") or "").strip()
        if not text:
            return web.json_response(
                {"ok": False, "error": "text 不能为空"}, status=400
            )
        conn = conn_registry.get(data.get("device_id"))
        if conn is None or getattr(conn, "websocket", None) is None:
            return web.json_response(
                {"ok": False, "error": "没有在线的设备连接"}, status=404
            )
        msg = _json.dumps(
            {"type": "listen", "state": "detect", "text": text}, ensure_ascii=False
        )
        asyncio.create_task(handleTextMessage(conn, msg))
        return web.json_response(
            {"ok": True, "device_id": getattr(conn, "device_id", None), "text": text}
        )

    async def handle_debug_tool(self, request):
        """直接调用设备端 MCP 工具（例如调音量/亮度），不经过大模型。

        用法: POST http://127.0.0.1:8003/debug/tool
              {"name": "self.audio_speaker.set_volume", "arguments": {"volume": 40}}
        """
        from core.utils import conn_registry
        from core.providers.tools.device_mcp.mcp_handler import call_mcp_tool

        try:
            data = await request.json()
        except Exception:
            data = {}
        name = (data.get("name") or "").strip()
        if not name:
            return web.json_response(
                {"ok": False, "error": "name 不能为空"}, status=400
            )
        conn = conn_registry.get(data.get("device_id"))
        if conn is None or getattr(conn, "websocket", None) is None:
            return web.json_response(
                {"ok": False, "error": "没有在线的设备连接"}, status=404
            )
        mcp_client = getattr(conn, "mcp_client", None)
        if not mcp_client:
            return web.json_response(
                {"ok": False, "error": "设备的 MCP 客户端没准备好"}, status=503
            )
        try:
            from core.utils.device_commands import resolve_tool_name

            tool = resolve_tool_name(conn, name) or name
            result = await call_mcp_tool(
                conn,
                mcp_client,
                tool,
                json.dumps(data.get("arguments") or {}, ensure_ascii=False),
            )
            return web.json_response(
                {"ok": True, "device_id": getattr(conn, "device_id", None), "name": name, "result": result}
            )
        except Exception as e:
            return web.json_response({"ok": False, "error": str(e)}, status=500)

    async def handle_debug_notify(self, request):
        """给设备首页消息卡推一条消息：POST /debug/notify {"text": "..."}。"""
        from core.utils import conn_registry

        try:
            data = await request.json()
        except Exception:
            data = {}
        text = (data.get("text") or "").strip()
        if not text:
            return web.json_response({"ok": False, "error": "text 不能为空"}, status=400)
        connections = conn_registry.all_devices()
        if not connections:
            return web.json_response({"ok": False, "error": "没有在线的设备连接"}, status=404)
        payload = json.dumps({"type": "foxy", "message": text}, ensure_ascii=False)
        sent = []
        for device_id, conn in connections.items():
            try:
                await conn.websocket.send(payload)
                sent.append(device_id)
            except Exception as e:
                self.logger.bind(tag=TAG).error(f"推送消息失败 {device_id}: {e}")
        return web.json_response({"ok": bool(sent), "devices": sent})

    async def handle_debug_state(self, request):
        """查看当前在线的设备连接。"""
        from core.utils import conn_registry

        devices = []
        for device_id, conn in conn_registry.all_devices().items():
            devices.append(
                {
                    "device_id": device_id,
                    "speaking": bool(getattr(conn, "client_is_speaking", False)),
                    "session_id": getattr(conn, "session_id", None),
                }
            )
        return web.json_response({"ok": True, "devices": devices})

    def _music_dir(self):
        import os

        return os.environ.get("FOXY_MUSIC_DIR", r"F:\cores3-server\music")

    def _music_tracks(self):
        import os

        folder = self._music_dir()
        if not os.path.isdir(folder):
            return []
        tracks = []
        for name in sorted(os.listdir(folder)):
            if not name.lower().endswith(".mp3"):
                continue
            path = os.path.join(folder, name)
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            tracks.append({"name": name, "size": size})
        return tracks

    async def handle_debug_music_list(self, request):
        """曲库列表：扫描 F:\\cores3-server\\music 下的 mp3。"""
        tracks = self._music_tracks()
        return web.json_response({"ok": True, "tracks": tracks, "dir": self._music_dir()})

    async def handle_debug_music_file(self, request):
        """按索引流式返回 mp3，设备自己解码播放。"""
        import os

        tracks = self._music_tracks()
        try:
            index = int(request.query.get("index", "0"))
        except ValueError:
            index = 0
        if index < 0 or index >= len(tracks):
            return web.Response(status=404, text="no such track")
        path = os.path.join(self._music_dir(), tracks[index]["name"])
        return web.FileResponse(
            path,
            headers={
                "Content-Type": "audio/mpeg",
                "Cache-Control": "no-store",
                "Accept-Ranges": "none",
            },
        )

    async def handle_debug_music_control(self, request):
        """远程控制设备播放：POST /debug/music/control {"action":"play","index":0}。"""
        from core.utils import conn_registry

        try:
            data = await request.json()
        except Exception:
            data = {}
        action = (data.get("action") or "play").strip()
        index = data.get("index", -1)
        connections = conn_registry.all_devices()
        if not connections:
            return web.json_response({"ok": False, "error": "没有在线的设备连接"}, status=404)
        payload = json.dumps({"type": "music", "action": action, "index": index}, ensure_ascii=False)
        sent = []
        for device_id, conn in connections.items():
            try:
                await conn.websocket.send(payload)
                sent.append(device_id)
            except Exception as e:
                self.logger.bind(tag=TAG).error(f"音乐控制失败 {device_id}: {e}")
        return web.json_response({"ok": bool(sent), "devices": sent, "action": action, "index": index})

    async def start(self):
        try:
            server_config = self.config["server"]
            read_config_from_api = self.config.get("read_config_from_api", False)
            host = server_config.get("ip", "0.0.0.0")
            port = int(server_config.get("http_port", 8003))

            if port:
                app = web.Application()

                if not read_config_from_api:
                    # 如果没有开启智控台，只是单模块运行，就需要再添加简单OTA接口，用于下发websocket接口
                    app.add_routes(
                        [
                            web.get("/xiaozhi/ota/", self.ota_handler.handle_get),
                            web.post("/xiaozhi/ota/", self.ota_handler.handle_post),
                            web.options(
                                "/xiaozhi/ota/", self.ota_handler.handle_options
                            ),
                            # 下载接口，仅提供 data/bin/*.bin 下载
                            web.get(
                                "/xiaozhi/ota/download/{filename}",
                                self.ota_handler.handle_download,
                            ),
                            web.options(
                                "/xiaozhi/ota/download/{filename}",
                                self.ota_handler.handle_options,
                            ),
                        ]
                    )
                # 添加路由
                app.add_routes(
                    [
                        web.get("/mcp/vision/explain", self.vision_handler.handle_get),
                        web.post(
                            "/mcp/vision/explain", self.vision_handler.handle_post
                        ),
                        web.options(
                            "/mcp/vision/explain", self.vision_handler.handle_options
                        ),
                        # 本机调试：把一句话注入在线设备会话，让板子念出来
                        web.post("/debug/say", self.handle_debug_say),
                        web.get("/debug/state", self.handle_debug_state),
                        # 直接调用设备端 MCP 工具（音量/亮度等），不经过大模型
                        web.post("/debug/tool", self.handle_debug_tool),
                        # 给首页消息卡推一条消息
                        web.post("/debug/notify", self.handle_debug_notify),
                        # 音乐：曲库列表 + 按索引流式取 mp3
                        web.get("/debug/music/list", self.handle_debug_music_list),
                        web.get("/debug/music/file", self.handle_debug_music_file),
                        web.post("/debug/music/control", self.handle_debug_music_control),
                    ]
                )

                # 运行服务
                runner = web.AppRunner(app)
                await runner.setup()
                site = web.TCPSite(runner, host, port)
                await site.start()

                # 保持服务运行
                while True:
                    await asyncio.sleep(3600)  # 每隔 1 小时检查一次
        except Exception as e:
            self.logger.bind(tag=TAG).error(f"HTTP服务器启动失败: {e}")
            import traceback

            self.logger.bind(tag=TAG).error(f"错误堆栈: {traceback.format_exc()}")
            raise
