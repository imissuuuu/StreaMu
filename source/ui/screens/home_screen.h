#pragma once
#include "../screen.h"
#include "../../app_context.h"
#include <vector>
#include <string>

class HomeScreen : public Screen {
public:
    void on_enter(AppContext& ctx) override;
    std::string update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) override;
    void draw_top(const RenderContext& ctx, UIManager& ui_mgr) override;
    void draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) override;

private:
    enum TileId {
        TILE_QA,          // Top-left
        TILE_PLAYING,     // Bottom-left
        TILE_SEARCH,      // Top-right
        TILE_PLAYLISTS,   // Mid-right
        TILE_SETTINGS,    // Bottom-right
        TILE_COUNT
    };

    struct Rect { float x, y, w, h; };

    // 2D selection: col (0=left, 1=right), row (within column)
    int sel_col_ = 1;
    int sel_row_ = 0;

    // QA internal selection
    int qa_sel_ = 0;
    bool qa_focused_ = false;

    bool show_search_result_ = false;

    // QA slots
    struct QASlot { std::string label; std::string playlist_id; };
    std::vector<QASlot> qa_slots_;
    void rebuild_qa_slots(const AppContext& ctx);

    // Helpers
    Rect get_tile_rect(TileId id) const;
    TileId get_selected_tile() const;
    bool is_playing_visible(const RenderContext& ctx) const;
    std::string get_tile_action(TileId id, AppContext& ctx);

    static std::vector<std::string> parse_qa_ids(const std::string& csv);
};
