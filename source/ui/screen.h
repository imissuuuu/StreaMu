#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <string>
#include "../app_context.h"
#include <vector>

// Forward declarations
struct AppContext;
class UIManager;

class Screen {
public:
    virtual ~Screen() = default;

    // Called once when the screen is pushed/switched to
    virtual void on_enter(AppContext& ctx) {}
    
    // Called once when the screen is popped/switched away from
    virtual void on_exit(AppContext& ctx) {}

    // Update logic (input handling, state changes)
    // Returns the name of the next screen if a transition is needed, otherwise empty string ""
    virtual std::string update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) = 0;

    // Drawing functions
    virtual void draw_top(const RenderContext& ctx, UIManager& ui_mgr) = 0;
    virtual void draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) = 0;
};
