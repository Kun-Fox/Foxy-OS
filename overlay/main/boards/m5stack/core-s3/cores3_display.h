#ifndef CORES3_DISPLAY_H
#define CORES3_DISPLAY_H

#include "display/lcd_display.h"

// CoreS3 专用界面：在小智原有状态栏/表情/字幕之上，加一条常驻控制条和一个信息页。
// 触摸事件先交给这里判定，未命中才落回“轻点屏幕开始对话”的默认行为。
class CoreS3LcdDisplay : public SpiLcdDisplay {
public:
    CoreS3LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    void SetupUI() override;

    // 显示/隐藏基类的对话层（状态栏、表情、字幕），四页切换时用
    void SetChatVisible(bool visible);

    // 服务器推送的消息卡
    void ShowMessageCard(const std::string& text) override;

    // 按下时记录起点（供分页滑动判定）
    void TouchPress(int x, int y);

    // 按住拖动（供滑条）
    void TouchMove(int x, int y);

    // 返回 true 表示这次触摸被界面元素消费掉了
    bool HandleTouch(int x, int y, bool short_tap = true);

private:
    void BuildControlBar();
    void RefreshControlBar();
    void ToggleSpeakerMute();
    void ChangeVolume(int delta);
    void ToggleInfoPanel();
    void HandleButton(int index);
    void UpdateInfoText();
    static void RefreshTimerCb(lv_timer_t* timer);

    lv_obj_t* bar_ = nullptr;
    lv_obj_t* buttons_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* talk_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* info_panel_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    bool speaker_muted_ = false;
};

#endif  // CORES3_DISPLAY_H
