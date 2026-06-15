#ifndef WEBM_AUDIBLE_START_POLICY_H
#define WEBM_AUDIBLE_START_POLICY_H

#include <stddef.h>

struct WebmAudibleStartPolicyConfig {
  int seek_pcm_guard_ms = 20;
  int seek_preroll_short_log_ms = 120;
  size_t startup_audio_page_target_bytes = 960U;
  size_t steady_audio_page_target_bytes = 2048U;
  size_t seek_audio_page_target_bytes = 960U;
};

struct WebmFirstEmitDecision {
  int packet_timecode_ms = 0;
  int warmup_ms = 0;
  int pcm_skip_samples_per_channel = 0;
  bool preroll_short = false;
};

static inline int webm_ms_to_48k_samples_per_channel(int ms) {
  return ms > 0 ? ms * 48 : 0;
}

static inline bool
webm_should_emit_packet_for_audible_start(int seek_start_ms, int seek_emit_ms,
                                          int packet_timecode_ms) {
  if (seek_start_ms <= 0) {
    return true;
  }
  if (packet_timecode_ms < 0) {
    return true;
  }
  const int emit_threshold_ms =
      seek_emit_ms >= 0 ? seek_emit_ms : seek_start_ms;
  return packet_timecode_ms >= emit_threshold_ms;
}

static inline WebmFirstEmitDecision
webm_first_emit_decision(int seek_start_ms, int packet_timecode_ms,
                         const WebmAudibleStartPolicyConfig &config) {
  WebmFirstEmitDecision decision = {};
  decision.packet_timecode_ms = packet_timecode_ms;
  if (seek_start_ms <= 0 || packet_timecode_ms < 0) {
    return decision;
  }

  decision.warmup_ms = seek_start_ms - packet_timecode_ms;
  if (decision.warmup_ms < 0) {
    decision.warmup_ms = 0;
  }
  decision.preroll_short =
      decision.warmup_ms < config.seek_preroll_short_log_ms;
  decision.pcm_skip_samples_per_channel = webm_ms_to_48k_samples_per_channel(
      decision.warmup_ms + config.seek_pcm_guard_ms);
  return decision;
}

static inline size_t
webm_audio_page_target_bytes(bool seek_active, bool first_audio_not_logged,
                             const WebmAudibleStartPolicyConfig &config) {
  if (seek_active) {
    return config.seek_audio_page_target_bytes;
  }
  return first_audio_not_logged ? config.startup_audio_page_target_bytes
                                : config.steady_audio_page_target_bytes;
}

#endif
