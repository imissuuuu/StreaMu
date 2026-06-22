#include "non_audio_interference_gate.h"

namespace {

static constexpr int kThumbnailFetchSafeQueuedWavebufs = 5;
static constexpr int kThumbnailUploadSafeQueuedWavebufs = 6;

static bool is_webm_opus_direct(const NonAudioInterferenceInput &input) {
  return input.is_opus_direct &&
         input.stream_mode == StreamContainerMode::ProxyWebmOpus;
}

} // namespace

const char *
non_audio_interference_reason_name(NonAudioInterferenceReason reason) {
  switch (reason) {
    case NonAudioInterferenceReason::None:
      return "none";
    case NonAudioInterferenceReason::NotWebmOpusDirect:
      return "not_webm_opus_direct";
    case NonAudioInterferenceReason::WaitingForDecoderStart:
      return "waiting_for_decoder_start";
    case NonAudioInterferenceReason::Prebuffering:
      return "prebuffering";
    case NonAudioInterferenceReason::Failed:
      return "failed";
    case NonAudioInterferenceReason::IdleBeforeAudio:
      return "idle_before_audio";
    case NonAudioInterferenceReason::Idle:
      return "idle";
    case NonAudioInterferenceReason::AudioNotStarted:
      return "audio_not_started";
    case NonAudioInterferenceReason::QueueBelowFetchTarget:
      return "queue_below_fetch_target";
    case NonAudioInterferenceReason::QueueBelowUploadTarget:
      return "queue_below_upload_target";
    case NonAudioInterferenceReason::BytesBelowFetchTarget:
      return "bytes_below_fetch_target";
  }
  return "unknown";
}

NonAudioInterferenceDecision
evaluate_thumbnail_fetch_interference(const NonAudioInterferenceInput &input) {
  if (!is_webm_opus_direct(input)) {
    return {false, NonAudioInterferenceReason::NotWebmOpusDirect};
  }

  switch (input.webm_stage) {
    case WebmPlaybackStage::WaitingForDecoderStart:
      return {true, NonAudioInterferenceReason::WaitingForDecoderStart};
    case WebmPlaybackStage::Prebuffering:
      return {true, NonAudioInterferenceReason::Prebuffering};
    case WebmPlaybackStage::Failed:
      return {true, NonAudioInterferenceReason::Failed};
    case WebmPlaybackStage::Idle:
      return {!input.audio_started,
              input.audio_started
                  ? NonAudioInterferenceReason::None
                  : NonAudioInterferenceReason::IdleBeforeAudio};
    case WebmPlaybackStage::Steady:
      break;
  }

  if (!input.audio_started) {
    return {true, NonAudioInterferenceReason::AudioNotStarted};
  }
  if (input.queued_wavebufs < kThumbnailFetchSafeQueuedWavebufs) {
    return {true, NonAudioInterferenceReason::QueueBelowFetchTarget};
  }
  return {false, NonAudioInterferenceReason::None};
}

NonAudioInterferenceDecision
evaluate_thumbnail_upload_interference(const NonAudioInterferenceInput &input) {
  if (!is_webm_opus_direct(input)) {
    return {false, NonAudioInterferenceReason::NotWebmOpusDirect};
  }
  if (!input.audio_started) {
    return {true, NonAudioInterferenceReason::AudioNotStarted};
  }

  switch (input.webm_stage) {
    case WebmPlaybackStage::WaitingForDecoderStart:
      return {true, NonAudioInterferenceReason::WaitingForDecoderStart};
    case WebmPlaybackStage::Prebuffering:
      return {true, NonAudioInterferenceReason::Prebuffering};
    case WebmPlaybackStage::Failed:
      return {true, NonAudioInterferenceReason::Failed};
    case WebmPlaybackStage::Idle:
      return {true, NonAudioInterferenceReason::Idle};
    case WebmPlaybackStage::Steady:
      break;
  }

  if (input.queued_wavebufs < kThumbnailUploadSafeQueuedWavebufs) {
    return {true, NonAudioInterferenceReason::QueueBelowUploadTarget};
  }
  return {false, NonAudioInterferenceReason::None};
}

bool should_defer_thumbnail_fetch(const NonAudioInterferenceInput &input) {
  return evaluate_thumbnail_fetch_interference(input).defer;
}

bool should_defer_thumbnail_upload(const NonAudioInterferenceInput &input) {
  return evaluate_thumbnail_upload_interference(input).defer;
}
