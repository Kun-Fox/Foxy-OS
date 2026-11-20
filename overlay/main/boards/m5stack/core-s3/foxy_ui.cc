#include "foxy_ui.h"

#include "audio/audio_codec.h"
#include "assets.h"

// LVGL binfont 加载器（LV_USE_FS_MEMFS 打开后可从内存里的字体文件建字库）
extern "C" lv_font_t* lv_binfont_create_from_buffer(void* buffer, uint32_t size);
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
#include <atomic>
#include <cstring>
#include <ctime>

#ifndef FOXY_UI_SELFTEST
#define FOXY_UI_SELFTEST 0
#endif

#define TAG "FoxyUI"

namespace {
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;
// 计算器 4x4 键区：布局与命中判定共用一套常量
constexpr int kCalcKeyW = 76;
constexpr int kCalcKeyH = 36;
constexpr int kCalcKeyGap = 3;
constexpr int kCalcKeyLeft = 3;
constexpr int kCalcKeyTop = 76;
constexpr int kBarH = 42;
constexpr int kStatusH = 0;                          // 不再保留状态栏，四页铺满整屏
constexpr int kPageH = kScreenH - kStatusH;          // 240，无栏铺满整屏
constexpr int kSwipeThreshold = 80;
// 小于这个位移算点击；介于它和滑动阈值之间算误触（交给滑条拖动）
constexpr int kTapMaxMove = 36;
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
constexpr int kPagePlaylist = 12;  // 歌单
constexpr int kPageCount = 13;

// 二级列表页：POD2 风格的整行列表（行高 34、步进 40）
constexpr int kRowX = 12;
constexpr int kRowW = 296;
constexpr int kRowTop = 46;
constexpr int kRowH = 34;
constexpr int kRowStride = 40;
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
constexpr uint32_t kBgDark = 0x141414;
constexpr uint32_t kCardBg = 0x262626;
constexpr uint32_t kAccent = 0xFFFFFF;   // POD2 风格：不用彩色，主色就是白
constexpr uint32_t kTextDim = 0x999999;

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


// POD2 的按压反馈：底色提亮 + 2px 白描边，松手自动恢复。
void ApplyPressedStyle(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(obj, LV_OPA_80, LV_STATE_PRESSED);
}

lv_obj_t* MakeCard(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(kCardBg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    ApplyPressedStyle(card);
    return card;
}

static void ToastDeleteCb(lv_timer_t* t) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
    if (obj != nullptr) {
        lv_obj_delete(obj);
    }
    lv_timer_delete(t);
}

// 轻提示：底部浮一条 1.2 秒后自动消失的小胶囊
// 二级页底部提示：这块屏没有返回键，靠右滑，得让人知道
static void AddBackHint(lv_obj_t* page) {
    if (page == nullptr) return;
    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, "右滑返回");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6E6E78), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -14, 16);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}
static void ShowToast(lv_obj_t* parent, const char* text) {
    if (parent == nullptr) {
        return;
    }
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 168, 34);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x2A2A32), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 17, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* label = lv_label_create(box);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF2F2F5), 0);
    lv_obj_center(label);
    lv_timer_t* timer = lv_timer_create(ToastDeleteCb, 1200, box);
    lv_timer_set_repeat_count(timer, 1);
}
// 给已有标签换字体后，LVGL 仍按旧字体的宽度排版，会出现只剩一个字的情况；
// 这里换完字体把宽度拉回自适应。
static void SetLabelFont(lv_obj_t* label, const lv_font_t* font) {
    if (label == nullptr || font == nullptr) return;
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
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
    lv_obj_set_style_opa(page, LV_OPA_0, 0);
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, page);
    lv_anim_set_values(&fade, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&fade, 150);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade, [](void* var, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_start(&fade);
}

FoxyUI& FoxyUI::Get() {
    static FoxyUI instance;
    return instance;
}

void FoxyUI::LoadFonts() {
    struct Item { const char* name; const lv_font_t** slot; };
    const Item items[] = {
        {"UI_FONT_14.bin", &font14_},
        {"UI_FONT_16.bin", &font16_},
        {"UI_FONT_18.bin", &font18_},
        {"UI_FONT_22.bin", &font22_},
    };
    for (const auto& item : items) {
        void* ptr = nullptr;
        size_t size = 0;
        if (!Assets::GetInstance().GetAssetData(item.name, ptr, size) || ptr == nullptr || size == 0) {
            ESP_LOGW(TAG, "font %s not in assets", item.name);
            continue;
        }
        *item.slot = lv_binfont_create_from_buffer(ptr, static_cast<uint32_t>(size));
        ESP_LOGI(TAG, "font %s size=%u -> %p", item.name, static_cast<unsigned>(size), static_cast<const void*>(*item.slot));
    }
}

void FoxyUI::ApplyPageFonts() {
    // POD2 的观感需要比 20px 更小的字：用固件自带的 noto sans 16px（同为思源黑体子集，含中文）
    LV_FONT_DECLARE(font_foxy_16);
    LV_FONT_DECLARE(font_foxy_14);
    LV_FONT_DECLARE(font_noto_sans_basic_14_1);
    LV_FONT_DECLARE(font_noto_sans_basic_20_4);
    font16_ = &font_foxy_16;
    font14_ = &font_foxy_14;
    font20_ = &font_noto_sans_basic_20_4;
    lv_obj_set_style_text_font(lv_screen_active(), font16_, 0);
    lv_obj_t* pages[] = {home_, apps_, settings_, applist_, toollist_, touchtest_, calc_,
                        chat_, music_, kbd_, novel_, alarm_, playlist_page_};
    for (auto* p : pages) {
        if (p != nullptr) lv_obj_set_style_text_font(p, font16_, 0);
    }
}

