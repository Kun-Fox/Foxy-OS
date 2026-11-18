#include "cores3_display.h"

#include "application.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "foxy_ui.h"
#include "board.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <cstdio>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

#define TAG "CoreS3Display"

// 临时性能探针：每 6 秒强制整屏重绘一次，打印耗时（决定滑动动画预算）
#define FOXY_FPS_PROBE 1

namespace {
constexpr int kBarHeight = 42;
constexpr int kButtonCount = 5;
constexpr int kStatusTapHeight = 26;
CoreS3LcdDisplay* g_refresh_instance = nullptr;
const lv_font_t* g_bar_font = nullptr;
}  // namespace

#if FOXY_FPS_PROBE
static void FpsProbeTask(void* arg) {
    auto* display = static_cast<CoreS3LcdDisplay*>(arg);
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (true) {
        int64_t t0 = esp_timer_get_time();
        {
            DisplayLockGuard lock(display);
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(lv_display_get_default());
        }
        int64_t t1 = esp_timer_get_time();
        ESP_LOGI(TAG, "PROBE full_refresh_ms=%.1f", (t1 - t0) / 1000.0);
        vTaskDelay(pdMS_TO_TICKS(6000));
    }
}
#endif

void CoreS3LcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    BuildControlBar();
    FoxyUI::Get().Init(this);
#if FOXY_FPS_PROBE
    xTaskCreate(FpsProbeTask, "fps_probe", 16384, this, 2, nullptr);
#endif
}

void CoreS3LcdDisplay::TouchPress(int x, int y) {
    FoxyUI::Get().OnPress(x, y);
}

void CoreS3LcdDisplay::BuildControlBar() {
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* bar_font = &BUILTIN_TEXT_FONT;
    g_bar_font = bar_font;
    if (lvgl_theme != nullptr) {
        auto font_holder = lvgl_theme->text_font();
        if (font_holder != nullptr && font_holder->font() != nullptr) {
            bar_font = font_holder->font();
        }
    }
    ESP_LOGI(TAG, "control bar font=%p", (const void*)bar_font);
    g_bar_font = bar_font;
    auto screen = lv_screen_active();

    // 字幕原本贴底，往上挪一条控制条的高度
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kBarHeight);
    }

    bar_ = lv_obj_create(screen);
    lv_obj_set_size(bar_, LV_HOR_RES, kBarHeight);
    lv_obj_set_style_radius(bar_, 0, 0);
    lv_obj_set_style_border_width(bar_, 0, 0);
    lv_obj_set_style_pad_all(bar_, 3, 0);
    lv_obj_set_style_pad_column(bar_, 4, 0);
    lv_obj_set_style_bg_color(bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_90, 0);
    lv_obj_set_flex_flow(bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    const char* labels[kButtonCount] = {"对话", "静音", "音-", "音+", "信息"};
    lv_obj_t** slots[kButtonCount] = {&talk_label_, &mute_label_, nullptr, nullptr, nullptr};
    for (int i = 0; i < kButtonCount; i++) {
        lv_obj_t* btn = lv_obj_create(bar_);
        lv_obj_set_size(btn, (LV_HOR_RES - 6 - 4 * (kButtonCount - 1)) / kButtonCount, kBarHeight - 6);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lvgl_theme->border_color(), 0);
        lv_obj_set_style_bg_color(btn, lvgl_theme->background_color(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);  // 触摸由 HandleTouch 统一处理
        buttons_[i] = btn;

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, labels[i]);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        if (slots[i] != nullptr) {
            *slots[i] = label;
        }
    }

    RefreshControlBar();

    g_refresh_instance = this;
    lv_timer_create(RefreshTimerCb, 700, nullptr);
    ESP_LOGI(TAG, "control bar ready");
}

void CoreS3LcdDisplay::RefreshControlBar() {
    auto state = Application::GetInstance().GetDeviceState();
    if (talk_label_ != nullptr) {
        bool busy = (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
                     state == kDeviceStateConnecting);
        lv_label_set_text(talk_label_, busy ? "停止" : "对话");
    }
    if (mute_label_ != nullptr) {
        lv_label_set_text(mute_label_, speaker_muted_ ? "开声音" : "静音");
    }
}

