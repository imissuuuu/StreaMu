---
description: Non-obvious patterns and prohibitions discovered through actual 3DS implementation issues. MUST read during design (Step 2) and implementation (Step 3).
paths:
  - "3ds-music-player/source/**/*.cpp"
  - "3ds-music-player/source/**/*.h"
  - "3ds-music-player/include/**/*.h"
---

# Coding Patterns & Critical Rules

> This file records ONLY rules discovered through actual bugs or crashes during implementation/testing.
> Do not document anything derivable from reading the source. Append immediately when new issues are found.

---

## Architecture

1. **Keep ThemeColors as a pointer**
   Making `RenderContext::theme` a value member causes a freeze on `RenderContext render_ctx = ctx;` copy (verified). Keep it as `const ThemeColors* theme`.

2. **Destruction order (mandatory)**
   `screen_mgr.clear()` → `g_renderer_ptr.reset()` → `g_ui_mgr_ptr.reset()` → each `*Exit()`
   Wrong order causes double-access crash on hardware resources.

3. **playing_tracks vs g_tracks**
   PlayingScreen must use `playing_tracks`. `g_tracks` is overwritten on every search/browse. `play_queue` indices also reference `playing_tracks`.

4. **PlayingScreen is playlist-only**
   `trigger_playing` / `has_now_playing` are only valid when `active_playlist_id` is non-empty. Do not show PlayingScreen for single search-result playback.

---

## Threading & Synchronization

5. **Always use LightLock**
   All access to shared state (`ctx.g_tracks`, `ctx.playlists`, `ctx.playing_tracks`, etc.) must be guarded with `LightLock_Lock/Unlock`.

6. **std::atomic is prohibited**
   Unstable on ARMv6K / libctru. Use `LightLock` exclusively for thread synchronization.

---

## Input Handling

7. **Consume input to prevent double-processing**
   After a state transition, clear the key with `kDown &= ~KEY_A` to prevent same-frame double handling.

---

## Rendering

8. **Never cache text objects**
   Do not hold `C2D_Text` in static or persistent variables. Call `C2D_TextParse()` every frame.

9. **Scroll calculation**
   Use `calc_scroll(scroll_offset_y, item_h, scroll_items, y_shift)` and draw at `y_base = START_Y - y_shift`. Touch hit-testing must use the same `y_base`.

10. **Hamburger button position**
    Never hardcode coordinates. Always use `get_menu_btn_rect(ctx.config)`.

11. **Render target order matters**
    Calling `begin_top_screen()` → `begin_bottom_screen()` routes all subsequent draw commands to the **bottom screen**. To draw on top screen, issue draw commands after `begin_top_screen()` but before `begin_bottom_screen()` (verified: progress bar was drawn on bottom screen).

12. **Never use frame count for timeouts**
    Blocking calls like `check_connection()` make frame rate unstable, causing frame-based timeouts to drift from real time (verified: 15s setting became 44s). Use `osGetTime()` for real-time measurement.

---

## Network

13. **WiFi stops when lid is closed**
    `aptSetSleepAllowed(false)` prevents app suspension but not WiFi shutdown. Network requests (streaming, etc.) that fail should use retry logic to wait for WiFi recovery (verified: track skip while lid closed caused streaming failure → SERVER OFFLINE display).

18. **Direct audio paths must stream first-decodable bytes**
    Do not wait for full download/remux completion before starting playback on direct paths. Emit the first decodable audio pages/frames incrementally, and set `Playing` only from the player `has_started_playing()` signal, not from buffered byte count or server "served" logs (verified: Opus Ogg full remux completed but 3DS stayed silent).

---

## Build

14. **`make clean` required after any header struct/class field change**
    Incremental builds don't recompile dependent .o files, causing ABI mismatch. Always run `make clean && make` after adding/removing/reordering fields in **any** header file (verified: `ui_renderer.h`, `theme.h`, `play_bar.h`).
    Rule of thumb: if a `.h` file changes a struct/class layout, run `make clean`.

15. **3DS system font has no symbols outside ASCII range**
    Unicode symbols like `▶` (U+25B6) render as garbage (verified). Use ASCII-only characters in UI text (e.g., `▶` → `>`).

16. **Change UniqueId when renaming app title or icon**
    Without changing `UniqueId` in `app.rsf`, system caches (friend list, etc.) retain old title info (verified: persisted through CIA reinstall and FBI full delete, resolved only by UniqueId change). Increment UniqueId whenever app name or icon changes.

17. **Never declare same-named fields in AppContext that exist in RenderContext**
    `AppContext : public RenderContext` uses slicing copy (`RenderContext render_ctx = ctx`) for the render path. Any field declared in both classes causes the derived version to shadow the base — the sliced render snapshot always sees the base version (0/default), not the derived version. Stale `.o` files can hide this bug for months (verified: `touch_state` and `playback_start_time` shadow caused seek bar to freeze at 0 after `make clean`). If a new field is needed in AppContext that already exists in RenderContext, use the inherited field directly.
