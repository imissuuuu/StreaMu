#include "home_screen.h"
#include "../ui_constants.h"
#include "../play_bar.h"

// --- QA ID helpers ---

std::vector<std::string> HomeScreen::parse_qa_ids(const std::string& csv) {
    std::vector<std::string> ids;
    if (csv.empty()) return ids;
    size_t start = 0;
    while (start < csv.size()) {
        size_t pos = csv.find(',', start);
        std::string id = (pos == std::string::npos)
                         ? csv.substr(start)
                         : csv.substr(start, pos - start);
        if (!id.empty()) ids.push_back(id);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return ids;
}

// --- rebuild_qa_slots ---

void HomeScreen::rebuild_qa_slots(const AppContext& ctx) {
    qa_slots_.clear();
    auto qa_ids = parse_qa_ids(ctx.config.quick_access_ids);
    for (const auto& qid : qa_ids) {
        std::string name = qid;
        for (const auto& pl : ctx.playlists) {
            if (pl.id == qid) { name = pl.name; break; }
        }
        qa_slots_.push_back({name, qid});
    }
    if ((int)qa_ids.size() < 4) {
        qa_slots_.push_back({"+ Add", ""});
    }
}

// --- tile geometry ---

HomeScreen::Rect HomeScreen::get_tile_rect(TileId id) const {
    switch (id) {
    case TILE_QA:        return {HOME_LEFT_X,  HOME_AREA_TOP, HOME_COL_W, HOME_QA_H};
    case TILE_PLAYING:   return {HOME_LEFT_X,  HOME_QA_H,     HOME_COL_W, HOME_AREA_BOTTOM - HOME_QA_H};
    case TILE_SEARCH:    return {HOME_RIGHT_X, HOME_AREA_TOP,             HOME_COL_W, HOME_TILE_H};
    case TILE_PLAYLISTS: return {HOME_RIGHT_X, HOME_TILE_H,               HOME_COL_W, HOME_TILE_H};
    case TILE_SETTINGS:  return {HOME_RIGHT_X, HOME_TILE_H * 2.0f,        HOME_COL_W, HOME_AREA_BOTTOM - HOME_TILE_H * 2.0f};
    default:             return {0, 0, 0, 0};
    }
}

HomeScreen::TileId HomeScreen::get_selected_tile() const {
    if (sel_col_ == 0) {
        return (sel_row_ == 0) ? TILE_QA : TILE_PLAYING;
    } else {
        if (sel_row_ == 0) return TILE_SEARCH;
        if (sel_row_ == 1) return TILE_PLAYLISTS;
        return TILE_SETTINGS;
    }
}

bool HomeScreen::is_playing_visible(const RenderContext& ctx) const {
    return !ctx.active_playlist_id.empty()
        || !ctx.playing_id.empty()
        || show_search_result_;
}

// --- on_enter ---

void HomeScreen::on_enter(AppContext& ctx) {
    ctx.current_state = STATE_HOME;
    rebuild_qa_slots(ctx);
    sel_col_ = 1;
    sel_row_ = 0;
    qa_sel_ = 0;
    qa_focused_ = false;
}

// --- draw ---

void HomeScreen::draw_top(const RenderContext& ctx, UIManager& ui_mgr) {
    (void)ctx; (void)ui_mgr;
}

void HomeScreen::draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
    C2D_TextBuf buf = ui_mgr.get_text_buf();
    C2D_Text text;

    TileId selected = get_selected_tile();
    u32 border_color = ctx.theme->text_dim & 0x60FFFFFF;

    // --- Draw tiles ---
    auto draw_tile = [&](TileId id, const char* label) {
        Rect r = get_tile_rect(id);
        bool is_sel = (id == selected && !qa_focused_);

        // Background highlight
        if (is_sel) {
            C2D_DrawRectSolid(r.x, r.y, 0, r.w, r.h, ctx.theme->accent);
        }

        // Label centered
        u32 col = is_sel ? ctx.theme->accent_text : ctx.theme->text_body;
        float font = HOME_TILE_FONT;
        float ty = r.y + (r.h - font * 24.0f) / 2.0f;
        float tx = r.x + HOME_TILE_PAD;
        C2D_TextParse(&text, buf, label);
        C2D_DrawText(&text, C2D_WithColor, tx, ty, 0, font, font, col);
    };

    // Right column tiles
    draw_tile(TILE_SEARCH, "Search");
    draw_tile(TILE_PLAYLISTS, "Playlists");
    draw_tile(TILE_SETTINGS, "Settings");

    // Left column: Playing tile
    if (is_playing_visible(ctx)) {
        const char* playing_label = !ctx.active_playlist_id.empty()
                                    ? "Playing"
                                    : "Search Result";
        draw_tile(TILE_PLAYING, playing_label);
    }

    // Left column: Quick Access tile
    {
        Rect r = get_tile_rect(TILE_QA);
        bool qa_tile_sel = (selected == TILE_QA && !qa_focused_);

        if (qa_tile_sel) {
            C2D_DrawRectSolid(r.x, r.y, 0, r.w, r.h, ctx.theme->accent);
        }

        // QA header
        u32 hdr_col = qa_tile_sel ? ctx.theme->accent_text : ctx.theme->text_dim;
        C2D_TextParse(&text, buf, "Quick Access");
        C2D_DrawText(&text, C2D_WithColor, r.x + HOME_TILE_PAD, r.y + HOME_TILE_PAD,
                     0, HOME_QA_HEADER_FONT, HOME_QA_HEADER_FONT, hdr_col);

        // QA slots
        float slot_y = r.y + HOME_TILE_PAD + HOME_QA_HEADER_FONT * 24.0f + 4.0f;
        for (int i = 0; i < (int)qa_slots_.size(); ++i) {
            bool slot_sel = (qa_focused_ && selected == TILE_QA && i == qa_sel_);

            if (slot_sel) {
                C2D_DrawRectSolid(r.x, slot_y, 0, r.w, HOME_QA_SLOT_H, ctx.theme->accent);
            }

            u32 slot_col;
            if (slot_sel) {
                slot_col = ctx.theme->accent_text;
            } else if (qa_tile_sel) {
                slot_col = ctx.theme->accent_text;
            } else {
                slot_col = ctx.theme->text_body;
            }

            float text_y = slot_y + (HOME_QA_SLOT_H - HOME_QA_SLOT_FONT * 24.0f) / 2.0f;
            C2D_TextParse(&text, buf, qa_slots_[i].label.c_str());
            C2D_DrawText(&text, C2D_WithColor, r.x + HOME_TILE_PAD + 8.0f, text_y,
                         0, HOME_QA_SLOT_FONT, HOME_QA_SLOT_FONT, slot_col);

            slot_y += HOME_QA_SLOT_H;
        }
    }

    // --- Border lines (block outlines, overlapping at edges) ---
    float bw = HOME_BORDER_W;

    // Each block draws its own outline (top, bottom, left, right edges)
    // This creates the "overlapping frame" look

    // Helper: draw rect outline
    auto draw_outline = [&](float x, float y, float w, float h) {
        C2D_DrawRectSolid(x, y, 0, w, bw, border_color);           // top
        C2D_DrawRectSolid(x, y + h - bw, 0, w, bw, border_color);  // bottom
        C2D_DrawRectSolid(x, y, 0, bw, h, border_color);            // left
        C2D_DrawRectSolid(x + w - bw, y, 0, bw, h, border_color);   // right
    };

    // Left column: QA
    draw_outline(HOME_LEFT_X, HOME_AREA_TOP, HOME_COL_W, HOME_QA_H);
    // Left column: Playing
    draw_outline(HOME_LEFT_X, HOME_QA_H, HOME_COL_W, HOME_AREA_BOTTOM - HOME_QA_H);
    // Right column: Search
    draw_outline(HOME_RIGHT_X, HOME_AREA_TOP, HOME_COL_W, HOME_TILE_H);
    // Right column: Playlists
    draw_outline(HOME_RIGHT_X, HOME_TILE_H, HOME_COL_W, HOME_TILE_H);
    // Right column: Settings
    draw_outline(HOME_RIGHT_X, HOME_TILE_H * 2.0f, HOME_COL_W, HOME_AREA_BOTTOM - HOME_TILE_H * 2.0f);

    PlayBar::draw(ctx, ui_mgr);
}