void CoreS3LcdDisplay::RefreshTimerCb(lv_timer_t* timer) {
    (void)timer;
    if (g_refresh_instance != nullptr) {
        g_refresh_instance->RefreshControlBar();
    }
}

void CoreS3LcdDisplay::ToggleSpeakerMute() {
    speaker_muted_ = !speaker_muted_;
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->EnableOutput(!speaker_muted_);
    }
    ESP_LOGI(TAG, "speaker muted=%d", speaker_muted_);
    RefreshControlBar();
}

void CoreS3LcdDisplay::ChangeVolume(int delta) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int volume = codec->output_volume() + delta;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    codec->SetOutputVolume(volume);
    ESP_LOGI(TAG, "volume -> %d", volume);
    if (info_panel_ != nullptr && !lv_obj_has_flag(info_panel_, LV_OBJ_FLAG_HIDDEN)) {
        UpdateInfoText();
    }
}

void CoreS3LcdDisplay::UpdateInfoText() {
    if (info_label_ == nullptr) {
        return;
    }
    int level = 0;
    bool charging = false;
    bool discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);

    char ip[32] = "未连接";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }

    int volume = 0;
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        volume = codec->output_volume();
    }

    const char* state_text = "待命";
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateListening:
            state_text = "在听";
            break;
        case kDeviceStateSpeaking:
            state_text = "在说";
            break;
        case kDeviceStateConnecting:
            state_text = "连接中";
            break;
        case kDeviceStateWifiConfiguring:
            state_text = "配网";
            break;
        default:
            break;
    }

    char text[256];
    snprintf(text, sizeof(text),
             "电池  %d%%%s\n电量  %s\n音量  %d%s\nIP     %s\n状态  %s\n\n点一下屏幕关闭",
             level, charging ? " (充电中)" : "", discharging ? "放电" : "供电", volume,
             speaker_muted_ ? " (静音)" : "", ip, state_text);
    lv_label_set_text(info_label_, text);
}

void CoreS3LcdDisplay::ToggleInfoPanel() {
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (info_panel_ == nullptr) {
        info_panel_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(info_panel_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(info_panel_, 0, 0);
        lv_obj_set_style_border_width(info_panel_, 0, 0);
        lv_obj_set_style_bg_color(info_panel_, lvgl_theme->background_color(), 0);
        lv_obj_set_style_bg_opa(info_panel_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(info_panel_, LV_OBJ_FLAG_CLICKABLE);

        info_label_ = lv_label_create(info_panel_);
        lv_obj_set_style_text_color(info_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_color(info_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_text_line_space(info_label_, 4, 0);
        lv_label_set_long_mode(info_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(info_label_, LV_HOR_RES - 24);
        lv_obj_center(info_label_);
    }

    bool visible = !lv_obj_has_flag(info_panel_, LV_OBJ_FLAG_HIDDEN);
    if (visible) {
        lv_obj_add_flag(info_panel_, LV_OBJ_FLAG_HIDDEN);
    } else {
        UpdateInfoText();
        lv_obj_clear_flag(info_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(info_panel_);
    }
}

void CoreS3LcdDisplay::HandleButton(int index) {
    switch (index) {
        case 0:
            Application::GetInstance().ToggleChatState();
            break;
        case 1:
            ToggleSpeakerMute();
            break;
        case 2:
            ChangeVolume(-10);
            break;
        case 3:
            ChangeVolume(10);
            break;
        case 4:
            ToggleInfoPanel();
            break;
        default:
            break;
    }
}

bool CoreS3LcdDisplay::HandleTouch(int x, int y) {
    DisplayLockGuard lock(this);
    ESP_LOGI(TAG, "touch x=%d y=%d", x, y);

    // 信息页开着时，点任意位置关闭
    if (info_panel_ != nullptr && !lv_obj_has_flag(info_panel_, LV_OBJ_FLAG_HIDDEN)) {
        ToggleInfoPanel();
        return true;
    }

    // 直接拿控件真实坐标做命中判断，避免自己算区域
    for (int i = 0; i < kButtonCount; i++) {
        if (buttons_[i] == nullptr) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(buttons_[i], &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            HandleButton(i);
            return true;
        }
    }

    // 控制条没命中，交给分页层（滑动切页 / 应用格点击）
    if (FoxyUI::Get().OnRelease(x, y, true)) {
        return true;
    }
    return false;
}
