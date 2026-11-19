#include "foxy_ui.h"

#include "audio/audio_codec.h"
#include "board.h"
#include "cores3_display.h"
#include "foxy_music.h"

#include "application.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include "cJSON.h"
#include <settings.h>
#include <memory>
#include <string>
#include <vector>

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
constexpr int kSwipeThreshold = 80;
// 小于这个位移算点击；介于它和滑动阈值之间算误触（交给滑条拖动）
constexpr int kTapMaxMove = 18;
// 页面语义：0 首页，1 应用页，2 设置页，3 = 基类对话界面（不画我们的层）
constexpr int kPageHome = 0;
constexpr int kPageApps = 1;
constexpr int kPageSettings = 2;
constexpr int kPageChat = 3;
constexpr int kPageAppList = 4;   // 应用列表：音乐/小说/闹钟/相机
constexpr int kPageToolList = 5;  // 工具列表：计算器/触摸测试/拍照测试/文件/SSH
constexpr int kPageTouch = 6;     // 触摸测试
constexpr int kPageCalc = 7;      // 计算器
constexpr int kPageMusic = 8;     // 音乐
constexpr int kPageKeyboard = 9;  // 屏幕键盘
constexpr int kPageNovel = 10;    // 小说
constexpr int kPageAlarm = 11;    // 闹钟
constexpr int kPageCount = 12;

// 二级列表页的网格参数（顶部留出返回键的位置）
constexpr int kListTop = 40;
constexpr int kListGap = 8;
constexpr int kListCellH = 60;

// Foxy 立绘：编译期用 target_add_binary_data 把 main/foxy_home.bin 嵌进固件，
// 运行时只声明起止符号，不占 RAM（数据在 flash 的 .rodata 里）。
extern "C" const uint8_t foxy_home_bin_start[] asm("_binary_foxy_home_bin_start");
extern "C" const uint8_t foxy_chat_bin_start[] asm("_binary_foxy_chat_bin_start");
static lv_image_dsc_t g_foxy_dsc;
static lv_image_dsc_t g_cover_dsc;
static lv_image_dsc_t g_chat_dsc;
constexpr int kFoxyW = 132;
constexpr int kFoxyH = 240;
constexpr int kChatW = 66;
constexpr int kChatH = 120;
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
    lv_obj_t* btn = MakeButton(parent, 8, 6, 62, 26, "返回");
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
        if (volume_ <= 10 && !codec->output_enabled()) {
            volume_ = 0;
        }
    }
    FoxyMusic::Get().Init(codec);
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
    music_ = MakePage(lv_screen_active());
    kbd_ = MakePage(lv_screen_active());
    chat_ = MakePage(lv_screen_active());
    BuildHome();
    BuildApps();
    BuildSettings();
    BuildAppList();
    BuildToolList();
    BuildTouchTest();
    BuildCalculator();
    BuildMusic();
    BuildKeyboard();
    BuildNovel();
    BuildAlarm();
    BuildChat();
    // 对话页的返回键挂在最顶层，原生对话界面也在这层，所以必须比它更新
    chat_back_ = nullptr;  // 对话页同样用滑动手势返回
    lv_obj_move_foreground(home_);
    lv_obj_move_foreground(apps_);
    lv_obj_move_foreground(settings_);
    lv_obj_move_foreground(applist_);
    lv_obj_move_foreground(toollist_);
    lv_obj_move_foreground(touchtest_);
    lv_obj_move_foreground(calc_);
    lv_obj_move_foreground(music_);
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
    // 左半边：Foxy 立绘。132x240 的画布已经按首页底色做过透明合成，
    // 所以这里用纯 RGB565 直接整张贴图，省掉 RGB565A8 的运行时混合。
    if (g_foxy_dsc.data == nullptr) {
        g_foxy_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        g_foxy_dsc.header.w = kFoxyW;
        g_foxy_dsc.header.h = kFoxyH;
        g_foxy_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        g_foxy_dsc.header.stride = kFoxyW * 2;
        g_foxy_dsc.data_size = kFoxyW * kFoxyH * 2;
        g_foxy_dsc.data = foxy_home_bin_start;
    }
    art_ = lv_image_create(home_);
    lv_image_set_src(art_, &g_foxy_dsc);
    lv_obj_set_pos(art_, 0, 0);
    lv_obj_clear_flag(art_, LV_OBJ_FLAG_CLICKABLE);
    // 呼吸感：整幅画上下浮动 3px，2.8 秒一个来回。
    // 之后换成真正的多帧立绘时，这里就是帧动画的挂点。
    lv_anim_t bob;
    lv_anim_init(&bob);
    lv_anim_set_var(&bob, art_);
    lv_anim_set_values(&bob, 0, 3);
    lv_anim_set_duration(&bob, 1400);
    lv_anim_set_playback_duration(&bob, 1400);
    lv_anim_set_repeat_count(&bob, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&bob, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&bob, [](void* var, int32_t v) {
        lv_obj_set_y(static_cast<lv_obj_t*>(var), v);
    });
    lv_anim_start(&bob);
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
    back_apps_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
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
    back_applist_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
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
    back_toollist_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
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
    back_touch_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
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
    back_calc_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
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
        calc_keys_[i] = MakeButton(calc_, kKeyLeft + col * (kKeyW + kKeyGap),
                                   kKeyTop + row * (kKeyH + kKeyGap), kKeyW, kKeyH, keys[i]);
    }
    UpdateCalcDisplay();
}

