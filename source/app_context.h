#pragma once
#include <3ds.h>
#include <string>
#include "ui/ui_renderer.h"
#include "ui/touch_state.h"

struct AppContext : public RenderContext {
    std::string search_query = "";
    bool is_downloading = false;
    bool is_running = true;
    std::string playing_title = ""; 
    // is_paused inherited from RenderContext (no shadowing)
    std::string current_stream_url = "";
    LightLock lock; // Mutex for thread synchronization
    TouchState touch_state; // Touch input state
};
