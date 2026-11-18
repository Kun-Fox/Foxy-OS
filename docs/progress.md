# 进度记录

## 2026-11-19 凌晨（自主推进，用户睡眠中）

目标：把准备给 CoreS3 的固件从"小智 + 一条按钮栏"推进到"Foxy-OS 雏形"，并保证早上开机就能看。

### 已完成

| 项 | 状态 | 说明 |
| --- | --- | --- |
| 四页 UI | 已烧录验证 | 对话(0)/首页(1)/应用(2)/设置(3)；左右滑动切页，页点指示 |
| 首页 | 已烧录验证 | 像素 Foxy 头像 + 时钟/日期 + 3 行消息卡 |
| 应用页 | 已烧录验证 | 设置 / 应用 / 对话 / 工具 四张卡片（设置与对话已接） |
| 设置页 | 已烧录验证 | 音量 −/+、亮度 −/+、电池/充电/音量/IP/版本 |
| 消息推送 | 已真机验证 | 服务器 `POST /debug/notify` → 设备 `{"type":"foxy"}` → 首页消息卡 |
| 音量 0 = 静音 | 已烧录验证 | 固件把 0 钳到 10，改为 `EnableOutput(false)`；MCP 工具同样处理 |
| 息屏待机 | 已烧录验证 | 60 秒无操作背光关到 0，不关机、保持待命连接 |
| 整屏帧率 | 实测 23–28 fps | SPI 80MHz + 40 行缓冲（原 40MHz/20 行） |
| 待命常连 | 已真机验证 | 回完话麦克风关、通道留，网页打字直接开口 |

### 关键数字

- 程序占用 2,742,528 B / 分区 4,128,768 B（33% 空闲）
- assets 分区 8 MB，已用 1.66 MB
- 整屏刷新 36–44 ms；全屏一帧 RGB565 153.6 KB
- 固件启动 684 ms 完成 UI 初始化，2,954 ms 打开待命通道

### 新增代码位置（固件）

- `main/boards/m5stack/core-s3/foxy_ui.h/.cc` — 四页、时钟、消息卡、设置项
- `main/boards/m5stack/core-s3/cores3_display.h/.cc` — 控制条、信息页、触摸派发
- `main/display/display.h` — 基类 `ShowMessageCard`
- `main/application.cc` — `type=="foxy"` 消息分支、待命定时器、ping 忽略
- `main/protocols/protocol.cc` — 通道超时 120 s → 600 s
- `main/mcp_server.cc` — 音量 0 走静音

### 服务器 / 桥

- `core/http_server.py` — `/debug/say`、`/debug/state`、`/debug/tool`、`/debug/notify`
- `core/connection.py` — 30 s ping 保活
- `core/utils/conn_registry.py`、`core/utils/device_commands.py` — 在线设备表、本地音量指令
- `bridge/foxy-voice-bridge.mjs` — `/say`、`/say-tool`、`/notify`、`Connection: close`

### 已知问题 / 下一步

1. ASR 识别率偏低（远场 + 无 AEC），下一步试热词与降噪参数。
2. 应用页另外两张卡片（应用、工具）还是占位。
3. 阶段 3 消息通道已通，但只有本地网页能推；接 QQ/邮箱需要再写一个轮询进程。
4. 音乐、小说、相机、计算器未做。
