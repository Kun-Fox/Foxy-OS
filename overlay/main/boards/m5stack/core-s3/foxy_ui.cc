#include "foxy_ui.h"

#include "audio/audio_codec.h"
#include "board.h"
#include "cores3_display.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#ifndef FOXY_UI_SELFTEST
#define FOXY_UI_SELFTEST 0
#endif

#define TAG "FoxyUI"

namespace {
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
constexpr int kBarH = 42;
constexpr int kStatusH = 0;                          // 不再保留状态栏，四页铺满整屏
constexpr int kPageH = kScreenH - kStatusH;          // 240，无栏铺满整屏
constexpr int kSwipeThreshold = 60;
// 页面语义：0 首页，1 应用页，2 设置页，3 = 基类对话界面（不画我们的层）
constexpr int kPageHome = 0;
constexpr int kPageApps = 1;
constexpr int kPageSettings = 2;
constexpr int kPageChat = 3;
constexpr int kPageAppList = 4;   // 应用列表：音乐/小说/闹钟/相机
constexpr int kPageToolList = 5;  // 工具列表：计算器/触摸测试/拍照测试/文件/SSH
constexpr int kPageTouch = 6;     // 触摸测试
constexpr int kPageCalc = 7;      // 计算器
constexpr int kPageCount = 8;

// 二级列表页的网格参数（顶部留出返回键的位置）
constexpr int kListTop = 40;
constexpr int kListGap = 8;
constexpr int kListCellH = 60;

constexpr uint32_t kBgDark = 0x14141A;
constexpr uint32_t kCardBg = 0x22222C;
constexpr uint32_t kAccent = 0xE8A33D;
constexpr uint32_t kTextDim = 0x8A8A96;

constexpr int kGridMargin = 12;
constexpr int kGridGap = 10;
constexpr int kCellW = (kScreenW - kGridMargin * 2 - kGridGap) / 2;  // 143
constexpr int kCellH = (kPageH - kGridMargin * 2 - kGridGap) / 2;     // 103

lv_obj_t* MakePage(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, kScreenW, kPageH);
    lv_obj_set_pos(page, 0, kStatusH);
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

lv_obj_t* g_back = nullptr;

lv_obj_t* MakeBackButton(lv_obj_t* parent) {
    lv_obj_t* btn = MakeButton(parent, 8, 6, 62, 26, "‹ 返回");
    g_back = btn;
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

// 把触摸横坐标映射成滑条数值：贴住轨道宽度算，不写死像素
int ValueFromX(lv_obj_t* track, int x, int min_value, int max_value) {
    if (track == nullptr) {
        return min_value;
    }
    lv_area_t area;
    lv_obj_get_coords(track, &area);
    int width = area.x2 - area.x1;
    if (width <= 0) {
        return min_value;
    }
    int clamped = x < area.x1 ? area.x1 : (x > area.x2 ? area.x2 : x);
    return min_value + ((clamped - area.x1) * (max_value - min_value)) / width;
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

void AnimXCallback(void* var, int32_t value) {
    lv_obj_set_x((lv_obj_t*)var, value);
}

// 切页时让新页面从屏幕外滑入，180ms ease-out，比硬切顺眼。
void SlideIn(lv_obj_t* page, int dir) {
    if (page == nullptr) {
        return;
    }
    if (dir == 0) {
        lv_obj_set_x(page, 0);
        return;
    }
    lv_obj_set_x(page, dir * kScreenW);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, page);
    lv_anim_set_values(&a, dir * kScreenW, 0);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, AnimXCallback);
    lv_anim_start(&a);
}

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
    // 先铺一块不透明的深色底板，再在上面建页面。
    // 页面滑动时挪开的那半边会露出底板，而不是露出底下的对话界面/状态栏。
    backdrop_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(backdrop_, kScreenW, kScreenH);
    lv_obj_set_pos(backdrop_, 0, 0);
    lv_obj_set_style_bg_color(backdrop_, lv_color_hex(kBgDark), 0);
    lv_obj_set_style_bg_opa(backdrop_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(backdrop_, 0, 0);
    lv_obj_set_style_radius(backdrop_, 0, 0);
    lv_obj_clear_flag(backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(backdrop_);
    home_ = MakePage(lv_screen_active());
    apps_ = MakePage(lv_screen_active());
    settings_ = MakePage(lv_screen_active());
    applist_ = MakePage(lv_screen_active());
    toollist_ = MakePage(lv_screen_active());
    touchtest_ = MakePage(lv_screen_active());
    calc_ = MakePage(lv_screen_active());
    BuildHome();
    BuildApps();
    BuildSettings();
    BuildAppList();
    BuildToolList();
    BuildTouchTest();
    BuildCalculator();
    // 对话页的返回键挂在最顶层，原生对话界面也在这层，所以必须比它更新
    chat_back_ = MakeButton(lv_layer_top(), 4, 4, 62, 26, "‹ 首页");
    lv_obj_add_flag(chat_back_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(home_);
    lv_obj_move_foreground(apps_);
    lv_obj_move_foreground(settings_);
    lv_obj_move_foreground(applist_);
    lv_obj_move_foreground(toollist_);
    lv_obj_move_foreground(touchtest_);
    lv_obj_move_foreground(calc_);
    lv_timer_create(ClockTimerCb, 1000, nullptr);
    // 开机落在首页，而不是原来的对话页，否则看起来和旧界面没区别
    SetPage(kPageHome);
#if FOXY_UI_SELFTEST
    xTaskCreate(SelfTestTask, "ui_selftest", 4096, nullptr, 2, nullptr);
#endif
    ESP_LOGI(TAG, "ui ready (pages=%d vol=%d)", kPageCount, volume_);
    lv_obj_update_layout(home_);
    lv_area_t a;
    lv_obj_get_coords(home_, &a);
    ESP_LOGI(TAG, "diag home=%p parent=%p screen=%p top=%p hidden=%d box=%d,%d %dx%d",
             home_, lv_obj_get_parent(home_), lv_screen_active(), lv_layer_top(),
             (int)lv_obj_has_flag(home_, LV_OBJ_FLAG_HIDDEN),
             (int)a.x1, (int)a.y1, (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1));
    ESP_LOGI(TAG, "diag children=%d", (int)lv_obj_get_child_count(home_));
}

void FoxyUI::BuildHome() {
    // 左半边：像素 Foxy 身体（圆脸 + 身子 + 两条腿 + 尾巴）
    lv_obj_t* face = lv_obj_create(home_);
    lv_obj_set_size(face, 88, 88);
    lv_obj_set_pos(face, 22, 26);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        lv_obj_t* eye = lv_obj_create(face);
        lv_obj_set_size(eye, 10, 16);
        lv_obj_set_pos(eye, 22 + i * 34, 30);
        lv_obj_set_style_bg_color(eye, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, 5, 0);
    }
    lv_obj_t* mouth = lv_obj_create(face);
    lv_obj_set_size(mouth, 16, 5);
    lv_obj_set_pos(mouth, 36, 60);
    lv_obj_set_style_bg_color(mouth, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mouth, 0, 0);
    lv_obj_set_style_radius(mouth, 3, 0);

    lv_obj_t* body = lv_obj_create(home_);
    lv_obj_set_size(body, 46, 42);
    lv_obj_set_pos(body, 43, 108);
    lv_obj_set_style_radius(body, 14, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0xC98A2E), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    for (int i = 0; i < 2; i++) {
        lv_obj_t* leg = lv_obj_create(home_);
        lv_obj_set_size(leg, 12, 18);
        lv_obj_set_pos(leg, 50 + i * 24, 146);
        lv_obj_set_style_radius(leg, 5, 0);
        lv_obj_set_style_bg_color(leg, lv_color_hex(0xC98A2E), 0);
        lv_obj_set_style_bg_opa(leg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(leg, 0, 0);
    }

    lv_obj_t* tail = lv_obj_create(home_);
    lv_obj_set_size(tail, 26, 14);
    lv_obj_set_pos(tail, 8, 118);
    lv_obj_set_style_radius(tail, 7, 0);
    lv_obj_set_style_bg_color(tail, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tail, 0, 0);

    lv_obj_t* name = MakeLabel(home_, "foxy", kAccent);
    lv_obj_set_pos(name, 36, 178);

    clock_label_ = MakeLabel(home_, "--:--", 0xF2F2F5);
    lv_obj_set_pos(clock_label_, 150, 22);

    date_label_ = MakeLabel(home_, "----/--/--", kTextDim);
    lv_obj_set_pos(date_label_, 150, 54);

    lv_obj_t* msg_card = MakeCard(home_, 136, 86, 172, 132);
    for (int i = 0; i < 3; i++) {
        msg_labels_[i] = MakeLabel(msg_card, i == 0 ? "暂无新消息" : "", i == 0 ? kTextDim : 0xE0E0E6);
        lv_label_set_long_mode(msg_labels_[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(msg_labels_[i], 158);
        lv_obj_set_pos(msg_labels_[i], 0, 2 + i * 30);
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
    // 所有二级页统一：右上角返回键、左上角标题
    back_apps_ = MakeButton(settings_, 250, 6, 62, 26, "‹ 返回");
    lv_obj_t* title = MakeLabel(settings_, "设置", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    // 音量：标题行在滑条上方，数值直接写在标签里，避免和轨道压字
    vol_label_ = MakeLabel(settings_, "音量 0", 0xF2F2F5);
    lv_obj_set_pos(vol_label_, 14, 44);
    vol_track_ = MakeCard(settings_, 14, 70, 292, 16);
    lv_obj_set_style_radius(vol_track_, 8, 0);
    lv_obj_set_style_bg_color(vol_track_, lv_color_hex(0x33333F), 0);
    vol_knob_ = MakeCard(settings_, 14, 66, 12, 24);
    lv_obj_set_style_radius(vol_knob_, 6, 0);
    lv_obj_set_style_bg_color(vol_knob_, lv_color_hex(kAccent), 0);

    // 亮度
    bri_label_ = MakeLabel(settings_, "亮度 100", 0xF2F2F5);
    lv_obj_set_pos(bri_label_, 14, 108);
    bri_track_ = MakeCard(settings_, 14, 134, 292, 16);
    lv_obj_set_style_radius(bri_track_, 8, 0);
    lv_obj_set_style_bg_color(bri_track_, lv_color_hex(0x33333F), 0);
    bri_knob_ = MakeCard(settings_, 14, 130, 12, 24);
    lv_obj_set_style_radius(bri_knob_, 6, 0);
    lv_obj_set_style_bg_color(bri_knob_, lv_color_hex(0x6FA8FF), 0);

    info_label_ = MakeLabel(settings_, "读取中…", kTextDim);
    lv_obj_set_pos(info_label_, 14, 166);
    lv_obj_set_style_text_line_space(info_label_, 2, 0);

    PlaceKnob(vol_track_, vol_knob_, volume_);
    PlaceKnob(bri_track_, bri_knob_, brightness_);
    UpdateSettingsInfo();
}

void FoxyUI::BuildAppList() {
    back_applist_ = MakeButton(applist_, 250, 6, 62, 26, "‹ 返回");
    lv_obj_t* title = MakeLabel(applist_, "应用", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    const char* names[4] = {"音乐", "小说", "闹钟", "相机"};
    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
        lv_obj_t* cell = MakeCard(applist_, kGridMargin + col * (kCellW + kGridGap),
                                  kListTop + row * 90, kCellW, 80);
        lv_obj_t* label = MakeLabel(cell, names[i], 0xF2F2F5);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
}

void FoxyUI::BuildToolList() {
    back_toollist_ = MakeButton(toollist_, 250, 6, 62, 26, "‹ 返回");
    lv_obj_t* title = MakeLabel(toollist_, "工具", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    const char* names[5] = {"计算器", "触摸测试", "拍照测试", "文件", "SSH"};
    for (int i = 0; i < 5; i++) {
        int col = i % 2;
        int row = i / 2;
        lv_obj_t* cell = MakeCard(toollist_, kGridMargin + col * (kCellW + kGridGap),
                                  kListTop + row * (kListCellH + kListGap), kCellW, kListCellH);
        lv_obj_t* label = MakeLabel(cell, names[i], 0xF2F2F5);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    }
}

void FoxyUI::BuildTouchTest() {
    back_touch_ = MakeButton(touchtest_, 250, 6, 62, 26, "‹ 返回");
    touch_label_ = MakeLabel(touchtest_, "触摸测试\n按下屏幕任意位置", 0xF2F2F5);
    lv_obj_set_pos(touch_label_, 14, 44);
    lv_obj_set_style_text_line_space(touch_label_, 8, 0);
    touch_dot_ = MakeCard(touchtest_, 156, 120, 16, 16);
    lv_obj_set_style_radius(touch_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch_dot_, lv_color_hex(kAccent), 0);
    lv_obj_add_flag(touch_dot_, LV_OBJ_FLAG_HIDDEN);
}

void FoxyUI::UpdateTouchTest(int x, int y) {
    DisplayLockGuard lock(display_);
    if (touch_label_ != nullptr) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "触摸测试\nx=%d  y=%d", x, y);
        lv_label_set_text(touch_label_, buffer);
    }
    if (touch_dot_ != nullptr) {
        lv_obj_clear_flag(touch_dot_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(touch_dot_, x - 8, y - 8);
    }
}

void FoxyUI::BuildCalculator() {
    back_calc_ = MakeButton(calc_, 250, 6, 62, 26, "‹ 返回");
    lv_obj_t* title = MakeLabel(calc_, "计算器", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    lv_obj_t* display = MakeCard(calc_, 4, 40, 312, 34);
    lv_obj_set_style_radius(display, 8, 0);
    calc_label_ = MakeLabel(display, "0", 0xF2F2F5);
    lv_obj_set_width(calc_label_, 300);
    lv_label_set_long_mode(calc_label_, LV_LABEL_LONG_CLIP);
    lv_obj_align(calc_label_, LV_ALIGN_RIGHT_MID, -4, 0);

    // 4 列 × 4 行键盘，键宽 76、高 38
    const char* keys[16] = {"7", "8", "9", "÷", "4", "5", "6", "×",
                            "1", "2", "3", "−", "C", "0", "=", "+"};
    constexpr int kKeyW = 76;
    constexpr int kKeyH = 38;
    constexpr int kKeyGap = 3;
    constexpr int kKeyLeft = 3;
    constexpr int kKeyTop = 80;
    for (int i = 0; i < 16; i++) {
        int col = i % 4;
        int row = i / 4;
        MakeButton(calc_, kKeyLeft + col * (kKeyW + kKeyGap), kKeyTop + row * (kKeyH + kKeyGap), kKeyW,
                   kKeyH, keys[i]);
    }
    UpdateCalcDisplay();
}

void FoxyUI::UpdateCalcDisplay() {
    if (calc_label_ == nullptr) {
        return;
    }
    char buffer[32];
    if (!calc_input_.empty()) {
        snprintf(buffer, sizeof(buffer), "%s", calc_input_.c_str());
    } else {
        snprintf(buffer, sizeof(buffer), "%.10g", calc_acc_);
    }
    DisplayLockGuard lock(display_);
    lv_label_set_text(calc_label_, buffer);
}

void FoxyUI::HandleCalcKey(int index) {
    static const char* keys[16] = {"7", "8", "9", "÷", "4", "5", "6", "×",
                                   "1", "2", "3", "−", "C", "0", "=", "+"};
    const char* key = keys[index];
    if (key[0] >= '0' && key[0] <= '9') {
        if (calc_fresh_) {
            calc_input_.clear();
            calc_fresh_ = false;
        }
        if (calc_input_.size() < 12) {
            calc_input_ += key[0];
        }
    } else if (key[0] == 'C') {
        calc_input_.clear();
        calc_acc_ = 0;
        calc_op_ = 0;
        calc_fresh_ = true;
    } else if (key[0] == '=') {
        double value = calc_input_.empty() ? calc_acc_ : strtod(calc_input_.c_str(), nullptr);
        if (calc_op_ == '+') {
            calc_acc_ += value;
        } else if (calc_op_ == '-') {
            calc_acc_ -= value;
        } else if (calc_op_ == '*') {
            calc_acc_ *= value;
        } else if (calc_op_ == '/') {
            calc_acc_ = (value == 0) ? 0 : calc_acc_ / value;
        } else {
            calc_acc_ = value;
        }
        calc_input_.clear();
        calc_op_ = 0;
        calc_fresh_ = true;
    } else {
        // 运算符：先把当前输入结算进累计值
        double value = calc_input_.empty() ? calc_acc_ : strtod(calc_input_.c_str(), nullptr);
        if (calc_op_ == '+') {
            calc_acc_ += value;
        } else if (calc_op_ == '-') {
            calc_acc_ -= value;
        } else if (calc_op_ == '*') {
            calc_acc_ *= value;
        } else if (calc_op_ == '/') {
            calc_acc_ = (value == 0) ? 0 : calc_acc_ / value;
        } else {
            calc_acc_ = value;
        }
        calc_op_ = strcmp(key, "÷") == 0 ? '/' : (strcmp(key, "×") == 0 ? '*' :
                                                  (strcmp(key, "−") == 0 ? '-' : '+'));
        calc_input_.clear();
        calc_fresh_ = true;
    }
    UpdateCalcDisplay();
    ESP_LOGI(TAG, "calc key=%s acc=%.6g op=%c", key, calc_acc_, calc_op_ ? calc_op_ : '-');
}

void FoxyUI::BuildPageDots() {
    for (int i = 0; i < kPageCount; i++) {
        lv_obj_t* dot = lv_obj_create(lv_obj_get_parent(home_));
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_pos(dot, 160 - (kPageCount * 14) / 2 + i * 14, kStatusH + kPageH - 10);
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
    int page_before = page_;
    page_ = page;
    DisplayLockGuard lock(display_);
    // 关键：对话页之外，把基类的对话层（状态栏/表情/字幕）整个藏起来，
    // 不依赖堆叠顺序，避免出现“页面画了但被旧界面盖住”的情况。
    if (display_ != nullptr) {
        display_->SetChatVisible(page_ == kPageChat);
    }
    // 对话页要把底板藏起来，否则底板会盖住原生对话界面
    if (backdrop_ != nullptr) {
        if (page_ == kPageChat) {
            lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(backdrop_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_t* pages[] = {home_, apps_, settings_, applist_, toollist_, touchtest_, calc_};
    for (lv_obj_t* p : pages) {
        if (p != nullptr) {
            lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (chat_back_ != nullptr) {
        if (page_ == kPageChat) {
            lv_obj_clear_flag(chat_back_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(chat_back_);
        } else {
            lv_obj_add_flag(chat_back_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    int dir = 0;
    if (page_ == kPageApps && page_before == kPageHome) {
        dir = 1;
    } else if (page_ == kPageHome && page_before == kPageApps) {
        dir = -1;
    }
    lv_obj_t* target = nullptr;
    switch (page_) {
        case kPageHome:
            target = home_;
            UpdateClock();
            break;
        case kPageApps:
            target = apps_;
            break;
        case kPageSettings:
            target = settings_;
            UpdateSettingsInfo();
            break;
        case kPageAppList:
            target = applist_;
            break;
        case kPageToolList:
            target = toollist_;
            break;
        case kPageTouch:
            target = touchtest_;
            break;
        case kPageCalc:
            target = calc_;
            break;
        default:
            break;  // 对话页交给基类渲染
    }
    if (target != nullptr) {
        lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
        SlideIn(target, dir);
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
    if (page_ == kPageTouch) {
        UpdateTouchTest(x, y);
    }
    return false;
}

bool FoxyUI::OnRelease(int x, int y, bool short_tap) {
    if (!pressed_) {
        return false;
    }
    pressed_ = false;
    drag_ = 0;
    int dx = x - press_x_;
    int dy = y - press_y_;
    bool horizontal = (dx > kSwipeThreshold && dx > dy * 2 && dx > -dy * 2) ||
                      (dx < -kSwipeThreshold && -dx > dy * 2 && -dx > -dy * 2);
    if (horizontal) {
        // 按方向区分：左滑 = 去下一页，右滑 = 回上一页。
        if (dx < 0) {
            if (page_ == kPageHome) {
                SetPage(kPageApps);
            }
        } else {
            if (page_ == kPageApps) {
                SetPage(kPageHome);
            } else if (page_ == kPageSettings || page_ == kPageAppList || page_ == kPageToolList) {
                SetPage(kPageApps);
            } else if (page_ == kPageChat) {
                SetPage(kPageHome);
            } else if (page_ == kPageTouch || page_ == kPageCalc) {
                SetPage(kPageToolList);
            }
        }
        return true;
    }
    if (!short_tap) {
        return page_ != kPageChat;
    }
    if (page_ == kPageHome) {
        SetPage(kPageApps);
        return true;
    }
    if (page_ == kPageApps) {
        int col = (x - kGridMargin) / (kCellW + kGridGap);
        int row = (y - kGridMargin) / (kCellH + kGridGap);
        if (col < 0 || col > 1 || row < 0 || row > 1) {
            return true;
        }
        int index = row * 2 + col;
        ESP_LOGI(TAG, "app tap index=%d", index);
        if (index == 0) {
            SetPage(kPageSettings);
        } else if (index == 1) {
            SetPage(kPageAppList);
        } else if (index == 2) {
            SetPage(kPageChat);
        } else {
            SetPage(kPageToolList);
        }
        return true;
    }
    if (page_ == kPageSettings) {
        if (HitTest(back_apps_, x, y)) {
            ESP_LOGI(TAG, "back to apps");
            SetPage(kPageApps);
            return true;
        }
        return HandleSettingsTap(x, y);
    }
    if (page_ == kPageAppList) {
        if (HitTest(back_applist_, x, y)) {
            SetPage(kPageApps);
            return true;
        }
        int col = (x - kGridMargin) / (kCellW + kGridGap);
        int row = (y - kListTop) / 90;
        if (col >= 0 && col <= 1 && row >= 0 && row <= 1) {
            static const char* names[4] = {"音乐", "小说", "闹钟", "相机"};
            ESP_LOGI(TAG, "app %s not implemented yet", names[row * 2 + col]);
        }
        return true;
    }
    if (page_ == kPageToolList) {
        if (HitTest(back_toollist_, x, y)) {
            SetPage(kPageApps);
            return true;
        }
        int col = (x - kGridMargin) / (kCellW + kGridGap);
        int row = (y - kListTop) / (kListCellH + kListGap);
        if (col >= 0 && col <= 1 && row >= 0 && row <= 2) {
            int index = row * 2 + col;
            if (index == 0) {
                SetPage(kPageCalc);
            } else if (index == 1) {
                SetPage(kPageTouch);
            } else if (index < 5) {
                static const char* names[5] = {"计算器", "触摸测试", "拍照测试", "文件", "SSH"};
                ESP_LOGI(TAG, "tool %s not implemented yet", names[index]);
            }
        }
        return true;
    }
    if (page_ == kPageCalc) {
        if (HitTest(back_calc_, x, y)) {
            SetPage(kPageToolList);
            return true;
        }
        constexpr int kKeyW = 76;
        constexpr int kKeyH = 38;
        constexpr int kKeyGap = 3;
        constexpr int kKeyLeft = 3;
        constexpr int kKeyTop = 80;
        int col = (x - kKeyLeft) / (kKeyW + kKeyGap);
        int row = (y - kKeyTop) / (kKeyH + kKeyGap);
        if (col >= 0 && col <= 3 && row >= 0 && row <= 3) {
            HandleCalcKey(row * 4 + col);
        }
        return true;
    }
    if (page_ == kPageTouch) {
        if (HitTest(back_touch_, x, y)) {
            SetPage(kPageToolList);
        }
        return true;
    }
    if (page_ == kPageChat && HitTest(chat_back_, x, y)) {
        ESP_LOGI(TAG, "chat back to home");
        SetPage(kPageHome);
        return true;
    }
    return false;
}

bool FoxyUI::HandleSettingsTap(int x, int y) {
    DisplayLockGuard lock(display_);
    // 音量行（标签+轨道整块）点哪都能拖；亮度行同理
    if (HitTest(vol_track_, x, y) || (y >= 40 && y <= 92)) {
        drag_ = 1;
        SetVolume(ValueFromX(vol_track_, x, 0, 100));
        return true;
    }
    if (HitTest(bri_track_, x, y) || (y >= 104 && y <= 156)) {
        drag_ = 2;
        SetBrightness(ValueFromX(bri_track_, x, 10, 100));
        return true;
    }
    return true;
}

void FoxyUI::PlaceKnob(lv_obj_t* track, lv_obj_t* knob, int value) {
    if (track == nullptr || knob == nullptr) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(track, &area);
    int knob_w = lv_obj_get_width(knob);
    int width = (area.x2 - area.x1 + 1) - knob_w;
    lv_obj_set_x(knob, area.x1 + (value * width) / 100);
}

void FoxyUI::SetVolume(int value) {
    volume_ = value < 0 ? 0 : (value > 100 ? 100 : value);
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetOutputVolume(volume_);
        codec->EnableOutput(volume_ > 0);
    }
    DisplayLockGuard lock(display_);
    PlaceKnob(vol_track_, vol_knob_, volume_);
    if (vol_label_ != nullptr) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "音量 %d", volume_);
        lv_label_set_text(vol_label_, buffer);
    }
    UpdateSettingsInfo();
    ESP_LOGI(TAG, "volume -> %d", volume_);
}

void FoxyUI::SetBrightness(int value) {
    brightness_ = value < 10 ? 10 : (value > 100 ? 100 : value);
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(brightness_);
    }
    DisplayLockGuard lock(display_);
    PlaceKnob(bri_track_, bri_knob_, brightness_);
    if (bri_label_ != nullptr) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "亮度 %d", brightness_);
        lv_label_set_text(bri_label_, buffer);
    }
    ESP_LOGI(TAG, "brightness -> %d", brightness_);
}

void FoxyUI::ChangeVolumeBy(int delta) {
    SetVolume(volume_ + delta);
}

void FoxyUI::ChangeBrightnessBy(int delta) {
    SetBrightness(brightness_ + delta);
}

void FoxyUI::OnMove(int x, int y) {
    if (!pressed_) {
        return;
    }
    if (page_ == kPageTouch) {
        UpdateTouchTest(x, y);
        return;
    }
    if (drag_ == 1) {
        SetVolume(ValueFromX(vol_track_, x, 0, 100));
    } else if (drag_ == 2) {
        SetBrightness(ValueFromX(bri_track_, x, 10, 100));
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
    snprintf(buffer, sizeof(buffer), "电池 %d%%%s   音量 %d\nIP %s   固件 %s", level,
             charging ? " 充电中" : "", volume_, ip, desc != nullptr ? desc->version : "?");
    DisplayLockGuard lock(display_);
    lv_label_set_text(info_label_, buffer);
    if (vol_label_ != nullptr) {
        char vol[16];
        snprintf(vol, sizeof(vol), "音量 %d", volume_);
        lv_label_set_text(vol_label_, vol);
    }
    if (bri_label_ != nullptr) {
        char bri[16];
        snprintf(bri, sizeof(bri), "亮度 %d", brightness_);
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
