#pragma once
#include "audio/webm_playback_controller.h"
#include "network/youtube_api.h"
#include "ui/theme.h"
#include "ui/touch_state.h"
#include "ui/ui_renderer.h"
#include "ui/wallpaper.h"
#include <3ds.h>
#include <string>
#include <vector>

// Forward declare to avoid circular include (full type in youtube_api.h via
// playlist_manager.h)
class YouTubeAPI;

enum class OpusPlaybackFailure {
  None,
  Network,
  WebmParse,
  WebmUnsupported,
  Decoder,
};

struct AppContext : public RenderContext {
  std::string search_query = "";
  bool is_downloading = false;
  bool is_running = true;
  std::string playing_title = "";
  // is_paused inherited from RenderContext (no shadowing)
  std::string current_stream_url = "";
  AudioPathConfig active_audio_path = AudioPathConfig::OPUS_DIRECT;
  StreamContainerMode active_stream_mode = StreamContainerMode::ProxyOggOpus;
  bool opus_webm_poc_enabled = false;
  WebmPlaybackStage webm_playback_stage = WebmPlaybackStage::Idle;
  OpusPlaybackFailure opus_playback_failure = OpusPlaybackFailure::None;
  LightLock lock; // Mutex for thread synchronization
  // touch_state inherited from RenderContext (no shadowing)

  // API pointer (set in main after YouTubeAPI is created)
  YouTubeAPI *api = nullptr;

  // Thumbnail for currently playing track
  Wallpaper thumbnail_tex;
  std::string thumbnail_vid_id; // video ID of the loaded thumbnail

  int seek_target_seconds = -1; // Requested seek position (-1 = none)

  // Thumbnail async download state (protected by lock)
  // playback_start_time inherited from RenderContext (no shadowing)
  bool thumbnail_loading = false; // true while thread is running
  bool thumbnail_ready = false;   // true when raw data is ready for GPU upload
  std::vector<uint8_t> thumbnail_pixels; // cropped RGBA pixels (set by thread)
  int thumbnail_crop_size = 0;           // side length of cropped square
  bool compare_thumbnail_fetch_logged = false;
  bool compare_thumbnail_done_logged = false;
  bool compare_thumbnail_upload_logged = false;
  bool compare_audio_audible_logged = false;
};