void FoxyUI::BuildChat() {
    chat_status_ = MakeLabel(chat_, "我在听", kTextDim);
    lv_obj_set_pos(chat_status_, 14, 12);

    if (g_chat_dsc.data == nullptr) {
        g_chat_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        g_chat_dsc.header.w = kChatW;
        g_chat_dsc.header.h = kChatH;
        g_chat_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        g_chat_dsc.header.stride = kChatW * 2;
        g_chat_dsc.data_size = kChatW * kChatH * 2;
        g_chat_dsc.data = foxy_chat_bin_start;
    }
    chat_art_ = lv_image_create(chat_);
    lv_image_set_src(chat_art_, &g_chat_dsc);
    lv_obj_set_pos(chat_art_, (kScreenW - kChatW) / 2, 8);
    lv_obj_clear_flag(chat_art_, LV_OBJ_FLAG_CLICKABLE);

    // 字幕：三行，居中，长文本自动换行
    chat_text_ = MakeLabel(chat_, "按住屏幕说句话，或者从左下角打字喵", 0xF2F2F5);
    lv_obj_set_width(chat_text_, 296);
    lv_obj_set_pos(chat_text_, 12, 140);
    lv_label_set_long_mode(chat_text_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(chat_text_, 4, 0);
    lv_obj_set_style_text_color(chat_text_, lv_color_hex(0xF2F2F5), 0);
}

void FoxyUI::SetChatText(const std::string& text) {
    if (display_ == nullptr || chat_text_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    std::string shown = text;
    if (shown.size() > 120) {
        shown = shown.substr(0, 120) + "…";
    }
    lv_label_set_text(chat_text_, shown.c_str());
}

void FoxyUI::SetChatStatus(const std::string& status) {
    if (display_ == nullptr || chat_status_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_label_set_text(chat_status_, status.c_str());
}

void FoxyUI::BuildMusic() {
    back_music_ = nullptr;  // 返回键已取消：二级页统一用滑动返回

    // 封面铺满整屏，控制条浮在底部
    music_cover_ = lv_image_create(music_);
    lv_obj_remove_style_all(music_cover_);
    lv_obj_set_pos(music_cover_, 0, 0);
    lv_obj_clear_flag(music_cover_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(music_cover_, LV_OBJ_FLAG_HIDDEN);

    music_panel_ = lv_obj_create(music_);
    lv_obj_remove_style_all(music_panel_);
    lv_obj_set_pos(music_panel_, 0, 148);
    lv_obj_set_size(music_panel_, kScreenW, kScreenH - 148);
    lv_obj_set_style_bg_color(music_panel_, lv_color_hex(0x0B0B10), 0);
    lv_obj_set_style_bg_opa(music_panel_, 208, 0);
    lv_obj_clear_flag(music_panel_, LV_OBJ_FLAG_CLICKABLE);

    music_title_ = MakeLabel(music_panel_, "-", 0xF2F2F5);
    lv_obj_set_pos(music_title_, 14, 6);
    lv_obj_set_width(music_title_, 210);
    lv_label_set_long_mode(music_title_, LV_LABEL_LONG_CLIP);

    music_time_ = MakeLabel(music_panel_, "0:00 / 0:00", kTextDim);
    lv_obj_set_pos(music_time_, 196, 6);
    lv_obj_set_width(music_time_, 110);
    lv_obj_set_style_text_align(music_time_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(music_time_, LV_LABEL_LONG_CLIP);

    music_track_ = lv_obj_create(music_panel_);
    lv_obj_remove_style_all(music_track_);
    lv_obj_set_pos(music_track_, 14, 30);
    lv_obj_set_size(music_track_, 292, 5);
    lv_obj_set_style_bg_color(music_track_, lv_color_hex(0x33333F), 0);
    lv_obj_set_style_bg_opa(music_track_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_track_, 3, 0);
    lv_obj_clear_flag(music_track_, LV_OBJ_FLAG_CLICKABLE);

    music_fill_ = lv_obj_create(music_panel_);
    lv_obj_remove_style_all(music_fill_);
    lv_obj_set_pos(music_fill_, 14, 30);
    lv_obj_set_size(music_fill_, 0, 5);
    lv_obj_set_style_bg_color(music_fill_, lv_color_hex(0xE8A33D), 0);
    lv_obj_set_style_bg_opa(music_fill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_fill_, 3, 0);
    lv_obj_clear_flag(music_fill_, LV_OBJ_FLAG_CLICKABLE);

    music_knob_ = lv_obj_create(music_panel_);
    lv_obj_remove_style_all(music_knob_);
    lv_obj_set_size(music_knob_, 12, 12);
    lv_obj_set_style_bg_color(music_knob_, lv_color_hex(0xF2F2F5), 0);
    lv_obj_set_style_bg_opa(music_knob_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_knob_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(music_knob_, 8, 26);
    lv_obj_clear_flag(music_knob_, LV_OBJ_FLAG_CLICKABLE);

    // 三个圆形按钮：上一首 / 播放暂停 / 下一首
    auto make_round = [this](int cx, const char* text) {
        lv_obj_t* btn = lv_obj_create(music_panel_);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 46, 46);
        lv_obj_set_pos(btn, cx, 44);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x24242E), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* label = MakeLabel(btn, text, 0xF2F2F5);
        lv_obj_center(label);
        return btn;
    };
    music_btn_prev_ = make_round(74, "上");
    music_play_ = make_round(137, "播");
    lv_obj_t* play_label = lv_obj_get_child(music_play_, 0);
    if (play_label != nullptr) {
        lv_label_set_text(play_label, "播");
    }
    music_btn_next_ = make_round(200, "下");

    // 歌词浮层：盖住封面区域，点任意处返回封面
    lyrics_panel_ = lv_obj_create(music_);
    lv_obj_remove_style_all(lyrics_panel_);
    lv_obj_set_pos(lyrics_panel_, 0, 0);
    lv_obj_set_size(lyrics_panel_, kScreenW, 148);
    lv_obj_set_style_bg_color(lyrics_panel_, lv_color_hex(0x101016), 0);
    lv_obj_set_style_bg_opa(lyrics_panel_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(lyrics_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 5; i++) {
        lyric_labels_[i] = MakeLabel(lyrics_panel_, "", kTextDim);
        lv_obj_set_width(lyric_labels_[i], 292);
        lv_obj_set_style_text_align(lyric_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lyric_labels_[i], 14, 18 + i * 26);
        lv_label_set_long_mode(lyric_labels_[i], LV_LABEL_LONG_CLIP);
    }

    UpdateMusicPage();
}

void FoxyUI::ToggleLyrics() {
    lyrics_visible_ = !lyrics_visible_;
    if (lyrics_panel_ == nullptr) {
        return;
    }
    if (lyrics_visible_) {
        lv_obj_clear_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
        UpdateLyrics();
    } else {
        lv_obj_add_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void FoxyUI::UpdateLyrics() {
    if (lyrics_panel_ == nullptr || !lyrics_visible_) {
        return;
    }
    auto& music = FoxyMusic::Get();
    int elapsed_ms = music.ElapsedSeconds() * 1000;
    std::string current = music.LyricAt(elapsed_ms);
    DisplayLockGuard lock(display_);
    // 只显示"当前句"附近：当前句高亮，其它留空——比滚动歌词省心，也够看
    for (int i = 0; i < 5; i++) {
        if (lyric_labels_[i] == nullptr) {
            continue;
        }
        if (i == 1) {
            lv_label_set_text(lyric_labels_[i], current.c_str());
            lv_obj_set_style_text_color(lyric_labels_[i], lv_color_hex(0xE8A33D), 0);
        } else {
            lv_label_set_text(lyric_labels_[i], "");
        }
    }
}

void FoxyUI::UpdateMusicProgress() {
    if (music_track_ == nullptr || music_fill_ == nullptr || music_knob_ == nullptr) {
        return;
    }
    auto& music = FoxyMusic::Get();
    int duration_ms = music.DurationMs();
    int elapsed_ms = music.ElapsedSeconds() * 1000;
    if (duration_ms <= 0) {
        duration_ms = 1;
    }
    int width = 292;
    int filled = elapsed_ms * width / duration_ms;
    if (filled < 0) {
        filled = 0;
    }
    if (filled > width) {
        filled = width;
    }
    DisplayLockGuard lock(display_);
    lv_obj_set_width(music_fill_, filled);
    lv_obj_set_pos(music_knob_, 14 + filled - 6, 26);
}

void FoxyUI::LoadMusicAssets(int index) {
    if (index < 0) {
        return;
    }
    FoxyMusic::Get().LoadLyrics(index);
    if (music_cover_ == nullptr) {
        return;
    }
    if (FoxyMusic::Get().LoadCover(index)) {
        const uint8_t* data = FoxyMusic::Get().CoverData();
        if (data != nullptr) {
            g_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            g_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            g_cover_dsc.header.w = 320;
            g_cover_dsc.header.h = 240;
            g_cover_dsc.header.stride = 640;
            g_cover_dsc.data_size = 320 * 240 * 2;
            g_cover_dsc.data = data;
            DisplayLockGuard lock(display_);
            lv_image_set_src(music_cover_, &g_cover_dsc);
            lv_obj_clear_flag(music_cover_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void FoxyUI::MusicAssetTask(void* arg) {
    int index = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    auto* ui = &FoxyUI::Get();
    ui->LoadMusicAssets(index);
    vTaskDelete(nullptr);
}

void FoxyUI::UpdateMusicPage() {
    if (music_ == nullptr) {
        return;
    }
    auto& music = FoxyMusic::Get();
    char title[64];
    int index = music.CurrentIndex();
    std::string name = index >= 0 ? music.TrackName(index) : std::string();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".mp3") == 0) {
        name = name.substr(0, name.size() - 4);
    }
    snprintf(title, sizeof(title), "%s", name.empty() ? "选一首歌" : name.c_str());

    int duration = music.DurationMs();
    int elapsed = music.ElapsedSeconds();
    char time_text[32];
    snprintf(time_text, sizeof(time_text), "%d:%02d / %d:%02d", elapsed / 60, elapsed % 60,
             duration / 60000, (duration / 1000) % 60);

    const char* play_text = music.IsPlaying() && !music.IsPaused() ? "停" : "播";

    {
        DisplayLockGuard lock(display_);
        if (music_title_ != nullptr) {
            lv_label_set_text(music_title_, title);
        }
        if (music_time_ != nullptr) {
            lv_label_set_text(music_time_, time_text);
        }
        if (music_play_ != nullptr) {
            lv_obj_t* label = lv_obj_get_child(music_play_, 0);
            if (label != nullptr) {
                lv_label_set_text(label, play_text);
            }
        }
    }
    UpdateMusicProgress();
    if (lyrics_visible_) {
        UpdateLyrics();
    }
}

void FoxyUI::HandleMusicCommand(const std::string& action, int index) {
    auto& music = FoxyMusic::Get();
    ESP_LOGI(TAG, "music cmd=%s index=%d", action.c_str(), index);
    if (action == "play") {
        if (index >= 0) {
            music.Play(index);
        } else if (music.IsPlaying()) {
            music.TogglePause();
        } else {
            music.Play(music.CurrentIndex() < 0 ? 0 : music.CurrentIndex());
        }
    } else if (action == "pause") {
        music.TogglePause();
    } else if (action == "stop") {
        music.Stop();
    } else if (action == "next") {
        music.Next();
    } else if (action == "prev") {
        music.Prev();
    } else if (action == "refresh") {
        music.Refresh();
        if (page_ == kPageMusic) {
            UpdateMusicPage();
        }
        return;
    }

    int current = music.CurrentIndex();
    if (current >= 0 && page_ == kPageMusic) {
        xTaskCreate(MusicAssetTask, "music_asset", 6144,
                    reinterpret_cast<void*>(static_cast<intptr_t>(current)), 3, nullptr);
    }
    if (page_ == kPageMusic) {
        UpdateMusicPage();
    }
}

void FoxyUI::MusicRefreshTask(void* arg) {
    (void)arg;
    FoxyMusic::Get().Refresh();
    FoxyUI::Get().LoadMusicAssets(FoxyMusic::Get().CurrentIndex() >= 0
                                      ? FoxyMusic::Get().CurrentIndex()
                                      : 0);
    vTaskDelete(nullptr);
}

bool FoxyUI::HandleMusicTap(int x, int y) {
    auto& music = FoxyMusic::Get();

    if (lyrics_visible_) {
        ToggleLyrics();
        return true;
    }

    if (y < 148) {
        // 点封面：切歌词
        ToggleLyrics();
        return true;
    }

    // 进度条区域：点哪跳哪
    if (y >= 168 && y <= 190 && x >= 8 && x <= 312) {
        int duration_ms = music.DurationMs();
        if (duration_ms > 0) {
            int ratio = (x - 14) * 1000 / 292;
            if (ratio < 0) {
                ratio = 0;
            }
            if (ratio > 1000) {
                ratio = 1000;
            }
            music.SeekTo(duration_ms * ratio / 1000 / 1000);
            UpdateMusicPage();
        }
        return true;
    }

    if (y >= 190) {
        if (x >= 74 && x < 120) {
            music.Prev();
        } else if (x >= 137 && x < 183) {
            if (music.IsPlaying()) {
                music.TogglePause();
            } else {
                music.Play(music.CurrentIndex() < 0 ? 0 : music.CurrentIndex());
            }
        } else if (x >= 200 && x < 246) {
            music.Next();
        }
        if (music.CurrentIndex() >= 0) {
            xTaskCreate(MusicAssetTask, "music_asset", 6144,
                        reinterpret_cast<void*>(static_cast<intptr_t>(music.CurrentIndex())), 3,
                        nullptr);
        }
        UpdateMusicPage();
        return true;
    }
    return true;
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
        // 对话也由我们自己画：原生那层永远藏着，避免浅色界面漏出来
        display_->SetChatVisible(false);
    }

    lv_obj_t* pages[] = {home_, apps_, settings_, applist_, toollist_, touchtest_, calc_, music_, chat_, kbd_, novel_, alarm_};
    for (lv_obj_t* p : pages) {
        if (p != nullptr) {
            lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (false) {
        if (page_ == kPageChat) {
            lv_obj_clear_flag(chat_back_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(chat_back_);
        } else {
            lv_obj_add_flag(chat_back_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // 切页方向按"层级深浅"算：首页 0 → 应用页 1 → 二级页 2。
    // 往里走从左往右推，往回走反过来，这样所有页面的转场方向都一致。
    auto depth_of = [](int page) {
        if (page == kPageHome) {
            return 0;
        }
        if (page == kPageApps) {
            return 1;
        }
        return 2;
    };
    int dir = 0;
    int depth_new = depth_of(page_);
    int depth_old = depth_of(page_before);
    if (depth_new > depth_old) {
        dir = 1;
    } else if (depth_new < depth_old) {
        dir = -1;
    }
    if (page_ == kPageChat) {
        // 进对话页就和音乐说再见，别和 TTS 抢喇叭
        FoxyMusic::Get().Stop();
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
        case kPageMusic:
            target = music_;
            UpdateMusicPage();
            // 列表要从服务器拉，别卡住 UI 线程
            xTaskCreate(MusicRefreshTask, "music_list", 4096, nullptr, 3, nullptr);
            break;
        case kPageKeyboard:
            target = kbd_;
            UpdateKeyboardText();
            break;
        case kPageNovel:
            target = novel_;
            NovelTurn(0);
            break;
        case kPageAlarm:
            target = alarm_;
            AlarmFetch();
            break;
        case kPageChat:
            target = chat_;
            break;
        default:
            break;
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
    consume_press_ = false;
    Ripple(x, y);
    if (page_ == kPageTouch) {
        UpdateTouchTest(x, y);
    }
    if (page_ == kPageCalc) {
        // 计算器在按下时就出结果：手指一碰键帽，显示条立刻变，不用等抬起。
        constexpr int kKeyW = 76;
        constexpr int kKeyH = 38;
        constexpr int kKeyGap = 3;
        constexpr int kKeyLeft = 3;
        constexpr int kKeyTop = 80;
        int col = (x - kKeyLeft) / (kKeyW + kKeyGap);
        int row = (y - kKeyTop) / (kKeyH + kKeyGap);
        if (col >= 0 && col <= 3 && row >= 0 && row <= 3) {
            int index = row * 4 + col;
            Flash(calc_keys_[index]);
            HandleCalcKey(index);
            consume_press_ = true;
        }
    }
    return false;
}

void FoxyUI::GoBack() {
    switch (page_) {
        case kPageSettings:
        case kPageAppList:
        case kPageToolList:
            SetPage(kPageApps);
            break;
        case kPageTouch:
        case kPageCalc:
            SetPage(kPageToolList);
            break;
        case kPageMusic:
            SetPage(kPageAppList);
            break;
        case kPageChat:
            SetPage(kPageHome);
            break;
        default:
            break;
    }
}

void FoxyUI::Ripple(int x, int y) {
    if (display_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    lv_obj_t* dot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 48, 48);
    lv_obj_set_pos(dot, x - 24, y - 24);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(dot);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, dot);
    lv_anim_set_values(&fade, LV_OPA_30, LV_OPA_0);
    lv_anim_set_duration(&fade, 220);
    lv_anim_set_exec_cb(&fade, [](void* var, int32_t value) {
        lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(value), 0);
    });
    lv_anim_set_ready_cb(&fade, [](lv_anim_t* anim) {
        lv_obj_delete(static_cast<lv_obj_t*>(anim->var));
    });
    lv_anim_start(&fade);
}

struct FlashJob {
    lv_obj_t* obj;
    lv_color_t color;
};

void FoxyUI::Flash(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    auto* job = new FlashJob{obj, lv_obj_get_style_bg_color(obj, LV_PART_MAIN)};
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x555566), 0);
    lv_timer_create(
        [](lv_timer_t* timer) {
            auto* j = static_cast<FlashJob*>(lv_timer_get_user_data(timer));
            if (j != nullptr) {
                if (lv_obj_is_valid(j->obj)) {
                    lv_obj_set_style_bg_color(j->obj, j->color, 0);
                }
                delete j;
            }
            lv_timer_delete(timer);
        },
        130, job);
}

bool FoxyUI::OnRelease(int x, int y, bool short_tap) {
    if (!pressed_) {
        return false;
    }
    pressed_ = false;
    drag_ = 0;
    int dx = x - press_x_;
    int dy = y - press_y_;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    // 滑动判定收紧：至少 80px、横向位移是纵向的两倍以上，才当翻页/返回。
    // 这样"点一下"或者"轻轻蹭一下"都不会再整页滑走。
    if (adx >= kSwipeThreshold && adx > ady * 2) {
        if (page_ == kPageNovel) {
            // 小说页左右滑动是翻页，不是翻界面
            NovelTurn(dx < 0 ? 1 : -1);
            return true;
        }
        if (dx < 0) {
            if (page_ == kPageHome) {
                SetPage(kPageApps);
            } else {
                GoBack();
            }
        } else if (page_ == kPageApps) {
            SetPage(kPageHome);
        } else if (page_ != kPageHome) {
            GoBack();
        }
        return true;
    }
    if (page_ == kPageKeyboard) {
        return HandleKeyboardTap(x, y);
    }
    if (consume_press_) {
        return true;
    }
    // 位移不小但没到阈值：既不是点击也不是翻页，直接忽略，避免误触。
    if (adx > kTapMaxMove || ady > kTapMaxMove) {
        return page_ != kPageChat;
    }
    if (!short_tap) {
        return page_ != kPageChat;
    }
    if (page_ == kPageHome) {
        return true;  // 首页只响应左滑，点击不翻页（避免误触）
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
    if (page_ == kPageNovel) {
        // 左边三分之一上一页，右边三分之一下一页，中间什么都不做
        if (x < 90) {
            NovelTurn(-1);
        } else if (x > 230) {
            NovelTurn(1);
        }
        return true;
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
            int index = row * 2 + col;
            if (index == 0) {
                SetPage(kPageMusic);
            } else if (index == 1) {
                novel_book_ = 0;
                novel_page_ = 0;
                novel_pages_ = 0;
                SetPage(kPageNovel);
            } else if (index == 2) {
                SetPage(kPageAlarm);
            } else {
                ESP_LOGI(TAG, "app %s not implemented yet", names[index]);
            }
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
    if (page_ == kPageMusic) {
        return HandleMusicTap(x, y);
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
    lv_obj_update_layout(track);
    lv_obj_update_layout(knob);
    lv_area_t area;
    lv_obj_get_coords(track, &area);
    int knob_w = lv_obj_get_width(knob);
    int width = (area.x2 - area.x1 + 1) - knob_w;
    int clamped = value < 0 ? 0 : (value > 100 ? 100 : value);
    lv_obj_set_x(knob, area.x1 + (clamped * width) / 100);
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
    auto& ui = FoxyUI::Get();
    ui.UpdateClock();
    if (ui.page() == kPageMusic) {
        ui.UpdateMusicPage();
    }
}










// ---------------- 屏幕键盘 ----------------
namespace {
constexpr int kKbdTop = 58;
constexpr int kKbdKeyH = 32;
constexpr int kKbdGap = 4;
constexpr int kKbdMargin = 6;
}  // namespace

void FoxyUI::BuildKeyboard() {
    kbd_panel_ = lv_obj_create(kbd_);
    lv_obj_remove_style_all(kbd_panel_);
    lv_obj_set_pos(kbd_panel_, 0, 0);
    lv_obj_set_size(kbd_panel_, kScreenW, 56);
    lv_obj_set_style_bg_color(kbd_panel_, lv_color_hex(0x1B1B24), 0);
    lv_obj_set_style_bg_opa(kbd_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(kbd_panel_, 0, 0);
    lv_obj_clear_flag(kbd_panel_, LV_OBJ_FLAG_CLICKABLE);

    kbd_text_label_ = MakeLabel(kbd_panel_, "点字母开始输入", kTextDim);
    lv_obj_set_pos(kbd_text_label_, 10, 8);
    lv_obj_set_width(kbd_text_label_, 300);
    lv_label_set_long_mode(kbd_text_label_, LV_LABEL_LONG_CLIP);

    lv_obj_t* hint = MakeLabel(kbd_panel_, "右滑退出 · 发送后自动开聊", kTextDim);
    lv_obj_set_pos(hint, 10, 32);

    kbd_key_count_ = 0;
    const char* rows_letters[] = {"1234567890", "qwertyuiop", "asdfghjkl"};
    const char* rows_symbols[] = {"!@#$%^&*()", "-_=+[]{};:", "'\"/?,.\\|~"};
    const char** rows = kbd_symbols_ ? rows_symbols : rows_letters;

    for (int r = 0; r < 3; r++) {
        const char* chars = rows[r];
        int n = static_cast<int>(strlen(chars));
        int width = (kScreenW - 2 * kKbdMargin - (n - 1) * kKbdGap) / n;
        for (int i = 0; i < n; i++) {
            int x = kKbdMargin + i * (width + kKbdGap);
            int y = kKbdTop + r * (kKbdKeyH + kKbdGap);
            lv_obj_t* key = MakeCard(kbd_, x, y, width, kKbdKeyH);
            lv_obj_set_style_radius(key, 6, 0);
            lv_obj_clear_flag(key, LV_OBJ_FLAG_CLICKABLE);
            char label[2] = {chars[i], 0};
            lv_obj_t* text = MakeLabel(key, label, 0xF2F2F5);
            lv_obj_center(text);
            kbd_keys_[kbd_key_count_] = key;
            kbd_codes_[kbd_key_count_] = static_cast<int>(static_cast<unsigned char>(chars[i]));
            kbd_key_count_++;
        }
    }

    // 第四行：符号层切换 + zxcvbnm + 退格
    {
        const char* chars = "zxcvbnm";
        int n = static_cast<int>(strlen(chars)) + 2;
        int width = (kScreenW - 2 * kKbdMargin - (n - 1) * kKbdGap) / n;
        int y = kKbdTop + 3 * (kKbdKeyH + kKbdGap);
        auto add_key = [&](int slot, const char* label, int code, int w) {
            int x = kKbdMargin + slot * (width + kKbdGap);
            lv_obj_t* key = MakeCard(kbd_, x, y, w, kKbdKeyH);
            lv_obj_set_style_radius(key, 6, 0);
            lv_obj_clear_flag(key, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* text = MakeLabel(key, label, 0xF2F2F5);
            lv_obj_center(text);
            kbd_keys_[kbd_key_count_] = key;
            kbd_codes_[kbd_key_count_] = code;
            kbd_key_count_++;
            return key;
        };
        add_key(0, kbd_symbols_ ? "abc" : "123", -5, width);
        for (int i = 0; i < static_cast<int>(strlen(chars)); i++) {
            char label[2] = {chars[i], 0};
            add_key(1 + i, label, static_cast<int>(chars[i]), width);
        }
        // 退格单独占位（用整行剩余宽度）
        int used = kKbdMargin + n * (width + kKbdGap) - kKbdGap;
        int w = kScreenW - kKbdMargin - used;
        if (w < width) {
            w = width;
        }
        add_key(n - 1, "<-", -2, w);
    }

    // 第五行：空格 + 发送
    {
        int y = kKbdTop + 4 * (kKbdKeyH + kKbdGap);
        int w_space = 200;
        lv_obj_t* space = MakeCard(kbd_, kKbdMargin, y, w_space, kKbdKeyH);
        lv_obj_set_style_radius(space, 6, 0);
        lv_obj_clear_flag(space, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* space_text = MakeLabel(space, "空格", 0xF2F2F5);
        lv_obj_center(space_text);
        kbd_keys_[kbd_key_count_] = space;
        kbd_codes_[kbd_key_count_] = -3;
        kbd_key_count_++;

        int x = kKbdMargin + w_space + kKbdGap;
        int w = kScreenW - kKbdMargin - x;
        lv_obj_t* send = MakeCard(kbd_, x, y, w, kKbdKeyH);
        lv_obj_set_style_radius(send, 6, 0);
        lv_obj_set_style_bg_color(send, lv_color_hex(0xE8A33D), 0);
        lv_obj_clear_flag(send, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* send_text = MakeLabel(send, "发送", 0x14141A);
        lv_obj_center(send_text);
        kbd_keys_[kbd_key_count_] = send;
        kbd_codes_[kbd_key_count_] = -4;
        kbd_key_count_++;
    }
}

bool FoxyUI::HandleKeyboardTap(int x, int y) {
    for (int i = 0; i < kbd_key_count_; i++) {
        lv_area_t area;
        lv_obj_get_coords(kbd_keys_[i], &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            Flash(kbd_keys_[i]);
            KeyboardKey(kbd_codes_[i]);
            return true;
        }
    }
    return true;
}

void FoxyUI::KeyboardKey(int code) {
    if (code == -1) {
        return;
    }
    if (code == -5) {
        kbd_symbols_ = !kbd_symbols_;
        bool shift = kbd_symbols_;
        // 重建键盘：直接删掉旧键，重新铺一遍
        for (int i = 0; i < kbd_key_count_; i++) {
            if (kbd_keys_[i] != nullptr) {
                lv_obj_delete(kbd_keys_[i]);
                kbd_keys_[i] = nullptr;
            }
        }
        kbd_key_count_ = 0;
        kbd_symbols_ = shift;
        BuildKeyboard();
        return;
    }
    if (code == -2) {
        if (!kbd_text_.empty()) {
            kbd_text_.pop_back();
        }
        UpdateKeyboardText();
        return;
    }
    if (code == -3) {
        if (kbd_text_.size() < 60) {
            kbd_text_.push_back(' ');
        }
        UpdateKeyboardText();
        return;
    }
    if (code == -4) {
        KeyboardSend();
        return;
    }
    if (kbd_text_.size() < 60) {
        kbd_text_.push_back(static_cast<char>(code));
    }
    UpdateKeyboardText();
}

void FoxyUI::UpdateKeyboardText() {
    if (kbd_text_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (kbd_text_.empty()) {
        lv_label_set_text(kbd_text_label_, "点字母开始输入");
        lv_obj_set_style_text_color(kbd_text_label_, lv_color_hex(0x77778A), 0);
    } else {
        lv_label_set_text(kbd_text_label_, kbd_text_.c_str());
        lv_obj_set_style_text_color(kbd_text_label_, lv_color_hex(0xF2F2F5), 0);
    }
}

void FoxyUI::KeyboardSend() {
    if (kbd_text_.empty()) {
        return;
    }
    std::string text = kbd_text_;
    kbd_text_.clear();
    UpdateKeyboardText();
    SetPage(kPageChat);
    SetChatText(text);
    SetChatStatus("发送中…");
    Application::GetInstance().SendTypedText(text);
}






// ==== novel page (auto-appended) ====
struct NovelPayload {
    std::string title;
    int page = 0;
    int pages = 1;
    std::string text;
};
// 小说正文由服务器按字数切页，设备只负责翻页与显示，省内存。
constexpr int kNovelCharsPerPage = 420;

static std::string NovelServerBase() {
    Settings settings("wifi", true);
    std::string ota_url = settings.GetString("ota_url");
    std::string host;
    if (!ota_url.empty()) {
        size_t scheme = ota_url.find("://");
        if (scheme != std::string::npos) {
            size_t start = scheme + 3;
            size_t slash = ota_url.find('/', start);
            host = ota_url.substr(0, slash == std::string::npos ? ota_url.size() : slash);
        }
    }
    if (host.empty()) {
        host = "http://192.168.3.10:8003";
    }
    return host;
}

struct NovelJob {
    int book;
    int page;
};

void FoxyUI::NovelFetch(int page) {
    // 直接在调用线程里同步拉一页（约 100-300ms），简单可靠：
    // 之前用独立任务做，跨任务改控件会把 LVGL 内部状态踩坏（IllegalInstruction）。
    std::string base = NovelServerBase();
    char url[256];
    snprintf(url, sizeof(url), "%s/debug/novel/page?index=%d&page=%d", base.c_str(), novel_book_, page);
    ESP_LOGI(TAG, "novel get %s", url);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 6000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return;
    }
    std::string body;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        std::vector<char> buf(512);
        int read = 0;
        while ((read = esp_http_client_read(client, buf.data(), buf.size())) > 0) {
            body.append(buf.data(), read);
            if (body.size() > 6144) {
                break;
            }
        }
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "novel status=%d bytes=%d", status, static_cast<int>(body.size()));
    if (status != 200) {
        return;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return;
    }
    cJSON* text_item = cJSON_GetObjectItem(root, "text");
    cJSON* page_item = cJSON_GetObjectItem(root, "page");
    cJSON* pages_item = cJSON_GetObjectItem(root, "pages");
    cJSON* title_item = cJSON_GetObjectItem(root, "title");
    std::string body_text = cJSON_IsString(text_item) ? text_item->valuestring : "";
    int page_now = cJSON_IsNumber(page_item) ? page_item->valueint : 0;
    int page_total = cJSON_IsNumber(pages_item) ? pages_item->valueint : 1;
    std::string book_name = cJSON_IsString(title_item) ? title_item->valuestring : "";
    cJSON_Delete(root);

    SetNovelText(book_name, page_now, page_total, body_text);
}

void FoxyUI::SetNovelText(const std::string& title, int page, int pages, const std::string& text) {
    DisplayLockGuard lock(display_);
    novel_page_ = page;
    novel_pages_ = pages;
    if (novel_title_ != nullptr) {
        size_t dot = title.rfind(".txt");
        std::string shown = dot == std::string::npos ? title : title.substr(0, dot);
        lv_label_set_text(novel_title_, shown.c_str());
    }
    if (novel_body_ != nullptr) {
        lv_label_set_text(novel_body_, text.c_str());
    }
    if (novel_foot_ != nullptr) {
        char foot[32];
        snprintf(foot, sizeof(foot), "%d / %d", page + 1, pages);
        lv_label_set_text(novel_foot_, foot);
    }
}

void FoxyUI::BuildNovel() {
    novel_ = MakePage(lv_screen_active());
    lv_obj_t* header = lv_label_create(novel_);
    lv_label_set_text(header, "小说");
    lv_obj_set_style_text_color(header, lv_color_hex(kTextDim), 0);
    lv_obj_set_pos(header, 14, 10);

    novel_title_ = lv_label_create(novel_);
    lv_label_set_text(novel_title_, "加载中…");
    lv_obj_set_style_text_color(novel_title_, lv_color_hex(kAccent), 0);
    lv_obj_set_pos(novel_title_, 70, 10);

    novel_body_ = lv_label_create(novel_);
    lv_label_set_text(novel_body_, "");
    lv_obj_set_style_text_color(novel_body_, lv_color_hex(0xECECF1), 0);
    lv_label_set_long_mode(novel_body_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(novel_body_, 296);
    lv_obj_set_pos(novel_body_, 12, 40);
    lv_obj_set_style_text_line_space(novel_body_, 4, 0);

    novel_foot_ = lv_label_create(novel_);
    lv_label_set_text(novel_foot_, "");
    lv_obj_set_style_text_color(novel_foot_, lv_color_hex(kTextDim), 0);
    lv_obj_set_pos(novel_foot_, 268, 220);
}

void FoxyUI::NovelTurn(int delta) {
    int target = novel_page_ + delta;
    if (target < 0) {
        // 已经是第一页，往回翻就退回应用列表
        SetPage(kPageAppList);
        return;
    }
    if (novel_pages_ > 0 && target >= novel_pages_) {
        ESP_LOGI(TAG, "novel at last page");
        return;
    }
    novel_page_ = target;
    NovelFetch(target);
}






void FoxyUI::BuildAlarm() {
    alarm_ = MakePage(lv_screen_active());
    lv_obj_t* header = lv_label_create(alarm_);
    lv_label_set_text(header, "闹钟");
    lv_obj_set_style_text_color(header, lv_color_hex(kAccent), 0);
    lv_obj_set_pos(header, 14, 12);

    lv_obj_t* hint = lv_label_create(alarm_);
    lv_label_set_text(hint, "说：十分钟后叫我");
    lv_obj_set_style_text_color(hint, lv_color_hex(kTextDim), 0);
    lv_obj_set_pos(hint, 14, 44);

    alarm_list_ = lv_label_create(alarm_);
    lv_label_set_text(alarm_list_, "读取中…");
    lv_label_set_long_mode(alarm_list_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(alarm_list_, 292);
    lv_obj_set_pos(alarm_list_, 14, 80);
    lv_obj_set_style_text_line_space(alarm_list_, 6, 0);
}

void FoxyUI::SetAlarmText(const std::string& text) {
    DisplayLockGuard lock(display_);
    if (alarm_list_ != nullptr) {
        lv_label_set_text(alarm_list_, text.c_str());
    }
}

void FoxyUI::AlarmFetch() {
    std::string base = NovelServerBase();
    char url[192];
    snprintf(url, sizeof(url), "%s/debug/alarms", base.c_str());
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 5000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return;
    }
    std::string body;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        std::vector<char> buf(256);
        int read = 0;
        while ((read = esp_http_client_read(client, buf.data(), buf.size())) > 0) {
            body.append(buf.data(), read);
            if (body.size() > 4096) {
                break;
            }
        }
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "alarms status=%d bytes=%d", status, (int)body.size());
    if (status != 200) {
        return;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return;
    }
    std::string lines;
    cJSON* list = cJSON_GetObjectItem(root, "alarms");
    int count = cJSON_GetArraySize(list);
    if (count == 0) {
        lines = "没有正在倒计时的闹钟。\n\n说一句\"三分钟后叫我\"就行。";
    } else {
        for (int i = 0; i < count && i < 5; i++) {
            cJSON* item = cJSON_GetArrayItem(list, i);
            cJSON* label = cJSON_GetObjectItem(item, "label");
            cJSON* left = cJSON_GetObjectItem(item, "seconds_left");
            char line[96];
            int seconds = cJSON_IsNumber(left) ? left->valueint : 0;
            snprintf(line, sizeof(line), "· %d:%02d  %s", seconds / 60, seconds % 60,
                     cJSON_IsString(label) ? label->valuestring : "闹钟");
            if (!lines.empty()) {
                lines += "\n";
            }
            lines += line;
        }
    }
    cJSON_Delete(root);
    SetAlarmText(lines);
}
