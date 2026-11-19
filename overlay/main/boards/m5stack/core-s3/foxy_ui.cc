#include "foxy_ui.h"

#include "audio/audio_codec.h"
#include "board.h"
#include "cores3_display.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <ctime>

#ifndef FOXY_UI_SELFTEST
#define FOXY_UI_SELFTEST 0
#endif

#define TAG "FoxyUI"

namespace {
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kBarH = 42;
constexpr int kPageH = kScreenH - kBarH;  // 198
constexpr int kSwipeThreshold = 60;
constexpr int kPageCount = 4;

constexpr uint32_t kBgDark = 0x14141A;
constexpr uint32_t kCardBg = 0x22222C;
constexpr uint32_t kAccent = 0xE8A33D;
constexpr uint32_t kTextDim = 0x8A8A96;

constexpr int kGridMargin = 12;
constexpr int kGridGap = 10;
constexpr int kCellW = (kScreenW - kGridMargin * 2 - kGridGap) / 2;  // 143
constexpr int kCellH = (kPageH - kGridMargin * 2 - kGridGap) / 2;     // 82

lv_obj_t* MakePage(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, kScreenW, kPageH);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(kBgDark), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

lv_obj_t* MakeCard(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

// 设置页里的小圆角按钮，文字由外部填
lv_obj_t* MakeButton(lv_obj_t* parent, int x, int y, int w, int h, const char* text) {
    lv_obj_t* btn = MakeCard(parent, x, y, w, h);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* label = MakeLabel(btn, text, 0xF2F2F5);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return btn;
}

bool HitTest(lv_obj_t* obj, int x, int y) {
    if (obj == nullptr) {
        return false;
    }
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2;
}

#if FOXY_UI_SELFTEST
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
void SelfTestTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(4000));
    for (int page = 1; page < kPageCount; page++) {
        FoxyUI::Get().SetPage(page);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    FoxyUI::Get().SetPage(0);
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGW("FoxyUI", "selftest done");
    vTaskDelete(nullptr);
}
#endif
}  // namespace

FoxyUI& FoxyUI::Get() {
    static FoxyUI instance;
    return instance;
}

void FoxyUI::Init(CoreS3LcdDisplay* display) {
    display_ = display;
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        volume_ = codec->output_volume();
    }
    DisplayLockGuard lock(display_);
    lv_obj_t* screen = lv_screen_active();
    home_ = MakePage(screen);
    apps_ = MakePage(screen);
    settings_ = MakePage(screen);
    BuildHome();
    BuildApps();
    BuildSettings();
    BuildPageDots();
    lv_timer_create(ClockTimerCb, 1000, nullptr);
    // 开机落在首页，而不是原来的对话页，否则看起来和旧界面没区别
    SetPage(1);
#if FOXY_UI_SELFTEST
    xTaskCreate(SelfTestTask, "ui_selftest", 4096, nullptr, 2, nullptr);
#endif
    ESP_LOGI(TAG, "ui ready (pages=%d vol=%d)", kPageCount, volume_);
}

