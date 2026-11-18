#include "foxy_ui.h"

#include "cores3_display.h"

#include <esp_log.h>
#include <ctime>

#define TAG "FoxyUI"

namespace {
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kBarH = 42;
constexpr int kPageH = kScreenH - kBarH;
constexpr int kSwipeThreshold = 60;

constexpr uint32_t kBgDark = 0x14141A;
constexpr uint32_t kCardBg = 0x22222C;
constexpr uint32_t kAccent = 0xE8A33D;
constexpr uint32_t kTextDim = 0x8A8A96;

// 图标格几何：2×2
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
}  // namespace

#ifndef FOXY_UI_SELFTEST
#define FOXY_UI_SELFTEST 1
#endif

#if FOXY_UI_SELFTEST
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace {
void SelfTestTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(4000));
    for (int page = 1; page <= 2; page++) {
        FoxyUI::Get().SetPage(page);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    FoxyUI::Get().SetPage(0);
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGW("FoxyUI", "selftest done");
    vTaskDelete(nullptr);
}
}  // namespace
#endif
FoxyUI& FoxyUI::Get() {
    static FoxyUI instance;
    return instance;
}

void FoxyUI::Init(CoreS3LcdDisplay* display) {
    display_ = display;
    DisplayLockGuard lock(display_);
    lv_obj_t* screen = lv_screen_active();
    home_ = MakePage(screen);
    apps_ = MakePage(screen);
    BuildHome();
    BuildApps();
    lv_timer_create(ClockTimerCb, 1000, nullptr);
    SetPage(0);
#if FOXY_UI_SELFTEST
    xTaskCreate(SelfTestTask, "ui_selftest", 4096, nullptr, 2, nullptr);
#endif
    ESP_LOGI(TAG, "ui ready (pages=3)");
}

void FoxyUI::BuildHome() {
    // 左边：像素 Foxy 占位形象（纯 LVGL 图形，等真素材再换）
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
    lv_obj_align(name, LV_ALIGN_TOP_MID, -78, 148);

    // 右侧：时钟 + 日期
    clock_label_ = MakeLabel(home_, "--:--", 0xF2F2F5);
    lv_obj_set_style_text_color(clock_label_, lv_color_hex(0xF2F2F5), 0);
    lv_obj_set_pos(clock_label_, 140, 22);

    date_label_ = MakeLabel(home_, "----/--/--", kTextDim);
    lv_obj_set_pos(date_label_, 142, 54);

    // 右下：消息提醒位（先放占位）
    lv_obj_t* msg_card = MakeCard(home_, 136, 82, 172, 96);
    const char* placeholders[3] = {"暂无新消息", "", ""};
    for (int i = 0; i < 3; i++) {
        msg_labels_[i] = MakeLabel(msg_card, placeholders[i], i == 0 ? kTextDim : 0xE0E0E6);
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

void FoxyUI::SetPage(int page) {
    if (page < 0 || page > 2) {
        return;
    }
    page_ = page;
    DisplayLockGuard lock(display_);
    if (page == 0) {
        lv_obj_add_flag(home_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(apps_, LV_OBJ_FLAG_HIDDEN);
    } else if (page == 1) {
        lv_obj_clear_flag(home_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(apps_, LV_OBJ_FLAG_HIDDEN);
        UpdateClock();
    } else {
        lv_obj_add_flag(home_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(apps_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "page=%d", page_);
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
        // 应用页：按格子坐标判断点了哪个（设置/应用/对话/工具）
        int col = (x - kGridMargin) / (kCellW + kGridGap);
        int row = (y - kGridMargin) / (kCellH + kGridGap);
        if (col >= 0 && col < 2 && row >= 0 && row < 2) {
            int index = row * 2 + col;
            ESP_LOGI(TAG, "app tap index=%d", index);
            if (index == 2) {
                SetPage(0);
            }
            return true;
        }
        return true;
    }
    if (page_ == 1) {
        return true;  // 首页暂时吞掉点击，避免误触发对话
    }
    return false;
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
