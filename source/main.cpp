#include <3ds.h>
#include <citro2d.h>
#include <iomanip>
#include <malloc.h>
#include <sstream>
#include <stdio.h>
#include <string>
#include <cmath>
#include <time.h>
#include <vector>


#include "audio/mp3_player.h"
#include "network/youtube_api.h"
#include "playlist_manager.h"
#include "ui/ui_constants.h"
#include "ui/ui_manager.h"
#include "ui/ui_renderer.h"
#include <memory>


std::unique_ptr<std::vector<uint8_t>>
    g_stream_buffer_ptr; // Dynamic buffer for MP3Player
LightLock stream_lock;   // Mutex protecting stream_buffer access

#include "app_context.h"
#include "config_manager.h"
#include "ui/screen_manager.h"
#include "ui/wallpaper.h"
#include "ui/screens/home_screen.h"
#include "ui/screens/settings_screen.h"
#include "ui/screens/search_screen.h"
#include "ui/screens/playlists_screen.h"
#include "ui/screens/playlist_detail_screen.h"
#include "ui/screens/playing_screen.h"
#include <memory>

// Use smart pointers to ensure destructors run before socExit (prevents crash).
std::unique_ptr<MP3Player> g_player_ptr;
#define player (*g_player_ptr)

std::string url_encode(const std::string &value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;
  for (std::string::const_iterator i = value.begin(), n = value.end(); i != n;
       ++i) {
    std::string::value_type c = (*i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      escaped << c;
    } else if (c == ' ') {
      escaped << "%20";
    } else {
      escaped << std::uppercase << '%' << std::setw(2) << int((unsigned char)c)
              << std::nouppercase;
    }
  }
  return escaped.str();
}

static bool validate_ip_port(const std::string& input) {
  size_t colon = input.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon == input.size() - 1)
    return false;
  // Check port part is digits only
  for (size_t i = colon + 1; i < input.size(); ++i) {
    if (input[i] < '0' || input[i] > '9') return false;
  }
  // Check IP part contains at least one '.'
  std::string ip_part = input.substr(0, colon);
  if (ip_part.find('.') == std::string::npos) return false;
  return true;
}

static std::string show_ip_keyboard(const std::string& initial) {
  std::string current = initial;
  while (true) {
    SwkbdState swkbd;
    char buf[64] = {0};
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, 63);
    if (!current.empty())
      swkbdSetInitialText(&swkbd, current.c_str());
    SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));
    if (button != SWKBD_BUTTON_CONFIRM || strlen(buf) == 0)
      return "";
    std::string input(buf);
    if (validate_ip_port(input))
      return input;
    // Validation failed: keep input and retry
    current = input;
  }
}

ThemeColors g_theme_colors;
std::unique_ptr<AppContext> g_ctx_ptr;

#define ctx (*g_ctx_ptr)

std::unique_ptr<PlaylistManager> g_playlist_manager_ptr;
#define playlist_manager (*g_playlist_manager_ptr)

C2D_TextBuf g_staticBuf; // Temporarily kept for compatibility

void update_playing_title_lines(C2D_TextBuf buf) {
  ctx.playing_title_lines.clear();
  if (ctx.playing_title.empty())
    return;

  // ---- Line 1: longest prefix that fits within 360px ----
  size_t pos = 0;
  {
    size_t best_len = 1, len = 1;
    while (pos + len <= ctx.playing_title.length()) {
      while (pos + len < ctx.playing_title.length() &&
             (ctx.playing_title[pos + len] & 0xC0) == 0x80)
        len++;
      std::string test_str = ctx.playing_title.substr(pos, len);
      C2D_Text test_text;
      C2D_TextBufClear(buf);
      C2D_TextParse(&test_text, buf, test_str.c_str());
      float w = 0, h = 0;
      C2D_TextGetDimensions(&test_text, 0.50f, 0.50f, &w, &h);
      if (w > 360.0f) break;
      best_len = len;
      if (pos + len >= ctx.playing_title.length()) break;
      len++;
    }
    ctx.playing_title_lines.push_back(ctx.playing_title.substr(pos, best_len));
    pos += best_len;
  }

  // ---- Line 2 (last line): fit remainder, truncate with "..." if needed ----
  if (pos < ctx.playing_title.length()) {
    std::string remaining = ctx.playing_title.substr(pos);
    C2D_Text test_text;
    C2D_TextBufClear(buf);
    C2D_TextParse(&test_text, buf, remaining.c_str());
    float w = 0, h = 0;
    C2D_TextGetDimensions(&test_text, 0.50f, 0.50f, &w, &h);
    if (w <= 360.0f) {
      ctx.playing_title_lines.push_back(remaining);
    } else {
      // Find longest prefix where "prefix..." fits within 360px
      size_t best_len = 0, len = 1;
      while (len <= remaining.length()) {
        while (len < remaining.length() &&
               (remaining[len] & 0xC0) == 0x80)
          len++;
        std::string candidate = remaining.substr(0, len) + "...";
        C2D_TextBufClear(buf);
        C2D_TextParse(&test_text, buf, candidate.c_str());
        float cw = 0, ch = 0;
        C2D_TextGetDimensions(&test_text, 0.50f, 0.50f, &cw, &ch);
        if (cw > 360.0f) break;
        best_len = len;
        if (len >= remaining.length()) break;
        len++;
      }
      ctx.playing_title_lines.push_back(
          remaining.substr(0, best_len) + "...");
    }
  }
}

void download_thread(void *arg) {
  YouTubeAPI *api = (YouTubeAPI *)arg;
  while (ctx.is_running) {
    std::string q = "";
    std::string lang = "en";
    LightLock_Lock(&ctx.lock);
    if (ctx.search_query != "") {
      q = ctx.search_query;
      lang = ctx.config.language;
      ctx.g_status_msg = "Fetching Results...";
      ctx.search_query = "";
    }
    LightLock_Unlock(&ctx.lock);

    if (q != "") {
      api->search(q, lang, [&](const std::vector<Track> &res, bool ok) {
        bool is_online = true;
        if ((!ok || res.empty()) && !YouTubeAPI::should_cancel) {
          is_online = api->check_connection();
        }

        LightLock_Lock(&ctx.lock);
        if (ok && !res.empty()) {
          ctx.g_tracks = res;
          ctx.g_status_msg = "";
          ctx.is_server_connected = true; // Mark online on success
        } else {
          ctx.g_status_msg = is_online ? "No Results" : "Offline";
          ctx.is_server_connected = is_online;
        }
        LightLock_Unlock(&ctx.lock);
      });
    }

    std::string stream_url = "";
    LightLock_Lock(&ctx.lock);
    if (ctx.is_downloading && ctx.current_stream_url != "") {
      stream_url = ctx.current_stream_url;
      ctx.current_stream_url = "";
    }
    LightLock_Unlock(&ctx.lock);

    if (stream_url != "") {
      bool success = api->start_streaming(stream_url);

      // Retry only if server is reachable (e.g. transient extract error)
      if (!success && !YouTubeAPI::should_cancel) {
        if (api->check_connection()) {
          svcSleepThread(2000ULL * 1000 * 1000); // 2s wait
          if (!YouTubeAPI::should_cancel) {
            success = api->start_streaming(stream_url);
          }
        }
      }

      LightLock_Lock(&ctx.lock);
      ctx.is_downloading = false;
      // Don't change connection state if failure was due to cancellation
      if (!YouTubeAPI::should_cancel) {
        bool is_online = success ? true : api->check_connection();
        ctx.is_server_connected = is_online;
        if (!success) {
          ctx.g_status_msg = is_online ? "Extract Error (Check Proxy)"
                                       : "Stream Error (Offline?)";
          MP3Player::is_playing = false;
          ctx.playing_id = "";
        }
      }
      LightLock_Unlock(&ctx.lock);
    }
    svcSleepThread(50 * 1000 * 1000);
  }
}