void FoxyUI::BuildHome() {
    lv_obj_t* face = lv_obj_create(home_);
    lv_obj_set_size(face, 92, 92);
    lv_obj_set_pos(face, 22, 40);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        lv_obj_t* eye = lv_obj_create(face);
        lv_obj_set_size(eye, 10, 16);
        lv_obj_set_pos(eye, 22 + i * 38, 34);
        lv_obj_set_style_bg_color(eye, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, 5, 0);
    }
    lv_obj_t* mouth = lv_obj_create(face);
    lv_obj_set_size(mouth, 16, 5);
    lv_obj_set_pos(mouth, 38, 62);
    lv_obj_set_style_bg_color(mouth, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mouth, 0, 0);
    lv_obj_set_style_radius(mouth, 3, 0);

    lv_obj_t* name = MakeLabel(home_, "foxy", kAccent);
    lv_obj_set_pos(name, 52, 146);

    clock_label_ = MakeLabel(home_, "--:--", 0xF2F2F5);
    lv_obj_set_pos(clock_label_, 142, 18);

    date_label_ = MakeLabel(home_, "----/--/--", kTextDim);
    lv_obj_set_pos(date_label_, 142, 50);

    lv_obj_t* msg_card = MakeCard(home_, 136, 82, 172, 96);
    for (int i = 0; i < 3; i++) {
        msg_labels_[i] = MakeLabel(msg_card, i == 0 ? "暂无新消息" : "", i == 0 ? kTextDim : 0xE0E0E6);
        lv_label_set_long_mode(msg_labels_[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(msg_labels_[i], 158);
        lv_obj_set_pos(msg_labels_[i], 0, 2 + i * 26);
    }
    UpdateClock();
}

void FoxyUI::BuildApps() {
    const char* names[4] = {"设置", "应用", "对话", "工具"};
    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
        lv_obj_t* cell = MakeCard(apps_, kGridMargin + col * (kCellW + kGridGap),
                                  kGridMargin + row * (kCellH + kGridGap), kCellW, kCellH);
        lv_obj_t* label = MakeLabel(cell, names[i], 0xF2F2F5);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
}

void FoxyUI::BuildSettings() {
    lv_obj_t* title = MakeLabel(settings_, "设置", 0xF2F2F5);
    lv_obj_set_pos(title, 20, 8);

    lv_obj_t* vol_text = MakeLabel(settings_, "音量", kTextDim);
    lv_obj_set_pos(vol_text, 20, 44);
    vol_minus_ = MakeButton(settings_, 88, 36, 40, 32, "−");
    vol_label_ = MakeLabel(settings_, "0", 0xF2F2F5);
    lv_obj_set_pos(vol_label_, 148, 44);
    vol_plus_ = MakeButton(settings_, 196, 36, 40, 32, "+");

    lv_obj_t* bri_text = MakeLabel(settings_, "亮度", kTextDim);
    lv_obj_set_pos(bri_text, 20, 88);
    bri_minus_ = MakeButton(settings_, 88, 80, 40, 32, "−");
    bri_label_ = MakeLabel(settings_, "100", 0xF2F2F5);
    lv_obj_set_pos(bri_label_, 148, 88);
    bri_plus_ = MakeButton(settings_, 196, 80, 40, 32, "+");

    info_label_ = MakeLabel(settings_, "读取中…", kTextDim);
    lv_obj_set_pos(info_label_, 20, 128);
    lv_obj_set_style_text_line_space(info_label_, 6, 0);

    UpdateSettingsInfo();
}

void FoxyUI::BuildPageDots() {
    for (int i = 0; i < kPageCount; i++) {
        lv_obj_t* dot = lv_obj_create(lv_obj_get_parent(home_));
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_pos(dot, 160 - (kPageCount * 14) / 2 + i * 14, kPageH - 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kTextDim), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        dots_[i] = dot;
    }
}

void FoxyUI::SetPage(int page) {
    if (page < 0 || page >= kPageCount) {
        return;
    }
    page_ = page;
    DisplayLockGuard lock(display_);
    for (int i = 0; i < kPageCount; i++) {
        if (dots_[i] == nullptr) {
            continue;
        }
        if (page_ == 0) {
            lv_obj_add_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(dots_[i],
                                      lv_color_hex(i == page_ ? kAccent : 0x3A3A46), 0);
        }
    }
    lv_obj_add_flag(home_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(apps_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_, LV_OBJ_FLAG_HIDDEN);
    if (page_ == 1) {
        lv_obj_clear_flag(home_, LV_OBJ_FLAG_HIDDEN);
        UpdateClock();
        UpdateDots();
    } else if (page_ == 2) {
        lv_obj_clear_flag(apps_, LV_OBJ_FLAG_HIDDEN);
        UpdateDots();
    } else if (page_ == 3) {
        lv_obj_clear_flag(settings_, LV_OBJ_FLAG_HIDDEN);
        UpdateSettingsInfo();
        UpdateDots();
    }
    ESP_LOGI(TAG, "page=%d", page_);
}

void FoxyUI::UpdateDots() {
    DisplayLockGuard lock(display_);
    for (int i = 0; i < kPageCount; i++) {
        if (dots_[i] != nullptr) {
            lv_obj_move_foreground(dots_[i]);
        }
    }
}

bool FoxyUI::OnPress(int x, int y) {
    press_x_ = x;
    press_y_ = y;
    pressed_ = true;
    return false;
}

bool FoxyUI::OnRelease(int x, int y, bool short_tap) {
    if (!pressed_) {
        return false;
    }
    pressed_ = false;
    int dx = x - press_x_;
    int dy = y - press_y_;
    bool horizontal = (dx > kSwipeThreshold && dx > dy * 2 && dx > -dy * 2) ||
                      (dx < -kSwipeThreshold && -dx > dy * 2 && -dx > -dy * 2);
    if (horizontal) {
        SetPage(dx < 0 ? page_ + 1 : page_ - 1);
        return true;
    }
    if (!short_tap) {
        return page_ != 0;
    }
    if (page_ == 2) {
        int col = (x - kGridMargin) / (kCellW + kGridGap);
        int row = (y - kGridMargin) / (kCellH + kGridGap);
        if (col >= 0 && col < 2 && row >= 0 && row < 2) {
            int index = row * 2 + col;
            ESP_LOGI(TAG, "app tap index=%d", index);
            if (index == 0) {
                SetPage(3);
            } else if (index == 2) {
                SetPage(0);
            }
            return true;
        }
        return true;
    }
    if (page_ == 3) {
        return HandleSettingsTap(x, y);
    }
    if (page_ == 1) {
        // 首页点一下就回到对话页，免得用户不知道要滑动
        SetPage(0);
        return true;
    }
    return false;
}

bool FoxyUI::HandleSettingsTap(int x, int y) {
    DisplayLockGuard lock(display_);
    if (HitTest(vol_minus_, x, y)) {
        ChangeVolumeBy(-10);
        return true;
    }
    if (HitTest(vol_plus_, x, y)) {
        ChangeVolumeBy(10);
        return true;
    }
    if (HitTest(bri_minus_, x, y)) {
        ChangeBrightnessBy(-20);
        return true;
    }
    if (HitTest(bri_plus_, x, y)) {
        ChangeBrightnessBy(20);
        return true;
    }
    return true;
}

void FoxyUI::ChangeVolumeBy(int delta) {
    volume_ += delta;
    if (volume_ < 0) {
        volume_ = 0;
    }
    if (volume_ > 100) {
        volume_ = 100;
    }
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        // 0 表示静音：先写入 0，再关输出
        codec->SetOutputVolume(volume_);
        codec->EnableOutput(volume_ > 0);
    }
    ESP_LOGI(TAG, "volume -> %d", volume_);
    DisplayLockGuard lock(display_);
    if (vol_label_ != nullptr) {
        char buffer[8];
        snprintf(buffer, sizeof(buffer), "%d", volume_);
        lv_label_set_text(vol_label_, buffer);
    }
    UpdateSettingsInfo();
}

void FoxyUI::ChangeBrightnessBy(int delta) {
    brightness_ += delta;
    if (brightness_ < 10) {
        brightness_ = 10;
    }
    if (brightness_ > 100) {
        brightness_ = 100;
    }
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(brightness_);
    }
    ESP_LOGI(TAG, "brightness -> %d", brightness_);
    DisplayLockGuard lock(display_);
    if (bri_label_ != nullptr) {
        char buffer[8];
        snprintf(buffer, sizeof(buffer), "%d", brightness_);
        lv_label_set_text(bri_label_, buffer);
    }
}

