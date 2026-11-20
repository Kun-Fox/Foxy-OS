#pragma once

#include <lvgl.h>

#include <string>

class CoreS3LcdDisplay;

// 页面语义：0=首页 1=应用页 2=设置页 3=原生对话界面（不画我们自己的层）
// 触摸：按下记坐标；按住拖动用于设置页的音量/亮度滑条；抬起按位移判定滑动或点击。
class FoxyUI {
public:
    static FoxyUI& Get();

    void Init(CoreS3LcdDisplay* display);

    bool OnPress(int x, int y);
    bool OnRelease(int x, int y, bool short_tap);
    void OnMove(int x, int y);

    void SetPage(int page);
    int page() const { return page_; }

    // 首页消息卡（服务器推送用）
    void PushMessage(const std::string& text);
    // 服务器远程控制音乐（play/pause/next/prev/stop/refresh）
    void HandleMusicCommand(const std::string& action, int index);
    // 对话字幕（板级 SetChatMessage 转发进来）
    void SetChatText(const std::string& text);
    void SetChatStatus(const std::string& status);

private:
    void BuildHome();
    void BuildApps();
    void BuildSettings();
    void BuildAppList();
    void BuildToolList();
    void BuildTouchTest();
    void BuildCalculator();
    void BuildMusic();
    void BuildPlaylist();
    void TogglePlaylist();
    void UpdatePlaylistText();
    void BuildChat();
    void BuildKeyboard();
    void BuildNovel();
    void BuildAlarm();
    void AlarmFetch();
    void SetAlarmText(const std::string& text);
    void NovelTurn(int delta);
    void SetNovelText(const std::string& title, int page, int pages, const std::string& text);
    static void NovelTask(void* arg);
    static void NovelApplyCb(void* arg);
    void NovelFetch(int page);

    bool HandleKeyboardTap(int x, int y);
    void KeyboardKey(int code);
    void KeyboardSend();
    void UpdateKeyboardText();

    void UpdateMusicPage();
    void UpdateMusicProgress();
    void ToggleLyrics();
    void UpdateLyrics();
    void LoadMusicAssets(int index);
    static void MusicAssetTask(void* arg);
    bool HandleMusicTap(int x, int y);
    static void MusicRefreshTask(void* arg);
    void UpdateTouchTest(int x, int y);
    void UpdateCalcDisplay();
    void HandleCalcKey(int index);
    void BuildPageDots();
    void UpdateClock();
    void UpdateSettingsInfo();
    void UpdateDots();
    void ChangeVolumeBy(int delta);
    void ChangeBrightnessBy(int delta);
    bool HandleSettingsTap(int x, int y);
    void GoBack();
    void Ripple(int x, int y);
    void Flash(lv_obj_t* obj);
    static void ClockTimerCb(lv_timer_t* timer);

    CoreS3LcdDisplay* display_ = nullptr;
    // 全屏深色底板：滑页动画时页面挪开的那半边显示它，而不是露出底下的对话界面
    lv_obj_t* backdrop_ = nullptr;
    lv_obj_t* home_ = nullptr;
    lv_obj_t* art_ = nullptr;
    lv_obj_t* apps_ = nullptr;
    lv_obj_t* settings_ = nullptr;
    // 二级页：应用列表（音乐/小说/闹钟/相机）、工具列表、触摸测试
    lv_obj_t* applist_ = nullptr;
    lv_obj_t* toollist_ = nullptr;
    lv_obj_t* touchtest_ = nullptr;
    lv_obj_t* touch_label_ = nullptr;
    lv_obj_t* touch_dot_ = nullptr;
    lv_obj_t* calc_ = nullptr;
    lv_obj_t* calc_label_ = nullptr;
    // 音乐页
    lv_obj_t* chat_ = nullptr;
    lv_obj_t* chat_art_ = nullptr;
    lv_obj_t* chat_text_ = nullptr;
    lv_obj_t* chat_status_ = nullptr;
    lv_obj_t* music_ = nullptr;
    lv_obj_t* music_status_ = nullptr;
    lv_obj_t* music_rows_[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* music_play_ = nullptr;
    lv_obj_t* music_cover_ = nullptr;
    lv_obj_t* music_panel_ = nullptr;
    lv_obj_t* music_title_ = nullptr;
    lv_obj_t* music_time_ = nullptr;
    lv_obj_t* music_track_ = nullptr;
    lv_obj_t* music_fill_ = nullptr;
    lv_obj_t* music_knob_ = nullptr;
    lv_obj_t* playlist_page_ = nullptr;
    lv_obj_t* playlist_rows_[5] = {nullptr};
    lv_obj_t* playlist_cells_[5] = {nullptr};
    bool lyrics_empty_ = false;
    lv_obj_t* music_btn_prev_ = nullptr;
    lv_obj_t* music_btn_next_ = nullptr;
    lv_obj_t* lyrics_panel_ = nullptr;
    lv_obj_t* lyric_labels_[5] = {};
    bool lyrics_visible_ = false;
    int cover_index_ = -1;

    lv_obj_t* kbd_ = nullptr;
    lv_obj_t* novel_ = nullptr;
    lv_obj_t* novel_title_ = nullptr;
    lv_obj_t* novel_body_ = nullptr;
    lv_obj_t* novel_foot_ = nullptr;
    int novel_book_ = 0;
    int novel_page_ = 0;
    int novel_pages_ = 0;
    lv_obj_t* alarm_ = nullptr;
    lv_obj_t* alarm_list_ = nullptr;
    lv_obj_t* kbd_panel_ = nullptr;
    lv_obj_t* kbd_text_label_ = nullptr;
    lv_obj_t* kbd_keys_[48] = {};
    int kbd_codes_[48] = {};
    int kbd_key_count_ = 0;
    std::string kbd_text_;
    bool kbd_shift_ = false;
    bool kbd_symbols_ = false;
    lv_obj_t* back_music_ = nullptr;
    lv_obj_t* calc_keys_[16] = {nullptr};
    std::string calc_input_;
    double calc_acc_ = 0;
    char calc_op_ = 0;  // '+', '-', '*', '/'，0 表示没有待计算的操作
    bool calc_fresh_ = true;
    // 对话页浮在原生对话层上的“返回首页”键
    lv_obj_t* chat_back_ = nullptr;
    lv_obj_t* back_apps_ = nullptr;
    lv_obj_t* back_applist_ = nullptr;
    lv_obj_t* back_toollist_ = nullptr;
    lv_obj_t* back_touch_ = nullptr;
    lv_obj_t* back_calc_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* msg_labels_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* vol_label_ = nullptr;
    lv_obj_t* bri_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_obj_t* vol_minus_ = nullptr;
    lv_obj_t* vol_plus_ = nullptr;
    lv_obj_t* bri_minus_ = nullptr;
    lv_obj_t* bri_plus_ = nullptr;
    lv_obj_t* dots_[16] = {nullptr};

    int page_ = 0;
    int press_x_ = 0;
    int press_y_ = 0;
    bool pressed_ = false;
    bool consume_press_ = false;
    int drag_ = 0;  // 0 无，1 音量滑条，2 亮度滑条
    lv_obj_t* vol_track_ = nullptr;
    lv_obj_t* vol_knob_ = nullptr;
    lv_obj_t* bri_track_ = nullptr;
    lv_obj_t* bri_knob_ = nullptr;
    void SetVolume(int value);
    void SetBrightness(int value);
    void PlaceKnob(lv_obj_t* track, lv_obj_t* knob, int value);
    int volume_ = 0;
    int brightness_ = 100;
};