// Draw a loading progress bar on the top screen.
// step: current step (1-4), total: 4
// label: text shown below the bar
// pulse_frame: if >= 0, animates the bar tip for indeterminate progress (step 4)
void draw_loading_screen(UIManager& ui_mgr, C2D_TextBuf buf,
                         const ThemeColors* theme,
                         int step, int total, const char* label,
                         int pulse_frame = -1) {
  ui_mgr.begin_top_screen(theme->bg_top);
  C2D_TextBufClear(buf);

  const float screen_w = 400.0f;
  const float bar_w = 240.0f;
  const float bar_h = 16.0f;
  const float bar_x = (screen_w - bar_w) / 2.0f; // = 80
  const float bar_y = 110.0f;

  // Background bar (dark gray)
  C2D_DrawRectSolid(bar_x, bar_y, 0, bar_w, bar_h, C2D_Color32(60, 60, 60, 255));

  // Filled portion
  float fill_w;
  if (pulse_frame < 0) {
    // Determinate: show completed steps
    fill_w = bar_w * static_cast<float>(step) / total;
  } else {
    // Indeterminate (step 4): show completed steps + pulsing segment
    fill_w = bar_w * static_cast<float>(step - 1) / total;
  }
  if (fill_w > 0) {
    C2D_DrawRectSolid(bar_x, bar_y, 0, fill_w, bar_h,
                      C2D_Color32(50, 120, 200, 255));
  }

  // Pulse animation for indeterminate step (bar extends up to 50% of segment)
  if (pulse_frame >= 0) {
    float base_fill = bar_w * static_cast<float>(step - 1) / total;
    float segment_w = bar_w / total;
    float t = 0.5f + 0.5f * sinf(pulse_frame * 0.05f);
    float pulse_w = segment_w * 0.5f * t; // max 50% of segment
    C2D_DrawRectSolid(bar_x + base_fill, bar_y, 0, pulse_w, bar_h,
                      C2D_Color32(50, 120, 200, 255));
  }

  // Bar border (outline)
  C2D_DrawRectSolid(bar_x, bar_y, 0, bar_w, 1, C2D_Color32(120, 120, 120, 255));
  C2D_DrawRectSolid(bar_x, bar_y + bar_h - 1, 0, bar_w, 1, C2D_Color32(120, 120, 120, 255));
  C2D_DrawRectSolid(bar_x, bar_y, 0, 1, bar_h, C2D_Color32(120, 120, 120, 255));
  C2D_DrawRectSolid(bar_x + bar_w - 1, bar_y, 0, 1, bar_h, C2D_Color32(120, 120, 120, 255));

  // Step text below bar (centered)
  C2D_Text text;
  char step_text[64];
  snprintf(step_text, sizeof(step_text), "Step %d/%d: %s", step, total, label);
  C2D_TextParse(&text, buf, step_text);
  C2D_TextOptimize(&text);
  float text_w = text.width * 0.5f; // scale 0.5
  float text_x = 200.0f - text_w / 2.0f; // center on screen (400/2=200)
  C2D_DrawText(&text, C2D_AtBaseline | C2D_WithColor,
               text_x, bar_y + bar_h + 20, 0, 0.5f, 0.5f,
               C2D_Color32(180, 180, 180, 255));

  ui_mgr.begin_bottom_screen(theme->bg_bottom);
  ui_mgr.end_frame();
}