void FoxyUI::Init(CoreS3LcdDisplay* display) {
    ESP_LOGI(TAG, "fonts 14=%d 16=%d 18=%d 22=%d", font14_ != nullptr, font16_ != nullptr, font18_ != nullptr, font22_ != nullptr);
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
    ApplyPageFonts();  // 先挂到 screen 上，页面与标签才会继承 16px 字体
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
    BuildPlaylist();
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
    lv_obj_set_pos(clock_label_, 148, 18);
    SetLabelFont(clock_label_, font20_);

    date_label_ = MakeLabel(home_, "----/--/--", kTextDim);
    lv_obj_set_pos(date_label_, 150, 46);
    SetLabelFont(date_label_, font14_);

    lv_obj_t* msg_title = MakeLabel(home_, "消息", 0x8A8A92);
    lv_obj_set_pos(msg_title, 140, 70);
    SetLabelFont(msg_title, font14_);
    status_label_ = MakeLabel(home_, "", 0x8A8A92);
    lv_obj_set_pos(status_label_, 250, 198);
    SetLabelFont(status_label_, font14_);
    lv_obj_t* msg_card = MakeCard(home_, 136, 90, 172, 102);
    for (int i = 1; i <= 2; i++) {
        lv_obj_t* line = lv_obj_create(msg_card);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, 0, i * 34);
        lv_obj_set_size(line, 140, 1);
        lv_obj_set_style_bg_color(line, lv_color_hex(0x33333C), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < 3; i++) {
        msg_labels_[i] = MakeLabel(msg_card, i == 0 ? "暂无新消息" : "", i == 0 ? 0xA8A8B2 : 0xE8E8EE);
        lv_label_set_long_mode(msg_labels_[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(msg_labels_[i], font14_ != nullptr ? font14_ : lv_obj_get_style_text_font(msg_card, LV_PART_MAIN), 0);
        lv_obj_set_width(msg_labels_[i], 158);
        lv_obj_set_pos(msg_labels_[i], 0, 2 + i * 30);

    }
    UpdateClock();
}

// 用基础图形画图标：这套固件的字体里没有 FontAwesome 符号，
// 而 POD2 的观感就是"白色细线条画出来的图标"，所以直接摆几何图形。
static lv_obj_t* IconPart(lv_obj_t* parent, int x, int y, int w, int h, int radius, uint32_t color) {
    lv_obj_t* part = lv_obj_create(parent);
    lv_obj_remove_style_all(part);
    lv_obj_set_size(part, w, h);
    lv_obj_set_pos(part, x, y);
    lv_obj_set_style_radius(part, radius, 0);
    lv_obj_set_style_bg_opa(part, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(part, 2, 0);
    lv_obj_set_style_border_color(part, lv_color_hex(color), 0);
    lv_obj_clear_flag(part, LV_OBJ_FLAG_CLICKABLE);
    return part;
}

static void DrawIcon(lv_obj_t* cell, int kind) {
    // 统一画在 40x40 的盒子里，坐标都相对盒子，改动时只改这里
    lv_obj_t* box = lv_obj_create(cell);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 40, 40);
    lv_obj_set_pos(box, (kCellW - 40) / 2, 8);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    const uint32_t ink = 0xF2F2F5;

    if (kind == 0) {
        // 设置：齿轮 = 外圈 + 挖空 + 中心点 + 四齿
        IconPart(box, 7, 7, 26, 26, LV_RADIUS_CIRCLE, ink);
        IconPart(box, 11, 11, 18, 18, LV_RADIUS_CIRCLE, 0x262626);
        IconPart(box, 18, 18, 4, 4, LV_RADIUS_CIRCLE, ink);
        IconPart(box, 18, 1, 4, 6, 2, ink);
        IconPart(box, 18, 33, 4, 6, 2, ink);
        IconPart(box, 1, 18, 6, 4, 2, ink);
        IconPart(box, 33, 18, 6, 4, 2, ink);
    } else if (kind == 1) {
        // 应用：四宫格，留出均匀的缝
        for (int i = 0; i < 4; i++) {
            IconPart(box, 5 + (i % 2) * 17, 5 + (i / 2) * 17, 13, 13, 4, ink);
        }
    } else if (kind == 2) {
        // 对话：圆角气泡 + 左下小尾巴 + 三个点
        IconPart(box, 4, 8, 32, 22, 8, ink);
        IconPart(box, 10, 27, 8, 5, 2, ink);
        IconPart(box, 10, 17, 4, 4, LV_RADIUS_CIRCLE, 0x262626);
        IconPart(box, 18, 17, 4, 4, LV_RADIUS_CIRCLE, 0x262626);
        IconPart(box, 26, 17, 4, 4, LV_RADIUS_CIRCLE, 0x262626);
    } else {
        // 工具：扳手 = 斜杆 + 端头圆环
        static lv_point_precise_t pts[2] = {{7, 33}, {25, 15}};
        lv_obj_t* line = lv_line_create(box);
        lv_obj_set_size(line, 40, 40);
        lv_line_set_points(line, pts, 2);
        lv_obj_set_style_line_width(line, 3, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(ink), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        IconPart(box, 22, 3, 14, 14, LV_RADIUS_CIRCLE, ink);
        IconPart(box, 26, 7, 6, 6, LV_RADIUS_CIRCLE, 0x262626);
    }
}
void FoxyUI::BuildApps() {
    const char* names[4] = {"设置", "应用", "对话", "工具"};
    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
        lv_obj_t* cell = MakeCard(apps_, kGridMargin + col * (kCellW + kGridGap),
                                  kGridMargin + row * (kCellH + kGridGap), kCellW, kCellH);
        DrawIcon(cell, i);
        lv_obj_t* label = MakeLabel(cell, names[i], 0xCCCCCC);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}

void FoxyUI::BuildSettings() {
    // 所有二级页统一：右上角返回键、左上角标题
    back_apps_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
    lv_obj_t* title = MakeLabel(settings_, "设置", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    lv_obj_t* vol_card = MakeCard(settings_, 12, 38, 296, 64);
    lv_obj_set_style_radius(vol_card, 16, 0);
    lv_obj_clear_flag(vol_card, LV_OBJ_FLAG_CLICKABLE);
    // 音量：标题行在滑条上方，数值直接写在标签里，避免和轨道压字
    vol_label_ = MakeLabel(vol_card, "音量 0", 0xF2F2F5);
    lv_obj_set_pos(vol_label_, 16, 8);
    vol_track_ = MakeCard(vol_card, 16, 36, 264, 16);
    lv_obj_set_style_radius(vol_track_, 8, 0);
    lv_obj_set_style_bg_color(vol_track_, lv_color_hex(0x484848), 0);
    vol_fill_ = MakeCard(vol_card, 16, 36, 0, 16);
    lv_obj_set_style_radius(vol_fill_, 8, 0);
    lv_obj_set_style_bg_color(vol_fill_, lv_color_hex(0xFFFFFF), 0);
    vol_knob_ = MakeCard(vol_card, 16, 32, 12, 24);
    lv_obj_set_style_radius(vol_knob_, 6, 0);
    lv_obj_set_style_bg_color(vol_knob_, lv_color_hex(kAccent), 0);

    // 亮度
    lv_obj_t* bri_card = MakeCard(settings_, 12, 108, 296, 60);
    lv_obj_set_style_radius(bri_card, 16, 0);
    lv_obj_clear_flag(bri_card, LV_OBJ_FLAG_CLICKABLE);
    bri_label_ = MakeLabel(bri_card, "亮度 100", 0xF2F2F5);
    lv_obj_set_pos(bri_label_, 16, 8);
    bri_track_ = MakeCard(bri_card, 16, 36, 264, 16);
    lv_obj_set_style_radius(bri_track_, 8, 0);
    lv_obj_set_style_bg_color(bri_track_, lv_color_hex(0x484848), 0);
    bri_fill_ = MakeCard(bri_card, 16, 36, 0, 16);
    lv_obj_set_style_radius(bri_fill_, 8, 0);
    lv_obj_set_style_bg_color(bri_fill_, lv_color_hex(0xFFFFFF), 0);
    bri_knob_ = MakeCard(bri_card, 16, 32, 12, 24);
    lv_obj_set_style_radius(bri_knob_, 6, 0);
    lv_obj_set_style_bg_color(bri_knob_, lv_color_hex(0xFFFFFF), 0);

    info_label_ = MakeLabel(settings_, "读取中…", 0x8A8A92);
    lv_obj_set_pos(info_label_, 14, 182);
    lv_obj_set_style_text_line_space(info_label_, 6, 0);
    lv_obj_set_style_text_line_space(info_label_, 2, 0);

    PlaceKnob(vol_track_, vol_knob_, volume_);
    PlaceKnob(bri_track_, bri_knob_, brightness_);
    UpdateSettingsInfo();
    AddBackHint(settings_);
}

void FoxyUI::BuildAppList() {
    back_applist_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
    lv_obj_t* title = MakeLabel(applist_, "应用", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    const char* names[4] = {"音乐", "小说", "闹钟", "相机"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* row = MakeCard(applist_, kRowX, kRowTop + i * kRowStride, kRowW, kRowH);
        lv_obj_t* label = MakeLabel(row, names[i], 0xF2F2F5);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_t* chevron = MakeLabel(row, ">", 0x9A9AA2);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -16, 0);
    }
    AddBackHint(applist_);
}

void FoxyUI::BuildToolList() {
    back_toollist_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
    lv_obj_t* title = MakeLabel(toollist_, "工具", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    const char* names[5] = {"计算器", "触摸测试", "拍照测试", "文件", "SSH"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t* row = MakeCard(toollist_, kRowX, kRowTop + i * kRowStride, kRowW, kRowH);
        lv_obj_t* label = MakeLabel(row, names[i], 0xF2F2F5);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_t* chevron = MakeLabel(row, ">", 0x9A9AA2);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -16, 0);
    }
    AddBackHint(toollist_);
}

void FoxyUI::BuildTouchTest() {
    back_touch_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
    lv_obj_t* title = MakeLabel(touchtest_, "触摸测试", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);
    lv_obj_t* hint = MakeLabel(touchtest_, "按下屏幕任意位置，坐标会显示在这里", 0x8A8A92);
    lv_obj_set_pos(hint, 14, 40);
    SetLabelFont(hint, font14_);
    lv_obj_t* card = MakeCard(touchtest_, 12, 76, 296, 96);
    touch_label_ = MakeLabel(card, "x ---   y ---", 0xF2F2F5);
    lv_obj_center(touch_label_);
    SetLabelFont(touch_label_, font20_);
    touch_dot_ = MakeCard(touchtest_, 156, 190, 10, 10);
    lv_obj_set_style_radius(touch_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch_dot_, lv_color_hex(kAccent), 0);
    lv_obj_add_flag(touch_dot_, LV_OBJ_FLAG_HIDDEN);
}

void FoxyUI::UpdateTouchTest(int x, int y) {
    DisplayLockGuard lock(display_);
    if (touch_label_ != nullptr) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "x %d   y %d", x, y);
        lv_label_set_text(touch_label_, buffer);
    }
    if (touch_dot_ != nullptr) {
        lv_obj_clear_flag(touch_dot_, LV_OBJ_FLAG_HIDDEN);
        int dot_x = 12 + x * 296 / 320;
        int dot_y = 190 + y * 40 / 240;
        lv_obj_set_pos(touch_dot_, dot_x - 5, dot_y - 5);
    }
}

void FoxyUI::BuildCalculator() {
    back_calc_ = nullptr;  // 返回键已取消：二级页统一用滑动返回
    lv_obj_t* title = MakeLabel(calc_, "计算器", 0xF2F2F5);
    lv_obj_set_pos(title, 14, 12);

    lv_obj_t* display = MakeCard(calc_, 6, 34, 308, 36);
    lv_obj_set_style_radius(display, 12, 0);
    calc_label_ = MakeLabel(display, "0", 0xF2F2F5);
    lv_obj_set_width(calc_label_, LV_SIZE_CONTENT);
    lv_obj_align(calc_label_, LV_ALIGN_RIGHT_MID, -14, 0);
    if (font20_ != nullptr) lv_obj_set_style_text_font(calc_label_, font20_, 0);

    // 4 列 × 4 行键盘，键宽 76、高 38
    const char* keys[16] = {"7", "8", "9", "÷", "4", "5", "6", "×",
                            "1", "2", "3", "−", "C", "0", "=", "+"};
    constexpr int kKeyW = kCalcKeyW;
    constexpr int kKeyH = kCalcKeyH;
    constexpr int kKeyGap = kCalcKeyGap;
    constexpr int kKeyLeft = kCalcKeyLeft;
    constexpr int kKeyTop = kCalcKeyTop;
    for (int i = 0; i < 16; i++) {
        int col = i % 4;
        int row = i / 4;
        calc_keys_[i] = MakeButton(calc_, kKeyLeft + col * (kKeyW + kKeyGap),
                                   kKeyTop + row * (kKeyH + kKeyGap), kKeyW, kKeyH, keys[i]);
        if (i == 14) {
            // "=" 做成白底黑字：整页唯一的主色强调，和音乐播放器的播放键呼应
            lv_obj_set_style_bg_color(calc_keys_[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_t* eq = lv_obj_get_child(calc_keys_[i], 0);
            if (eq != nullptr) {
                lv_obj_set_style_text_color(eq, lv_color_hex(0x000000), 0);
            }
        }
    }
    UpdateCalcDisplay();
}

void FoxyUI::BuildChat() {
    lv_obj_t* pill = lv_obj_create(chat_);
    lv_obj_remove_style_all(pill);
    lv_obj_set_pos(pill, 12, 10);
    lv_obj_set_size(pill, 92, 26);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x262626), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 13, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    chat_status_ = MakeLabel(pill, "我在听", 0xCCCCCC);
    lv_obj_center(chat_status_);
    SetLabelFont(chat_status_, font14_);

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
    lv_obj_set_pos(chat_art_, (kScreenW - kChatW) / 2, 54);
    lv_obj_clear_flag(chat_art_, LV_OBJ_FLAG_CLICKABLE);

    // 字幕：三行，居中，长文本自动换行
    chat_text_ = MakeLabel(chat_, "按住屏幕说句话，或者从左下角打字喵", 0xF2F2F5);
    lv_obj_set_width(chat_text_, 296);
    lv_obj_set_pos(chat_text_, 16, 176);
    lv_label_set_long_mode(chat_text_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(chat_text_, 8, 0);
    lv_obj_set_style_text_color(chat_text_, lv_color_hex(0xF2F2F5), 0);
}

void FoxyUI::SetChatText(const std::string& text) {
    if (display_ == nullptr || chat_text_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    std::string shown = text;
    if (shown.find("待命中") != std::string::npos) {
        shown = "摸摸屏幕，或说 Hi ESP 叫醒我";
    }
    if (shown.size() > 120) {
        shown = shown.substr(0, 120) + "…";
    }
    lv_label_set_text(chat_text_, shown.c_str());
    // 字幕出现时淡入，换句不生硬
    lv_obj_set_style_opa(chat_text_, LV_OPA_30, 0);
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, chat_text_);
    lv_anim_set_values(&fade, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 180);
    lv_anim_set_exec_cb(&fade, [](void* var, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_start(&fade);
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

    // POD2 的播放器观感：封面完整居中（不裁切），下面一条控制区
    constexpr int kCoverSize = 168;
    constexpr int kPanelY = 168;
    music_cover_ = lv_image_create(music_);
    lv_obj_remove_style_all(music_cover_);
    lv_obj_set_pos(music_cover_, (kScreenW - kCoverSize) / 2, 6);
    lv_obj_set_size(music_cover_, kCoverSize, kCoverSize);
    lv_obj_set_style_radius(music_cover_, 18, 0);
    lv_obj_set_style_clip_corner(music_cover_, true, 0);
    lv_obj_clear_flag(music_cover_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(music_cover_, LV_OBJ_FLAG_HIDDEN);

    music_panel_ = lv_obj_create(music_);
    lv_obj_remove_style_all(music_panel_);
    lv_obj_set_pos(music_panel_, 0, kPanelY);
    lv_obj_set_size(music_panel_, kScreenW, kScreenH - kPanelY);
    lv_obj_set_style_bg_color(music_panel_, lv_color_hex(kBgDark), 0);
    lv_obj_set_style_bg_opa(music_panel_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(music_panel_, LV_OBJ_FLAG_CLICKABLE);

    music_title_ = MakeLabel(music_panel_, "-", 0xF2F2F5);
    lv_obj_set_width(music_title_, 200);
    lv_label_set_long_mode(music_title_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(music_title_, 16, 4);

    music_time_ = MakeLabel(music_panel_, "0:00 / 0:00", 0xB0B0BA);
    lv_obj_set_pos(music_time_, 200, 5);
    lv_obj_set_width(music_time_, 104);
    lv_obj_set_style_text_align(music_time_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(music_time_, LV_LABEL_LONG_CLIP);

    music_track_ = lv_obj_create(music_panel_);
    lv_obj_remove_style_all(music_track_);
    lv_obj_set_pos(music_track_, 16, 24);
    lv_obj_set_size(music_track_, 288, 3);
    lv_obj_set_style_bg_color(music_track_, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_opa(music_track_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_track_, 2, 0);
    lv_obj_clear_flag(music_track_, LV_OBJ_FLAG_CLICKABLE);

    music_fill_ = lv_obj_create(music_panel_);
    lv_obj_remove_style_all(music_fill_);
    lv_obj_set_pos(music_fill_, 16, 24);
    lv_obj_set_size(music_fill_, 0, 3);
    lv_obj_set_style_bg_color(music_fill_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(music_fill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_fill_, 2, 0);
    lv_obj_clear_flag(music_fill_, LV_OBJ_FLAG_CLICKABLE);

    music_knob_ = lv_obj_create(music_panel_);
    lv_obj_remove_style_all(music_knob_);
    lv_obj_set_size(music_knob_, 9, 9);
    lv_obj_set_style_bg_color(music_knob_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(music_knob_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(music_knob_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(music_knob_, 12, 21);
    lv_obj_clear_flag(music_knob_, LV_OBJ_FLAG_CLICKABLE);

    // 三个圆钮：中间白色实心（黑字），左右描边空心 —— POD2 的播放器
    auto make_round = [this](int cx, int cy, int size, const char* text, bool filled) {
        lv_obj_t* btn = lv_obj_create(music_panel_);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, size, size);
        lv_obj_set_pos(btn, cx, cy);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        if (filled) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_50, 0);
        }
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* label = MakeLabel(btn, text, filled ? 0x0A0A0A : 0xF2F2F5);
        lv_obj_center(label);
        return btn;
    };
    music_btn_prev_ = make_round(82, 34, 34, "<<", false);
    music_play_ = make_round(140, 30, 42, "\u25b6", true);
    music_btn_next_ = make_round(204, 34, 34, ">>", false);
    // 歌词浮层：盖住封面区域，点任意处返回封面
    lyrics_panel_ = lv_obj_create(music_);
    lv_obj_remove_style_all(lyrics_panel_);
    lv_obj_set_pos(lyrics_panel_, 0, 0);
    lv_obj_set_size(lyrics_panel_, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(lyrics_panel_, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(lyrics_panel_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(lyrics_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* lyr_title = MakeLabel(lyrics_panel_, "歌词", 0x8A8A92);
    lv_obj_set_pos(lyr_title, 14, 14);
    SetLabelFont(lyr_title, font14_);
    for (int i = 0; i < 5; i++) {
        lyric_labels_[i] = MakeLabel(lyrics_panel_, "", kTextDim);
        lv_obj_set_width(lyric_labels_[i], 292);
        lv_obj_set_style_text_align(lyric_labels_[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lyric_labels_[i], 14, 40 + i * 30);
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
        lv_obj_move_foreground(lyrics_panel_);  // 必须置顶，否则被封面盖住
        if (lyrics_empty_) {
            lv_label_set_text(lyric_labels_[0], "这首没有歌词");
            lv_obj_set_style_text_color(lyric_labels_[0], lv_color_hex(kTextDim), 0);
        } else {
            UpdateLyrics();
        }
    } else {
        lv_obj_add_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void FoxyUI::UpdateLyrics() {
    if (lyrics_panel_ == nullptr || !lyrics_visible_) {
        return;
    }
    auto& music = FoxyMusic::Get();
    DisplayLockGuard lock(display_);
    // 没有 .lrc 的曲子说清楚，别让歌词页空着像坏了
    if (lyrics_empty_ || music.LyricAt(0).empty()) {
        if (lyric_labels_[0] != nullptr) {
            lv_label_set_long_mode(lyric_labels_[0], LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(lyric_labels_[0], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(lyric_labels_[0], 8, 0);
            lv_obj_set_pos(lyric_labels_[0], 14, 80);
            lv_obj_set_style_text_color(lyric_labels_[0], lv_color_hex(kTextDim), 0);
            lv_label_set_text(lyric_labels_[0], "这首没有歌词\n\n把同名 .lrc 放进 music 目录");
        }
        for (int i = 1; i < 5; i++) {
            if (lyric_labels_[i] != nullptr) {
                lv_label_set_text(lyric_labels_[i], "");
            }
        }
        return;
    }
    int elapsed_ms = music.ElapsedSeconds() * 1000;
    std::string current = music.LyricAt(elapsed_ms);
    // 只显示"当前句"附近：当前句高亮，其它留空——比滚动歌词省心，也够看
    for (int i = 0; i < 5; i++) {
        if (lyric_labels_[i] == nullptr) {
            continue;
        }
        if (i == 1) {
            // 一句歌词太长就掐断加省略号：这块屏没有更小的字号，硬塞会压到屏幕外
            std::string shown = current;
            if (shown.size() > 18) {
                shown = shown.substr(0, 18) + "…";
            }
            lv_label_set_text(lyric_labels_[i], shown.c_str());
            lv_obj_set_style_text_color(lyric_labels_[i], lv_color_hex(0xFFFFFF), 0);
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
    // 同一时刻只允许一个封面/歌词拉取任务，避免切页与刷新任务撞在一起各拉一遍
    static std::atomic<bool> loading{false};
    if (loading.exchange(true)) {
        return;
    }
    struct Release {
        std::atomic<bool>* flag;
        ~Release() { flag->store(false); }
    } release{&loading};

    if (index < 0) {
        return;
    }
    lyrics_empty_ = !FoxyMusic::Get().LoadLyrics(index);
    if (music_cover_ == nullptr) {
        return;
    }
    if (FoxyMusic::Get().LoadCover(index)) {
        const uint8_t* data = FoxyMusic::Get().CoverData();
        if (data != nullptr) {
            g_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            g_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            g_cover_dsc.header.w = 168;
            g_cover_dsc.header.h = 168;
            g_cover_dsc.header.stride = 336;
            g_cover_dsc.data_size = 168 * 168 * 2;
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
    // 还没点播时，先显示曲库第一首的名字/时长，页面上不至于只剩"选一首歌"
    if (index < 0 && music.TrackCount() > 0) {
        index = 0;
    }
    std::string name = index >= 0 ? music.TrackName(index) : std::string();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".mp3") == 0) {
        name = name.substr(0, name.size() - 4);
    }
    snprintf(title, sizeof(title), "%s", name.empty() ? "选一首歌" : name.c_str());

    int duration = (index >= 0 && !music.IsPlaying()) ? music.TrackDurationMs(index) : music.DurationMs();
    int elapsed = music.ElapsedSeconds();
    char time_text[32];
    snprintf(time_text, sizeof(time_text), "%d:%02d / %d:%02d", elapsed / 60, elapsed % 60,
             duration / 60000, (duration / 1000) % 60);

    const char* play_text = music.IsPlaying() && !music.IsPaused() ? "||" : "\u25b6";

    {
        DisplayLockGuard lock(display_);
        if (music_title_ != nullptr) {
            {
            // 标题太长会压到右边的时长上，先掐到 20 个字（含省略号）
            std::string shown = title;
            if (shown.size() > 20) {
                shown = shown.substr(0, 20) + "…";
            }
            lv_label_set_text(music_title_, shown.c_str());
        }
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

static int g_pending_asset_index = -1;

static void MusicAssetsDeferredCb(lv_timer_t* timer) {
    lv_timer_delete(timer);
    int index = g_pending_asset_index;
    g_pending_asset_index = -1;
    if (index >= 0) {
        FoxyUI::Get().LoadMusicAssets(index);
    }
}

void FoxyUI::ScheduleMusicAssets(int index) {
    g_pending_asset_index = index;
    lv_timer_t* timer = lv_timer_create(MusicAssetsDeferredCb, 80, nullptr);
    if (timer != nullptr) {
        lv_timer_set_repeat_count(timer, 1);
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
    } else if (action == "raw") {
            FoxyMusic::Get().PlayRawTest();
            return;
        } else if (action == "beep") {
            FoxyMusic::Get().PlayTone();
            return;
        }
        if (action == "stop") {
        music.Stop();
    } else if (action == "next") {
        music.Next();
    } else if (action == "prev") {
        music.Prev();
    } else if (action == "lyrics") {
        ToggleLyrics();
        return;
    } else if (action == "playlist") {
        TogglePlaylist();
        return;
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
    ESP_LOGI(TAG, "music tap x=%d y=%d playing=%d", x, y, (int)music.IsPlaying());

    if (page_ == kPagePlaylist) {
        // 60px 起每行 34px，点中第几行就播第几首
        int row = (y - 60) / 34;
        if (y >= 48 && row >= 0 && row < music.TrackCount() && row < 5) {
            music.Play(row);
            xTaskCreate(MusicAssetTask, "music_asset", 6144,
                        reinterpret_cast<void*>(static_cast<intptr_t>(row)), 3, nullptr);
            UpdateMusicPage();
            return true;
        }
        SetPage(kPageMusic);
        return true;
    }

    if (lyrics_visible_) {
        ToggleLyrics();
        return true;
    }

    if (y < 166) {
        // 点封面：切歌词
        ToggleLyrics();
        return true;
    }

    // 进度条区域：点哪跳哪
    if (y >= 168 && y <= 192 && x >= 8 && x <= 312) {
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

    lv_obj_t* pages[] = {home_, apps_, settings_, applist_, toollist_, touchtest_, calc_, music_,
                         chat_, kbd_, novel_, alarm_, playlist_page_};
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
            // 封面/歌词要发 HTTP，放到页面画完之后再做（用一次性 timer），
            // 否则点"音乐"会先卡住几百毫秒到十几秒（服务器不通时）。
            if (!FoxyMusic::Get().IsPlaying()) {
                int index = FoxyMusic::Get().CurrentIndex();
                if (index < 0) {
                    index = 0;
                }
                ScheduleMusicAssets(index);
            }
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
        case kPagePlaylist:
            target = playlist_page_;
            UpdatePlaylistText();
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
        constexpr int kKeyW = kCalcKeyW;
        constexpr int kKeyH = kCalcKeyH;
        constexpr int kKeyGap = kCalcKeyGap;
        constexpr int kKeyLeft = kCalcKeyLeft;
        constexpr int kKeyTop = kCalcKeyTop;
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
        case kPageKeyboard:
            SetPage(kPageChat);
            break;
        case kPageNovel:
        case kPageAlarm:
            SetPage(kPageAppList);
            break;
        case kPagePlaylist:
            SetPage(kPageMusic);
            break;
        default:
            // 兜底：任何没显式登记的页面都退回应用页，绝不让用户卡在某一页出不来。
            SetPage(kPageApps);
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
        if (page_ == kPageMusic && ady >= 50 && ady > adx) {
            // 音乐页下滑 = 打开歌单
            SetPage(kPagePlaylist);
            return true;
        }
        if (page_ == kPagePlaylist && ady >= 50 && ady > adx) {
            // 歌单页下滑 / 上滑都收起
            SetPage(kPageMusic);
            return true;
        }
        if (page_ == kPageNovel) {
            // 小说页左右滑动是翻页；已经在第一页还往右滑，那就是想退出去。
            if (dx > 0 && novel_page_ <= 0) {
                GoBack();
            } else {
                NovelTurn(dx < 0 ? 1 : -1);
            }
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
    // 音乐页/歌单页在判定"误触忽略"之前就先处理：
    // 手指在 2 寸屏上按圆按钮经常带十几二十像素的位移，不能因此当成误触丢掉。
    if (page_ == kPageMusic) {
        if (ady >= 50 && ady > adx) {
            SetPage(kPagePlaylist);
            return true;
        }
        return HandleMusicTap(x, y);
    }
    if (page_ == kPagePlaylist) {
        if (ady >= 50 && ady > adx) {
            SetPage(kPageMusic);
            return true;
        }
        int row = (y - 60) / 34;
        if (y >= 48 && row >= 0 && row < FoxyMusic::Get().TrackCount() && row < 5) {
            FoxyMusic::Get().Play(row);
            SetPage(kPageMusic);
            xTaskCreate(MusicAssetTask, "music_asset", 6144,
                        reinterpret_cast<void*>(static_cast<intptr_t>(row)), 3, nullptr);
            return true;
        }
        return true;
    }
    if (page_ == kPageChat) {
        // 对话页下半屏是字幕区：点它打开屏幕键盘，用来打字而不是说话。
        if (y >= 132) {
            SetPage(kPageKeyboard);
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
        int row = (y - kRowTop) / kRowStride;
        bool in_row = (y >= kRowTop) && ((y - kRowTop) % kRowStride) < kRowH && x >= kRowX && x <= kRowX + kRowW;
        if (in_row && row >= 0 && row <= 3) {
            static const char* names[4] = {"音乐", "小说", "闹钟", "相机"};
            int index = row;
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
            ShowToast(applist_, "这个还没做喵");
            }
        }
        return true;
    }
    if (page_ == kPageToolList) {
        if (HitTest(back_toollist_, x, y)) {
            SetPage(kPageApps);
            return true;
        }
        // 与 BuildToolList 的整行列表保持一致
        int index = -1;
        if (y >= kRowTop && x >= kRowX && x <= kRowX + kRowW) {
            int row = (y - kRowTop) / kRowStride;
            if (((y - kRowTop) % kRowStride) < kRowH && row >= 0 && row <= 4) {
                index = row;
            }
        }
        if (index >= 0) {
            if (index == 0) {
                SetPage(kPageCalc);
            } else if (index == 1) {
                SetPage(kPageTouch);
            } else if (index < 5) {
                static const char* names[5] = {"计算器", "触摸测试", "拍照测试", "文件", "SSH"};
                ESP_LOGI(TAG, "tool %s not implemented yet", names[index]);
            ShowToast(toollist_, "这个还没做喵");
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
    if (HitTest(vol_track_, x, y) || (y >= 38 && y <= 102)) {
        drag_ = 1;
        SetVolume(ValueFromX(vol_track_, x, 0, 100));
        return true;
    }
    if (HitTest(bri_track_, x, y) || (y >= 108 && y <= 168)) {
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
    // track 现在挂在卡片里，set_x 是相对父对象；用父对象坐标换算，避免旋钮跑到卡片外
    lv_area_t parent_area;
    lv_obj_get_coords(lv_obj_get_parent(knob), &parent_area);
    int rel_x = area.x1 - parent_area.x1 + (clamped * width) / 100;
    lv_obj_set_x(knob, rel_x);
    // 已经走过的那一段填白，扫一眼就知道大概多少
    lv_obj_t* fill = (knob == vol_knob_) ? vol_fill_ : bri_fill_;
    if (fill != nullptr) {
        lv_obj_set_width(fill, (clamped * width) / 100 + knob_w / 2);
    }
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
    if (status_label_ != nullptr) {
        int level = 0; bool charging = false, discharging = false;
        Board::GetInstance().GetBatteryLevel(level, charging, discharging);
        char status[48];
        snprintf(status, sizeof(status), "%d%%%s", level, charging ? " 充电中" : "");
        lv_label_set_text(status_label_, status);
    }
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
constexpr int kKbdTop = 56;
constexpr int kKbdKeyH = 32;
constexpr int kKbdGap = 4;
constexpr int kKbdMargin = 6;
}  // namespace

void FoxyUI::BuildKeyboard() {
    kbd_panel_ = lv_obj_create(kbd_);
    lv_obj_remove_style_all(kbd_panel_);
    lv_obj_set_pos(kbd_panel_, 0, 0);
    lv_obj_set_size(kbd_panel_, kScreenW, 56);
    lv_obj_set_style_bg_color(kbd_panel_, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(kbd_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(kbd_panel_, 0, 0);
    lv_obj_clear_flag(kbd_panel_, LV_OBJ_FLAG_CLICKABLE);

    kbd_text_label_ = MakeLabel(kbd_panel_, "点字母开始输入", 0xF2F2F5);
    lv_obj_set_pos(kbd_text_label_, 12, 6);
    lv_obj_set_width(kbd_text_label_, 300);
    lv_label_set_long_mode(kbd_text_label_, LV_LABEL_LONG_CLIP);

    lv_obj_t* hint = MakeLabel(kbd_panel_, "右滑退出 · 发送后自动开聊", 0x8A8A92);
    lv_obj_set_pos(hint, 12, 30);
    SetLabelFont(hint, font14_);

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
        lv_obj_set_style_bg_color(send, lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(send, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* send_text = MakeLabel(send, "发送", 0x000000);
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
    lv_obj_set_style_opa(novel_body_, LV_OPA_30, 0);
    lv_anim_t nfade;
    lv_anim_init(&nfade);
    lv_anim_set_var(&nfade, novel_body_);
    lv_anim_set_values(&nfade, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_duration(&nfade, 160);
    lv_anim_set_exec_cb(&nfade, [](void* var, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_start(&nfade);
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
    lv_obj_set_style_text_color(header, lv_color_hex(0xF2F2F5), 0);
    lv_obj_set_pos(header, 14, 12);

    lv_obj_t* hint = lv_label_create(alarm_);
    lv_label_set_text(hint, "说：十分钟后叫我");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8A8A92), 0);
    lv_obj_set_pos(hint, 14, 38);
    SetLabelFont(hint, font14_);

    lv_obj_t* card = MakeCard(alarm_, 12, 66, 296, 118);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    alarm_list_ = lv_label_create(card);
    lv_label_set_text(alarm_list_, "读取中…");
    lv_label_set_long_mode(alarm_list_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(alarm_list_, 272);
    lv_obj_set_pos(alarm_list_, 16, 14);
    lv_obj_set_style_text_color(alarm_list_, lv_color_hex(0xF2F2F5), 0);
    lv_obj_set_style_text_line_space(alarm_list_, 6, 0);
    AddBackHint(alarm_);
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
        lines = "暂时没有闹钟\n说一句「三分钟后叫我」就行";
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




void FoxyUI::BuildPlaylist() {
    // 歌单做成独立的一页（浮层那套在这块屏上一直画不出来，换页最稳）
    playlist_page_ = MakePage(lv_screen_active());

    lv_obj_t* title = MakeLabel(playlist_page_, "歌单", 0xFFFFFF);
    lv_obj_set_pos(title, 14, 10);
    lv_obj_t* hint = MakeLabel(playlist_page_, "点一首播放 · 下滑收起", kTextDim);
    lv_obj_set_pos(hint, 14, 34);

    for (int i = 0; i < 5; i++) {
        lv_obj_t* cell = MakeCard(playlist_page_, 12, 60 + i * 34, 296, 28);
        playlist_rows_[i] = MakeLabel(cell, "", 0xF2F2F5);
        lv_obj_set_width(playlist_rows_[i], 280);
        lv_label_set_long_mode(playlist_rows_[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(playlist_rows_[i], LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
        playlist_cells_[i] = cell;
    }
}

void FoxyUI::TogglePlaylist() {
    if (page_ == kPageMusic) {
        SetPage(kPagePlaylist);
    } else if (page_ == kPagePlaylist) {
        SetPage(kPageMusic);
    }
}

void FoxyUI::UpdatePlaylistText() {
    auto& music = FoxyMusic::Get();
    int count = music.TrackCount();
    if (count > 5) {
        count = 5;
    }
    DisplayLockGuard lock(display_);
    for (int i = 0; i < 5; i++) {
        if (playlist_cells_[i] == nullptr || playlist_rows_[i] == nullptr) {
            continue;
        }
        if (i >= count) {
            lv_obj_add_flag(playlist_cells_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(playlist_cells_[i], LV_OBJ_FLAG_HIDDEN);
        std::string name = music.TrackName(i);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".mp3") == 0) {
            name = name.substr(0, name.size() - 4);
        }
        if (name.size() > 24) {
            name = name.substr(0, 24) + "…";
        }
        char line[96];
        snprintf(line, sizeof(line), "%s%d  %s", (i == music.CurrentIndex()) ? "▶ " : "", i + 1,
                 name.c_str());
        lv_label_set_text(playlist_rows_[i], line);
        lv_obj_set_style_text_color(playlist_rows_[i],
                                    lv_color_hex(i == music.CurrentIndex() ? 0xFFFFFF : 0xF2F2F5), 0);
    }
}




void FoxyUI::SimulateTouch(int x1, int y1, int x2, int y2) {
    ESP_LOGI(TAG, "simulate touch (%d,%d)->(%d,%d) page=%d", x1, y1, x2, y2, page_);
    // 开发期入口：先按下再抬起，走和真实触摸完全相同的路径
    OnPress(x1, y1);
    int dx = x2 - x1;
    int dy = y2 - y1;
    bool short_tap = (dx > -18 && dx < 18 && dy > -18 && dy < 18);
    OnRelease(x2, y2, short_tap);
}





