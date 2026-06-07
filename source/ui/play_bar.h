#pragma once
#include "ui_constants.h"
#include "ui_icon_cache.h"
#include "ui_manager.h"
#include "ui_renderer.h"
#include <string>

namespace PlayBar {
constexpr float BAR_Y = 200.0f;
constexpr float BAR_H = 40.0f;
constexpr float SEEK_H = 14.0f; // Seek bar height at top of BAR
// 5 zones: LOOP | PREV | PLAY/PAUSE | NEXT | SHUF (below seek bar)
constexpr float ZONE_1 = 55.0f;
constexpr float ZONE_2 = 118.0f;
constexpr float ZONE_3 = 202.0f;
constexpr float ZONE_4 = 265.0f;

// Parse "M:SS" or "H:MM:SS" duration string to total seconds. Returns 0 on
// failure.
inline int parse_duration_secs(const std::string &dur) {
  if (dur.empty() || dur == "?")
    return 0;
  int parts[3] = {0, 0, 0};
  int count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= dur.size() && count < 3; ++i) {
    if (i == dur.size() || dur[i] == ':') {
      if (i > start) {
        parts[count] = 0;
        for (size_t j = start; j < i; ++j)
          parts[count] = parts[count] * 10 + (dur[j] - '0');
        ++count;
      }
      start = i + 1;
    }
  }
  if (count == 2)
    return parts[0] * 60 + parts[1];
  if (count == 3)
    return parts[0] * 3600 + parts[1] * 60 + parts[2];
  return 0;
}

