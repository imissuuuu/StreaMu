#pragma once
#include "../screen.h"
#include "../ui_manager.h"
#include "../play_bar.h"

class PlayingScreen : public Screen {
public:
    void on_enter(AppContext& ctx) override;
    std::string update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) override;
    void draw_top(const RenderContext& ctx, UIManager& ui_mgr) override;
    void draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) override;

private:
    static constexpr float LIST_START_Y = 8.0f;
    static constexpr float LIST_END_Y   = PlayBar::BAR_Y;
    static constexpr float ITEM_H       = 30.0f;
    static constexpr float HOME_BTN_X   = 280.0f;
    static constexpr float HOME_BTN_Y   = PlayBar::BAR_Y - 30.0f;
};