int main(int argc, char *argv[]) {
  // Dynamic allocation (RAII) to ensure destructors run before OS exit (socExit etc.)
  g_ctx_ptr = std::make_unique<AppContext>();
  g_player_ptr = std::make_unique<MP3Player>();
  g_playlist_manager_ptr = std::make_unique<PlaylistManager>();
  g_stream_buffer_ptr = std::make_unique<std::vector<uint8_t>>();

  LightLock_Init(&ctx.lock);
  LightLock_Init(&stream_lock);

  srand(time(NULL));

  // Dynamically allocated for manual destruction before system exit
  auto g_ui_mgr_ptr = std::make_unique<UIManager>();
  UIManager &ui_mgr = *g_ui_mgr_ptr;
  ui_mgr.init();

  // Load and apply theme settings
  ConfigManager::load(ctx.config);
  apply_theme(ctx.config, g_theme_colors);
  ctx.theme = &g_theme_colors;
  auto g_renderer_ptr = std::make_unique<UIRenderer>(ui_mgr, g_theme_colors);
  UIRenderer &renderer = *g_renderer_ptr;

  // Load wallpaper texture
  Wallpaper g_wallpaper;
  if (!ctx.config.wallpaper_file.empty()) {
    std::string wp_path = std::string("sdmc:/3ds/StreaMu/wallpaper/") + ctx.config.wallpaper_file;
    g_wallpaper.load(wp_path);
  }
  renderer.set_wallpaper(&g_wallpaper);

  g_staticBuf = ui_mgr.get_text_buf();

  // --- Step 1/4: System Init ---
  draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 1, 4, "Initializing system...");
  svcSleepThread(300000000LL); // 0.3s minimum display

  aptSetSleepAllowed(false);
  osSetSpeedupEnable(true); // Enable New 3DS 804MHz CPU + L2 cache (no-op on Old 3DS)
  romfsInit();
  ndspInit();
  ndmuInit();
  NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
  player.init();
  ptmuInit();
  u32 *soc_buffer = (u32 *)memalign(0x1000, 0x100000);
  if (soc_buffer)
    socInit(soc_buffer, 0x100000);

  auto g_api_ptr = std::make_unique<YouTubeAPI>();
  YouTubeAPI &api = *g_api_ptr;
  api.init();

  // --- Step 2/4: Config loaded ---
  draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 2, 4, "Loading config...");
  svcSleepThread(300000000LL); // 0.3s minimum display

  playlist_manager.init();
  ctx.playlists = playlist_manager.get_playlists();

  // --- Step 3/4: Playlists loaded ---
  draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 3, 4, "Loading playlists...");
  svcSleepThread(500000000LL); // 0.5s minimum display

  // UI Architecture Phase 1: ScreenManager Setup
  ScreenManager screen_mgr;
  screen_mgr.add_screen("HomeScreen", std::make_unique<HomeScreen>());
  screen_mgr.add_screen("SettingsScreen",
                        std::make_unique<SettingsScreen>(g_theme_colors, &g_wallpaper));
  screen_mgr.add_screen("SearchScreen",
                        std::make_unique<SearchScreen>());
  screen_mgr.add_screen("PlaylistsScreen",
                        std::make_unique<PlaylistsScreen>());
  screen_mgr.add_screen("PlaylistDetailScreen",
                        std::make_unique<PlaylistDetailScreen>());
  screen_mgr.add_screen("PlayingScreen",
                        std::make_unique<PlayingScreen>());
  screen_mgr.change_screen(ctx, "HomeScreen");

  // Server IP setup (prompt with swkbd if not set)
  if (ctx.config.server_ip.empty()) {
    // Show IP guidance on top screen before swkbd
    ui_mgr.begin_top_screen(ctx.theme->bg_top);
    C2D_TextBufClear(g_staticBuf);
    C2D_Text text;
    C2D_TextParse(&text, g_staticBuf, "Enter the IP address of the PC\nrunning proxy.py\n(e.g. 192.168.1.10)");
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_AlignCenter, 200.0f, 100.0f, 0.5f, 0.55f, 0.55f);
    ui_mgr.end_frame();

    std::string ip = show_ip_keyboard("");
    if (!ip.empty()) {
      ctx.config.server_ip = ip;
      ConfigManager::save(ctx.config);
    }
  }
  if (!ctx.config.server_ip.empty()) {
    api.set_server_ip(ctx.config.server_ip);
  }

  // --- Step 4/4: Server connection check ---
  ctx.is_server_connected = api.check_connection();
  int anim_counter = 0;
  bool is_timeout = false;
  int timeout_selected_index = 0;
  bool is_confirming_exit = false;
  int confirm_selected_index = 0;
  u64 connect_start_ms = osGetTime();
  u64 last_check_ms = connect_start_ms;

  while (aptMainLoop() && !ctx.is_server_connected) {
    if (!is_timeout) {
      // Progress bar with pulse animation
      draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 4, 4,
                          "Connecting to server...", anim_counter);
    } else {
      // Timeout error menu (top screen)
      ui_mgr.begin_top_screen(ctx.theme->bg_top);
      C2D_TextBufClear(g_staticBuf);
      C2D_Text text;

      if (!is_confirming_exit) {
        C2D_TextParse(&text, g_staticBuf, "Connection Error (Timeout)");
        C2D_DrawText(&text, C2D_AtBaseline | C2D_WithColor, 80, 90, 0, 0.7f,
                     0.7f, C2D_Color32(255, 80, 80, 255));

        const char *labels[] = {"Retry", "Change IP", "Exit"};
        for (int i = 0; i < 3; ++i) {
          char line[32];
          snprintf(line, sizeof(line), "%s %s", (timeout_selected_index == i) ? ">" : " ", labels[i]);
          C2D_TextParse(&text, g_staticBuf, line);
          C2D_DrawText(
              &text, C2D_AtBaseline | C2D_WithColor, 130, 130.0f + i * 30.0f, 0, 0.7f, 0.7f,
              (timeout_selected_index == i) ? C2D_Color32(50, 200, 50, 255)
                                            : C2D_Color32(200, 200, 200, 255));
        }
      } else {
        C2D_TextParse(&text, g_staticBuf, "Are you sure you want to exit?");
        C2D_DrawText(&text, C2D_AtBaseline | C2D_WithColor, 60, 90, 0, 0.65f,
                     0.65f, C2D_Color32(60, 60, 60, 255));

        const char *c1 = (confirm_selected_index == 0) ? "[ NO ]" : "  NO  ";
        const char *c2 = (confirm_selected_index == 1) ? "[ YES ]" : "  YES  ";

        C2D_TextParse(&text, g_staticBuf, c1);
        C2D_DrawText(
            &text, C2D_AtBaseline | C2D_WithColor, 110, 140, 0, 0.7f, 0.7f,
            (confirm_selected_index == 0) ? C2D_Color32(50, 120, 200, 255)
                                          : C2D_Color32(150, 150, 150, 255));

        C2D_TextParse(&text, g_staticBuf, c2);
        C2D_DrawText(
            &text, C2D_AtBaseline | C2D_WithColor, 230, 140, 0, 0.7f, 0.7f,
            (confirm_selected_index == 1) ? C2D_Color32(200, 50, 50, 255)
                                          : C2D_Color32(150, 150, 150, 255));

        C2D_TextParse(&text, g_staticBuf, "Press A to select");
        C2D_DrawText(&text, C2D_AtBaseline | C2D_WithColor, 120, 190, 0, 0.5f,
                     0.5f, C2D_Color32(120, 120, 120, 255));
      }
      ui_mgr.begin_bottom_screen(ctx.theme->bg_bottom);
      ui_mgr.end_frame();
    }

    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kRepeat = hidKeysDownRepeat();

    if (is_timeout) {
      if (!is_confirming_exit) {
        if (kRepeat & KEY_DDOWN)
          timeout_selected_index = (timeout_selected_index + 1) % 3;
        if (kRepeat & KEY_DUP)
          timeout_selected_index = (timeout_selected_index + 2) % 3;

        if (kDown & KEY_A) {
          if (timeout_selected_index == 0) {  // Retry
            is_timeout = false;
            connect_start_ms = osGetTime();
            last_check_ms = connect_start_ms;
          } else if (timeout_selected_index == 1) {  // Change IP
            std::string ip = show_ip_keyboard(ctx.config.server_ip);
            if (!ip.empty()) {
              ctx.config.server_ip = ip;
              ConfigManager::save(ctx.config);
              api.set_server_ip(ctx.config.server_ip);
            }
            is_timeout = false;
            connect_start_ms = osGetTime();
            last_check_ms = connect_start_ms;
          } else {  // Exit
            is_confirming_exit = true;
            confirm_selected_index = 0;
          }
        }
      } else {
        if (kRepeat & KEY_DRIGHT)
          confirm_selected_index = 1;
        if (kRepeat & KEY_DLEFT)
          confirm_selected_index = 0;

        if (kDown & KEY_A) {
          if (confirm_selected_index == 1) {
            ctx.is_running = false;
            break;
          } else
            is_confirming_exit = false;
        }
      }
    } else {
      anim_counter++;
      u64 now_ms = osGetTime();
      if (now_ms - connect_start_ms >= 15000) { // 15 seconds (real time)
        is_timeout = true;
      } else if (now_ms - last_check_ms >= 500) { // Check every 0.5s (real time)
        last_check_ms = now_ms;
        ctx.is_server_connected = api.check_connection();
      }
    }
    svcSleepThread(16666666); // ~60fps
  }

  Thread threadId = nullptr;
  if (ctx.is_running) {
    // 6th arg = false (Attached) so we can reliably join on exit
    threadId = threadCreate(download_thread, &api, 0x10000, 0x3F, -2, false);
  }

  auto start_playback = [&](const Track &track) {
    // --- Fully stop and discard previous playback ---
    YouTubeAPI::should_cancel = true; // Signal download thread to stop
    { // Safely read is_downloading under lock
      bool still_dl;
      do {
        LightLock_Lock(&ctx.lock);
        still_dl = ctx.is_downloading;
        LightLock_Unlock(&ctx.lock);
        if (still_dl) svcSleepThread(10 * 1000 * 1000); // Wait until fully stopped
      } while (still_dl);
    }

    ctx.is_paused = false;
    ndspChnSetPaused(0, false);
    player.stop(); // Stop audio playback and flush hardware buffers

    LightLock_Lock(&stream_lock);
    if (g_stream_buffer_ptr)
      g_stream_buffer_ptr->clear(); // Discard buffer
    LightLock_Unlock(&stream_lock);

    YouTubeAPI::should_cancel = false; // Clear cancel flag

    // --- Set new track info ---
    LightLock_Lock(&ctx.lock);
    ctx.playing_id       = track.id;
    ctx.playing_title    = track.title;
    ctx.playing_duration = track.duration;

    // Build metadata line
    std::string meta = "";
    if (!track.duration.empty() && track.duration != "?")
      meta += track.duration;
    if (!track.views.empty() && track.views != "?") {
      if (!meta.empty())
        meta += " ";
      meta += track.views;
    }
    if (!track.upload_date.empty() && track.upload_date != "?") {
      if (!meta.empty())
        meta += " ";
      meta += track.upload_date;
    }
    ctx.playing_meta = meta;

    update_playing_title_lines(ui_mgr.get_text_buf());
    MP3Player::is_playing = true;
    ctx.g_status_msg = "Buffering...";
    LightLock_Unlock(&ctx.lock);

    api.get_audio_stream_url(ctx.playing_id, [&](const std::string &url,
                                                 bool ok) {
      LightLock_Lock(&ctx.lock);
      if (ok && !url.empty()) {
        ctx.current_stream_url = url;
        ctx.is_downloading = true;
      } else {
        ctx.g_status_msg = "Stream Error";
        MP3Player::is_playing = false;

        // On error during playlist playback, auto-skip to next track.
        // Keep is_playing=true so the track-finished detection triggers auto-next.
        if (!ctx.play_queue.empty() && ctx.current_track_idx >= 0 &&
            ctx.current_track_idx < (int)ctx.play_queue.size() - 1) {
          MP3Player::is_playing = true; // Trick finish-detection into triggering auto-next
        } else {
          ctx.playing_id = "";
        }
      }
      LightLock_Unlock(&ctx.lock);
    });
  };

  while (aptMainLoop()) {
    if (!ctx.is_running)
      break; // Exit immediately if START pressed right after boot

    // === Auto-advance to next track (track end detection) ===
    LightLock_Lock(&ctx.lock);
    bool should_auto_next = false;

    player.set_downloading_status(ctx.is_downloading);

    if (MP3Player::is_playing && !ctx.is_downloading &&
        player.is_track_finished()) {
      if (!ctx.play_queue.empty()) {
        // Auto-advance: loop when reaching the end (per user request)
        should_auto_next = true;
      } else {
        // End of playlist or single track
        MP3Player::is_playing = false;
        ctx.g_status_msg = "";
        ctx.playing_id = "";
        ctx.current_track_idx = -1;
      }
    }
    LightLock_Unlock(&ctx.lock);

    if (should_auto_next) {
      svcSleepThread(300 * 1000 *
                     1000); // Brief 0.3s gap between tracks
      LightLock_Lock(&ctx.lock);
      if (ctx.loop_mode == LOOP_ONE) {
        // Replay the same track
        if (ctx.current_track_idx >= 0 && ctx.current_track_idx < (int)ctx.play_queue.size()) {
          int cur_idx = ctx.play_queue[ctx.current_track_idx];
          if (cur_idx >= 0 && cur_idx < (int)ctx.playing_tracks.size()) {
            Track cur_track = ctx.playing_tracks[cur_idx];
            LightLock_Unlock(&ctx.lock);
            start_playback(cur_track);
          } else {
            MP3Player::is_playing = false;
            ctx.playing_id = "";
            LightLock_Unlock(&ctx.lock);
          }
        } else {
          MP3Player::is_playing = false;
          ctx.playing_id = "";
          LightLock_Unlock(&ctx.lock);
        }
      } else {
        ctx.current_track_idx++;
        if (ctx.current_track_idx >= (int)ctx.play_queue.size()) {
          if (ctx.loop_mode == LOOP_ALL) {
            ctx.current_track_idx = 0;
          } else {
            // LOOP_OFF: stop playback
            MP3Player::is_playing = false;
            ctx.g_status_msg = "";
            ctx.playing_id = "";
            ctx.current_track_idx = -1;
            LightLock_Unlock(&ctx.lock);
            goto auto_next_done;
          }
        }
        int next_idx = ctx.play_queue[ctx.current_track_idx];
        if (next_idx < 0 || next_idx >= (int)ctx.playing_tracks.size()) {
          ctx.play_queue.clear();
          ctx.current_track_idx = -1;
          MP3Player::is_playing = false;
          LightLock_Unlock(&ctx.lock);
        } else {
          Track next_track = ctx.playing_tracks[next_idx];
          ctx.selected_index = next_idx;
          LightLock_Unlock(&ctx.lock);
          start_playback(next_track);
        }
      }
      auto_next_done:;
    }

    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();
    u32 kRepeat = hidKeysDownRepeat();

    // D-pad held repeat (respects sensitivity setting)
    {
      static const u32 delays[]    = {90, 75, 62, 52, 43, 35, 28, 22, 15, 8};
      static const u32 intervals[] = {35, 28, 22, 17, 13, 10,  7,  5,  4, 3};
      int spd_idx = ctx.config.dpad_speed - 1;
      if (spd_idx < 0) spd_idx = 0;
      if (spd_idx > 9) spd_idx = 9;
      u32 rep_delay    = delays[spd_idx];
      u32 rep_interval = intervals[spd_idx];

      static u32 s_dpad_held = 0;
      u32 dpad_bits = kHeld & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
      if (dpad_bits) { s_dpad_held++; } else { s_dpad_held = 0; }
      if (s_dpad_held > rep_delay &&
          (s_dpad_held - rep_delay) % rep_interval == 0) {
        kRepeat |= dpad_bits;
      }
    }

    // L/R button handling (customizable in settings)
    // R button
    if (kDown & KEY_R) {
      if (ctx.config.r_action == LR_SKIP) {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          ctx.current_track_idx++;
          if (ctx.current_track_idx >= (int)ctx.play_queue.size()) {
            ctx.current_track_idx = 0;
          }
          int next_idx = ctx.play_queue[ctx.current_track_idx];
          Track next_track = ctx.playing_tracks[next_idx];
          LightLock_Unlock(&ctx.lock);
          start_playback(next_track);
          svcSleepThread(500 * 1000 * 1000);
        } else {
          LightLock_Unlock(&ctx.lock);
        }
      } else if (ctx.config.r_action == LR_PLAY_PAUSE) {
        if (ctx.playing_id != "") {
          ctx.is_paused = !ctx.is_paused;
          ndspChnSetPaused(0, ctx.is_paused);
          ctx.g_status_msg = ctx.is_paused ? "Paused" : "Playing";
        }
      }
      // LR_DISABLED: do nothing
    }
    // L button
    if (kDown & KEY_L) {
      if (ctx.config.l_action == LR_SKIP) {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          ctx.current_track_idx--;
          if (ctx.current_track_idx < 0) {
            ctx.current_track_idx = (int)ctx.play_queue.size() - 1;
          }
          int prev_idx = ctx.play_queue[ctx.current_track_idx];
          Track prev_track = ctx.playing_tracks[prev_idx];
          LightLock_Unlock(&ctx.lock);
          start_playback(prev_track);
          svcSleepThread(500 * 1000 * 1000);
        } else {
          LightLock_Unlock(&ctx.lock);
        }
      } else if (ctx.config.l_action == LR_PLAY_PAUSE) {
        if (ctx.playing_id != "") {
          ctx.is_paused = !ctx.is_paused;
          ndspChnSetPaused(0, ctx.is_paused);
          ctx.g_status_msg = ctx.is_paused ? "Paused" : "Playing";
        }
      }
    }

    if (kDown & KEY_START) {
      LightLock_Lock(&ctx.lock);
      if (ctx.current_state != STATE_EXIT_CONFIRM) {
        ctx.previous_state = ctx.current_state;
        ctx.current_state = STATE_EXIT_CONFIRM;
        ctx.popup_selected_index = 0;
      }
      LightLock_Unlock(&ctx.lock);
    }

    if (kDown & KEY_SELECT) {
      LightLock_Lock(&ctx.lock);
      if (ctx.current_state == STATE_SEARCH) {
        if (!ctx.g_tracks.empty() &&
            ctx.g_tracks[ctx.selected_index].id != "SEARCH_BTN") {
          ctx.previous_state = ctx.current_state;
          ctx.current_state = STATE_POPUP_TRACK_OPTIONS;
          ctx.popup_selected_index = 0;
          ctx.selected_track_id = ctx.g_tracks[ctx.selected_index].id;
        }
      } else if (ctx.current_state == STATE_PLAYLISTS) {
        if (!ctx.playlists.empty()) {
          ctx.previous_state = ctx.current_state;
          ctx.current_state = STATE_POPUP_PLAYLIST_OPTIONS;
          ctx.popup_selected_index = 0;
          ctx.selected_playlist_id = ctx.playlists[ctx.selected_index].id;
        }
      } else if (ctx.current_state == STATE_PLAYLIST_DETAIL) {
        if (!ctx.g_tracks.empty() &&
            ctx.g_tracks[ctx.selected_index].id != "MODE_BTN") {
          ctx.previous_state = ctx.current_state;
          ctx.current_state = STATE_POPUP_TRACK_OPTIONS;
          ctx.popup_selected_index = 0;
          ctx.selected_track_id = ctx.g_tracks[ctx.selected_index].id;
        }
      } else if (ctx.current_state == STATE_PLAYING_UI) {
        if (!ctx.playing_tracks.empty() &&
            ctx.selected_index >= 0 && ctx.selected_index < (int)ctx.playing_tracks.size()) {
          ctx.previous_state = ctx.current_state;
          ctx.current_state = STATE_POPUP_TRACK_OPTIONS;
          ctx.popup_selected_index = 0;
          ctx.selected_track_id = ctx.playing_tracks[ctx.selected_index].id;
        }
      }
      LightLock_Unlock(&ctx.lock);
    }

    if (kDown & KEY_B) {
      LightLock_Lock(&ctx.lock);
      bool was_popup = (ctx.current_state == STATE_POPUP_PLAYLIST_ADD ||
                        ctx.current_state == STATE_POPUP_PLAYLIST_OPTIONS ||
                        ctx.current_state == STATE_POPUP_TRACK_OPTIONS ||
                        ctx.current_state == STATE_POPUP_TRACK_DETAILS ||
                        ctx.current_state == STATE_EXIT_CONFIRM ||
                        ctx.current_state == STATE_POPUP_NAV ||
                        ctx.current_state == STATE_POPUP_QA_ADD ||
                        ctx.current_state == STATE_POPUP_QA_REMOVE);
      if (was_popup) {
        ctx.current_state = ctx.previous_state;
      }
      LightLock_Unlock(&ctx.lock);
      if (!was_popup) {
        if (ctx.current_state == STATE_SEARCH ||
            ctx.current_state == STATE_PLAYLISTS) {
          screen_mgr.change_screen(ctx, "HomeScreen");
        } else if (ctx.current_state == STATE_PLAYLIST_DETAIL) {
          screen_mgr.change_screen(ctx, "PlaylistsScreen");
        } else if (ctx.current_state == STATE_PLAYING_UI) {
          screen_mgr.change_screen(ctx, "HomeScreen");
        }
      }
    }

    if (kDown & KEY_X) {
      if (ctx.playing_id != "") {
        ctx.is_paused = !ctx.is_paused;
        ndspChnSetPaused(0, ctx.is_paused);
        ctx.g_status_msg = ctx.is_paused ? "Paused" : "Playing";
      }
    }

    if (ctx.current_state == STATE_HOME ||
        ctx.current_state == STATE_SEARCH ||
        ctx.current_state == STATE_PLAYLISTS ||
        ctx.current_state == STATE_PLAYLIST_DETAIL ||
        ctx.current_state == STATE_SETTINGS ||
        ctx.current_state == STATE_PLAYING_UI) {
      std::string action = screen_mgr.update(ctx, kDown, kHeld, kRepeat);
      if (action == "trigger_search") {
        kDown |= KEY_Y;
      } else if (action == "trigger_playlists") {
        screen_mgr.change_screen(ctx, "PlaylistsScreen");
        kDown &= ~KEY_A;
      } else if (action == "trigger_settings") {
        screen_mgr.change_screen(ctx, "SettingsScreen");
        kDown &= ~KEY_A;
      } else if (action == "trigger_playing") {
        if (!ctx.active_playlist_id.empty()) {
          screen_mgr.change_screen(ctx, "PlayingScreen");
        } else {
          // Search track (playing or finished) -> SearchScreen
          screen_mgr.change_screen(ctx, "SearchScreen");
        }
        kDown &= ~KEY_A;
      } else if (action == "qa_add_popup") {
        if (!ctx.playlists.empty()) {
          ctx.previous_state = ctx.current_state;
          ctx.current_state = STATE_POPUP_QA_ADD;
          ctx.popup_selected_index = 0;
        }
        kDown &= ~KEY_A;
      } else if (action == "qa_remove_popup") {
        ctx.previous_state = ctx.current_state;
        ctx.current_state = STATE_POPUP_QA_REMOVE;
        ctx.popup_selected_index = 0;
        kDown &= ~KEY_A;
      } else if (action == "open_qa_playlist") {
        // Quick Access slot tap -> navigate to playlist detail
        LightLock_Lock(&ctx.lock);
        bool found = false;
        for (const auto& pl : ctx.playlists) {
          if (pl.id == ctx.selected_playlist_id) {
            ctx.g_tracks       = pl.tracks;
            ctx.selected_index = 0;
            found = true;
            break;
          }
        }
        LightLock_Unlock(&ctx.lock);
        if (found) {
          if (!ctx.active_playlist_id.empty() &&
              ctx.selected_playlist_id == ctx.active_playlist_id) {
            screen_mgr.change_screen(ctx, "PlayingScreen");
          } else {
            screen_mgr.change_screen(ctx, "PlaylistDetailScreen");
          }
        }
        kDown &= ~KEY_A;
      } else if (action == "trigger_home") {
        screen_mgr.change_screen(ctx, "HomeScreen");
      } else if (action == "trigger_search_keyboard") {
        kDown |= KEY_Y;
      } else if (action == "start_playback") {
        start_playback(ctx.g_tracks[ctx.selected_index]);
        LightLock_Lock(&ctx.lock);
        ctx.active_playlist_id   = "";
        ctx.active_playlist_name = "";
        ctx.play_queue.clear();
        ctx.current_track_idx = -1;
        LightLock_Unlock(&ctx.lock);
      } else if (action == "toggle_pause") {
        if (!ctx.playing_id.empty()) {
          ctx.is_paused = !ctx.is_paused;
          ndspChnSetPaused(0, ctx.is_paused);
          ctx.g_status_msg = ctx.is_paused ? "Paused" : "Playing";
        }
      } else if (action == "prev_track") {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          if (ctx.loop_mode == LOOP_ONE) {
            // Replay same track
            if (ctx.current_track_idx >= 0 && ctx.current_track_idx < (int)ctx.play_queue.size()) {
              int cur_idx = ctx.play_queue[ctx.current_track_idx];
              Track cur_track = ctx.playing_tracks[cur_idx];
              LightLock_Unlock(&ctx.lock);
              start_playback(cur_track);
            } else {
              LightLock_Unlock(&ctx.lock);
            }
          } else {
            ctx.current_track_idx--;
            if (ctx.current_track_idx < 0) {
              if (ctx.loop_mode == LOOP_ALL) {
                ctx.current_track_idx = (int)ctx.play_queue.size() - 1;
              } else {
                ctx.current_track_idx = 0;
                LightLock_Unlock(&ctx.lock);
                goto prev_done;
              }
            }
            int prev_idx = ctx.play_queue[ctx.current_track_idx];
            Track prev_track = ctx.playing_tracks[prev_idx];
            LightLock_Unlock(&ctx.lock);
            start_playback(prev_track);
          }
        } else {
          LightLock_Unlock(&ctx.lock);
        }
        prev_done:;
      } else if (action == "next_track") {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          if (ctx.loop_mode == LOOP_ONE) {
            if (ctx.current_track_idx >= 0 && ctx.current_track_idx < (int)ctx.play_queue.size()) {
              int cur_idx = ctx.play_queue[ctx.current_track_idx];
              Track cur_track = ctx.playing_tracks[cur_idx];
              LightLock_Unlock(&ctx.lock);
              start_playback(cur_track);
            } else {
              LightLock_Unlock(&ctx.lock);
            }
          } else {
            ctx.current_track_idx++;
            if (ctx.current_track_idx >= (int)ctx.play_queue.size()) {
              if (ctx.loop_mode == LOOP_ALL) {
                ctx.current_track_idx = 0;
              } else {
                ctx.current_track_idx = (int)ctx.play_queue.size() - 1;
                LightLock_Unlock(&ctx.lock);
                goto next_done;
              }
            }
            int next_idx = ctx.play_queue[ctx.current_track_idx];
            Track next_track = ctx.playing_tracks[next_idx];
            LightLock_Unlock(&ctx.lock);
            start_playback(next_track);
          }
        } else {
          LightLock_Unlock(&ctx.lock);
        }
        next_done:;
      } else if (action == "toggle_loop") {
        if (ctx.loop_mode == LOOP_OFF) ctx.loop_mode = LOOP_ALL;
        else if (ctx.loop_mode == LOOP_ALL) ctx.loop_mode = LOOP_ONE;
        else ctx.loop_mode = LOOP_OFF;
      } else if (action == "toggle_shuffle") {
        ctx.shuffle_mode = !ctx.shuffle_mode;
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          if (ctx.shuffle_mode) {
            // Shuffle: place current track at front
            int cur_playing = (ctx.current_track_idx >= 0 && ctx.current_track_idx < (int)ctx.play_queue.size())
                              ? ctx.play_queue[ctx.current_track_idx] : -1;
            ctx.play_queue.clear();
            for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
              ctx.play_queue.push_back(i);
            for (size_t i = ctx.play_queue.size() - 1; i > 0; --i) {
              size_t j = rand() % (i + 1);
              std::swap(ctx.play_queue[i], ctx.play_queue[j]);
            }
            if (cur_playing >= 0) {
              for (size_t i = 0; i < ctx.play_queue.size(); ++i) {
                if (ctx.play_queue[i] == cur_playing) {
                  std::swap(ctx.play_queue[0], ctx.play_queue[i]);
                  break;
                }
              }
              ctx.current_track_idx = 0;
            }
          } else {
            // Restore sequential order
            int cur_playing = (ctx.current_track_idx >= 0 && ctx.current_track_idx < (int)ctx.play_queue.size())
                              ? ctx.play_queue[ctx.current_track_idx] : -1;
            ctx.play_queue.clear();
            for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
              ctx.play_queue.push_back(i);
            ctx.current_track_idx = cur_playing >= 0 ? cur_playing : 0;
          }
        }
        LightLock_Unlock(&ctx.lock);
      } else if (action == "open_playlist_detail") {
        if (!ctx.active_playlist_id.empty() &&
            ctx.selected_playlist_id == ctx.active_playlist_id) {
          screen_mgr.change_screen(ctx, "PlayingScreen");
        } else {
          screen_mgr.change_screen(ctx, "PlaylistDetailScreen");
        }
        kDown &= ~KEY_A;
      } else if (action == "trigger_nav_menu") {
        ctx.previous_state = ctx.current_state;
        ctx.current_state  = STATE_POPUP_NAV;
        ctx.popup_selected_index = 0;
      } else if (action == "play_selected_track") {
        LightLock_Lock(&ctx.lock);
        int sel = ctx.selected_index;
        bool valid = sel >= 0 && sel < (int)ctx.playing_tracks.size();
        Track t;
        if (valid) {
          t = ctx.playing_tracks[sel];
          // Update position in play_queue to sync current_track_idx
          for (size_t j = 0; j < ctx.play_queue.size(); ++j) {
            if (ctx.play_queue[j] == sel) {
              ctx.current_track_idx = (int)j;
              break;
            }
          }
        }
        LightLock_Unlock(&ctx.lock);
        if (valid) start_playback(t);
        kDown &= ~KEY_A;
      } else if (action == "start_playback_from_playlist") {
        // Remove MODE_BTN if present and adjust index
        int play_idx = ctx.selected_index;
        LightLock_Lock(&ctx.lock);
        if (!ctx.g_tracks.empty() && ctx.g_tracks[0].id == "MODE_BTN") {
          ctx.g_tracks.erase(ctx.g_tracks.begin());
          play_idx--; // Offset for removed MODE_BTN
          if (play_idx < 0) play_idx = 0;
        }
        LightLock_Unlock(&ctx.lock);
        start_playback(ctx.g_tracks[play_idx]);
        LightLock_Lock(&ctx.lock);
        ctx.active_playlist_id = ctx.selected_playlist_id;
        for (const auto& pl : ctx.playlists) {
          if (pl.id == ctx.selected_playlist_id) {
            ctx.active_playlist_name = pl.name;
            break;
          }
        }
        ctx.playing_tracks = ctx.g_tracks;
        // Build queue (shuffled or sequential based on mode)
        ctx.play_queue.clear();
        for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
          ctx.play_queue.push_back(i);
        if (ctx.shuffle_mode) {
          for (size_t i = ctx.play_queue.size() - 1; i > 0; --i) {
            size_t j = rand() % (i + 1);
            std::swap(ctx.play_queue[i], ctx.play_queue[j]);
          }
        }
        for (size_t i = 0; i < ctx.play_queue.size(); ++i) {
          if (ctx.play_queue[i] == play_idx) {
            ctx.current_track_idx = i;
            break;
          }
        }
        LightLock_Unlock(&ctx.lock);
        screen_mgr.change_screen(ctx, "PlayingScreen");
      } else if (action == "start_shuffle_playback" || action == "start_order_playback") {
        ctx.shuffle_mode = (action == "start_shuffle_playback");
        LightLock_Lock(&ctx.lock);
        // Remove MODE_BTN
        if (!ctx.g_tracks.empty() && ctx.g_tracks[0].id == "MODE_BTN") {
          ctx.g_tracks.erase(ctx.g_tracks.begin());
        }
        if (!ctx.g_tracks.empty()) {
          ctx.active_playlist_id = ctx.selected_playlist_id;
          for (const auto& pl : ctx.playlists) {
            if (pl.id == ctx.selected_playlist_id) {
              ctx.active_playlist_name = pl.name;
              break;
            }
          }
          ctx.playing_tracks = ctx.g_tracks;
          ctx.play_queue.clear();
          for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
            ctx.play_queue.push_back(i);
          if (ctx.shuffle_mode) {
            for (size_t i = ctx.play_queue.size() - 1; i > 0; --i) {
              size_t j = rand() % (i + 1);
              std::swap(ctx.play_queue[i], ctx.play_queue[j]);
            }
          }
          ctx.current_track_idx = 0;
          int first_track = ctx.play_queue[0];
          Track first = ctx.playing_tracks[first_track];
          LightLock_Unlock(&ctx.lock);
          start_playback(first);
          screen_mgr.change_screen(ctx, "PlayingScreen");
        } else {
          LightLock_Unlock(&ctx.lock);
        }
        kDown &= ~KEY_A;
      }
    }

    // --- Common popup touch handling ---
    bool is_popup_state = (ctx.current_state == STATE_POPUP_PLAYLIST_ADD ||
                           ctx.current_state == STATE_POPUP_PLAYLIST_OPTIONS ||
                           ctx.current_state == STATE_POPUP_TRACK_OPTIONS ||
                           ctx.current_state == STATE_POPUP_TRACK_DETAILS ||
                           ctx.current_state == STATE_EXIT_CONFIRM ||
                           ctx.current_state == STATE_POPUP_NAV ||
                           ctx.current_state == STATE_POPUP_QA_ADD ||
                           ctx.current_state == STATE_POPUP_QA_REMOVE);
    if (is_popup_state) {
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
          int tap_x = ctx.touch_state.current_x;
          int tap_y = ctx.touch_state.current_y;

          // Calculate popup box (must match draw code)
          int popup_item_count = 0;
          if (ctx.current_state == STATE_POPUP_PLAYLIST_ADD)
            popup_item_count = (int)ctx.playlists.size() + 1;
          else if (ctx.current_state == STATE_POPUP_PLAYLIST_OPTIONS)
            popup_item_count = 3;
          else if (ctx.current_state == STATE_POPUP_TRACK_OPTIONS)
            popup_item_count = (ctx.previous_state == STATE_PLAYLIST_DETAIL || ctx.previous_state == STATE_PLAYING_UI) ? 3 : 2;
          else if (ctx.current_state == STATE_POPUP_TRACK_DETAILS)
            popup_item_count = 5;
          else if (ctx.current_state == STATE_EXIT_CONFIRM)
            popup_item_count = 2;
          else if (ctx.current_state == STATE_POPUP_NAV)
            popup_item_count = 4;
          else if (ctx.current_state == STATE_POPUP_QA_ADD) {
            // Count filtered (non-registered) playlists
            std::vector<std::string> qa_ids_tmp;
            {
              const std::string& csv = ctx.config.quick_access_ids;
              size_t s = 0;
              while (s < csv.size()) {
                size_t p = csv.find(',', s);
                std::string id = (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
                if (!id.empty()) qa_ids_tmp.push_back(id);
                if (p == std::string::npos) break;
                s = p + 1;
              }
            }
            for (const auto& pl : ctx.playlists) {
              bool already = false;
              for (const auto& qid : qa_ids_tmp) {
                if (qid == pl.id) { already = true; break; }
              }
              if (!already) popup_item_count++;
            }
          }
          else if (ctx.current_state == STATE_POPUP_QA_REMOVE)
            popup_item_count = 2;

          int box_h = (int)(POPUP_HEADER_H + 8 + popup_item_count * POPUP_ITEM_H);
          if (box_h > POPUP_MAX_H) box_h = POPUP_MAX_H;
          int box_y = (240 - box_h) / 2;

          // Tap outside popup -> close (same as B)
          if (tap_x < (int)POPUP_MARGIN_X || tap_x > (int)(POPUP_MARGIN_X + POPUP_WIDTH) ||
              tap_y < box_y || tap_y > box_y + box_h) {
            if (ctx.current_state == STATE_POPUP_TRACK_DETAILS) {
              ctx.current_state = STATE_POPUP_TRACK_OPTIONS;
              ctx.popup_selected_index = 0;
            } else {
              ctx.current_state = ctx.previous_state;
            }
          } else {
            // Tap inside popup -> determine tapped item
            int max_items = (int)((POPUP_MAX_H - POPUP_HEADER_H - 8) / POPUP_ITEM_H);
            int start_idx = 0;
            if (ctx.popup_selected_index > max_items / 2)
              start_idx = ctx.popup_selected_index - max_items / 2;
            if (start_idx + max_items > popup_item_count)
              start_idx = popup_item_count - max_items;
            if (start_idx < 0) start_idx = 0;

            float items_y = box_y + POPUP_HEADER_H + 4;
            if (tap_y >= (int)items_y) {
              int tapped_idx = (int)((tap_y - items_y) / POPUP_ITEM_H) + start_idx;
              if (tapped_idx >= 0 && tapped_idx < popup_item_count) {
                ctx.popup_selected_index = tapped_idx;
                kDown |= KEY_A; // Execute immediately (delegate to KEY_A handler)
              }
            }
          }
        }
      }
    }

    if (ctx.current_state == STATE_POPUP_PLAYLIST_ADD) {
      LightLock_Lock(&ctx.lock);
      int item_count = ctx.playlists.size() + 1; // +1 for "Create New"
      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }
      LightLock_Unlock(&ctx.lock);

      if (kDown & KEY_A) {
        if (ctx.popup_selected_index == 0) { // Create new
          SwkbdState swkbd;
          char mybuf[256] = "";
          swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
          swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT);
          swkbdSetHintText(&swkbd, "New Playlist Name");
          if (swkbdInputText(&swkbd, mybuf, sizeof(mybuf)) ==
                  SWKBD_BUTTON_CONFIRM &&
              strlen(mybuf) > 0) {
            playlist_manager.create_playlist(std::string(mybuf));
            ctx.playlists = playlist_manager.get_playlists();
            std::string new_pl_id = ctx.playlists.back().id;
            playlist_manager.add_track(new_pl_id,
                                       ctx.g_tracks[ctx.selected_index]);
            ctx.playlists = playlist_manager.get_playlists();
          }
        } else { // Add to existing playlist
          std::string pl_id = ctx.playlists[ctx.popup_selected_index - 1].id;
          playlist_manager.add_track(pl_id, ctx.g_tracks[ctx.selected_index]);
          ctx.playlists = playlist_manager.get_playlists();
        }
        ctx.current_state = ctx.previous_state;
      }
    } else if (ctx.current_state == STATE_POPUP_PLAYLIST_OPTIONS) {
      int item_count = 3; // Edit title, QA toggle, Delete
      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }

      if (kDown & KEY_A) {
        if (ctx.popup_selected_index == 0) { // Edit title
          SwkbdState swkbd;
          char mybuf[256] = "";
          swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
          swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT);
          swkbdSetHintText(&swkbd, "Rename Playlist");
          if (swkbdInputText(&swkbd, mybuf, sizeof(mybuf)) ==
                  SWKBD_BUTTON_CONFIRM &&
              strlen(mybuf) > 0) {
            playlist_manager.rename_playlist(ctx.selected_playlist_id,
                                             std::string(mybuf));
            ctx.playlists = playlist_manager.get_playlists();
          }
        } else if (ctx.popup_selected_index == 1) { // Quick Access add/remove
          // parse existing QA IDs
          std::vector<std::string> qa_ids;
          {
            const std::string& csv = ctx.config.quick_access_ids;
            size_t s = 0;
            while (s < csv.size()) {
              size_t p = csv.find(',', s);
              std::string id = (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
              if (!id.empty()) qa_ids.push_back(id);
              if (p == std::string::npos) break;
              s = p + 1;
            }
          }
          bool in_qa = false;
          for (size_t i = 0; i < qa_ids.size(); ++i) {
            if (qa_ids[i] == ctx.selected_playlist_id) { in_qa = true; qa_ids.erase(qa_ids.begin() + i); break; }
          }
          if (!in_qa && (int)qa_ids.size() < 4) {
            qa_ids.push_back(ctx.selected_playlist_id);
          }
          // join back
          std::string joined;
          for (size_t i = 0; i < qa_ids.size(); ++i) {
            if (i > 0) joined += ',';
            joined += qa_ids[i];
          }
          ctx.config.quick_access_ids = joined;
          ConfigManager::save(ctx.config);
        } else if (ctx.popup_selected_index == 2) { // Delete
          // Also remove from Quick Access if registered
          {
            std::vector<std::string> qa_ids;
            const std::string& csv = ctx.config.quick_access_ids;
            size_t s = 0;
            while (s < csv.size()) {
              size_t p = csv.find(',', s);
              std::string id = (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
              if (!id.empty() && id != ctx.selected_playlist_id) qa_ids.push_back(id);
              if (p == std::string::npos) break;
              s = p + 1;
            }
            std::string joined;
            for (size_t i = 0; i < qa_ids.size(); ++i) {
              if (i > 0) joined += ',';
              joined += qa_ids[i];
            }
            if (joined != ctx.config.quick_access_ids) {
              ctx.config.quick_access_ids = joined;
              ConfigManager::save(ctx.config);
            }
          }
          playlist_manager.delete_playlist(ctx.selected_playlist_id);
          ctx.playlists = playlist_manager.get_playlists();
          ctx.selected_index = 0;
        }
        ctx.current_state = ctx.previous_state;
      }
    } else if (ctx.current_state == STATE_POPUP_TRACK_OPTIONS) {
      // Search: [Details, Add] = 2 items / PL/Playing: [Details, Remove, Add] = 3 items
      bool has_delete = (ctx.previous_state == STATE_PLAYLIST_DETAIL || ctx.previous_state == STATE_PLAYING_UI);
      int item_count = has_delete ? 3 : 2;
      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }

      if (kDown & KEY_A) {
        if (ctx.popup_selected_index == 0) { // Show details
          ctx.current_state = STATE_POPUP_TRACK_DETAILS;
          ctx.popup_selected_index = 4; // Cursor on "[ Close ]"
        } else if (has_delete && ctx.popup_selected_index == 1) { // Remove
          if (ctx.previous_state == STATE_PLAYING_UI) {
            // Remove from PlayingScreen: use active_playlist_id
            playlist_manager.remove_track(ctx.active_playlist_id,
                                          ctx.selected_track_id);
            ctx.playlists = playlist_manager.get_playlists();
            // Remove from playing_tracks
            for (auto it = ctx.playing_tracks.begin(); it != ctx.playing_tracks.end(); ++it) {
              if (it->id == ctx.selected_track_id) {
                ctx.playing_tracks.erase(it);
                break;
              }
            }
            // Rebuild play_queue
            int cur_playing_idx = -1;
            for (int i = 0; i < (int)ctx.playing_tracks.size(); ++i) {
              if (ctx.playing_tracks[i].id == ctx.playing_id) {
                cur_playing_idx = i;
                break;
              }
            }
            ctx.play_queue.clear();
            for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
              ctx.play_queue.push_back(i);
            if (ctx.shuffle_mode) {
              for (size_t i = ctx.play_queue.size() - 1; i > 0; --i) {
                size_t j = rand() % (i + 1);
                std::swap(ctx.play_queue[i], ctx.play_queue[j]);
              }
              if (cur_playing_idx >= 0) {
                for (size_t i = 0; i < ctx.play_queue.size(); ++i) {
                  if (ctx.play_queue[i] == cur_playing_idx) {
                    std::swap(ctx.play_queue[0], ctx.play_queue[i]);
                    break;
                  }
                }
                ctx.current_track_idx = 0;
              }
            } else {
              ctx.current_track_idx = cur_playing_idx >= 0 ? cur_playing_idx : 0;
            }
            // Adjust selected_index
            if (ctx.selected_index >= (int)ctx.playing_tracks.size())
              ctx.selected_index = (int)ctx.playing_tracks.size() - 1;
            if (ctx.selected_index < 0) ctx.selected_index = 0;
            // Stop if the currently playing track was removed
            if (ctx.playing_id == ctx.selected_track_id) {
              ctx.playing_id = "";
              MP3Player::is_playing = false;
            }
          } else {
            // Remove from PlaylistDetailScreen (existing logic)
            playlist_manager.remove_track(ctx.selected_playlist_id,
                                          ctx.selected_track_id);
            ctx.playlists = playlist_manager.get_playlists();
            for (auto &p : ctx.playlists) {
              if (p.id == ctx.selected_playlist_id) {
                ctx.g_tracks = p.tracks;
                break;
              }
            }
            if (ctx.selected_index >= (int)ctx.g_tracks.size())
              ctx.selected_index = ctx.g_tracks.size() - 1;
            if (ctx.selected_index < 0)
              ctx.selected_index = 0;
          }
          ctx.current_state = ctx.previous_state;
        } else { // Add to playlist (Search:idx1, PL/Playing:idx2)
          ctx.current_state = STATE_POPUP_PLAYLIST_ADD;
          ctx.popup_selected_index = 0;
        }
      }
    } else if (ctx.current_state == STATE_POPUP_TRACK_DETAILS) {
      int item_count = 5; // 4 text lines + "[ Close ]"
      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }

      if (kDown & KEY_A || kDown & KEY_B) {
        ctx.current_state = STATE_POPUP_TRACK_OPTIONS;
        ctx.popup_selected_index = 0;
      }
    } else if (ctx.current_state == STATE_EXIT_CONFIRM) {
      int item_count = 2; // No, Yes
      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }

      if (kDown & KEY_A) {
        if (ctx.popup_selected_index == 1) { // Yes (exit)
          ctx.is_running = false;
          YouTubeAPI::should_cancel = true; // Stop download thread immediately

          // Draw shutdown screen and wait
          ui_mgr.begin_top_screen(ctx.theme->bg_top);
          ui_mgr.begin_bottom_screen(ctx.theme->bg_bottom);
          C2D_Text text_exit;
          C2D_TextParse(&text_exit, ui_mgr.get_text_buf(),
                        "Shutting down safely...");
          C2D_DrawText(&text_exit, C2D_AtBaseline, 80, 110, 0, 0.6f, 0.6f,
                       C2D_Color32(200, 200, 200, 255));
          ui_mgr.end_frame();

          break;
        } else { // No (go back)
          ctx.current_state = ctx.previous_state;
        }
      }
    } else if (ctx.current_state == STATE_POPUP_NAV) {
      int item_count = 4; // Home, Search, Playlists, Settings
      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }

      if (kDown & KEY_A) {
        bool is_grayed = false;
        if (ctx.popup_selected_index == 2) is_grayed = (ctx.previous_state == STATE_PLAYLISTS);
        if (ctx.popup_selected_index == 3) is_grayed = (ctx.previous_state == STATE_SETTINGS);

        if (!is_grayed) {
          ctx.current_state = ctx.previous_state;
          kDown &= ~KEY_A;
          if (ctx.popup_selected_index == 0) {
            screen_mgr.change_screen(ctx, "HomeScreen");
          } else if (ctx.popup_selected_index == 1) {
            kDown |= KEY_Y; // Open search keyboard
          } else if (ctx.popup_selected_index == 2) {
            screen_mgr.change_screen(ctx, "PlaylistsScreen");
          } else if (ctx.popup_selected_index == 3) {
            screen_mgr.change_screen(ctx, "SettingsScreen");
          }
        }
      }
    } else if (ctx.current_state == STATE_POPUP_QA_ADD) {
      // Build filtered list (exclude already-registered PLs)
      std::vector<std::string> qa_ids;
      {
        const std::string& csv = ctx.config.quick_access_ids;
        size_t s = 0;
        while (s < csv.size()) {
          size_t p = csv.find(',', s);
          std::string id = (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
          if (!id.empty()) qa_ids.push_back(id);
          if (p == std::string::npos) break;
          s = p + 1;
        }
      }
      std::vector<int> filtered_indices;
      for (int i = 0; i < (int)ctx.playlists.size(); ++i) {
        bool already = false;
        for (const auto& qid : qa_ids) {
          if (qid == ctx.playlists[i].id) { already = true; break; }
        }
        if (!already) filtered_indices.push_back(i);
      }
      int item_count = (int)filtered_indices.size();

      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }
      if (kDown & KEY_A) {
        if (item_count > 0 && ctx.popup_selected_index >= 0 && ctx.popup_selected_index < item_count) {
          std::string pl_id = ctx.playlists[filtered_indices[ctx.popup_selected_index]].id;
          if ((int)qa_ids.size() < 4) {
            qa_ids.push_back(pl_id);
            std::string joined;
            for (size_t i = 0; i < qa_ids.size(); ++i) {
              if (i > 0) joined += ',';
              joined += qa_ids[i];
            }
            ctx.config.quick_access_ids = joined;
            ConfigManager::save(ctx.config);
          }
        }
        ctx.current_state = ctx.previous_state;
        screen_mgr.change_screen(ctx, "HomeScreen");
      }
    } else if (ctx.current_state == STATE_POPUP_QA_REMOVE) {
      int item_count = 2;
      if (kRepeat & KEY_DDOWN) {
        ctx.popup_selected_index++;
        if (ctx.popup_selected_index >= item_count)
          ctx.popup_selected_index = 0;
      }
      if (kRepeat & KEY_DUP) {
        ctx.popup_selected_index--;
        if (ctx.popup_selected_index < 0)
          ctx.popup_selected_index = item_count - 1;
      }
      if (kDown & KEY_A) {
        if (ctx.popup_selected_index == 0) { // Remove
          std::vector<std::string> qa_ids;
          const std::string& csv = ctx.config.quick_access_ids;
          size_t s = 0;
          while (s < csv.size()) {
            size_t p = csv.find(',', s);
            std::string id = (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
            if (!id.empty() && id != ctx.selected_playlist_id) qa_ids.push_back(id);
            if (p == std::string::npos) break;
            s = p + 1;
          }
          std::string joined;
          for (size_t i = 0; i < qa_ids.size(); ++i) {
            if (i > 0) joined += ',';
            joined += qa_ids[i];
          }
          ctx.config.quick_access_ids = joined;
          ConfigManager::save(ctx.config);
        }
        // index 1 = Cancel -> close without action
        ctx.current_state = ctx.previous_state;
        screen_mgr.change_screen(ctx, "HomeScreen");
      }
    }

    if (kDown & KEY_Y) {
      SwkbdState swkbd;
      char mybuf[256] = "";
      swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
      swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT);
      swkbdSetHintText(&swkbd, "Search YouTube");
      SwkbdButton button = swkbdInputText(&swkbd, mybuf, sizeof(mybuf));

      if (button == SWKBD_BUTTON_CONFIRM && strlen(mybuf) > 0) {
        YouTubeAPI::should_cancel = true;

        // Wait for cancel with 3s timeout, avoiding active-wait
        int timeout_ms = 3000;
        while (timeout_ms > 0) {
          LightLock_Lock(&ctx.lock);
          bool still_dl = ctx.is_downloading;
          LightLock_Unlock(&ctx.lock);
          if (!still_dl) break;
          svcSleepThread(50 * 1000 * 1000); // Check every 50ms
          timeout_ms -= 50;
        }

        player.stop();
        if (g_stream_buffer_ptr)
          g_stream_buffer_ptr->clear();
        YouTubeAPI::should_cancel = false;
        ctx.playing_id = "";
        ctx.playing_title = "";
        ctx.playing_title_lines.clear();

        ctx.g_tracks.clear();
        ctx.search_query = url_encode(std::string(mybuf));
        ctx.selected_index = 0;
        screen_mgr.change_screen(ctx, "SearchScreen");
      }
    }

    player.update();

    LightLock_Lock(&stream_lock);
    size_t sb_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);

    if (MP3Player::is_playing && sb_size > 50000 && !ctx.is_paused) {
      LightLock_Lock(&ctx.lock);
      ctx.g_status_msg = "Playing";
      LightLock_Unlock(&ctx.lock);
    }

    // --- Rendering (MVC: View) ---
    LightLock_Lock(&ctx.lock);
    RenderContext render_ctx =
        ctx; // Slicing copy for safe, fast data snapshot
    LightLock_Unlock(&ctx.lock);

    // Draw without holding the lock
    // If it's a popup, UIRenderer needs to draw the overlay on top of whatever
    // screen was behind it.
    bool is_popup = (render_ctx.current_state == STATE_POPUP_PLAYLIST_ADD ||
                     render_ctx.current_state == STATE_POPUP_PLAYLIST_OPTIONS ||
                     render_ctx.current_state == STATE_POPUP_TRACK_OPTIONS ||
                     render_ctx.current_state == STATE_POPUP_TRACK_DETAILS ||
                     render_ctx.current_state == STATE_EXIT_CONFIRM ||
                     render_ctx.current_state == STATE_POPUP_QA_ADD ||
                     render_ctx.current_state == STATE_POPUP_QA_REMOVE ||
                     render_ctx.current_state == STATE_POPUP_NAV);

    AppState bg_state =
        is_popup ? render_ctx.previous_state : render_ctx.current_state;

    // Start frame for ALL cases
    ui_mgr.begin_top_screen(render_ctx.theme->bg_top);
    if (bg_state == STATE_SETTINGS) {
      screen_mgr.draw_top(render_ctx, ui_mgr);
    } else {
      renderer.draw_top_screen(render_ctx);
    }

    ui_mgr.begin_bottom_screen(render_ctx.theme->bg_bottom);
    if (bg_state == STATE_HOME || bg_state == STATE_SETTINGS ||
        bg_state == STATE_SEARCH || bg_state == STATE_PLAYLISTS ||
        bg_state == STATE_PLAYLIST_DETAIL || bg_state == STATE_PLAYING_UI) {
      screen_mgr.draw_bottom(render_ctx, ui_mgr);
    }

    if (is_popup) {
      renderer.draw_popup_overlay(render_ctx);
    }

    ui_mgr.end_frame(); // We call end_frame exactly once per iteration

    // Temporary yield to prevent freeze when lid is closed
    // (ideally controlled by C3D_FrameBegin return value)
    if (osGetTime() % 100 < 10)
      svcSleepThread(
          1000000); // 1ms sleep occasionally to prevent complete starvation

  } // End of main loop

  YouTubeAPI::should_cancel = true;
  ctx.is_running = false;
  // --- Shutdown phase ---
  // 1. Wait for thread to fully terminate (attached, so U64_MAX blocks reliably)
  if (threadId) {
    threadJoin(threadId, U64_MAX);
    threadFree(threadId);
  }

  // 2. Safely stop player
  player.stop();

  // 3. Clean up network library before socExit
  api.cleanup();

  // 4. Destroy C++ heap objects before hardware shutdown
  if (g_stream_buffer_ptr) {
    g_stream_buffer_ptr->clear();
    g_stream_buffer_ptr->shrink_to_fit();
    g_stream_buffer_ptr.reset();
  }

  g_player_ptr.reset();
  g_playlist_manager_ptr.reset();
  g_ctx_ptr.reset();

  // 5. Destroy network class before socExit
  g_api_ptr.reset();

  // 6. Explicitly destroy local C++ objects before scope exit
  // (destructors would access already-exited hardware, causing crash)
  // Unload wallpaper texture before GPU shutdown (stack var, must unload manually)
  g_wallpaper.unload();
  // Destroy ScreenManager first (HomeScreen holds TouchButton vector)
  screen_mgr.clear();
  // Destroy UIRenderer first (holds reference to UIManager)
  g_renderer_ptr.reset();
  // UIManager's cleanup() calls C2D_Fini/C3D_Fini/gfxExit
  g_ui_mgr_ptr.reset();

  // 7. Shutdown system services and hardware (last to destroy)
  NDMU_LeaveExclusiveState();
  ndmuExit();

  ndspExit();
  ptmuExit();
  socExit();

  if (soc_buffer)
    free(soc_buffer);

  return 0;
}