inline void draw(const RenderContext &ctx, UIManager &ui_mgr) {
  C2D_TextBuf buf = ui_mgr.get_text_buf();
  C2D_Text text;
  constexpr float FONT_TIME = 0.44f;
  static std::string s_cached_duration_text;
  static int s_cached_total_secs = 0;
  static float s_cached_duration_width = 0.0f;

  u32 bar_bg = (ctx.config.mode == THEME_DARK)
                   ? C2D_Color32(55, 55, 55, 255)
                   : C2D_Color32(230, 230, 230, 255);
  u32 bar_text = ctx.theme->text_body;

  if (ctx.playing_id.empty())
    return;

  C2D_DrawRectSolid(0, BAR_Y, 0, 320, BAR_H, bar_bg);

  // === Seek bar (top 8px of play bar) ===
  {
    if (s_cached_duration_text != ctx.playing_duration) {
      s_cached_duration_text = ctx.playing_duration;
      s_cached_total_secs = parse_duration_secs(ctx.playing_duration);
      if (!ctx.playing_duration.empty() && ctx.playing_duration != "?") {
        C2D_Text dur_text;
        C2D_TextParse(&dur_text, buf, ctx.playing_duration.c_str());
        s_cached_duration_width = dur_text.width * FONT_TIME;
      } else {
        s_cached_duration_width = 0.0f;
      }
    }
    const int total_secs = s_cached_total_secs;
    if (total_secs > 0) {
      // Check if user is currently dragging on the seek bar
      const TouchState &ts = ctx.touch_state;
      bool is_drag_seeking = ts.is_touching && ts.start_y >= (int)BAR_Y &&
                             ts.start_y < (int)(BAR_Y + SEEK_H);

      float ratio;
      int elapsed;
      if (is_drag_seeking) {
        // Follow finger during drag
        ratio = (float)ts.current_x / 320.0f;
        if (ratio < 0.0f)
          ratio = 0.0f;
        if (ratio > 1.0f)
          ratio = 1.0f;
        elapsed = (int)(ratio * total_secs);
      } else {
        u64 now = osGetTime();
        u64 total_paused = ctx.pause_accumulated_ms;
        if ((ctx.is_paused || ctx.is_buffering) && ctx.pause_started_at > 0)
          total_paused += now - ctx.pause_started_at;
        elapsed = (ctx.playback_start_time > 0)
                      ? (int)((now - ctx.playback_start_time - total_paused) /
                              1000ULL)
                      : 0;
        if (elapsed < 0)
          elapsed = 0;
        if (elapsed > total_secs)
          elapsed = total_secs;
        ratio = (float)elapsed / (float)total_secs;
      }

      // Filled portion — clearly distinct from both bar_bg and text_body
      u32 seek_fill = (ctx.config.mode == THEME_DARK)
                          ? C2D_Color32(100, 100, 100, 255)
                          : C2D_Color32(145, 145, 145, 255);
      float fill_w = ratio * 320.0f;
      C2D_DrawRectSolid(0, BAR_Y, 0, fill_w, SEEK_H, seek_fill);

      // Playhead (4px wide, contrasting dot)
      u32 head_color = (ctx.config.mode == THEME_DARK)
                           ? C2D_Color32(30, 30, 30, 255)
                           : C2D_Color32(255, 255, 255, 255);
      float head_x = fill_w - 2.0f;
      if (head_x < 0.0f)
        head_x = 0.0f;
      C2D_DrawRectSolid(head_x, BAR_Y, 0, 4.0f, SEEK_H, head_color);

      // Elapsed time — left side
      char el_buf[16];
      int el_m = elapsed / 60, el_s = elapsed % 60;
      snprintf(el_buf, sizeof(el_buf), "%d:%02d", el_m, el_s);
      C2D_Text elapsed_text;
      C2D_TextParse(&elapsed_text, buf, el_buf);
      C2D_DrawText(&elapsed_text, C2D_WithColor, 2.0f, BAR_Y, 0, FONT_TIME,
                   FONT_TIME, bar_text & 0xA0FFFFFF);

      // Total duration — right side (fixed string from ctx)
      if (!ctx.playing_duration.empty() && ctx.playing_duration != "?") {
        C2D_Text dur_text;
        C2D_TextParse(&dur_text, buf, ctx.playing_duration.c_str());
        C2D_DrawText(&dur_text, C2D_WithColor,
                     320.0f - s_cached_duration_width - 2.0f, BAR_Y, 0,
                     FONT_TIME, FONT_TIME, bar_text & 0xA0FFFFFF);
      }
    }
  }

  bool has_queue = !ctx.play_queue.empty();
  u32 skip_color = has_queue ? bar_text : (ctx.theme->text_dim & 0x60FFFFFF);
  u32 dim_color = ctx.theme->text_dim & 0x80FFFFFF;

  // Divider lines (4, below seek bar)
  C2D_DrawRectSolid(ZONE_1, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);
  C2D_DrawRectSolid(ZONE_2, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);
  C2D_DrawRectSolid(ZONE_3, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);
  C2D_DrawRectSolid(ZONE_4, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);

  float cy = BAR_Y + SEEK_H + (BAR_H - SEEK_H) / 2.0f; // center of button area

  // --- LOOP icon (0-55, center=27.5) ---
  {
    u32 lc = (ctx.play_queue.empty() || ctx.loop_mode == LOOP_OFF) ? dim_color
                                                                   : bar_text;
    float lx = 27.5f;
    draw_ui_icon(UiIconId::Loop, lx - 11.0f, cy - 11.0f, lc);

    if (ctx.loop_mode == LOOP_ONE) {
      C2D_TextParse(&text, buf, "1");
      C2D_DrawText(&text, C2D_WithColor, lx - 3, cy - 7, 0, FONT_SM, FONT_SM,
                   lc);
    }
  }

  // --- PREV icon (55-118, center=86.5): |◀ ---
  {
    float px = 86.5f;
    draw_ui_icon(UiIconId::Prev, px - 13.0f, cy - 9.0f, skip_color);
  }

  // --- PLAY/PAUSE icon (118-202, center=160) ---
  {
    float px = 160.0f;
    if (ctx.is_paused) {
      draw_ui_icon_centered(UiIconId::Play, px, cy, bar_text);
    } else {
      draw_ui_icon_centered(UiIconId::Pause, px, cy, bar_text);
    }
  }

  // --- NEXT icon (202-265, center=233.5): ▶| ---
  {
    float nx = 233.5f;
    draw_ui_icon(UiIconId::Next, nx - 12.0f, cy - 9.0f, skip_color);
  }

  // --- SHUF icon (265-320, center=292.5) ---
  // 32x32 pixel art, 1px per dot
  {
    u32 sc = has_queue ? (ctx.shuffle_mode ? bar_text : dim_color) : dim_color;
    draw_ui_icon_centered(UiIconId::Shuffle, 292.5f, cy, sc);
  }
}

inline std::string handle_touch(int tx, int ty, const RenderContext &ctx) {
  if (ty < (int)BAR_Y || ctx.playing_id.empty())
    return "";
  if (ty < (int)(BAR_Y + SEEK_H))
    return ""; // seek bar handled by caller
  if (tx < (int)ZONE_1)
    return ctx.play_queue.empty() ? "" : "toggle_loop";
  if (tx < (int)ZONE_2)
    return ctx.play_queue.empty() ? "" : "prev_track";
  if (tx < (int)ZONE_3)
    return "toggle_pause";
  if (tx < (int)ZONE_4)
    return ctx.play_queue.empty() ? "" : "next_track";
  return ctx.play_queue.empty() ? "" : "toggle_shuffle";
}
} // namespace PlayBar
