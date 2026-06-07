#include "webm_playback_controller.h"

namespace {

static const int kDefaultTargetQueuedWavebufs = 7;
static const int kConservativeTargetQueuedWavebufs = 8;
static const int kSteadyMaxDecodeBuffers = 2;
static const int kConservativeMaxDecodeBuffers = 1;
static const int kAggressiveMaxDecodeBuffers = 3;

} // namespace

WebmPlaybackControllerConfig webm_playback_controller_default_config() {
  return WebmPlaybackControllerConfig{};
}

WebmPlaybackControllerDecision decide_webm_playback_step(
    WebmPlaybackStage stage, const WebmPlaybackControllerInput &input,
    const WebmPlaybackControllerConfig &config) {
  WebmPlaybackControllerDecision decision = {};
  decision.target_queued_wavebufs = kDefaultTargetQueuedWavebufs;
  decision.max_decode_buffers = kSteadyMaxDecodeBuffers;

  if (input.decode_failed) {
    decision.transition_to_failed = true;
    return decision;
  }

  switch (stage) {
  case WebmPlaybackStage::Idle:
  case WebmPlaybackStage::Failed:
    return decision;
  case WebmPlaybackStage::WaitingForDecoderStart:
    decision.should_start_decoder =
        input.stream_buffer_bytes >= config.decoder_start_bytes ||
        (input.download_complete && input.stream_buffer_bytes > 0U);
    decision.keep_ndsp_paused = false;
    decision.target_queued_wavebufs = config.initial_wavebuf_target;
    decision.max_decode_buffers = kAggressiveMaxDecodeBuffers;
    return decision;
  case WebmPlaybackStage::Prebuffering:
    decision.keep_ndsp_paused = !input.user_paused;
    decision.target_queued_wavebufs = config.initial_wavebuf_target;
    decision.max_decode_buffers = kAggressiveMaxDecodeBuffers;
    decision.release_prebuffer =
        input.queued_wavebufs >= config.initial_wavebuf_target &&
        (input.stream_buffer_bytes >= config.playback_release_bytes ||
         input.download_complete);
    return decision;
  case WebmPlaybackStage::Steady:
    if (input.queued_wavebufs <= config.initial_wavebuf_target) {
      decision.target_queued_wavebufs = kConservativeTargetQueuedWavebufs;
      decision.max_decode_buffers = kAggressiveMaxDecodeBuffers;
    } else if (input.decode_ticks >= config.high_decode_ticks) {
      decision.target_queued_wavebufs = kConservativeTargetQueuedWavebufs;
      decision.max_decode_buffers = kConservativeMaxDecodeBuffers;
    } else if (input.decode_ticks <= config.low_decode_ticks) {
      decision.target_queued_wavebufs = kDefaultTargetQueuedWavebufs;
      decision.max_decode_buffers = kAggressiveMaxDecodeBuffers;
    }
    return decision;
  }

  return decision;
}

WebmPlaybackStage next_webm_playback_stage(
    WebmPlaybackStage stage, const WebmPlaybackControllerInput &input,
    const WebmPlaybackControllerDecision &decision) {
  if (decision.transition_to_failed) {
    return WebmPlaybackStage::Failed;
  }

  switch (stage) {
  case WebmPlaybackStage::Idle:
  case WebmPlaybackStage::Failed:
    return stage;
  case WebmPlaybackStage::WaitingForDecoderStart:
    if (decision.should_start_decoder) {
      return WebmPlaybackStage::Prebuffering;
    }
    return stage;
  case WebmPlaybackStage::Prebuffering:
    if (decision.release_prebuffer || input.has_started_playing) {
      return WebmPlaybackStage::Steady;
    }
    return stage;
  case WebmPlaybackStage::Steady:
    return stage;
  }

  return stage;
}
