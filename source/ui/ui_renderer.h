#pragma once
#include "playlist_manager.h"
#include "theme.h"
#include "ui_manager.h"
#include "wallpaper.h"
#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <vector>


// Current app state (screen or popup)
enum AppState {
  STATE_HOME,
  STATE_SEARCH,
  STATE_PLAYLISTS,
  STATE_PLAYLIST_DETAIL,
  // --- Popup (submenu) states ---
  STATE_POPUP_PLAYLIST_ADD,     // Add track to playlist
  STATE_POPUP_PLAYLIST_OPTIONS, // Playlist operations (rename/delete)
  STATE_POPUP_TRACK_OPTIONS,    // Track operations within playlist
  STATE_POPUP_TRACK_DETAILS,
  STATE_EXIT_CONFIRM, // Exit confirmation on START press
  STATE_SETTINGS,     // Settings screen
  STATE_POPUP_NAV,    // Hamburger menu popup
  STATE_PLAYING_UI,   // Playing screen
  STATE_POPUP_QA_ADD, // Quick Access add popup
  STATE_POPUP_QA_REMOVE // Quick Access remove confirmation popup
};

// Playback loop mode
enum LoopMode { LOOP_OFF, LOOP_ALL, LOOP_ONE };

// Struct containing all data needed for rendering (passed from AppCtx etc.)
struct RenderContext {
  AppState current_state;
  // State before popup opened (for B button return)
  AppState previous_state;
  std::string g_status_msg;
  bool is_server_connected = true;

  // Top screen related
  std::vector<std::string> playing_title_lines;

  // Bottom screen related
  int home_selected_index;

  std::vector<Track> g_tracks;
  std::vector<Track> playing_tracks; // Copy of g_tracks at playback start; independent of browsing
  int selected_index;
  int scroll_x;

  // Playlist related
  std::vector<Playlist> playlists;
  int popup_selected_index; // Popup menu cursor

  // Currently selected target details
  std::string
      selected_playlist_id; // Target ID for PLAYLIST_DETAIL and popups
  std::string selected_track_id; // Target track ID for TRACK_OPTIONS etc.

  std::string playing_id;
  std::string playing_duration; // Current track duration ("3:45")
  std::string playing_meta; // Current track metadata ("3:45 · 2.4K · 3y ago")
  bool is_paused = false;

  // --- Playlist playback (shuffle) state ---
  std::string active_playlist_id;   // Currently playing playlist ID
  std::string active_playlist_name; // Currently playing playlist name
  int current_track_idx = -1;       // Current position in play queue
  std::vector<int> play_queue;      // Shuffled playback index sequence
  bool shuffle_mode = false;        // Shuffle playback mode
  LoopMode loop_mode = LOOP_OFF;    // Loop mode
  int mode_btn_focus = 1;           // PlaylistDetail button row focus (0=SHUFFLE, 1=ORDER)

  // Drag scroll
  float scroll_offset_y = 0.0f;

  // Theme config
  AppConfig config;
  const ThemeColors* theme = nullptr;
};

class UIRenderer {
public:
  UIRenderer(UIManager &manager, const ThemeColors &colors);
  ~UIRenderer();

  void draw_top_screen(const RenderContext &ctx);
  void draw_popup_overlay(const RenderContext &ctx);
  void set_wallpaper(Wallpaper* wp) { m_wallpaper = wp; }
  void set_thumbnail(Wallpaper* th) { m_thumbnail = th; }

private:
  UIManager &m_ui;
  const ThemeColors &m_colors;
  Wallpaper* m_wallpaper = nullptr;
  Wallpaper* m_thumbnail = nullptr;
};