// --- update ---

std::string HomeScreen::update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
    // Detect search track playback -> manage show_search_result_ flag
    if (!ctx.active_playlist_id.empty()) {
        show_search_result_ = false;
    } else if (!ctx.playing_id.empty()) {
        show_search_result_ = true;
    }

    // Rebuild QA slots on re-enter
    if (qa_slots_.empty()) {
        rebuild_qa_slots(ctx);
    }

    // --- Touch ---
    touchPosition touch;
    hidTouchRead(&touch);

    if (kDown & KEY_TOUCH) {
        ctx.touch_state.begin(touch.px, touch.py);
    }
    if ((kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        ctx.touch_state.update(touch.px, touch.py);
    }
    if (!(kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        bool was_tap = ctx.touch_state.end();
        if (was_tap) {
            int tx = ctx.touch_state.current_x;
            int ty = ctx.touch_state.current_y;

            // PlayBar touch
            if (ty >= (int)PlayBar::BAR_Y) {
                std::string bar_action = PlayBar::handle_touch(tx, ty, ctx);
                if (!bar_action.empty()) return bar_action;
            } else {
                // Hit test tiles
                for (int id = 0; id < TILE_COUNT; ++id) {
                    TileId tid = static_cast<TileId>(id);

                    // Skip invisible Playing tile
                    if (tid == TILE_PLAYING && !is_playing_visible(ctx)) continue;

                    Rect r = get_tile_rect(tid);
                    if (tx >= (int)r.x && tx < (int)(r.x + r.w) &&
                        ty >= (int)r.y && ty < (int)(r.y + r.h)) {

                        if (tid == TILE_QA) {
                            // Determine which QA slot was tapped
                            Rect qr = get_tile_rect(TILE_QA);
                            float slot_start = qr.y + HOME_TILE_PAD + HOME_QA_HEADER_FONT * 24.0f + 4.0f;
                            int slot_idx = (int)((ty - slot_start) / HOME_QA_SLOT_H);
                            if (slot_idx >= 0 && slot_idx < (int)qa_slots_.size()) {
                                qa_sel_ = slot_idx;
                                qa_focused_ = true;
                                sel_col_ = 0;
                                sel_row_ = 0;
                                return get_tile_action(TILE_QA, ctx);
                            }
                        } else {
                            // Update selection state
                            if (tid == TILE_SEARCH)    { sel_col_ = 1; sel_row_ = 0; }
                            if (tid == TILE_PLAYLISTS) { sel_col_ = 1; sel_row_ = 1; }
                            if (tid == TILE_SETTINGS)  { sel_col_ = 1; sel_row_ = 2; }
                            if (tid == TILE_PLAYING)   { sel_col_ = 0; sel_row_ = 1; }
                            qa_focused_ = false;
                            return get_tile_action(tid, ctx);
                        }
                        break;
                    }
                }
            }
        }
    }

    // --- D-pad navigation ---
    if (kRepeat & KEY_DDOWN) {
        if (qa_focused_) {
            // Move within QA slots
            if (qa_sel_ < (int)qa_slots_.size() - 1) {
                qa_sel_++;
            } else {
                // Exit QA → Playing (if visible) or wrap
                qa_focused_ = false;
                if (is_playing_visible(ctx)) {
                    sel_row_ = 1;
                } else {
                    sel_row_ = 0;
                    qa_sel_ = 0;
                }
            }
        } else if (sel_col_ == 0) {
            if (sel_row_ == 0) {
                if (is_playing_visible(ctx)) {
                    sel_row_ = 1;
                } else {
                    sel_row_ = 0; // wrap: stay on QA
                }
            } else {
                sel_row_ = 0; // wrap: Playing → QA
            }
        } else {
            sel_row_ = (sel_row_ + 1) % 3;
        }
    }

    if (kRepeat & KEY_DUP) {
        if (qa_focused_) {
            if (qa_sel_ > 0) {
                qa_sel_--;
            } else {
                qa_focused_ = false; // Exit QA focus upward
            }
        } else if (sel_col_ == 0) {
            if (sel_row_ == 1) {
                sel_row_ = 0;
            } else {
                // wrap: QA → Playing (if visible) or stay
                if (is_playing_visible(ctx)) {
                    sel_row_ = 1;
                }
            }
        } else {
            sel_row_ = (sel_row_ + 2) % 3; // wrap up
        }
    }

    if (kRepeat & KEY_DRIGHT) {
        if (sel_col_ == 0) {
            qa_focused_ = false;
            sel_col_ = 1;
            // Map left row to right row
            if (sel_row_ == 0) {
                sel_row_ = 0; // QA → Search
            } else {
                sel_row_ = 2; // Playing → Settings
            }
        }
    }

    if (kRepeat & KEY_DLEFT) {
        if (sel_col_ == 1) {
            sel_col_ = 0;
            if (sel_row_ <= 1) {
                sel_row_ = 0; // Search/Playlists → QA
            } else {
                // Settings → Playing if visible, else QA
                sel_row_ = is_playing_visible(ctx) ? 1 : 0;
            }
        }
    }

    // SELECT on QA slot → remove popup
    if (kDown & KEY_SELECT) {
        if (qa_focused_ && qa_sel_ >= 0 && qa_sel_ < (int)qa_slots_.size()) {
            if (!qa_slots_[qa_sel_].playlist_id.empty()) {
                ctx.selected_playlist_id = qa_slots_[qa_sel_].playlist_id;
                return "qa_remove_popup";
            }
        }
    }

    // A button
    if (kDown & KEY_A) {
        TileId tile = get_selected_tile();

        // Enter QA focus on first A press
        if (tile == TILE_QA && !qa_focused_ && !qa_slots_.empty()) {
            qa_focused_ = true;
            qa_sel_ = 0;
            return "";
        }

        return get_tile_action(tile, ctx);
    }

    // B button exits QA focus
    if (kDown & KEY_B) {
        if (qa_focused_) {
            qa_focused_ = false;
            return "";
        }
    }

    return "";
}

// --- get_tile_action ---

std::string HomeScreen::get_tile_action(TileId id, AppContext& ctx) {
    switch (id) {
    case TILE_SEARCH:    return "trigger_search";
    case TILE_PLAYLISTS: return "trigger_playlists";
    case TILE_SETTINGS:  return "trigger_settings";
    case TILE_PLAYING:
        if (is_playing_visible(ctx)) return "trigger_playing";
        return "";
    case TILE_QA:
        if (qa_focused_ && qa_sel_ >= 0 && qa_sel_ < (int)qa_slots_.size()) {
            if (qa_slots_[qa_sel_].playlist_id.empty()) {
                return "qa_add_popup";
            } else {
                ctx.selected_playlist_id = qa_slots_[qa_sel_].playlist_id;
                return "open_qa_playlist";
            }
        }
        return "";
    default: return "";
    }
}
