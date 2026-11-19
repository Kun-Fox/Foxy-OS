import asyncio
import json
import os
import pathlib
import time
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

    _duration_cache = {}

    def _music_duration_ms(self, path):
        """用 ffprobe 读时长，结果按 (路径, 修改时间) 缓存，避免每次列表都跑子进程。"""
        import os
        import subprocess

        try:
            key = (path, os.path.getmtime(path))
        except OSError:
            return 0
        cached = self._duration_cache.get(key)
        if cached is not None:
            return cached
        ffprobe = os.environ.get(
            "FOXY_FFPROBE", r"F:\kunfox\ffmpeg-8.1.2-essentials_build\bin\ffprobe.exe"
        )
        duration = 0
        try:
            out = subprocess.run(
                [ffprobe, "-v", "error", "-show_entries", "format=duration",
                 "-of", "default=noprint_wrappers=1:nokey=1", path],
                capture_output=True, text=True, timeout=5,
            ).stdout.strip()
            duration = int(float(out) * 1000)
        except Exception:
            duration = 0
        self._duration_cache[key] = duration
        return duration

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
            stem = os.path.splitext(name)[0]
            duration_ms = self._music_duration_ms(path)
            cover = ""
            for ext in (".jpg", ".jpeg", ".png"):
                candidate = os.path.join(folder, stem + ext)
                if os.path.exists(candidate):
                    cover = stem + ext
                    break
            lyrics = stem + ".lrc"
            tracks.append(
                {
                    "name": name,
                    "size": size,
                    "cover": cover,
                    "duration_ms": duration_ms,
                    "lyrics": lyrics if os.path.exists(os.path.join(folder, lyrics)) else "",
                }
            )
        return tracks

    async def handle_debug_music_cover(self, request):
        """专辑封面。

        默认返回原图；带 ?fmt=rgb565&w=320&h=240 时，先在服务端缩放到目标尺寸
        （居中裁切，不变形）并转成 RGB565 原始像素，设备拿到就能直接贴屏，
        省掉在 MCU 上跑 JPEG 解码。
        """
        import io
        import os
        import struct

        tracks = self._music_tracks()
        try:
            index = int(request.query.get("index", "0"))
        except ValueError:
            index = 0
        if index < 0 or index >= len(tracks) or not tracks[index]["cover"]:
            return web.Response(status=404, text="no cover")
        path = os.path.join(self._music_dir(), tracks[index]["cover"])

        fmt = request.query.get("fmt", "")
        if fmt != "rgb565":
            content_type = "image/png" if path.lower().endswith(".png") else "image/jpeg"
            return web.FileResponse(path, headers={"Content-Type": content_type, "Cache-Control": "no-store"})

        try:
            width = int(request.query.get("w", "320"))
            height = int(request.query.get("h", "240"))
        except ValueError:
            width, height = 320, 240

        from PIL import Image

        image = Image.open(path).convert("RGB")
        # 居中裁切到目标比例再缩放，避免拉伸变形
        target_ratio = width / height
        src_ratio = image.width / image.height
        if src_ratio > target_ratio:
            new_w = int(image.height * target_ratio)
            left = (image.width - new_w) // 2
            image = image.crop((left, 0, left + new_w, image.height))
        else:
            new_h = int(image.width / target_ratio)
            top = (image.height - new_h) // 2
            image = image.crop((0, top, image.width, top + new_h))
        image = image.resize((width, height), Image.LANCZOS)

        raw = image.tobytes()
        packed = bytearray(width * height * 2)
        for i in range(width * height):
            r, g, b = raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]
            value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            struct.pack_into("<H", packed, i * 2, value)
        return web.Response(
            body=bytes(packed),
            headers={"Content-Type": "application/octet-stream", "Cache-Control": "no-store"},
        )

    async def handle_debug_music_lyrics(self, request):
        """歌词：同名 .lrc，纯文本返回。"""
        import os

        tracks = self._music_tracks()
        try:
            index = int(request.query.get("index", "0"))
        except ValueError:
            index = 0
        if index < 0 or index >= len(tracks) or not tracks[index]["lyrics"]:
            return web.Response(status=404, text="no lyrics")
        path = os.path.join(self._music_dir(), tracks[index]["lyrics"])
        with open(path, "rb") as handle:
            data = handle.read()
        return web.Response(body=data, headers={"Content-Type": "text/plain; charset=utf-8"})

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

    async def handle_debug_push(self, request):
        """把任意 JSON 原样推给在线设备：POST /debug/push {"json": {...}}。"""
        from core.utils import conn_registry

        try:
            data = await request.json()
        except Exception:
            data = {}
        payload = data.get("json")
        if not isinstance(payload, dict):
            return web.json_response({"ok": False, "error": "缺少 json 字段"}, status=400)
        connections = conn_registry.all_devices()
        if not connections:
            return web.json_response({"ok": False, "error": "没有在线的设备连接"}, status=404)
        text = json.dumps(payload, ensure_ascii=False)
        sent = []
        for device_id, conn in connections.items():
            try:
                await conn.websocket.send(text)
                sent.append(device_id)
            except Exception as e:
                self.logger.bind(tag=TAG).error(f"推送失败 {device_id}: {e}")
        return web.json_response({"ok": bool(sent), "devices": sent, "sent": text})

    async def handle_debug_screen_upload(self, request):
        """设备截屏回传：POST /debug/screen/upload（body 为原始 RGB565）。

        头里带 X-Width / X-Height / X-Format，落盘到 F:\\cores3-server\\screens\\。
        """
        width = request.headers.get("X-Width", "0")
        height = request.headers.get("X-Height", "0")
        fmt = request.headers.get("X-Format", "rgb565")
        body = await request.read()
        if not body:
            return web.json_response({"ok": False, "error": "空请求体"}, status=400)
        out_dir = pathlib.Path(os.environ.get("FOXY_SCREEN_DIR", r"F:\cores3-server\screens"))
        out_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%H%M%S")
        base = out_dir / f"screen-{stamp}-{width}x{height}"
        (base.with_suffix(".bin")).write_bytes(body)
        (base.with_suffix(".json")).write_text(
            json.dumps({"width": int(width), "height": int(height), "format": fmt}),
            encoding="utf-8",
        )
        self.logger.bind(tag=TAG).info(f"截屏已保存 {base}.bin ({len(body)} 字节, {width}x{height})")
        return web.json_response({"ok": True, "bytes": len(body), "file": str(base.with_suffix(".bin"))})

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
                        web.get("/debug/music/cover", self.handle_debug_music_cover),
                        web.get("/debug/music/lyrics", self.handle_debug_music_lyrics),
                        web.post("/debug/music/control", self.handle_debug_music_control),
                        # 设备截屏回传（开发期看真实画面）
                        web.post("/debug/screen/upload", self.handle_debug_screen_upload),
                        # 任意 JSON 推给设备（调试用）
                        web.post("/debug/push", self.handle_debug_push),
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



