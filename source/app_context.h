#pragma once
#include "audio/non_audio_interference_gate.h"
#include "audio/webm_seek_types.h"
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
  AudioPathConfig active_audio_path = AudioPathConfig::PROXY_OGG_OPUS;
  StreamContainerMode active_stream_mode = StreamContainerMode::ProxyOggOpus;
  WebmPlaybackStage webm_playback_stage = WebmPlaybackStage::Idle;
  OpusPlaybackFailure opus_playback_failure = OpusPlaybackFailure::None;
  LightLock lock; // Mutex for thread synchronization
  // touch_state inherited from RenderContext (no shadowing)

  // API pointer (set in main after YouTubeAPI is created)
  YouTubeAPI *api = nullptr;

  // Thumbnail for currently playing track
  Wallpaper thumbnail_tex;
  std::string thumbnail_vid_id; // video ID of the loaded thumbnail

  int seek_target_seconds = -1;   // Requested seek position (-1 = none)
  int pending_stream_seek_ms = 0; // Applied when the next direct decoder opens
  int pending_stream_emit_start_ms = 0;
  uint64_t pending_stream_parser_prefetch_byte = 0;
  bool pending_stream_parser_cluster_aligned = false;
  int pending_stream_seek_seq = 0;
  WebmSeekPlan pending_stream_seek_plan;
  bool pending_stream_repeated_seek = false;
  WebmSeekReuseClass pending_stream_reuse_class = WebmSeekReuseClass::Cold;
  int active_stream_seek_seq = 0;
  int active_stream_seek_target_ms = 0;
  WebmSeekPlan active_stream_seek_plan;
  bool active_stream_repeated_seek = false;
  WebmSeekReuseClass active_stream_reuse_class = WebmSeekReuseClass::Cold;
  u64 active_stream_seek_plan_ready_at_ms = 0;
  bool active_stream_first_pcm_queued_logged = false;
  bool active_stream_runtime_cache_saved = false;
  u64 active_stream_first_pcm_queued_at_ms = 0;
  uint64_t pending_seek_backtrack_bytes = 0;
  int pending_seek_retry_count = 0;
  WebmSeekTrackState webm_seek_track_state;
  std::string webm_seek_track_state_video_id;
  std::vector<uint8_t> webm_parser_initial_seed_bytes;
  std::string webm_parser_initial_seed_video_id;
  Thread webm_startup_warmup_thread = nullptr;
  bool webm_startup_warmup_done = true;
  std::string webm_startup_warmup_video_id;

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
  NonAudioInterferenceReason thumbnail_fetch_defer_reason =
      NonAudioInterferenceReason::None;
  NonAudioInterferenceReason thumbnail_upload_defer_reason =
      NonAudioInterferenceReason::None;
  u64 thumbnail_fetch_defer_since_ms = 0;
  u64 thumbnail_upload_defer_since_ms = 0;
  u64 thumbnail_fetch_defer_last_log_ms = 0;
  u64 thumbnail_upload_defer_last_log_ms = 0;
};
