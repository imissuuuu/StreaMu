#pragma once
#include "../screen.h"
#include <string>

class PlaylistDetailScreen : public Screen {
public:
    void on_enter(AppContext& ctx) override;
    std::string update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) override;
    void draw_top(const RenderContext& ctx, UIManager& ui_mgr) override;
    void draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) override;

private:
    int mode_btn_focus_ = 1; // 0=SHUFFLE, 1=ORDER (default ORDER)
};
