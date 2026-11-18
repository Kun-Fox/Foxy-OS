# 服务器补丁（optional，接不上也能用官方云）

本目录里的 patch 打在 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 的 `main/xiaozhi-server/` 上。

```bash
cd xiaozhi-esp32-server/main/xiaozhi-server
git apply --ignore-whitespace /path/to/Foxy-OS/patches/server-foxy.patch
# 再拷两个新文件
cp /path/to/Foxy-OS/server/core/utils/device_commands.py core/utils/
cp /path/to/Foxy-OS/server/core/utils/conn_registry.py core/utils/
# 配置参考
cp /path/to/Foxy-OS/server/data/_config.example.yaml data/.config.yaml
```

## 补丁做了什么

| 文件 | 改动 |
|---|---|
| `core/connection.py` | 每 30 秒发 `{"type":"ping"}` 保活，设备不再被静默断开；`conn_registry` 注册/注销连接 |
| `core/http_server.py` | 新增 `POST /debug/say`（网页打字让板子念）、`POST /debug/tool`（直调 MCP 工具）、`GET /debug/state` |
| `core/handle/receiveAudioHandle.py` | `startToChat()` 里先本地解析音量/亮度指令，命中就不走大模型 |
| `core/utils/opus_encoder_utils.py` | Opus 码率 24kbps → 48kbps（24kbps 下齿音会"发毛"，听起来时好时坏） |

## 关键配置（`data/.config.yaml`）

```yaml
server:
  ip: 0.0.0.0
  port: 8000
  http_port: 8003
  websocket: ws://192.168.3.10:8000/xiaozhi/v1/   # 改成你的电脑 IP
selected_module:
  VAD: SileroVAD
  ASR: SherpaASR          # 本地 SenseVoice，免费、0.2 秒
  LLM: FoxyLLM            # 指向 Foxy 桥
  TTS: EdgeTTS
  Memory: nomem
  Intent: nointent
LLM:
  FoxyLLM:
    type: openai
    base_url: http://127.0.0.1:8010/v1
    model_name: foxy-voice
    api_key: sk-foxy-local
```

设备侧的 OTA 地址写在 NVS 的 `wifi/ota_url`，不需要重编译固件：

```powershell
# 备份
esptool --chip esp32s3 -p COM7 read-flash 0x9000 0x4000 nvs_backup.bin
# 改写（用 esp-idf-nvs-partition-gen 生成后写回）
esptool --chip esp32s3 -p COM7 write-flash 0x9000 nvs_new.bin
```
