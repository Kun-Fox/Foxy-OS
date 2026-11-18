#pragma once

#include <lvgl.h>

class CoreS3LcdDisplay;

// 轻量分页：0=对话（沿用小智原生界面）1=首页 2=应用
// 触摸判定：按下记录坐标，抬起时算位移；横向 >=60px 视为切页。
class FoxyUI {
public:
    static FoxyUI& Get();

    void Init(CoreS3LcdDisplay* display);

    // 返回 true 表示这次触摸已被 UI 消费（不要再触发对话）
    bool OnPress(int x, int y);
    bool OnRelease(int x, int y, bool short_tap);

    void SetPage(int page);
    int page() const { return page_; }

private:
    void BuildHome();
    void BuildApps();
    void BuildPageShells();
    void UpdateClock();
    static void ClockTimerCb(lv_timer_t* timer);

    CoreS3LcdDisplay* display_ = nullptr;
    lv_obj_t* home_ = nullptr;
    lv_obj_t* apps_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* msg_labels_[3] = {nullptr, nullptr, nullptr};

    int page_ = 0;
    int press_x_ = 0;
    int press_y_ = 0;
    bool pressed_ = false;
};
