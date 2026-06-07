#include "non_audio_interference_gate.h"

namespace {

static constexpr size_t kThumbnailFetchSafeBufferBytes = 192U * 1024U;
static constexpr int kThumbnailFetchSafeQueuedWavebufs = 5;
static constexpr int kThumbnailUploadSafeQueuedWavebufs = 6;

static bool is_webm_opus_direct(const NonAudioInterferenceInput &input) {
  return input.is_opus_direct &&
         input.stream_mode == StreamContainerMode::ProxyWebmOpus;
}

} // namespace

bool should_defer_thumbnail_fetch(const NonAudioInterferenceInput &input) {
  if (!is_webm_opus_direct(input)) {
    return false;
  }

  switch (input.webm_stage) {
    case WebmPlaybackStage::WaitingForDecoderStart:
    case WebmPlaybackStage::Prebuffering:
    case WebmPlaybackStage::Failed:
      return true;
    case WebmPlaybackStage::Idle:
      return !input.audio_started;
    case WebmPlaybackStage::Steady:
      break;
  }

  if (input.queued_wavebufs < kThumbnailFetchSafeQueuedWavebufs) {
    return true;
  }
  if (!input.download_complete &&
      input.stream_buffer_bytes < kThumbnailFetchSafeBufferBytes) {
    return true;
  }
  return false;
}

bool should_defer_thumbnail_upload(const NonAudioInterferenceInput &input) {
  if (!is_webm_opus_direct(input)) {
    return false;
  }
  if (!input.audio_started) {
    return true;
  }

  switch (input.webm_stage) {
    case WebmPlaybackStage::WaitingForDecoderStart:
    case WebmPlaybackStage::Prebuffering:
    case WebmPlaybackStage::Failed:
      return true;
    case WebmPlaybackStage::Idle:
      return true;
    case WebmPlaybackStage::Steady:
      break;
  }

  return input.queued_wavebufs < kThumbnailUploadSafeQueuedWavebufs;
}
