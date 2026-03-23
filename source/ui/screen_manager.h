#pragma once
#include "screen.h"
#include <map>
#include <string>
#include <memory>

class ScreenManager {
public:
    void add_screen(const std::string& name, std::unique_ptr<Screen> screen) {
        screens[name] = std::move(screen);
    }

    void change_screen(AppContext& ctx, const std::string& name) {
        if (current_screen) {
            current_screen->on_exit(ctx);
        }
        auto it = screens.find(name);
        if (it != screens.end()) {
            current_screen = it->second.get();
            current_screen_name = name;
            current_screen->on_enter(ctx);
        }
    }

    std::string update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
        if (current_screen) {
            std::string action = current_screen->update(ctx, kDown, kHeld, kRepeat);
            // If the action represents a local screen name (e.g. "HomeScreen") we might switch it here
            // But since our screens currently return "trigger_*" or "", pass it up to main.cpp
            return action;
        }
        return "";
    }

    void draw_top(const RenderContext& ctx, UIManager& ui_mgr) {
        if (current_screen) current_screen->draw_top(ctx, ui_mgr);
    }

    void draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
        if (current_screen) current_screen->draw_bottom(ctx, ui_mgr);
    }

    // Explicitly release all screen resources (call before hardware shutdown)
    void clear() {
        current_screen = nullptr;
        current_screen_name = "";
        screens.clear();
    }

private:
    std::map<std::string, std::unique_ptr<Screen>> screens;
    Screen* current_screen = nullptr;
    std::string current_screen_name = "";
};
