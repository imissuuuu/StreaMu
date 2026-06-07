#ifndef WEBM_PLAYBACK_CONTROLLER_H
#define WEBM_PLAYBACK_CONTROLLER_H

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

enum class WebmPlaybackStage {
  Idle,
  WaitingForDecoderStart,
  Prebuffering,
  Steady,
  Failed,
};

struct WebmPlaybackControllerConfig {
  size_t decoder_start_bytes = 16U * 1024U;
  size_t playback_release_bytes = 24U * 1024U;
  int initial_wavebuf_target = 2;
  u64 high_decode_ticks = 9500000ULL;
  u64 low_decode_ticks = 7000000ULL;
};

struct WebmPlaybackControllerInput {
  size_t stream_buffer_bytes = 0;
  bool download_complete = false;
  int queued_wavebufs = 0;
  int free_wavebufs = 0;
  int decoded_buffers = 0;
  u64 decode_ticks = 0;
  bool has_started_playing = false;
  bool decode_failed = false;
  bool user_paused = false;
};

struct WebmPlaybackControllerDecision {
  bool should_start_decoder = false;
  bool keep_ndsp_paused = false;
  bool release_prebuffer = false;
  bool transition_to_failed = false;
  int target_queued_wavebufs = 0;
  int max_decode_buffers = 0;
};

WebmPlaybackControllerConfig webm_playback_controller_default_config();
WebmPlaybackControllerDecision decide_webm_playback_step(
    WebmPlaybackStage stage, const WebmPlaybackControllerInput &input,
    const WebmPlaybackControllerConfig &config);
WebmPlaybackStage next_webm_playback_stage(
    WebmPlaybackStage stage, const WebmPlaybackControllerInput &input,
    const WebmPlaybackControllerDecision &decision);

#endif
