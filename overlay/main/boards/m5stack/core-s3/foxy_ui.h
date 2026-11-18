#pragma once

#include <lvgl.h>

#include <string>

class CoreS3LcdDisplay;

// 四页结构：0=对话（原生界面）1=首页 2=应用 3=设置
// 触摸：按下记坐标，抬起算位移；横向 ≥60px 视为切页，其余按页内控件命中。
class FoxyUI {
public:
    static FoxyUI& Get();

    void Init(CoreS3LcdDisplay* display);

    bool OnPress(int x, int y);
    bool OnRelease(int x, int y, bool short_tap);

    void SetPage(int page);
    int page() const { return page_; }

    // 首页消息卡（服务器推送用）
    void PushMessage(const std::string& text);

private:
    void BuildHome();
    void BuildApps();
    void BuildSettings();
    void BuildPageDots();
    void UpdateClock();
    void UpdateSettingsInfo();
    void UpdateDots();
    void ChangeVolumeBy(int delta);
    void ChangeBrightnessBy(int delta);
    bool HandleSettingsTap(int x, int y);
    static void ClockTimerCb(lv_timer_t* timer);

    CoreS3LcdDisplay* display_ = nullptr;
    lv_obj_t* home_ = nullptr;
    lv_obj_t* apps_ = nullptr;
    lv_obj_t* settings_ = nullptr;
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
    lv_obj_t* dots_[4] = {nullptr, nullptr, nullptr, nullptr};

    int page_ = 0;
    int press_x_ = 0;
    int press_y_ = 0;
    bool pressed_ = false;
    int volume_ = 0;
    int brightness_ = 100;
};