void FoxyUI::UpdateSettingsInfo() {
    if (info_label_ == nullptr) {
        return;
    }
    int level = 0;
    bool charging = false;
    bool discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);

    char ip[16] = "0.0.0.0";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
        }
    }
    const esp_app_desc_t* desc = esp_app_get_description();
    char buffer[160];
    snprintf(buffer, sizeof(buffer), "电池 %d%%%s\n音量 %d\nIP %s\n固件 %s", level,
             charging ? " (充电中)" : "", volume_, ip, desc != nullptr ? desc->version : "?");
    DisplayLockGuard lock(display_);
    lv_label_set_text(info_label_, buffer);
    if (vol_label_ != nullptr) {
        char vol[8];
        snprintf(vol, sizeof(vol), "%d", volume_);
        lv_label_set_text(vol_label_, vol);
    }
    if (bri_label_ != nullptr) {
        char bri[8];
        snprintf(bri, sizeof(bri), "%d", brightness_);
        lv_label_set_text(bri_label_, bri);
    }
}

void FoxyUI::PushMessage(const std::string& text) {
    if (msg_labels_[0] == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "message: %s", text.c_str());
    DisplayLockGuard lock(display_);
    const char* first = lv_label_get_text(msg_labels_[0]);
    const char* second = lv_label_get_text(msg_labels_[1]);
    lv_label_set_text(msg_labels_[2], second != nullptr ? second : "");
    lv_label_set_text(msg_labels_[1], first != nullptr ? first : "");
    lv_label_set_text(msg_labels_[0], text.c_str());
}

void FoxyUI::UpdateClock() {
    if (clock_label_ == nullptr || display_ == nullptr) {
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    char clock[32];
    char date[48];
    if (tm_now.tm_year > 120) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        snprintf(date, sizeof(date), "%04d/%02d/%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                 tm_now.tm_mday);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
        snprintf(date, sizeof(date), "同步中");
    }
    DisplayLockGuard lock(display_);
    lv_label_set_text(clock_label_, clock);
    lv_label_set_text(date_label_, date);
}

void FoxyUI::ClockTimerCb(lv_timer_t* timer) {
    (void)timer;
    FoxyUI::Get().UpdateClock();
}
