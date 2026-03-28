#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include "ui/ui_renderer.h"
#include "ui/touch_state.h"
#include "ui/wallpaper.h"

// Forward declare to avoid circular include (full type in youtube_api.h via playlist_manager.h)
class YouTubeAPI;

struct AppContext : public RenderContext {
    std::string search_query = "";
    bool is_downloading = false;
    bool is_running = true;
    std::string playing_title = "";
    // is_paused inherited from RenderContext (no shadowing)
    std::string current_stream_url = "";
    LightLock lock; // Mutex for thread synchronization
    TouchState touch_state; // Touch input state

    // API pointer (set in main after YouTubeAPI is created)
    YouTubeAPI* api = nullptr;

    // Thumbnail for currently playing track
    Wallpaper thumbnail_tex;
    std::string thumbnail_vid_id; // video ID of the loaded thumbnail

    // Thumbnail async download state (protected by lock)
    bool thumbnail_loading = false;       // true while thread is running
    bool thumbnail_ready = false;         // true when raw data is ready for GPU upload
    std::vector<uint8_t> thumbnail_pixels; // cropped RGBA pixels (set by thread)
    int thumbnail_crop_size = 0;          // side length of cropped square
};
