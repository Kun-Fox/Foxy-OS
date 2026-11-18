# 踩坑记录（按发现顺序）

每条都带证据位置，方便以后回头核对。

## 1. CoreS3 跑不了设备端 AEC
- `main/boards/m5stack/core-s3/config.h:8` → `#define AUDIO_INPUT_REFERENCE false`
- `main/audio/engines/afe_audio_engine.cc:313` → `Device AEC requires a playback reference channel`
- `main/Kconfig.projbuild:1068-1081` → `USE_DEVICE_AEC` 的板卡白名单不含 CoreS3
- 结论：只能半双工（说完才放音），或打开实验性的 `USE_SERVER_AEC`（服务端只是"边放边收 + 打断"，不是真回声消除）

## 2. OTA 地址只能在编译期改（或用 NVS）
- `main/ota.cc:59-70` `GetCheckVersionUrl()` 先读 `Settings("wifi")` 的 `ota_url`，空则用 `CONFIG_OTA_URL`
- 全仓库只有这一处读 `ota_url`，没有任何地方写它
- 默认值 `https://api.tenclass.net/xiaozhi/ota/`（`main/Kconfig.projbuild:3-7`）
- **实践**：直接往 NVS 写 `wifi/ota_url` 即可换服务器，不用重编译；`esptool read-flash 0x9000 0x4000` 读出来、改完再写回。写前务必备份

## 3. 烧整包会顺手抹掉 NVS
- `merged-binary.bin` 覆盖 0x0 起 10MB，把 0x9000 的 NVS（Wi-Fi、ota_url、音量、休眠）一起写掉
- 症状：板上电直接进配网模式，日志 `SsidManager: NVS namespace wifi doesn't exist`
- **实践**：日常只烧 app 分区 `write-flash 0x20000 build/xiaozhi.bin`

## 4. Node 的 keep-alive 把 LLM 连接弄死
- 桥（node http.Server）默认 `keepAliveTimeout = 5s`，空闲就静默关连接
- 服务器侧 openai SDK 的连接池会复用这条死连接，下一句话直接 `APIConnectionError: Connection error.`
- 表现：隔一会儿说话，服务器回兜底文案"主人，小智现在有点忙"
- **修复**：SSE 响应头 `Connection: close`，并设 `server.keepAliveTimeout = 65000`、`headersTimeout = 66000`

## 5. LVGL 给标签显式设字体会崩
- 崩溃：`Guru Meditation ... InstrFetchProhibited` → `lv_font_get_glyph_width` ← `lv_text_get_size` ← `lv_label_event`
- 触发条件：`lv_obj_set_style_text_font(label, font, 0)`，字体指针看起来合法（0x3fceba94）
- **修复**：标签一律继承屏幕主题字体，不显式设字体；字号差异用缩放/换主题字体

## 6. 触摸命中不能按比例算
- 按 `x * count / width` 自己切格子会算偏，表现为"按了没反应"
- **修复**：`lv_obj_get_coords(buttons_[i], &area)` 拿控件真实矩形来判

## 7. 回完话继续听 = 回声自激
- 原设计：tts stop 后回到 Listening 保持 20 秒
- 后果：喇叭尾音被自己麦克风收到，ASR 识别成"我要把这个盐切了"，模型开始自问自答
- **修复**：tts stop 后直接 `SetDeviceState(kDeviceStateIdle)`，麦克风关、WebSocket 留着

## 8. 服务器重启后设备不会自己重连
- 设备只在开机/唤醒时主动建链；服务器重启后它会一直离线，直到有人说话或点屏
- 待办：在 `protocol.cc` 里加断线重连（退避 1s/2s/5s/10s）

## 9. 串口工具一开就复位
- pyserial 默认拉 DTR/RTS，打开 COM7 的瞬间板子重启
- **修复**：`ser.dtr = False; ser.rts = False` 之后再 `open()`

## 10. 构建失败仍会烧旧 bin
- `build.py` 失败时 `build/xiaozhi.bin` 还是上一次的产物，直接 flash 会把坏固件又写回去
- **实践**：先看 `build_exit`，再烧

## 11. 全屏刷新探针任务栈要 16KB
- lv_refr_now() 是在**调用者任务栈**上完成渲染的，4096 栈直接 stack overflow
- 现象：st:0xc (RTC_SW_CPU_RST) + ApplicationStackOverflowHook 回到 efr_configured_layer
- **修复**：xTaskCreate(FpsProbeTask, "fps_probe", 16384, ...)

## 12. -Werror=format-truncation 会卡编译
- char clock[16] 配 snprintf("%02d:%02d") 报 output may be truncated
- **修复**：时间/日期缓冲给到 32 / 48 字节

## 13. 分页层与原生界面的层叠关系
- 页面容器做 320x198、y=0；控制条在 y=198..240，两者不重叠，切页时控制条始终可用
- 页面容器是不透明底 + 默认隐藏，只有 page!=0 才显示；对话页直接用原生界面

## 14. 触摸按下/抬起要成对才有滑动判定
- 只在抬起时回调拿不到起点，滑动永远判不出来
- **做法**：OnPress 记住坐标，OnRelease 算位移（横向 ≥60px 且 dx 占优 → 切页）

## 15. 开机日志抓不到 FoxyUI 初始化
- UI 在 ~650ms 就初始化完了，串口晚开就错过
- **做法**：sptool --after hard-reset flash-id 之后立刻开串口（DTR/RTS 置 False），能抓全

## 11. 息屏与待命通道互相打架（2026-11-19 修）
现象：NVS 写了 `sleep_mode=1/sleep_dim_min=1`，但屏幕永不熄。
原因：`Application::CanEnterSleepMode()` 里有 `if (protocol_->IsAudioChannelOpened()) return false;`，而待命常连模式下通道永远开着。
修法：删掉该判断（只保留"非 Idle 或音频忙则不睡"）；并在 `SetDeviceState` 里对非 Idle 状态调 `SetPowerSaveLevel(PERFORMANCE)`（触发 `PowerSaveTimer::WakeUp` 点亮屏幕）、Idle 时回 `LOW_POWER`。
验证日志：`I (63164) PowerSaveTimer: Enabling power save mode` → 68.7 s 时收到文本 `idle -> speaking` + `Exiting power save mode`。

## 12. 调试探针要能关
`FOXY_FPS_PROBE` 开着会让每 6 秒强制整屏重绘，白白发热耗电，还掩盖真实功耗表现。默认必须为 0。
