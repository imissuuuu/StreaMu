#include <3ds.h>
#include <citro2d.h>
#include <cmath>
#include <iomanip>
#include <malloc.h>
#include <sstream>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <vector>

#include "audio/opus_decode_tuning.h"
#include "audio/opus_perf_metrics.h"
#include "audio/opus_poc_player.h"
#include "audio/non_audio_interference_gate.h"
#include "audio/playback_observer.h"
#include "audio/webm_seek_index.h"
#include "audio/webm_seek_planner.h"
#include "audio/webm_seek_runtime.h"
#include "audio/opus_stream_pipeline.h"
#include "audio/webm_playback_controller.h"
#include "network/youtube_api.h"
#include "playlist_manager.h"
#include "ui/ui_constants.h"
#include "ui/ui_icon_cache.h"
#include "ui/ui_manager.h"
#include "ui/ui_renderer.h"
#include "ui/track_list_helpers.h"
#include <memory>

std::unique_ptr<std::vector<uint8_t>>
    g_stream_buffer_ptr; // Dynamic buffer for network audio
LightLock stream_lock;   // Mutex protecting stream_buffer access
bool g_stream_download_complete = true;
static u64 g_opus_playback_perf_start_ms = 0;
static bool g_opus_first_pcm_queued_logged = false;
static bool g_opus_audio_started_logged = false;
static bool g_webm_first_pcm_queued_logged = false;
static bool g_webm_audio_started_logged = false;
static u64 g_opus_last_frame_observe_ms = 0;
static int g_opus_observed_max_decoded_buffers = 0;
static bool g_opus_observed_decode_failure = false;
static u64 g_webm_update_pressure_last_log_ms = 0;
static u64 g_startup_perf_start_ms = 0;
static int g_webm_seek_trace_seq = 0;
static const uint64_t kWebmSeekPlanPrerollMs = 80;
static const uint64_t kWebmSeekHeaderProbeSizeBytes = 512ULL * 1024ULL;
static const uint64_t kWebmSeekProbeSizeBytes = 512ULL * 1024ULL;
static const uint64_t kWebmParserInitialSeedBytes = 128ULL * 1024ULL;
static const u64 kWebmSlowUpdateTicks = 9500000ULL;

static uint64_t backtrack_bytes_for_retry(int retry_count);

static const char *playback_status_message(bool is_paused, bool is_buffering) {
  if (is_paused) {
    return "Paused";
  }
  if (is_buffering) {
    return "Buffering...";
  }
  return "Playing";
}

static OpusPlaybackFailure webm_error_to_failure(WebmRemuxError error) {
  switch (error) {
    case WebmRemuxError::UnsupportedChannels:
    case WebmRemuxError::UnsupportedTrackCount:
    case WebmRemuxError::UnsupportedFeature:
      return OpusPlaybackFailure::WebmUnsupported;
    case WebmRemuxError::SeekPrerollInsufficient:
    case WebmRemuxError::None:
      return OpusPlaybackFailure::None;
    default:
      return OpusPlaybackFailure::WebmParse;
  }
}

static StreamContainerMode stream_mode_for_audio_path(AudioPathConfig path) {
  return audio_path_uses_webm_direct(path) ? StreamContainerMode::ProxyWebmOpus
                                           : StreamContainerMode::ProxyOggOpus;
}

static const char *playback_failure_message(OpusPlaybackFailure failure) {
  switch (failure) {
    case OpusPlaybackFailure::None:
      return "";
    case OpusPlaybackFailure::Network:
      return "Stream Error";
    case OpusPlaybackFailure::WebmParse:
      return "WebM parse failed";
    case OpusPlaybackFailure::WebmUnsupported:
      return "WebM unsupported";
    case OpusPlaybackFailure::Decoder:
      return "Opus failed";
  }
  return "Playback failed";
}

static void append_webm_playback_log(const char *event, size_t bytes) {
  if (!event) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = now_ms >= g_opus_playback_perf_start_ms
                             ? now_ms - g_opus_playback_perf_start_ms
                             : 0;
  fprintf(f, "[webm-perf] +%llums %s bytes=%lu\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          static_cast<unsigned long>(bytes));
  fclose(f);
}

enum class Phase5aDominantStage {
  Unknown,
  Network,
  BufferWait,
  DecodeToPcm,
};

static const char *phase5a_dominant_stage_name(Phase5aDominantStage stage) {
  switch (stage) {
    case Phase5aDominantStage::Network:
      return "network";
    case Phase5aDominantStage::BufferWait:
      return "buffer_wait";
    case Phase5aDominantStage::DecodeToPcm:
      return "decode_to_pcm";
    case Phase5aDominantStage::Unknown:
      return "unknown";
  }
  return "unknown";
}

static const char *
playback_session_kind_name(PlaybackSessionKind session_kind) {
  switch (session_kind) {
    case PlaybackSessionKind::Startup:
      return "startup";
    case PlaybackSessionKind::Seek:
      return "seek";
  }
  return "unknown";
}

static Phase5aDominantStage
dominant_stage_for_phase5a(const PlaybackObserverEventTimes &times,
                           u64 *out_network_ms, u64 *out_buffer_wait_ms,
                           u64 *out_decode_to_pcm_ms) {
  if (out_network_ms) {
    *out_network_ms = 0;
  }
  if (out_buffer_wait_ms) {
    *out_buffer_wait_ms = 0;
  }
  if (out_decode_to_pcm_ms) {
    *out_decode_to_pcm_ms = 0;
  }
  if (!times.first_byte_seen || !times.decoder_open_start_seen ||
      !times.decoder_open_ok_seen || !times.first_pcm_queued_seen) {
    return Phase5aDominantStage::Unknown;
  }

  const u64 network_ms = times.first_byte_ms;
  const u64 buffer_wait_ms =
      times.decoder_open_start_ms >= times.first_byte_ms
          ? (times.decoder_open_start_ms - times.first_byte_ms)
          : 0;
  const u64 decode_to_pcm_ms =
      times.first_pcm_queued_ms >= times.decoder_open_ok_ms
          ? (times.first_pcm_queued_ms - times.decoder_open_ok_ms)
          : 0;
  if (out_network_ms) {
    *out_network_ms = network_ms;
  }
  if (out_buffer_wait_ms) {
    *out_buffer_wait_ms = buffer_wait_ms;
  }
  if (out_decode_to_pcm_ms) {
    *out_decode_to_pcm_ms = decode_to_pcm_ms;
  }

  if (network_ms >= buffer_wait_ms && network_ms >= decode_to_pcm_ms) {
    return Phase5aDominantStage::Network;
  }
  if (buffer_wait_ms >= decode_to_pcm_ms) {
    return Phase5aDominantStage::BufferWait;
  }
  return Phase5aDominantStage::DecodeToPcm;
}

static void
append_webm_phase5a_summary_log(PlaybackSessionKind session_kind,
                                const PlaybackObserverEventTimes &times) {
  u64 network_ms = 0;
  u64 buffer_wait_ms = 0;
  u64 decode_to_pcm_ms = 0;
  const Phase5aDominantStage dominant = dominant_stage_for_phase5a(
      times, &network_ms, &buffer_wait_ms, &decode_to_pcm_ms);
  if (dominant == Phase5aDominantStage::Unknown) {
    return;
  }

  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  fprintf(f,
          "[webm-phase5a] kind=%s dominant=%s network_ms=%llu "
          "buffer_wait_ms=%llu decode_to_pcm_ms=%llu first_byte_ms=%llu "
          "decoder_open_start_ms=%llu decoder_open_ok_ms=%llu "
          "first_pcm_ms=%llu audio_audible_ms=%llu\n",
          playback_session_kind_name(session_kind),
          phase5a_dominant_stage_name(dominant),
          static_cast<unsigned long long>(network_ms),
          static_cast<unsigned long long>(buffer_wait_ms),
          static_cast<unsigned long long>(decode_to_pcm_ms),
          static_cast<unsigned long long>(times.first_byte_ms),
          static_cast<unsigned long long>(times.decoder_open_start_ms),
          static_cast<unsigned long long>(times.decoder_open_ok_ms),
          static_cast<unsigned long long>(times.first_pcm_queued_ms),
          static_cast<unsigned long long>(times.audio_audible_ms));
  fclose(f);
}

static bool should_log_webm_seek_plan_event(const char *event) {
  if (!event) {
    return false;
  }
  return strcmp(event, "parser_seek_plan_ready") == 0 ||
         strcmp(event, "seek_plan_ready") == 0 ||
         strcmp(event, "parser_seek_plan_invalid") == 0 ||
         strcmp(event, "seek_plan_invalid") == 0 ||
         strcmp(event, "parser_seek_metadata_failed") == 0 ||
         strcmp(event, "seek_plan_metadata_failed") == 0;
}

static void append_webm_seek_plan_log(const char *event, int target_ms,
                                      uint64_t start_byte, int emit_start_ms) {
  if (!should_log_webm_seek_plan_event(event)) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = now_ms >= g_opus_playback_perf_start_ms
                             ? now_ms - g_opus_playback_perf_start_ms
                             : 0;
  fprintf(
      f,
      "[webm-perf] +%llums %s target_ms=%d start_byte=%llu emit_start_ms=%d\n",
      static_cast<unsigned long long>(elapsed_ms), event, target_ms,
      static_cast<unsigned long long>(start_byte), emit_start_ms);
  fclose(f);
}

static const char *
webm_seek_cache_lookup_status_name(WebmSeekCacheLookupStatus value) {
  switch (value) {
    case WebmSeekCacheLookupStatus::NotChecked:
      return "na";
    case WebmSeekCacheLookupStatus::Empty:
      return "empty";
    case WebmSeekCacheLookupStatus::ExactHit:
      return "exact_hit";
    case WebmSeekCacheLookupStatus::WarmStartHit:
      return "warm_start_hit";
    case WebmSeekCacheLookupStatus::ExactMissNoCandidate:
      return "exact_miss_no_candidate";
    case WebmSeekCacheLookupStatus::ExactRejectedGap:
      return "exact_rejected_gap";
    case WebmSeekCacheLookupStatus::WarmStartRejectedGap:
      return "warm_start_rejected_gap";
    case WebmSeekCacheLookupStatus::RejectedFuture:
      return "rejected_future";
    case WebmSeekCacheLookupStatus::RejectedInvalid:
      return "rejected_invalid";
    case WebmSeekCacheLookupStatus::ProbeEstimateHit:
      return "probe_estimate_hit";
    case WebmSeekCacheLookupStatus::ProbeEstimateMiss:
      return "probe_estimate_miss";
  }
  return "unknown";
}

static const char *
webm_seek_cache_store_status_name(WebmSeekCacheStoreStatus value) {
  switch (value) {
    case WebmSeekCacheStoreStatus::IgnoredInvalid:
      return "ignored_invalid";
    case WebmSeekCacheStoreStatus::Added:
      return "added";
    case WebmSeekCacheStoreStatus::UpdatedExisting:
      return "updated_existing";
    case WebmSeekCacheStoreStatus::EvictedOldest:
      return "evicted_oldest";
  }
  return "unknown";
}

static const char *
webm_prebuffer_hold_reason_name(WebmPrebufferHoldReason reason) {
  switch (reason) {
    case WebmPrebufferHoldReason::None:
      return "none";
    case WebmPrebufferHoldReason::UserPaused:
      return "user_paused";
    case WebmPrebufferHoldReason::QueueBelowTarget:
      return "queue_below_target";
    case WebmPrebufferHoldReason::BytesBelowRelease:
      return "bytes_below_release";
    case WebmPrebufferHoldReason::WaitingForDecoder:
      return "waiting_for_decoder";
  }
  return "unknown";
}

static const char *webm_stage_name_for_log(WebmPlaybackStage stage) {
  switch (stage) {
    case WebmPlaybackStage::Idle:
      return "idle";
    case WebmPlaybackStage::WaitingForDecoderStart:
      return "waiting_for_decoder_start";
    case WebmPlaybackStage::Prebuffering:
      return "prebuffering";
    case WebmPlaybackStage::Steady:
      return "steady";
    case WebmPlaybackStage::Failed:
      return "failed";
  }
  return "unknown";
}

static void reset_thumbnail_interference_log_state(AppContext *context) {
  if (!context) {
    return;
  }
  context->thumbnail_fetch_defer_reason = NonAudioInterferenceReason::None;
  context->thumbnail_upload_defer_reason = NonAudioInterferenceReason::None;
  context->thumbnail_fetch_defer_since_ms = 0;
  context->thumbnail_upload_defer_since_ms = 0;
  context->thumbnail_fetch_defer_last_log_ms = 0;
  context->thumbnail_upload_defer_last_log_ms = 0;
}

static bool
should_log_thumbnail_interference(NonAudioInterferenceReason current_reason,
                                  NonAudioInterferenceReason *last_reason,
                                  u64 *since_ms, u64 *last_log_ms, u64 now_ms,
                                  u64 *out_waited_ms) {
  if (!last_reason || !since_ms || !last_log_ms || !out_waited_ms) {
    return false;
  }
  *out_waited_ms = 0;
  if (current_reason == NonAudioInterferenceReason::None) {
    if (*since_ms == 0) {
      *last_reason = NonAudioInterferenceReason::None;
      return false;
    }
    *out_waited_ms = now_ms >= *since_ms ? now_ms - *since_ms : 0;
    *last_reason = NonAudioInterferenceReason::None;
    *since_ms = 0;
    *last_log_ms = 0;
    return true;
  }
  if (*since_ms == 0 || *last_reason != current_reason) {
    *last_reason = current_reason;
    *since_ms = now_ms;
    *last_log_ms = now_ms;
    return true;
  }
  *out_waited_ms = now_ms >= *since_ms ? now_ms - *since_ms : 0;
  if (now_ms >= *last_log_ms + 1000ULL) {
    *last_log_ms = now_ms;
    return true;
  }
  return false;
}

static void append_webm_thumbnail_interference_log(
    const char *event, const NonAudioInterferenceInput &input,
    const NonAudioInterferenceDecision &decision, bool track_changed,
    bool thumbnail_loading, bool thumbnail_ready, u64 waited_ms) {
  if (!event) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = now_ms >= g_opus_playback_perf_start_ms
                             ? now_ms - g_opus_playback_perf_start_ms
                             : 0;
  fprintf(f,
          "[webm-thumb] +%llums %s reason=%s stage=%s queued=%d bytes=%lu "
          "complete=%d audio_started=%d loading=%d ready=%d track_changed=%d "
          "waited_ms=%llu\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          non_audio_interference_reason_name(decision.reason),
          webm_stage_name_for_log(input.webm_stage), input.queued_wavebufs,
          static_cast<unsigned long>(input.stream_buffer_bytes),
          input.download_complete ? 1 : 0, input.audio_started ? 1 : 0,
          thumbnail_loading ? 1 : 0, thumbnail_ready ? 1 : 0,
          track_changed ? 1 : 0, static_cast<unsigned long long>(waited_ms));
  fclose(f);
}

static int next_webm_seek_trace_seq() {
  ++g_webm_seek_trace_seq;
  if (g_webm_seek_trace_seq <= 0) {
    g_webm_seek_trace_seq = 1;
  }
  return g_webm_seek_trace_seq;
}

static bool should_log_webm_seek_trace_event(const char *event,
                                             const WebmSeekPlan *plan,
                                             u64 elapsed_ms) {
  if (!event) {
    return false;
  }
  if (strcmp(event, "seek_request") == 0 ||
      strcmp(event, "seek_plan_ready") == 0 ||
      strcmp(event, "first_pcm_after_seek") == 0) {
    return true;
  }
  if (strcmp(event, "seek_info_done") == 0) {
    return elapsed_ms >= 200ULL;
  }
  if (strcmp(event, "seek_index_lookup_done") == 0) {
    return true;
  }
  if (strcmp(event, "seek_probe_done") == 0) {
    return (plan && plan->source != WebmSeekPlanSource::CoarseEstimate) ||
           elapsed_ms >= 200ULL;
  }
  if (strcmp(event, "seek_runtime_cache_store") == 0 ||
      strcmp(event, "seek_runtime_cache_harvest") == 0) {
    return true;
  }
  return false;
}

static void append_webm_seek_trace_log(
    const char *event, int seek_seq, const WebmSeekRequest &request,
    bool repeated_seek, WebmSeekReuseClass reuse_class,
    const WebmSeekPlan *plan, const WebmSeekCacheLookupTrace *cache_trace,
    uint64_t start_byte, int cluster_ms, bool cache_hit, u64 elapsed_ms) {
  if (!should_log_webm_seek_trace_event(event, plan, elapsed_ms)) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const char *seek_case = repeated_seek ? "repeated" : "cold";
  const WebmSeekPlanSource source =
      plan ? plan->source : WebmSeekPlanSource::Invalid;
  const bool cluster_aligned = plan ? plan->cluster_aligned : false;
  const int plan_cluster_ms =
      cluster_ms >= 0 ? cluster_ms : (plan ? plan->selected_cluster_ms : -1);
  const int gap_ms = (request.target_ms > 0 && plan_cluster_ms >= 0)
                         ? (request.target_ms - plan_cluster_ms)
                         : -1;
  const size_t cache_size = cache_trace ? cache_trace->cache_size : 0U;
  const size_t valid_points = cache_trace ? cache_trace->valid_point_count : 0U;
  const size_t invalid_points =
      cache_trace ? cache_trace->invalid_point_count : 0U;
  const size_t future_points =
      cache_trace ? cache_trace->future_point_count : 0U;
  const int max_warm_start_gap_ms =
      cache_trace ? cache_trace->max_warm_start_gap_ms : 0;
  const int best_cluster_ms = cache_trace ? cache_trace->best_cluster_ms : -1;
  const int best_gap_ms = cache_trace ? cache_trace->best_gap_ms : -1;
  const uint64_t best_start_byte =
      cache_trace ? cache_trace->best_start_byte : 0U;
  const uint64_t estimated_probe_start_byte =
      cache_trace ? cache_trace->estimated_probe_start_byte : 0U;
  const char *cache_trace_basis = "none";
  if (cache_trace) {
    cache_trace_basis =
        cache_trace->probe_status != WebmSeekCacheLookupStatus::NotChecked
            ? "probe"
            : (cache_trace->exact_status !=
                       WebmSeekCacheLookupStatus::NotChecked
                   ? "exact"
                   : "none");
  }
  fprintf(
      f,
      "[webm-seek] %s seek_seq=%d target_ms=%d reuse=%s cold_or_repeated=%s "
      "plan_source=%s cluster_aligned=%d start_byte=%llu cluster_ms=%d "
      "gap_ms=%d cache_hit=%d cache_size=%lu exact=%s probe=%s "
      "valid_points=%lu invalid_points=%lu future_points=%lu "
      "cache_trace_basis=%s max_warm_start_gap_ms=%d "
      "warm_start_gap_limit_ms=%d "
      "best_cluster_ms=%d best_gap_ms=%d best_start_byte=%llu "
      "estimated_probe_start_byte=%llu elapsed_ms=%llu\n",
      event, seek_seq, request.target_ms,
      webm_seek_reuse_class_name(reuse_class), seek_case,
      webm_seek_plan_source_name(source), cluster_aligned ? 1 : 0,
      static_cast<unsigned long long>(start_byte), plan_cluster_ms, gap_ms,
      cache_hit ? 1 : 0, static_cast<unsigned long>(cache_size),
      webm_seek_cache_lookup_status_name(
          cache_trace ? cache_trace->exact_status
                      : WebmSeekCacheLookupStatus::NotChecked),
      webm_seek_cache_lookup_status_name(
          cache_trace ? cache_trace->probe_status
                      : WebmSeekCacheLookupStatus::NotChecked),
      static_cast<unsigned long>(valid_points),
      static_cast<unsigned long>(invalid_points),
      static_cast<unsigned long>(future_points), cache_trace_basis,
      max_warm_start_gap_ms, max_warm_start_gap_ms, best_cluster_ms, best_gap_ms,
      static_cast<unsigned long long>(best_start_byte),
      static_cast<unsigned long long>(estimated_probe_start_byte),
      static_cast<unsigned long long>(elapsed_ms));
  fclose(f);
}

static void
append_webm_seek_cache_store_log(const char *event, int seek_seq, int target_ms,
                                 const WebmSeekCacheStoreTrace &store_trace) {
  if (!event) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  fprintf(f,
          "[webm-seek] %s seek_seq=%d target_ms=%d store=%s point_ms=%d "
          "start_byte=%llu cache_size=%lu\n",
          event, seek_seq, target_ms,
          webm_seek_cache_store_status_name(store_trace.status),
          store_trace.timecode_ms,
          static_cast<unsigned long long>(store_trace.start_byte),
          static_cast<unsigned long>(store_trace.cache_size_after));
  fclose(f);
}

static void append_webm_seek_planning_breakdown_log(
    int seek_seq, int target_ms, const WebmSeekPlanningBreakdown &breakdown,
    const WebmSeekPlan &plan, WebmSeekReuseClass reuse_class) {
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  fprintf(
      f,
      "[webm-seek-plan] seek_seq=%d target_ms=%d reuse=%s source=%s "
      "request_to_metadata_ms=%llu metadata_to_cache_ms=%llu "
      "cache_to_probe_ms=%llu probe_to_ready_ms=%llu request_to_ready_ms=%llu "
      "probes_header=%lu probes_cues=%lu probes_cluster=%lu probes_extra=%lu "
      "cached_metadata=%d checked_cues=%d used_cues=%d "
      "used_cluster_probe=%d used_extra_probe=%d\n",
      seek_seq, target_ms, webm_seek_reuse_class_name(reuse_class),
      webm_seek_plan_source_name(plan.source),
      static_cast<unsigned long long>(breakdown.request_to_metadata_ms),
      static_cast<unsigned long long>(breakdown.metadata_to_cache_ms),
      static_cast<unsigned long long>(breakdown.cache_to_probe_ms),
      static_cast<unsigned long long>(breakdown.probe_to_ready_ms),
      static_cast<unsigned long long>(breakdown.request_to_ready_ms),
      static_cast<unsigned long>(breakdown.header_probe_count),
      static_cast<unsigned long>(breakdown.cues_probe_count),
      static_cast<unsigned long>(breakdown.cluster_probe_count),
      static_cast<unsigned long>(breakdown.extra_probe_count),
      breakdown.used_cached_metadata ? 1 : 0, breakdown.checked_cues ? 1 : 0,
      breakdown.used_cues ? 1 : 0, breakdown.used_cluster_probe ? 1 : 0,
      breakdown.used_extra_probe ? 1 : 0);
  fclose(f);
}

static void reset_webm_seek_track_state(AppContext *context) {
  if (!context) {
    return;
  }
  context->webm_seek_track_state = WebmSeekTrackState{};
  context->webm_seek_track_state_video_id.clear();
  context->webm_parser_initial_seed_bytes.clear();
  context->webm_parser_initial_seed_video_id.clear();
}

static bool get_cached_webm_parser_initial_seed(AppContext *context,
                                                const std::string &video_id,
                                                std::string *seed_bytes) {
  if (!context || !seed_bytes || video_id.empty()) {
    return false;
  }
  LightLock_Lock(&context->lock);
  const bool cache_hit =
      context->webm_parser_initial_seed_video_id == video_id &&
      !context->webm_parser_initial_seed_bytes.empty();
  if (cache_hit) {
    seed_bytes->assign(context->webm_parser_initial_seed_bytes.begin(),
                       context->webm_parser_initial_seed_bytes.end());
  }
  LightLock_Unlock(&context->lock);
  return cache_hit;
}

static void store_webm_parser_initial_seed(AppContext *context,
                                           const std::string &video_id,
                                           const std::string &seed_bytes) {
  if (!context || video_id.empty() || seed_bytes.empty()) {
    return;
  }
  LightLock_Lock(&context->lock);
  context->webm_parser_initial_seed_video_id = video_id;
  context->webm_parser_initial_seed_bytes.assign(seed_bytes.begin(),
                                                 seed_bytes.end());
  LightLock_Unlock(&context->lock);
}

static std::string fetch_webm_parser_initial_seed(YouTubeAPI *api,
                                                  AppContext *context,
                                                  const std::string &video_id) {
  std::string seed_bytes;
  if (get_cached_webm_parser_initial_seed(context, video_id, &seed_bytes)) {
    return seed_bytes;
  }
  if (!api || video_id.empty()) {
    return seed_bytes;
  }
  seed_bytes =
      api->get_webm_seek_probe(video_id, 0, kWebmParserInitialSeedBytes);
  if (!seed_bytes.empty()) {
    store_webm_parser_initial_seed(context, video_id, seed_bytes);
  }
  return seed_bytes;
}

static void clear_pending_webm_seek_runtime(AppContext *context) {
  if (!context) {
    return;
  }
  context->pending_stream_seek_ms = 0;
  context->pending_stream_emit_start_ms = 0;
  context->pending_stream_parser_prefetch_byte = 0;
  context->pending_stream_parser_cluster_aligned = false;
  context->pending_stream_seek_seq = 0;
  context->pending_stream_seek_plan = WebmSeekPlan{};
  context->pending_stream_repeated_seek = false;
  context->pending_stream_reuse_class = WebmSeekReuseClass::Cold;
}

static void clear_active_webm_seek_runtime(AppContext *context) {
  if (!context) {
    return;
  }
  context->active_stream_seek_seq = 0;
  context->active_stream_seek_target_ms = 0;
  context->active_stream_seek_plan = WebmSeekPlan{};
  context->active_stream_repeated_seek = false;
  context->active_stream_reuse_class = WebmSeekReuseClass::Cold;
  context->active_stream_seek_plan_ready_at_ms = 0;
  context->active_stream_first_pcm_queued_logged = false;
  context->active_stream_runtime_cache_saved = false;
  context->active_stream_first_pcm_queued_at_ms = 0;
}

struct WebmStartupInfoWarmupTask {
  AppContext *context = nullptr;
  YouTubeAPI *api = nullptr;
  std::string video_id;
  uint64_t preroll_ms = 0;
  uint64_t backtrack_bytes = 0;
};

static void warmup_webm_startup_info_thread(void *arg) {
  std::unique_ptr<WebmStartupInfoWarmupTask> task(
      static_cast<WebmStartupInfoWarmupTask *>(arg));
  if (!task || !task->context || !task->api || task->video_id.empty()) {
    return;
  }

  WebmSeekStreamInfo seek_info = {};
  if (!task->api->get_webm_seek_stream_info(task->video_id, &seek_info) ||
      !seek_info.has_filesize || !seek_info.has_duration_ms ||
      seek_info.filesize == 0 || seek_info.duration_ms == 0) {
    return;
  }

  LightLock_Lock(&task->context->lock);
  const bool same_track =
      task->context->playing_id == task->video_id &&
      task->context->active_stream_mode == StreamContainerMode::ProxyWebmOpus &&
      audio_path_uses_webm_direct(task->context->active_audio_path) &&
      OpusPocPlayer::is_playing;
  if (same_track && !task->context->webm_seek_track_state.source_info_ready) {
    WebmSeekSourceInfo source_info = {};
    source_info.has_filesize = true;
    source_info.filesize = seek_info.filesize;
    source_info.has_duration_ms = true;
    source_info.duration_ms = seek_info.duration_ms;
    source_info.preroll_ms = task->preroll_ms;
    source_info.backtrack_bytes = task->backtrack_bytes;
    task->context->webm_seek_track_state.source_info = source_info;
    task->context->webm_seek_track_state.source_info_ready = true;
    task->context->webm_seek_track_state_video_id = task->video_id;
  }
  task->context->webm_startup_warmup_done = true;
  LightLock_Unlock(&task->context->lock);
}

static void cleanup_finished_webm_startup_warmup_thread(AppContext *context,
                                                        bool wait) {
  if (!context) {
    return;
  }
  Thread warmup_thread = nullptr;
  LightLock_Lock(&context->lock);
  if (context->webm_startup_warmup_thread &&
      (wait || context->webm_startup_warmup_done)) {
    warmup_thread = context->webm_startup_warmup_thread;
    context->webm_startup_warmup_thread = nullptr;
    context->webm_startup_warmup_done = true;
    context->webm_startup_warmup_video_id.clear();
  }
  LightLock_Unlock(&context->lock);
  if (warmup_thread) {
    threadJoin(warmup_thread, U64_MAX);
    threadFree(warmup_thread);
  }
}

static void log_playback_compare_event(StreamContainerMode stream_mode,
                                       PlaybackCompareEvent event,
                                       const OpusPlayerUpdateStats &stats) {
  const PlaybackCompareSnapshot snapshot =
      collect_playback_compare_snapshot(stream_mode, stats);
  playback_observer_log(event, snapshot);
}

static WebmPlaybackControllerInput
make_webm_controller_input(const PlaybackCoreSnapshot &core_snapshot,
                           const OpusPlayerUpdateStats &update_stats,
                           const OpusPocPlayer &player,
                           const AppContext &context) {
  WebmPlaybackControllerInput input = {};
  input.stream_buffer_bytes = core_snapshot.stream_buffer_bytes;
  input.download_complete = core_snapshot.download_complete;
  input.queued_wavebufs = core_snapshot.queued_wavebufs;
  input.free_wavebufs = core_snapshot.free_wavebufs;
  input.decoded_buffers = update_stats.decoded_buffers;
  input.decode_ticks = update_stats.decode_ticks;
  input.has_started_playing = player.has_started_playing();
  input.decode_failed = player.has_decode_failed();
  input.user_paused = context.is_paused;
  return input;
}

static void append_webm_queue_state_log(
    const char *event, PlaybackSessionKind session_kind,
    WebmPlaybackStage stage, const WebmPlaybackControllerInput &input,
    const WebmPlaybackControllerDecision &decision,
    const OpusPlayerUpdateStats &stats, bool seek_active, bool user_paused) {
  if (!event) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = now_ms >= g_opus_playback_perf_start_ms
                             ? now_ms - g_opus_playback_perf_start_ms
                             : 0;
  fprintf(f,
          "[webm-queue] +%llums %s kind=%s stage=%d seek_active=%d "
          "queued_before=%d queued_after=%d free_before=%d free_after=%d "
          "decoded=%d target=%d max_decode=%d release=%d keep_paused=%d "
          "user_paused=%d has_started=%d hold=%s\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          playback_session_kind_name(session_kind), static_cast<int>(stage),
          seek_active ? 1 : 0, stats.queued_before_update,
          stats.queued_after_update, stats.free_before_update,
          stats.free_after_update, stats.decoded_buffers,
          stats.target_queued_wavebufs, stats.max_decode_buffers,
          decision.release_prebuffer ? 1 : 0, decision.keep_ndsp_paused ? 1 : 0,
          user_paused ? 1 : 0, input.has_started_playing ? 1 : 0,
          webm_prebuffer_hold_reason_name(decision.hold_reason));
  fclose(f);
}

static void append_webm_update_pressure_log(
    const char *event, PlaybackSessionKind session_kind,
    WebmPlaybackStage stage, const WebmPlaybackControllerInput &input,
    const WebmPlaybackControllerDecision &decision,
    const OpusPlayerUpdateStats &stats, bool seek_active, bool user_paused) {
  if (!event) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = now_ms >= g_opus_playback_perf_start_ms
                             ? now_ms - g_opus_playback_perf_start_ms
                             : 0;
  fprintf(f,
          "[webm-update] +%llums %s kind=%s stage=%s seek_active=%d "
          "queued_before=%d queued_after=%d free_before=%d free_after=%d "
          "decoded=%d target=%d max_decode=%d decode_ticks=%llu bytes=%lu "
          "complete=%d release=%d keep_paused=%d user_paused=%d "
          "has_started=%d hold=%s\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          playback_session_kind_name(session_kind),
          webm_stage_name_for_log(stage), seek_active ? 1 : 0,
          stats.queued_before_update, stats.queued_after_update,
          stats.free_before_update, stats.free_after_update,
          stats.decoded_buffers, stats.target_queued_wavebufs,
          stats.max_decode_buffers,
          static_cast<unsigned long long>(stats.decode_ticks),
          static_cast<unsigned long>(input.stream_buffer_bytes),
          input.download_complete ? 1 : 0, decision.release_prebuffer ? 1 : 0,
          decision.keep_ndsp_paused ? 1 : 0, user_paused ? 1 : 0,
          input.has_started_playing ? 1 : 0,
          webm_prebuffer_hold_reason_name(decision.hold_reason));
  fclose(f);
}

static bool
should_log_webm_update_pressure(const OpusPlayerUpdateStats &stats,
                                const WebmPlaybackControllerInput &input,
                                bool seek_active, u64 now_ms,
                                const char **out_event) {
  if (!out_event) {
    return false;
  }
  *out_event = NULL;
  if (stats.decode_ticks >= kWebmSlowUpdateTicks) {
    *out_event = "slow_update";
  } else if (input.has_started_playing && stats.decoded_buffers == 0 &&
             stats.queued_after_update <= 2) {
    *out_event = "low_queue_no_decode";
  } else if (seek_active && stats.decoded_buffers == 0 &&
             stats.queued_after_update == 0) {
    *out_event = "seek_wait_no_decode";
  }
  if (!*out_event) {
    return false;
  }
  if (g_webm_update_pressure_last_log_ms != 0 &&
      now_ms < g_webm_update_pressure_last_log_ms + 1000ULL) {
    return false;
  }
  g_webm_update_pressure_last_log_ms = now_ms;
  return true;
}

static void
append_webm_seek_stage_summary_log(const char *event, int seek_seq,
                                   int target_ms, u64 plan_ready_to_current_ms,
                                   u64 request_to_current_ms,
                                   const PlaybackObserverEventTimes &times) {
  if (!event) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const long long request_to_first_byte_ms =
      times.first_byte_seen ? static_cast<long long>(times.first_byte_ms)
                            : -1LL;
  const long long first_byte_to_open_start_ms =
      (times.first_byte_seen && times.decoder_open_start_seen &&
       times.decoder_open_start_ms >= times.first_byte_ms)
          ? static_cast<long long>(times.decoder_open_start_ms -
                                   times.first_byte_ms)
          : -1LL;
  const long long open_start_to_open_ok_ms =
      (times.decoder_open_start_seen && times.decoder_open_ok_seen &&
       times.decoder_open_ok_ms >= times.decoder_open_start_ms)
          ? static_cast<long long>(times.decoder_open_ok_ms -
                                   times.decoder_open_start_ms)
          : -1LL;
  const long long open_ok_to_first_pcm_ms =
      (times.decoder_open_ok_seen && times.first_pcm_queued_seen &&
       times.first_pcm_queued_ms >= times.decoder_open_ok_ms)
          ? static_cast<long long>(times.first_pcm_queued_ms -
                                   times.decoder_open_ok_ms)
          : -1LL;
  const long long first_pcm_to_audible_ms =
      (times.first_pcm_queued_seen && times.audio_audible_seen &&
       times.audio_audible_ms >= times.first_pcm_queued_ms)
          ? static_cast<long long>(times.audio_audible_ms -
                                   times.first_pcm_queued_ms)
          : -1LL;
  fprintf(f,
          "[webm-seek-stage] event=%s seek_seq=%d target_ms=%d "
          "plan_ready_to_current_ms=%llu request_to_current_ms=%llu "
          "request_to_first_byte_ms=%lld first_byte_to_open_start_ms=%lld "
          "open_start_to_open_ok_ms=%lld open_ok_to_first_pcm_ms=%lld "
          "first_pcm_to_audible_ms=%lld audible_seen=%d\n",
          event, seek_seq, target_ms,
          static_cast<unsigned long long>(plan_ready_to_current_ms),
          static_cast<unsigned long long>(request_to_current_ms),
          request_to_first_byte_ms, first_byte_to_open_start_ms,
          open_start_to_open_ok_ms, open_ok_to_first_pcm_ms,
          first_pcm_to_audible_ms, times.audio_audible_seen ? 1 : 0);
  fclose(f);
}

static NonAudioInterferenceInput make_non_audio_interference_input(
    size_t stream_buffer_size, bool stream_download_complete,
    const OpusPocPlayer &player, const AppContext &context) {
  NonAudioInterferenceInput input = {};
  input.stream_mode = context.active_stream_mode;
  input.webm_stage = context.webm_playback_stage;
  input.is_opus_direct = audio_path_uses_webm_direct(context.active_audio_path);
  input.download_complete = stream_download_complete;
  input.stream_buffer_bytes = stream_buffer_size;
  input.queued_wavebufs =
      OpusPocPlayer::is_playing ? player.queued_wavebuf_count() : 0;
  input.audio_started =
      OpusPocPlayer::is_playing && player.has_started_playing();
  return input;
}

static RenderContext make_render_snapshot_locked(const AppContext &context) {
  RenderContext snapshot = {};

  snapshot.current_state = context.current_state;
  snapshot.previous_state = context.previous_state;
  snapshot.g_status_msg = context.g_status_msg;
  snapshot.is_server_connected = context.is_server_connected;
  snapshot.playing_title_lines = context.playing_title_lines;
  snapshot.home_selected_index = context.home_selected_index;
  snapshot.selected_index = context.selected_index;
  snapshot.scroll_x = context.scroll_x;
  snapshot.popup_selected_index = context.popup_selected_index;
  snapshot.selected_playlist_id = context.selected_playlist_id;
  snapshot.selected_track_id = context.selected_track_id;
  snapshot.playing_id = context.playing_id;
  snapshot.playing_duration = context.playing_duration;
  snapshot.playing_meta = context.playing_meta;
  snapshot.is_paused = context.is_paused;
  snapshot.active_playlist_id = context.active_playlist_id;
  snapshot.active_playlist_name = context.active_playlist_name;
  snapshot.current_track_idx = context.current_track_idx;
  snapshot.play_queue = context.play_queue;
  snapshot.shuffle_mode = context.shuffle_mode;
  snapshot.loop_mode = context.loop_mode;
  snapshot.mode_btn_focus = context.mode_btn_focus;
  snapshot.playback_start_time = context.playback_start_time;
  snapshot.pause_accumulated_ms = context.pause_accumulated_ms;
  snapshot.pause_started_at = context.pause_started_at;
  snapshot.is_buffering = context.is_buffering;
  snapshot.touch_state = context.touch_state;
  snapshot.scroll_offset_y = context.scroll_offset_y;
  snapshot.config = context.config;
  snapshot.theme = context.theme;

  const AppState active_state = is_popup_state(context.current_state)
                                    ? context.previous_state
                                    : context.current_state;

  const bool need_playlists =
      active_state == STATE_HOME || active_state == STATE_PLAYLISTS ||
      context.current_state == STATE_POPUP_PLAYLIST_ADD ||
      context.current_state == STATE_POPUP_PLAYLIST_OPTIONS ||
      context.current_state == STATE_POPUP_QA_ADD;
  const bool need_g_tracks =
      active_state == STATE_PLAYLIST_DETAIL ||
      (context.current_state == STATE_POPUP_TRACK_DETAILS &&
       context.previous_state == STATE_PLAYLIST_DETAIL);
  const bool need_search_tracks =
      active_state == STATE_SEARCH ||
      (context.current_state == STATE_POPUP_TRACK_DETAILS &&
       context.previous_state == STATE_SEARCH);
  const bool need_playing_tracks =
      active_state == STATE_PLAYING_UI ||
      context.current_state == STATE_POPUP_TRACK_DETAILS;

  if (need_playlists) {
    snapshot.playlists = context.playlists;
  }
  if (need_g_tracks) {
    snapshot.g_tracks = context.g_tracks;
  }
  if (need_search_tracks) {
    snapshot.search_tracks = context.search_tracks;
  }
  if (need_playing_tracks) {
    snapshot.playing_tracks = context.playing_tracks;
  }

  return snapshot;
}

static void append_opus_playback_perf_log(const char *event, size_t bytes) {
#if STREAMU_ENABLE_OPUS_PERF_LOG
  if (!event || g_opus_playback_perf_start_ms == 0) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/opus_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = now_ms >= g_opus_playback_perf_start_ms
                             ? now_ms - g_opus_playback_perf_start_ms
                             : 0;
  fprintf(f, "[opus-perf] +%llums %s bytes=%lu\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          static_cast<unsigned long>(bytes));
  fclose(f);
#else
  (void)event;
  (void)bytes;
#endif
}

static void append_opus_playback_perf_log(OpusPerfEvent event, size_t bytes) {
  append_opus_playback_perf_log(opus_perf_event_name(event), bytes);
}

static void append_opus_playback_perf_log(OpusPerfEvent event,
                                          const OpusPerfSnapshot &snapshot) {
#if STREAMU_ENABLE_OPUS_PERF_LOG
  if (g_opus_playback_perf_start_ms == 0) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/opus_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = now_ms >= g_opus_playback_perf_start_ms
                             ? now_ms - g_opus_playback_perf_start_ms
                             : 0;
  fprintf(f, "[opus-perf] +%llums %s bytes=%lu queued=%d free=%d decoded=%d\n",
          static_cast<unsigned long long>(elapsed_ms),
          opus_perf_event_name(event),
          static_cast<unsigned long>(snapshot.stream_buffer_bytes),
          snapshot.queued_wavebufs, snapshot.free_wavebufs,
          snapshot.decoded_chunks_in_frame);
  fclose(f);
#else
  (void)event;
  (void)snapshot;
#endif
}

static OpusPerfSnapshot
make_opus_perf_snapshot(const OpusPipelineState &pipeline_state,
                        const OpusPlayerUpdateStats &update_stats) {
  OpusPerfSnapshot snapshot = {};
  snapshot.stream_buffer_bytes = pipeline_state.stream_buffer_bytes;
  snapshot.queued_wavebufs = pipeline_state.queued_wavebufs;
  snapshot.free_wavebufs = pipeline_state.free_wavebufs;
  snapshot.decoded_chunks_in_frame = update_stats.decoded_buffers;
  return snapshot;
}

static void ensure_streamu_data_dir() { mkdir("sdmc:/3ds/StreaMu", 0777); }

static void append_startup_perf_log(const char *event, const char *detail) {
  if (!event) {
    return;
  }
  ensure_streamu_data_dir();
  FILE *f = fopen("sdmc:/3ds/StreaMu/startup_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (g_startup_perf_start_ms > 0 && now_ms >= g_startup_perf_start_ms)
          ? now_ms - g_startup_perf_start_ms
          : 0;
  fprintf(f, "[startup] +%llums %s",
          static_cast<unsigned long long>(elapsed_ms), event);
  if (detail && detail[0] != '\0') {
    fprintf(f, " %s", detail);
  }
  fputc('\n', f);
  fclose(f);
}

static void append_startup_result_log(const char *event, Result rc) {
  char detail[64];
  snprintf(detail, sizeof(detail), "rc=0x%08lX",
           static_cast<unsigned long>(rc));
  append_startup_perf_log(event, detail);
}

#include "app_context.h"
#include "config_manager.h"
#include "stb_image.h"
#include "ui/screen_manager.h"
#include "ui/screens/home_screen.h"
#include "ui/screens/playing_screen.h"
#include "ui/screens/playlist_detail_screen.h"
#include "ui/screens/playlists_screen.h"
#include "ui/screens/search_screen.h"
#include "ui/screens/settings_screen.h"
#include "ui/wallpaper.h"

// Use smart pointers to ensure destructors run before socExit (prevents crash).
std::unique_ptr<OpusPocPlayer> g_opus_player_ptr;
#define opus_player (*g_opus_player_ptr)

std::string url_encode(const std::string &value) {
  std::ostringstream escaped;
  escaped << std::setfill('0');
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

static bool validate_ip_port(const std::string &input) {
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

static std::string show_ip_keyboard(const std::string &initial) {
  std::string current = initial;
  while (true) {
    SwkbdState swkbd;
    char buf[64] = {0};
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, 63);
    if (!current.empty()) swkbdSetInitialText(&swkbd, current.c_str());
    SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));
    if (button != SWKBD_BUTTON_CONFIRM || strlen(buf) == 0) return "";
    std::string input(buf);
    if (validate_ip_port(input)) return input;
    // Validation failed: keep input and retry
    current = input;
  }
}

static constexpr size_t OPUS_STREAM_START_BYTES = 16U * 1024U;
static const WebmPlaybackControllerConfig kWebmPlaybackControllerConfig =
    webm_playback_controller_default_config();

static WebmPlaybackControllerConfig
controller_config_for_seek(bool seek_active) {
  WebmPlaybackControllerConfig config = kWebmPlaybackControllerConfig;
  if (seek_active) {
    config.decoder_start_bytes = 0U;
    config.playback_release_bytes = 0U;
    config.initial_wavebuf_target = 2;
  }
  return config;
}

// Parser-level seek against the current "header + ranged cluster relay" stream
// is not reliable yet. Keep the older 3DS-side coarse seek path active until
// we have true random-access backed IO for nestegg.
static const bool kWebmUseParserLevelSeek = true;
static const uint64_t kWebmSeekRetryBacktrackBytes[] = {
    128ULL * 1024ULL,
    512ULL * 1024ULL,
    1024ULL * 1024ULL,
};

static uint64_t backtrack_bytes_for_retry(int retry_count) {
  if (retry_count <= 0) {
    return kWebmSeekRetryBacktrackBytes[0];
  }
  const size_t max_index = (sizeof(kWebmSeekRetryBacktrackBytes) /
                            sizeof(kWebmSeekRetryBacktrackBytes[0])) -
                           1U;
  const size_t index = static_cast<size_t>(retry_count) > max_index
                           ? max_index
                           : static_cast<size_t>(retry_count);
  return kWebmSeekRetryBacktrackBytes[index];
}

ThemeColors g_theme_colors;
std::unique_ptr<AppContext> g_ctx_ptr;

#define ctx (*g_ctx_ptr)

std::unique_ptr<PlaylistManager> g_playlist_manager_ptr;
#define playlist_manager (*g_playlist_manager_ptr)

C2D_TextBuf g_staticBuf; // Temporarily kept for compatibility

struct StartupConnectionCheck {
  LightLock lock;
  YouTubeAPI api;
  bool done = false;
  bool result = false;
  u64 elapsed_ms = 0;
  int attempt = 0;
  long timeout_ms = 300;
};

static void startup_connection_check_thread(void *arg) {
  StartupConnectionCheck *check = static_cast<StartupConnectionCheck *>(arg);
  const u64 start_ms = osGetTime();
  bool result = check->api.check_connection(check->timeout_ms);
  const u64 end_ms = osGetTime();
  LightLock_Lock(&check->lock);
  check->result = result;
  check->done = true;
  check->elapsed_ms = (end_ms >= start_ms) ? (end_ms - start_ms) : 0;
  LightLock_Unlock(&check->lock);
}

static void reset_startup_connection_check(StartupConnectionCheck &check,
                                           const std::string &server_ip,
                                           int attempt, long timeout_ms) {
  LightLock_Lock(&check.lock);
  check.done = false;
  check.result = false;
  check.elapsed_ms = 0;
  check.attempt = attempt;
  check.timeout_ms = timeout_ms;
  LightLock_Unlock(&check.lock);
  check.api.set_server_ip(server_ip);
}

static bool poll_startup_connection_check(StartupConnectionCheck &check,
                                          bool &result, u64 &elapsed_ms,
                                          int &attempt) {
  LightLock_Lock(&check.lock);
  bool done = check.done;
  result = check.result;
  elapsed_ms = check.elapsed_ms;
  attempt = check.attempt;
  LightLock_Unlock(&check.lock);
  return done;
}

void update_playing_title_lines(C2D_TextBuf buf) {
  ctx.playing_title_lines.clear();
  if (ctx.playing_title.empty()) return;

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
        while (len < remaining.length() && (remaining[len] & 0xC0) == 0x80)
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
      ctx.playing_title_lines.push_back(remaining.substr(0, best_len) + "...");
    }
  }
}

// Thumbnail download thread: fetch JPEG, decode, crop to square, store pixels
static void thumbnail_dl_thread(void *arg) {
  AppContext *c = static_cast<AppContext *>(arg);

  LightLock_Lock(&c->lock);
  std::string vid_id = c->thumbnail_vid_id;
  YouTubeAPI *api = c->api;
  LightLock_Unlock(&c->lock);

  if (!api || vid_id.empty()) {
    LightLock_Lock(&c->lock);
    c->thumbnail_loading = false;
    LightLock_Unlock(&c->lock);
    return;
  }

  std::vector<uint8_t> raw;
  bool ok = api->download_thumbnail(vid_id, raw);

  if (ok && !raw.empty()) {
    int w = 0, h = 0, ch = 0;
    u8 *rgba =
        stbi_load_from_memory(raw.data(), (int)raw.size(), &w, &h, &ch, 4);
    if (rgba && w > 0 && h > 0) {
      int crop = w < h ? w : h;
      int ox = (w - crop) / 2;
      int oy = (h - crop) / 2;
      std::vector<uint8_t> pixels(crop * crop * 4);
      for (int row = 0; row < crop; row++) {
        memcpy(pixels.data() + row * crop * 4, rgba + ((oy + row) * w + ox) * 4,
               crop * 4);
      }
      stbi_image_free(rgba);

      LightLock_Lock(&c->lock);
      c->thumbnail_pixels = std::move(pixels);
      c->thumbnail_crop_size = crop;
      c->thumbnail_ready = true;
      c->thumbnail_loading = false;
      LightLock_Unlock(&c->lock);
      return;
    }
    if (rgba) stbi_image_free(rgba);
  }

  LightLock_Lock(&c->lock);
  c->thumbnail_loading = false;
  LightLock_Unlock(&c->lock);
}

void download_thread(void *arg) {
  YouTubeAPI *api = static_cast<YouTubeAPI *>(arg);
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
          is_online = api->check_connection(1000L);
        }

        LightLock_Lock(&ctx.lock);
        if (ok && !res.empty()) {
          ctx.search_tracks = res;
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
    std::string stream_video_id = "";
    StreamContainerMode stream_mode = StreamContainerMode::ProxyOggOpus;
    int pending_seek_ms = 0;
    uint64_t pending_seek_backtrack_bytes = 0;
    int pending_seek_retry_count = 0;
    int pending_seek_seq = 0;
    WebmSeekTrackState webm_seek_track_state = {};
    std::string webm_seek_track_state_video_id = "";
    LightLock_Lock(&ctx.lock);
    if (ctx.is_downloading && ctx.current_stream_url != "") {
      stream_url = ctx.current_stream_url;
      stream_video_id = ctx.playing_id;
      stream_mode = ctx.active_stream_mode;
      pending_seek_ms = ctx.pending_stream_seek_ms;
      pending_seek_backtrack_bytes = ctx.pending_seek_backtrack_bytes;
      pending_seek_retry_count = ctx.pending_seek_retry_count;
      pending_seek_seq = ctx.pending_stream_seek_seq;
      webm_seek_track_state = ctx.webm_seek_track_state;
      webm_seek_track_state_video_id = ctx.webm_seek_track_state_video_id;
      ctx.current_stream_url = "";
    }
    LightLock_Unlock(&ctx.lock);

    if (stream_url != "") {
      if (stream_mode == StreamContainerMode::ProxyWebmOpus &&
          pending_seek_ms > 0 && !stream_video_id.empty()) {
        const WebmSeekRequest seek_request = {
            pending_seek_ms,
            pending_seek_seq > 0 ? pending_seek_seq
                                 : next_webm_seek_trace_seq(),
        };
        append_webm_seek_plan_log(kWebmUseParserLevelSeek ? "seek_mode_parser"
                                                          : "seek_mode_coarse",
                                  pending_seek_ms, 0, 0);
        PreparedWebmSeekPlanResult prepared_seek = prepare_webm_seek_plan(
            api, stream_video_id, seek_request, pending_seek_backtrack_bytes,
            pending_seek_retry_count, kWebmSeekPlanPrerollMs,
            kWebmSeekHeaderProbeSizeBytes, kWebmSeekProbeSizeBytes,
            backtrack_bytes_for_retry, append_webm_seek_trace_log,
            &webm_seek_track_state);
        LightLock_Lock(&ctx.lock);
        ctx.webm_seek_track_state = webm_seek_track_state;
        ctx.webm_seek_track_state_video_id = webm_seek_track_state_video_id;
        if (prepared_seek.plan_ok) {
          stream_url = prepared_seek.stream_url;
          append_webm_seek_plan_log(
              kWebmUseParserLevelSeek ? "parser_seek_plan_ready"
                                      : "seek_plan_ready",
              prepared_seek.seek_plan.seek_target_ms,
              prepared_seek.seek_plan.reconnect_start_byte,
              prepared_seek.seek_plan.emit_start_ms);
          append_webm_seek_planning_breakdown_log(
              seek_request.seek_seq, prepared_seek.seek_plan.seek_target_ms,
              prepared_seek.planning_breakdown, prepared_seek.seek_plan,
              prepared_seek.reuse_class);
          ctx.pending_stream_seek_ms = prepared_seek.seek_plan.seek_target_ms;
          ctx.pending_stream_emit_start_ms =
              prepared_seek.seek_plan.emit_start_ms;
          ctx.pending_stream_parser_prefetch_byte =
              kWebmUseParserLevelSeek
                  ? prepared_seek.seek_plan.reconnect_start_byte
                  : 0;
          ctx.pending_stream_parser_cluster_aligned =
              kWebmUseParserLevelSeek && prepared_seek.parser_cluster_aligned;
          ctx.pending_stream_seek_seq = seek_request.seek_seq;
          ctx.pending_stream_seek_plan = prepared_seek.seek_plan;
          ctx.pending_stream_repeated_seek = prepared_seek.repeated_seek;
          ctx.pending_stream_reuse_class = prepared_seek.reuse_class;
          ctx.active_stream_seek_target_ms =
              prepared_seek.seek_plan.seek_target_ms;
          ctx.active_stream_seek_plan_ready_at_ms = osGetTime();
          ctx.webm_seek_track_state.source_info = prepared_seek.source_info;
          ctx.webm_seek_track_state.source_info_ready = true;
          ctx.webm_seek_track_state_video_id = stream_video_id;
        } else {
          append_webm_seek_plan_log(
              prepared_seek.metadata_ok
                  ? (kWebmUseParserLevelSeek ? "parser_seek_plan_invalid"
                                             : "seek_plan_invalid")
                  : (kWebmUseParserLevelSeek ? "parser_seek_metadata_failed"
                                             : "seek_plan_metadata_failed"),
              pending_seek_ms, 0, 0);
          clear_pending_webm_seek_runtime(&ctx);
          ctx.webm_seek_track_state.source_info = WebmSeekSourceInfo{};
          ctx.webm_seek_track_state.source_info_ready = false;
          ctx.webm_seek_track_state_video_id = stream_video_id;
          clear_active_webm_seek_runtime(&ctx);
        }
        LightLock_Unlock(&ctx.lock);
      }

      bool success = false;
      if (stream_mode == StreamContainerMode::ProxyWebmOpus &&
          pending_seek_ms > 0 && !stream_video_id.empty() &&
          kWebmUseParserLevelSeek) {
        const std::string initial_bytes =
            fetch_webm_parser_initial_seed(api, &ctx, stream_video_id);
        LightLock_Lock(&stream_lock);
        if (g_stream_buffer_ptr) {
          g_stream_buffer_ptr->clear();
          if (!initial_bytes.empty()) {
            g_stream_buffer_ptr->insert(g_stream_buffer_ptr->end(),
                                        initial_bytes.begin(),
                                        initial_bytes.end());
          }
        }
        g_stream_download_complete = false;
        LightLock_Unlock(&stream_lock);
        success = !initial_bytes.empty();
      } else {
        success = api->start_streaming(stream_url, stream_mode);
      }

      // Retry only if server is reachable (e.g. transient extract error)
      if (!success && !YouTubeAPI::should_cancel) {
        if (api->check_connection(1000L)) {
          svcSleepThread(500ULL * 1000 * 1000); // 0.5s wait
          if (!YouTubeAPI::should_cancel) {
            if (stream_mode == StreamContainerMode::ProxyWebmOpus &&
                pending_seek_ms > 0 && !stream_video_id.empty() &&
                kWebmUseParserLevelSeek) {
              const std::string initial_bytes =
                  fetch_webm_parser_initial_seed(api, &ctx, stream_video_id);
              LightLock_Lock(&stream_lock);
              if (g_stream_buffer_ptr) {
                g_stream_buffer_ptr->clear();
                if (!initial_bytes.empty()) {
                  g_stream_buffer_ptr->insert(g_stream_buffer_ptr->end(),
                                              initial_bytes.begin(),
                                              initial_bytes.end());
                }
              }
              g_stream_download_complete = false;
              LightLock_Unlock(&stream_lock);
              success = !initial_bytes.empty();
            } else {
              success = api->start_streaming(stream_url, stream_mode);
            }
          }
        }
      }

      LightLock_Lock(&ctx.lock);
      ctx.is_downloading = false;
      // Don't change connection state if failure was due to cancellation
      if (!YouTubeAPI::should_cancel) {
        const bool playback_started =
            OpusPocPlayer::is_playing && opus_player.has_started_playing();
        bool is_online = success ? true : api->check_connection(1000L);
        ctx.is_server_connected = is_online;
        if (!success && !playback_started) {
          ctx.is_buffering = false;
          clear_pending_webm_seek_runtime(&ctx);
          ctx.webm_seek_track_state.source_info = WebmSeekSourceInfo{};
          ctx.webm_seek_track_state.source_info_ready = false;
          clear_active_webm_seek_runtime(&ctx);
          ctx.opus_playback_failure = OpusPlaybackFailure::Network;
          if (!ctx.is_paused && ctx.pause_started_at > 0) {
            ctx.pause_accumulated_ms += osGetTime() - ctx.pause_started_at;
            ctx.pause_started_at = 0;
          }
          ctx.g_status_msg =
              is_online ? "Opus Error (Check Proxy)"
                        : playback_failure_message(ctx.opus_playback_failure);
          OpusPocPlayer::is_playing = false;
          ctx.webm_playback_stage = WebmPlaybackStage::Idle;
          ctx.playing_id = "";
          reset_thumbnail_interference_log_state(&ctx);
          playback_observer_end_session();
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
// pulse_frame: if >= 0, animates an ASCII spinner next to the label (step 4)
void draw_loading_screen(UIManager &ui_mgr, C2D_TextBuf buf,
                         const ThemeColors *theme, int step, int total,
                         const char *label, int pulse_frame = -1,
                         const char *bottom_hint = nullptr) {
  ui_mgr.begin_top_screen(theme->bg_top);
  C2D_TextBufClear(buf);

  const float screen_w = 400.0f;
  const float bar_w = 240.0f;
  const float bar_h = 16.0f;
  const float bar_x = (screen_w - bar_w) / 2.0f; // = 80
  const float bar_y = 110.0f;

  // Background bar (dark gray)
  C2D_DrawRectSolid(bar_x, bar_y, 0, bar_w, bar_h,
                    C2D_Color32(60, 60, 60, 255));

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

  // Step 4 waits on network I/O. Keep the bar fixed so it reads as waiting,
  // not as progress that moves backward.

  // Bar border (outline)
  C2D_DrawRectSolid(bar_x, bar_y, 0, bar_w, 1, C2D_Color32(120, 120, 120, 255));
  C2D_DrawRectSolid(bar_x, bar_y + bar_h - 1, 0, bar_w, 1,
                    C2D_Color32(120, 120, 120, 255));
  C2D_DrawRectSolid(bar_x, bar_y, 0, 1, bar_h, C2D_Color32(120, 120, 120, 255));
  C2D_DrawRectSolid(bar_x + bar_w - 1, bar_y, 0, 1, bar_h,
                    C2D_Color32(120, 120, 120, 255));

  // Step text below bar (centered)
  C2D_Text text;
  char step_text[64];
  if (pulse_frame >= 0) {
    static const char spinner[] = {'|', '/', '-', '\\'};
    char spin = spinner[(pulse_frame / 8) % 4];
    snprintf(step_text, sizeof(step_text), "Step %d/%d: %s %c", step, total,
             label, spin);
  } else {
    snprintf(step_text, sizeof(step_text), "Step %d/%d: %s", step, total,
             label);
  }
  C2D_TextParse(&text, buf, step_text);
  C2D_TextOptimize(&text);
  float text_w = text.width * 0.5f;      // scale 0.5
  float text_x = 200.0f - text_w / 2.0f; // center on screen (400/2=200)
  C2D_DrawText(&text, C2D_AtBaseline | C2D_WithColor, text_x,
               bar_y + bar_h + 20, 0, 0.5f, 0.5f,
               C2D_Color32(180, 180, 180, 255));

  ui_mgr.begin_bottom_screen(theme->bg_bottom);
  if (bottom_hint) {
    C2D_TextParse(&text, buf, bottom_hint);
    C2D_TextOptimize(&text);
    float hint_x = 160.0f - (text.width * 0.6f) / 2.0f;
    C2D_DrawText(&text, C2D_AtBaseline | C2D_WithColor, hint_x, 120.0f, 0, 0.6f,
                 0.6f, C2D_Color32(120, 120, 120, 255));
  }
  ui_mgr.end_frame();
}

int main(int argc, char *argv[]) {
  g_startup_perf_start_ms = osGetTime();
  append_startup_perf_log("boot-start", "");

  // Dynamic allocation (RAII) to ensure destructors run before OS exit (socExit
  // etc.)
  g_ctx_ptr = std::make_unique<AppContext>();
  g_opus_player_ptr = std::make_unique<OpusPocPlayer>();
  g_playlist_manager_ptr = std::make_unique<PlaylistManager>();
  g_stream_buffer_ptr = std::make_unique<std::vector<uint8_t>>();
  append_startup_perf_log("alloc-core-state", "");

  LightLock_Init(&ctx.lock);
  LightLock_Init(&stream_lock);
  append_startup_perf_log("locks-ready", "");

  srand(time(NULL));
  append_startup_perf_log("rng-ready", "");

  // Dynamically allocated for manual destruction before system exit
  auto g_ui_mgr_ptr = std::make_unique<UIManager>();
  UIManager &ui_mgr = *g_ui_mgr_ptr;
  append_startup_perf_log("ui-init-begin", "");
  if (!ui_mgr.init()) {
    append_startup_perf_log("ui-init-failed", "");
    return 1;
  }
  if (!init_ui_icon_cache()) {
    append_startup_perf_log("ui-icon-cache-init-failed", "");
    return 1;
  }
  append_startup_perf_log("ui-init-ok", "");

  // Load and apply theme settings
  ConfigManager::load(ctx.config);
  apply_theme(ctx.config, g_theme_colors);
  ctx.theme = &g_theme_colors;
  append_startup_perf_log("config-theme-ready", "");
  auto g_renderer_ptr = std::make_unique<UIRenderer>(ui_mgr, g_theme_colors);
  UIRenderer &renderer = *g_renderer_ptr;

  Wallpaper g_wallpaper;
  renderer.set_wallpaper(&g_wallpaper);
  renderer.set_thumbnail(&ctx.thumbnail_tex);

  g_staticBuf = ui_mgr.get_text_buf();

  // --- Step 1/4: System Init ---
  draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 1, 4,
                      "Initializing system...");
  const u64 step1_start_ms = osGetTime();

  aptSetSleepAllowed(false);
  append_startup_perf_log("apt-sleep-disabled", "");
  osSetSpeedupEnable(
      true); // Enable New 3DS 804MHz CPU + L2 cache (no-op on Old 3DS)
  append_startup_perf_log("cpu-speedup-requested", "");
  const Result romfs_rc = romfsInit();
  append_startup_result_log("romfs-init", romfs_rc);
  const Result ndsp_rc = ndspInit();
  append_startup_result_log("ndsp-init", ndsp_rc);
  const Result ndmu_rc = ndmuInit();
  append_startup_result_log("ndmu-init", ndmu_rc);
  const Result ndmu_state_rc =
      NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
  append_startup_result_log("ndmu-exclusive", ndmu_state_rc);
  bool opus_ready = opus_player.init();
  append_startup_perf_log(
      opus_ready ? "opus-player-init-ok" : "opus-player-init-failed", "");
  ctx.active_audio_path = ctx.config.audio_path;
  const Result ptmu_rc = ptmuInit();
  append_startup_result_log("ptmu-init", ptmu_rc);
  u32 *soc_buffer = static_cast<u32 *>(memalign(0x1000, 0x100000));
  append_startup_perf_log(
      soc_buffer ? "soc-buffer-alloc-ok" : "soc-buffer-alloc-failed", "");
  if (soc_buffer) {
    const Result soc_rc = socInit(soc_buffer, 0x100000);
    append_startup_result_log("soc-init", soc_rc);
  }

  auto g_api_ptr = std::make_unique<YouTubeAPI>();
  YouTubeAPI &api = *g_api_ptr;
  api.init();
  append_startup_perf_log("youtube-api-init-ok", "");
  ctx.api = &api;
  {
    char detail[64];
    const u64 now_ms = osGetTime();
    const u64 duration_ms =
        now_ms >= step1_start_ms ? now_ms - step1_start_ms : 0;
    snprintf(detail, sizeof(detail), "duration_ms=%llu",
             static_cast<unsigned long long>(duration_ms));
    append_startup_perf_log("step1-system-init", detail);
  }

  // Load wallpaper texture (after system init so loading screen is visible
  // first)
  const u64 wallpaper_start_ms = osGetTime();
  if (!ctx.config.wallpaper_file.empty()) {
    std::string wp_path =
        std::string("sdmc:/3ds/StreaMu/wallpaper/") + ctx.config.wallpaper_file;
    g_wallpaper.load(wp_path);
  }
  {
    char detail[96];
    const u64 now_ms = osGetTime();
    const u64 duration_ms =
        now_ms >= wallpaper_start_ms ? now_ms - wallpaper_start_ms : 0;
    snprintf(detail, sizeof(detail), "duration_ms=%llu loaded=%s",
             static_cast<unsigned long long>(duration_ms),
             ctx.config.wallpaper_file.empty() ? "false" : "true");
    append_startup_perf_log("wallpaper-load", detail);
  }

  // --- Step 2/4: Config loaded ---
  draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 2, 4,
                      "Loading config...");

  const u64 playlist_start_ms = osGetTime();
  playlist_manager.init();
  ctx.playlists = playlist_manager.get_playlists();
  {
    char detail[96];
    const u64 now_ms = osGetTime();
    const u64 duration_ms =
        now_ms >= playlist_start_ms ? now_ms - playlist_start_ms : 0;
    snprintf(detail, sizeof(detail), "duration_ms=%llu playlists=%lu",
             static_cast<unsigned long long>(duration_ms),
             static_cast<unsigned long>(ctx.playlists.size()));
    append_startup_perf_log("playlist-load", detail);
  }

  // --- Step 3/4: Playlists loaded ---
  draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 3, 4,
                      "Loading playlists...");

  // UI Architecture Phase 1: ScreenManager Setup
  ScreenManager screen_mgr;
  screen_mgr.add_screen("HomeScreen", std::make_unique<HomeScreen>());
  screen_mgr.add_screen("SettingsScreen", std::make_unique<SettingsScreen>(
                                              g_theme_colors, &g_wallpaper));
  screen_mgr.add_screen("SearchScreen", std::make_unique<SearchScreen>());
  screen_mgr.add_screen("PlaylistsScreen", std::make_unique<PlaylistsScreen>());
  screen_mgr.add_screen("PlaylistDetailScreen",
                        std::make_unique<PlaylistDetailScreen>());
  screen_mgr.add_screen("PlayingScreen", std::make_unique<PlayingScreen>());
  screen_mgr.change_screen(ctx, "HomeScreen");

  // Server IP setup (prompt with swkbd if not set)
  if (ctx.config.server_ip.empty()) {
    // Show IP guidance on top screen before swkbd
    ui_mgr.begin_top_screen(ctx.theme->bg_top);
    C2D_TextBufClear(g_staticBuf);
    C2D_Text text;
    C2D_TextParse(&text, g_staticBuf,
                  "Enter the IP address of the PC\nrunning proxy.py\n(e.g. "
                  "192.168.1.10)");
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
  ctx.is_server_connected = false;
  int anim_counter = 0;
  bool is_timeout = false;
  int timeout_selected_index = 0;
  bool is_confirming_exit = false;
  int confirm_selected_index = 0;
  u64 connect_start_ms = osGetTime();
  u64 next_startup_check_ms = connect_start_ms;
  int startup_check_attempt_count = 0;
  bool startup_timeout_logged = false;
  StartupConnectionCheck startup_check;
  LightLock_Init(&startup_check.lock);
  Thread startup_check_thread = nullptr;

  auto cleanup_startup_check_thread = [&]() {
    if (startup_check_thread) {
      threadJoin(startup_check_thread, U64_MAX);
      threadFree(startup_check_thread);
      startup_check_thread = nullptr;
    }
  };

  auto start_startup_check = [&]() {
    cleanup_startup_check_thread();
    startup_check_attempt_count++;
    const long timeout_ms = 300L;
    reset_startup_connection_check(startup_check, ctx.config.server_ip,
                                   startup_check_attempt_count, timeout_ms);
    {
      char detail[128];
      snprintf(detail, sizeof(detail), "attempt=%d timeout_ms=%ld server=%s",
               startup_check_attempt_count, timeout_ms,
               ctx.config.server_ip.c_str());
      append_startup_perf_log("startup-connect-begin", detail);
    }
    startup_check_thread =
        threadCreate(startup_connection_check_thread, &startup_check, 0x8000,
                     0x3F, -2, false);
  };

  auto enter_startup_timeout = [&]() {
    if (!startup_timeout_logged) {
      char detail[64];
      const u64 now_ms = osGetTime();
      const u64 duration_ms =
          now_ms >= connect_start_ms ? now_ms - connect_start_ms : 0;
      snprintf(detail, sizeof(detail), "duration_ms=%llu",
               static_cast<unsigned long long>(duration_ms));
      append_startup_perf_log("startup-connect-timeout", detail);
      startup_timeout_logged = true;
    }
    cleanup_startup_check_thread();
    is_timeout = true;
    is_confirming_exit = false;
    timeout_selected_index = 1; // Change IP
  };

  if (!ctx.config.server_ip.empty()) {
    start_startup_check();
  } else {
    is_timeout = true;
    timeout_selected_index = 1; // Change IP
  }

  while (aptMainLoop() && !ctx.is_server_connected) {
    if (!is_timeout) {
      // Fixed progress bar with a small text spinner while checking the server.
      draw_loading_screen(ui_mgr, g_staticBuf, ctx.theme, 4, 4,
                          "Connecting to server...", anim_counter, "B: Cancel");
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
          snprintf(line, sizeof(line), "%s %s",
                   (timeout_selected_index == i) ? ">" : " ", labels[i]);
          C2D_TextParse(&text, g_staticBuf, line);
          C2D_DrawText(&text, C2D_AtBaseline | C2D_WithColor, 130,
                       130.0f + i * 30.0f, 0, 0.7f, 0.7f,
                       (timeout_selected_index == i)
                           ? C2D_Color32(50, 200, 50, 255)
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
          if (timeout_selected_index == 0) { // Retry
            is_timeout = false;
            connect_start_ms = osGetTime();
            next_startup_check_ms = connect_start_ms;
            startup_timeout_logged = false;
            start_startup_check();
          } else if (timeout_selected_index == 1) { // Change IP
            std::string ip = show_ip_keyboard(ctx.config.server_ip);
            if (!ip.empty()) {
              ctx.config.server_ip = ip;
              ConfigManager::save(ctx.config);
              api.set_server_ip(ctx.config.server_ip);
            }
            is_timeout = false;
            connect_start_ms = osGetTime();
            next_startup_check_ms = connect_start_ms;
            startup_timeout_logged = false;
            start_startup_check();
          } else { // Exit
            is_confirming_exit = true;
            confirm_selected_index = 0;
          }
        }
      } else {
        if (kRepeat & KEY_DRIGHT) confirm_selected_index = 1;
        if (kRepeat & KEY_DLEFT) confirm_selected_index = 0;

        if (kDown & KEY_A) {
          if (confirm_selected_index == 1) {
            ctx.is_running = false;
            break;
          } else
            is_confirming_exit = false;
        }
      }
    } else {
      if (kDown & KEY_B) {
        enter_startup_timeout();
      } else {
        anim_counter++;
        u64 now_ms = osGetTime();
        if (now_ms - connect_start_ms >= 15000) { // 15 seconds (real time)
          enter_startup_timeout();
        } else {
          bool check_result = false;
          u64 check_elapsed_ms = 0;
          int check_attempt = 0;
          if (startup_check_thread &&
              poll_startup_connection_check(startup_check, check_result,
                                            check_elapsed_ms, check_attempt)) {
            cleanup_startup_check_thread();
            {
              char detail[128];
              snprintf(detail, sizeof(detail),
                       "attempt=%d result=%s duration_ms=%llu", check_attempt,
                       check_result ? "ok" : "fail",
                       static_cast<unsigned long long>(check_elapsed_ms));
              append_startup_perf_log("startup-connect-finish", detail);
            }
            ctx.is_server_connected = check_result;
            if (!check_result) {
              // Retry quickly after a failed probe; the timeout itself already
              // spent 300ms waiting, so an extra 500ms gap just adds latency.
              next_startup_check_ms = now_ms + 50;
            }
          } else if (!startup_check_thread && now_ms >= next_startup_check_ms) {
            start_startup_check();
          }
        }
      }
    }
    svcSleepThread(16666666); // ~60fps
  }

  cleanup_startup_check_thread();
  cleanup_finished_webm_startup_warmup_thread(&ctx, true);
  {
    char detail[160];
    const u64 now_ms = osGetTime();
    const u64 duration_ms =
        now_ms >= connect_start_ms ? now_ms - connect_start_ms : 0;
    snprintf(
        detail, sizeof(detail), "connected=%s attempts=%d duration_ms=%llu",
        ctx.is_server_connected ? "true" : "false", startup_check_attempt_count,
        static_cast<unsigned long long>(duration_ms));
    append_startup_perf_log("step4-connection-check", detail);
    append_startup_perf_log("startup-ready", detail);
  }

  Thread threadId = nullptr;
  if (ctx.is_running) {
    // 6th arg = false (Attached) so we can reliably join on exit
    threadId = threadCreate(download_thread, &api, 0x10000, 0x3F, -2, false);
  }

  auto start_playback = [&](const Track &track) {
    // --- Fully stop and discard previous playback ---
    YouTubeAPI::should_cancel = true; // Signal download thread to stop
    const bool keep_paused_after_seek =
        ctx.seek_target_seconds >= 0 && ctx.is_paused;
    const bool preserve_seek_cache = (ctx.playing_id == track.id);
    { // Safely read is_downloading under lock
      bool still_dl;
      do {
        LightLock_Lock(&ctx.lock);
        still_dl = ctx.is_downloading;
        LightLock_Unlock(&ctx.lock);
        if (still_dl)
          svcSleepThread(10 * 1000 * 1000); // Wait until fully stopped
      } while (still_dl);
    }

    ctx.is_paused = keep_paused_after_seek;
    ndspChnSetPaused(0, keep_paused_after_seek);
    opus_player.stop();

    LightLock_Lock(&stream_lock);
    if (g_stream_buffer_ptr) g_stream_buffer_ptr->clear(); // Discard buffer
    g_stream_download_complete = false;
    LightLock_Unlock(&stream_lock);

    YouTubeAPI::should_cancel = false; // Clear cancel flag

    // --- Set new track info ---
    int seek_secs = ctx.seek_target_seconds;
    ctx.seek_target_seconds = -1;
    g_opus_playback_perf_start_ms = osGetTime();
    g_opus_first_pcm_queued_logged = false;
    g_opus_audio_started_logged = false;
    g_opus_last_frame_observe_ms = 0;
    g_opus_observed_max_decoded_buffers = 0;
    g_opus_observed_decode_failure = false;
    g_webm_update_pressure_last_log_ms = 0;
    cleanup_finished_webm_startup_warmup_thread(&ctx, false);
    append_opus_playback_perf_log(OpusPerfEvent::PlaybackRequest, 0);

    LightLock_Lock(&ctx.lock);
    ctx.pause_accumulated_ms = 0;
    ctx.pause_started_at = osGetTime(); // freeze bar: buffering starts now
    ctx.is_buffering = true;
    clear_pending_webm_seek_runtime(&ctx);
    clear_active_webm_seek_runtime(&ctx);
    ctx.pending_seek_backtrack_bytes = 0;
    ctx.pending_seek_retry_count = 0;
    if (!preserve_seek_cache) {
      reset_webm_seek_track_state(&ctx);
    } else {
      ctx.webm_seek_track_state.source_info.preroll_ms = kWebmSeekPlanPrerollMs;
      ctx.webm_seek_track_state.source_info.backtrack_bytes = 0;
      ctx.webm_seek_track_state_video_id = track.id;
    }
    ctx.playback_start_time =
        (seek_secs > 0) ? osGetTime() - (u64)seek_secs * 1000ULL : osGetTime();
    ctx.playing_id = track.id;
    ctx.playing_title = track.title;
    ctx.playing_duration = track.duration;

    // Build metadata line
    std::string meta = "";
    if (!track.duration.empty() && track.duration != "?")
      meta += track.duration;
    if (!track.views.empty() && track.views != "?" &&
        ctx.active_playlist_id.empty()) {
      if (!meta.empty()) meta += " ";
      meta += track.views;
    }
    if (!track.upload_date.empty() && track.upload_date != "?") {
      if (!meta.empty()) meta += " ";
      meta += track.upload_date;
    }
    ctx.playing_meta = meta;

    update_playing_title_lines(ui_mgr.get_text_buf());
    const AudioPathConfig selected_audio_path = ctx.config.audio_path;
    const StreamContainerMode stream_mode =
        stream_mode_for_audio_path(selected_audio_path);
    const PlaybackSessionKind session_kind = seek_secs > 0
                                                 ? PlaybackSessionKind::Seek
                                                 : PlaybackSessionKind::Startup;
    playback_observer_end_session();
    playback_observer_begin_session(stream_mode, session_kind,
                                    track.id.c_str());
    ctx.active_audio_path = selected_audio_path;
    ctx.active_stream_mode = stream_mode;
    ctx.opus_playback_failure = OpusPlaybackFailure::None;
    ctx.webm_playback_stage = opus_ready
                                  ? WebmPlaybackStage::WaitingForDecoderStart
                                  : WebmPlaybackStage::Idle;
    ctx.compare_thumbnail_fetch_logged = false;
    ctx.compare_thumbnail_done_logged = false;
    ctx.compare_thumbnail_upload_logged = false;
    ctx.compare_audio_audible_logged = false;
    reset_thumbnail_interference_log_state(&ctx);
    OpusPocPlayer::is_playing = opus_ready;
    g_webm_first_pcm_queued_logged = false;
    g_webm_audio_started_logged = false;
    opus_player.set_decode_tuning(
        stream_mode == StreamContainerMode::ProxyWebmOpus
            ? (seek_secs > 0 ? webm_seek_decode_tuning()
                             : webm_startup_decode_tuning())
            : default_opus_decode_tuning());
    if (opus_ready) {
      ctx.g_status_msg =
          playback_status_message(ctx.is_paused, ctx.is_buffering);
    } else {
      ctx.is_buffering = false;
      ctx.playing_id = "";
      reset_thumbnail_interference_log_state(&ctx);
      ctx.g_status_msg = "Opus init failed";
    }
    LightLock_Unlock(&ctx.lock);

    if (!opus_ready) {
      return;
    }

    api.get_audio_stream_url(
        ctx.playing_id, seek_secs, stream_mode,
        [&, seek_secs, stream_mode](const std::string &url, bool ok) {
          Thread finished_warmup_thread = nullptr;
          LightLock_Lock(&ctx.lock);
          const std::string final_url =
              (stream_mode == StreamContainerMode::ProxyWebmOpus)
                  ? api.build_webm_stream_url(track.id, 0)
                  : url;
          if (ok && !final_url.empty()) {
            ctx.current_stream_url = final_url;
            ctx.is_downloading = true;
            ctx.pending_stream_seek_ms =
                (stream_mode == StreamContainerMode::ProxyWebmOpus &&
                 seek_secs > 0)
                    ? seek_secs * 1000
                    : 0;
            ctx.pending_stream_emit_start_ms = 0;
            ctx.pending_stream_parser_prefetch_byte = 0;
            ctx.pending_stream_parser_cluster_aligned = false;
            ctx.pending_stream_seek_seq = 0;
            ctx.pending_stream_seek_plan = WebmSeekPlan{};
            ctx.pending_stream_repeated_seek = false;
            if (stream_mode == StreamContainerMode::ProxyWebmOpus &&
                seek_secs <= 0 &&
                !ctx.webm_seek_track_state.source_info_ready) {
              if (ctx.webm_startup_warmup_thread &&
                  ctx.webm_startup_warmup_done) {
                finished_warmup_thread = ctx.webm_startup_warmup_thread;
                ctx.webm_startup_warmup_thread = nullptr;
                ctx.webm_startup_warmup_video_id.clear();
              }
              const bool warmup_already_running =
                  ctx.webm_startup_warmup_thread &&
                  !ctx.webm_startup_warmup_done;
              if (!warmup_already_running) {
                std::unique_ptr<WebmStartupInfoWarmupTask> warmup_task =
                    std::make_unique<WebmStartupInfoWarmupTask>();
                warmup_task->context = &ctx;
                warmup_task->api = &api;
                warmup_task->video_id = track.id;
                warmup_task->preroll_ms = kWebmSeekPlanPrerollMs;
                warmup_task->backtrack_bytes = backtrack_bytes_for_retry(0);
                WebmStartupInfoWarmupTask *warmup_arg = warmup_task.release();
                Thread warmup_thread =
                    threadCreate(warmup_webm_startup_info_thread, warmup_arg,
                                 0x8000, 0x3F, -2, false);
                if (warmup_thread) {
                  ctx.webm_startup_warmup_thread = warmup_thread;
                  ctx.webm_startup_warmup_done = false;
                  ctx.webm_startup_warmup_video_id = track.id;
                } else {
                  std::unique_ptr<WebmStartupInfoWarmupTask>
                      cleanup_warmup_task(warmup_arg);
                }
              }
            }
          } else {
            ctx.opus_playback_failure = OpusPlaybackFailure::Network;
            ctx.g_status_msg =
                playback_failure_message(ctx.opus_playback_failure);
            OpusPocPlayer::is_playing = false;
            ctx.webm_playback_stage = WebmPlaybackStage::Idle;
            ctx.seek_target_seconds = -1;
            clear_pending_webm_seek_runtime(&ctx);
            reset_webm_seek_track_state(&ctx);
            clear_active_webm_seek_runtime(&ctx);

            ctx.playing_id = "";
            reset_thumbnail_interference_log_state(&ctx);
            playback_observer_end_session();
          }
          LightLock_Unlock(&ctx.lock);
          if (finished_warmup_thread) {
            threadJoin(finished_warmup_thread, U64_MAX);
            threadFree(finished_warmup_thread);
          }
        });
  };

  auto try_get_queued_track_locked = [&](int queue_pos, Track *out_track,
                                         int *out_track_index) -> bool {
    if (queue_pos < 0 || queue_pos >= (int)ctx.play_queue.size()) {
      return false;
    }
    const int track_index = ctx.play_queue[queue_pos];
    if (track_index < 0 || track_index >= (int)ctx.playing_tracks.size()) {
      return false;
    }
    if (out_track) {
      *out_track = ctx.playing_tracks[track_index];
    }
    if (out_track_index) {
      *out_track_index = track_index;
    }
    return true;
  };

  while (aptMainLoop()) {
    cleanup_finished_webm_startup_warmup_thread(&ctx, false);
    if (!ctx.is_running)
      break; // Exit immediately if START pressed right after boot

    // === Auto-advance to next track (track end detection) ===
    LightLock_Lock(&ctx.lock);
    bool should_auto_next = false;

    bool active_player_finished =
        OpusPocPlayer::is_playing && opus_player.is_track_finished();
    if (active_player_finished) {
      if (!ctx.play_queue.empty()) {
        // Auto-advance: loop when reaching the end (per user request)
        should_auto_next = true;
      } else {
        // End of playlist or single track
        OpusPocPlayer::is_playing = false;
        ctx.webm_playback_stage = WebmPlaybackStage::Idle;
        ctx.g_status_msg = "";
        ctx.playing_id = "";
        reset_thumbnail_interference_log_state(&ctx);
        ctx.current_track_idx = -1;
        playback_observer_end_session();
      }
    }
    LightLock_Unlock(&ctx.lock);

    if (should_auto_next) {
      svcSleepThread(300 * 1000 * 1000); // Brief 0.3s gap between tracks
      LightLock_Lock(&ctx.lock);
      if (ctx.loop_mode == LOOP_ONE) {
        // Replay the same track
        if (ctx.current_track_idx >= 0 &&
            ctx.current_track_idx < (int)ctx.play_queue.size()) {
          int cur_idx = ctx.play_queue[ctx.current_track_idx];
          if (cur_idx >= 0 && cur_idx < (int)ctx.playing_tracks.size()) {
            Track cur_track = ctx.playing_tracks[cur_idx];
            LightLock_Unlock(&ctx.lock);
            start_playback(cur_track);
          } else {
            OpusPocPlayer::is_playing = false;
            ctx.playing_id = "";
            reset_thumbnail_interference_log_state(&ctx);
            LightLock_Unlock(&ctx.lock);
          }
        } else {
          OpusPocPlayer::is_playing = false;
          ctx.playing_id = "";
          reset_thumbnail_interference_log_state(&ctx);
          LightLock_Unlock(&ctx.lock);
        }
      } else {
        ctx.current_track_idx++;
        bool do_play = true;
        if (ctx.current_track_idx >= (int)ctx.play_queue.size()) {
          if (ctx.loop_mode == LOOP_ALL) {
            ctx.current_track_idx = 0;
          } else {
            // LOOP_OFF: stop playback
            OpusPocPlayer::is_playing = false;
            ctx.g_status_msg = "";
            ctx.playing_id = "";
            reset_thumbnail_interference_log_state(&ctx);
            ctx.current_track_idx = -1;
            LightLock_Unlock(&ctx.lock);
            do_play = false;
          }
        }
        if (do_play) {
          int next_idx = ctx.play_queue[ctx.current_track_idx];
          if (next_idx < 0 || next_idx >= (int)ctx.playing_tracks.size()) {
            ctx.play_queue.clear();
            ctx.current_track_idx = -1;
            OpusPocPlayer::is_playing = false;
            LightLock_Unlock(&ctx.lock);
          } else {
            Track next_track = ctx.playing_tracks[next_idx];
            ctx.selected_index = next_idx;
            LightLock_Unlock(&ctx.lock);
            start_playback(next_track);
          }
        }
      }
    }

    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();
    u32 kRepeat = hidKeysDownRepeat();

    // D-pad held repeat (respects sensitivity setting)
    {
      static const u32 delays[] = {90, 75, 62, 52, 43, 35, 28, 22, 15, 8};
      static const u32 intervals[] = {35, 28, 22, 17, 13, 10, 7, 5, 4, 3};
      int spd_idx = ctx.config.dpad_speed - 1;
      if (spd_idx < 0) spd_idx = 0;
      if (spd_idx > 9) spd_idx = 9;
      u32 rep_delay = delays[spd_idx];
      u32 rep_interval = intervals[spd_idx];

      static u32 s_dpad_held = 0;
      u32 dpad_bits = kHeld & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
      if (dpad_bits) {
        s_dpad_held++;
      } else {
        s_dpad_held = 0;
      }
      if (s_dpad_held > rep_delay &&
          (s_dpad_held - rep_delay) % rep_interval == 0) {
        kRepeat |= dpad_bits;
      }
    }

    // L/R button handling (customizable in settings)
    // L/R button handling (customizable in settings)
    auto handle_lr = [&](LRAction action) {
      if (action == LR_SKIP_FORWARD) {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          Track next_track;
          ctx.current_track_idx++;
          if (ctx.current_track_idx >= (int)ctx.play_queue.size())
            ctx.current_track_idx = 0;
          if (try_get_queued_track_locked(ctx.current_track_idx, &next_track,
                                          nullptr)) {
            LightLock_Unlock(&ctx.lock);
            start_playback(next_track);
            svcSleepThread(500 * 1000 * 1000);
          } else {
            ctx.g_status_msg = "Queue desync";
            LightLock_Unlock(&ctx.lock);
          }
        } else {
          LightLock_Unlock(&ctx.lock);
        }
      } else if (action == LR_SKIP_BACK) {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          Track prev_track;
          ctx.current_track_idx--;
          if (ctx.current_track_idx < 0)
            ctx.current_track_idx = (int)ctx.play_queue.size() - 1;
          if (try_get_queued_track_locked(ctx.current_track_idx, &prev_track,
                                          nullptr)) {
            LightLock_Unlock(&ctx.lock);
            start_playback(prev_track);
            svcSleepThread(500 * 1000 * 1000);
          } else {
            ctx.g_status_msg = "Queue desync";
            LightLock_Unlock(&ctx.lock);
          }
        } else {
          LightLock_Unlock(&ctx.lock);
        }
      } else if (action == LR_PLAY_PAUSE) {
        if (ctx.playing_id != "") {
          ctx.is_paused = !ctx.is_paused;
          ndspChnSetPaused(0, ctx.is_paused);
          ctx.g_status_msg =
              playback_status_message(ctx.is_paused, ctx.is_buffering);
          if (ctx.is_paused) {
            if (!ctx.is_buffering) ctx.pause_started_at = osGetTime();
          } else {
            if (!ctx.is_buffering) {
              if (ctx.pause_started_at > 0)
                ctx.pause_accumulated_ms += osGetTime() - ctx.pause_started_at;
              ctx.pause_started_at = 0;
            }
          }
        }
      }
      // LR_DISABLED: do nothing
    };
    if (kDown & KEY_R) handle_lr(ctx.config.r_action);
    if (kDown & KEY_L) handle_lr(ctx.config.l_action);

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
        if (!ctx.search_tracks.empty()) {
          ctx.previous_state = ctx.current_state;
          ctx.current_state = STATE_POPUP_TRACK_OPTIONS;
          ctx.popup_selected_index = 0;
          ctx.selected_track_id = ctx.search_tracks[ctx.selected_index].id;
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
        if (!ctx.playing_tracks.empty() && ctx.selected_index >= 0 &&
            ctx.selected_index < (int)ctx.playing_tracks.size()) {
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
      bool was_popup = is_popup_state(ctx.current_state);
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
        ctx.g_status_msg =
            playback_status_message(ctx.is_paused, ctx.is_buffering);
        if (ctx.is_paused) {
          // Start freeze only if not already frozen by buffering
          if (!ctx.is_buffering) ctx.pause_started_at = osGetTime();
        } else {
          // End freeze only if buffering is also done
          if (!ctx.is_buffering) {
            if (ctx.pause_started_at > 0)
              ctx.pause_accumulated_ms += osGetTime() - ctx.pause_started_at;
            ctx.pause_started_at = 0;
          }
        }
      }
    }

    if (ctx.current_state == STATE_HOME || ctx.current_state == STATE_SEARCH ||
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
        for (const auto &pl : ctx.playlists) {
          if (pl.id == ctx.selected_playlist_id) {
            ctx.g_tracks = pl.tracks;
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
        LightLock_Lock(&ctx.lock);
        ctx.active_playlist_id = "";
        ctx.active_playlist_name = "";
        ctx.play_queue.clear();
        ctx.current_track_idx = -1;
        LightLock_Unlock(&ctx.lock);
        start_playback(ctx.search_tracks[ctx.selected_index]);
      } else if (action == "toggle_pause") {
        if (!ctx.playing_id.empty()) {
          ctx.is_paused = !ctx.is_paused;
          ndspChnSetPaused(0, ctx.is_paused);
          ctx.g_status_msg =
              playback_status_message(ctx.is_paused, ctx.is_buffering);
          if (ctx.is_paused) {
            if (!ctx.is_buffering) ctx.pause_started_at = osGetTime();
          } else {
            if (!ctx.is_buffering) {
              if (ctx.pause_started_at > 0)
                ctx.pause_accumulated_ms += osGetTime() - ctx.pause_started_at;
              ctx.pause_started_at = 0;
            }
          }
        }
      } else if (action == "prev_track") {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          if (ctx.loop_mode == LOOP_ONE) {
            // Replay same track
            if (ctx.current_track_idx >= 0 &&
                ctx.current_track_idx < (int)ctx.play_queue.size()) {
              Track cur_track;
              if (try_get_queued_track_locked(ctx.current_track_idx, &cur_track,
                                              nullptr)) {
                LightLock_Unlock(&ctx.lock);
                start_playback(cur_track);
              } else {
                ctx.g_status_msg = "Queue desync";
                LightLock_Unlock(&ctx.lock);
              }
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
            Track prev_track;
            if (try_get_queued_track_locked(ctx.current_track_idx, &prev_track,
                                            nullptr)) {
              LightLock_Unlock(&ctx.lock);
              start_playback(prev_track);
            } else {
              ctx.g_status_msg = "Queue desync";
              LightLock_Unlock(&ctx.lock);
            }
          }
        } else {
          LightLock_Unlock(&ctx.lock);
        }
      prev_done:;
      } else if (action == "next_track") {
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          if (ctx.loop_mode == LOOP_ONE) {
            if (ctx.current_track_idx >= 0 &&
                ctx.current_track_idx < (int)ctx.play_queue.size()) {
              Track cur_track;
              if (try_get_queued_track_locked(ctx.current_track_idx, &cur_track,
                                              nullptr)) {
                LightLock_Unlock(&ctx.lock);
                start_playback(cur_track);
              } else {
                ctx.g_status_msg = "Queue desync";
                LightLock_Unlock(&ctx.lock);
              }
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
            Track next_track;
            if (try_get_queued_track_locked(ctx.current_track_idx, &next_track,
                                            nullptr)) {
              LightLock_Unlock(&ctx.lock);
              start_playback(next_track);
            } else {
              ctx.g_status_msg = "Queue desync";
              LightLock_Unlock(&ctx.lock);
            }
          }
        } else {
          LightLock_Unlock(&ctx.lock);
        }
      next_done:;
      } else if (action == "seek") {
        LightLock_Lock(&ctx.lock);
        Track seek_track;
        bool seek_valid = false;
        for (const auto &t : ctx.playing_tracks) {
          if (t.id == ctx.playing_id) {
            seek_track = t;
            seek_valid = true;
            break;
          }
        }
        // Fallback: search result playback uses search_tracks (playing_tracks
        // is empty)
        if (!seek_valid) {
          for (const auto &t : ctx.search_tracks) {
            if (t.id == ctx.playing_id) {
              seek_track = t;
              seek_valid = true;
              break;
            }
          }
        }
        LightLock_Unlock(&ctx.lock);
        if (seek_valid)
          start_playback(seek_track);
        else
          ctx.seek_target_seconds = -1;
      } else if (action == "toggle_loop") {
        if (ctx.loop_mode == LOOP_OFF)
          ctx.loop_mode = LOOP_ALL;
        else if (ctx.loop_mode == LOOP_ALL)
          ctx.loop_mode = LOOP_ONE;
        else
          ctx.loop_mode = LOOP_OFF;
      } else if (action == "toggle_shuffle") {
        ctx.shuffle_mode = !ctx.shuffle_mode;
        LightLock_Lock(&ctx.lock);
        if (!ctx.play_queue.empty()) {
          if (ctx.shuffle_mode) {
            // Shuffle: place current track at front
            int cur_playing =
                (ctx.current_track_idx >= 0 &&
                 ctx.current_track_idx < (int)ctx.play_queue.size())
                    ? ctx.play_queue[ctx.current_track_idx]
                    : -1;
            ctx.play_queue.clear();
            for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
              ctx.play_queue.push_back(i);
            if (ctx.play_queue.size() > 1) {
              for (size_t i = ctx.play_queue.size() - 1; i > 0; --i) {
                size_t j = rand() % (i + 1);
                std::swap(ctx.play_queue[i], ctx.play_queue[j]);
              }
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
            int cur_playing =
                (ctx.current_track_idx >= 0 &&
                 ctx.current_track_idx < (int)ctx.play_queue.size())
                    ? ctx.play_queue[ctx.current_track_idx]
                    : -1;
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
        ctx.current_state = STATE_POPUP_NAV;
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
        // Set active_playlist_id BEFORE start_playback so playing_meta omits
        // views
        ctx.active_playlist_id = ctx.selected_playlist_id;
        for (const auto &pl : ctx.playlists) {
          if (pl.id == ctx.selected_playlist_id) {
            ctx.active_playlist_name = pl.name;
            break;
          }
        }
        LightLock_Unlock(&ctx.lock);
        start_playback(ctx.g_tracks[play_idx]);
        LightLock_Lock(&ctx.lock);
        ctx.playing_tracks = ctx.g_tracks;
        // Build queue (shuffled or sequential based on mode)
        ctx.play_queue.clear();
        for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
          ctx.play_queue.push_back(i);
        if (ctx.shuffle_mode && ctx.play_queue.size() > 1) {
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
      } else if (action == "start_shuffle_playback" ||
                 action == "start_order_playback") {
        ctx.shuffle_mode = (action == "start_shuffle_playback");
        LightLock_Lock(&ctx.lock);
        // Remove MODE_BTN
        if (!ctx.g_tracks.empty() && ctx.g_tracks[0].id == "MODE_BTN") {
          ctx.g_tracks.erase(ctx.g_tracks.begin());
        }
        if (!ctx.g_tracks.empty()) {
          ctx.active_playlist_id = ctx.selected_playlist_id;
          for (const auto &pl : ctx.playlists) {
            if (pl.id == ctx.selected_playlist_id) {
              ctx.active_playlist_name = pl.name;
              break;
            }
          }
          ctx.playing_tracks = ctx.g_tracks;
          ctx.play_queue.clear();
          for (size_t i = 0; i < ctx.playing_tracks.size(); ++i)
            ctx.play_queue.push_back(i);
          if (ctx.shuffle_mode && ctx.play_queue.size() > 1) {
            for (size_t i = ctx.play_queue.size() - 1; i > 0; --i) {
              size_t j = rand() % (i + 1);
              std::swap(ctx.play_queue[i], ctx.play_queue[j]);
            }
          }
          ctx.current_track_idx = 0;
          Track first_track;
          if (try_get_queued_track_locked(0, &first_track, nullptr)) {
            LightLock_Unlock(&ctx.lock);
            start_playback(first_track);
            screen_mgr.change_screen(ctx, "PlayingScreen");
          } else {
            ctx.g_status_msg = "Queue desync";
            LightLock_Unlock(&ctx.lock);
          }
        } else {
          LightLock_Unlock(&ctx.lock);
        }
        kDown &= ~KEY_A;
      }
    }

    // --- Common popup touch handling ---
    if (is_popup_state(ctx.current_state)) {
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
            popup_item_count = (ctx.previous_state == STATE_PLAYLIST_DETAIL ||
                                ctx.previous_state == STATE_PLAYING_UI)
                                   ? 4
                                   : 2;
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
              const std::string &csv = ctx.config.quick_access_ids;
              size_t s = 0;
              while (s < csv.size()) {
                size_t p = csv.find(',', s);
                std::string id = (p == std::string::npos)
                                     ? csv.substr(s)
                                     : csv.substr(s, p - s);
                if (!id.empty()) qa_ids_tmp.push_back(id);
                if (p == std::string::npos) break;
                s = p + 1;
              }
            }
            for (const auto &pl : ctx.playlists) {
              bool already = false;
              for (const auto &qid : qa_ids_tmp) {
                if (qid == pl.id) {
                  already = true;
                  break;
                }
              }
              if (!already) popup_item_count++;
            }
          } else if (ctx.current_state == STATE_POPUP_QA_REMOVE)
            popup_item_count = 2;

          int box_h =
              (int)(POPUP_HEADER_H + 8 + popup_item_count * POPUP_ITEM_H);
          if (box_h > POPUP_MAX_H) box_h = POPUP_MAX_H;
          int box_y = (240 - box_h) / 2;

          // Tap outside popup -> close (same as B)
          if (tap_x < (int)POPUP_MARGIN_X ||
              tap_x > (int)(POPUP_MARGIN_X + POPUP_WIDTH) || tap_y < box_y ||
              tap_y > box_y + box_h) {
            if (ctx.current_state == STATE_POPUP_TRACK_DETAILS) {
              ctx.current_state = STATE_POPUP_TRACK_OPTIONS;
              ctx.popup_selected_index = 0;
            } else {
              ctx.current_state = ctx.previous_state;
            }
          } else {
            // Tap inside popup -> determine tapped item
            int max_items =
                (int)((POPUP_MAX_H - POPUP_HEADER_H - 8) / POPUP_ITEM_H);
            int start_idx = 0;
            if (ctx.popup_selected_index > max_items / 2)
              start_idx = ctx.popup_selected_index - max_items / 2;
            if (start_idx + max_items > popup_item_count)
              start_idx = popup_item_count - max_items;
            if (start_idx < 0) start_idx = 0;

            float items_y = box_y + POPUP_HEADER_H + 4;
            if (tap_y >= (int)items_y) {
              int tapped_idx =
                  (int)((tap_y - items_y) / POPUP_ITEM_H) + start_idx;
              if (tapped_idx >= 0 && tapped_idx < popup_item_count) {
                ctx.popup_selected_index = tapped_idx;
                kDown |=
                    KEY_A; // Execute immediately (delegate to KEY_A handler)
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
            {
              Track t;
              auto f = [&](const std::vector<Track> &v) {
                for (const auto &tr : v)
                  if (tr.id == ctx.selected_track_id) {
                    t = tr;
                    return true;
                  }
                return false;
              };
              f(ctx.playing_tracks) || f(ctx.search_tracks) || f(ctx.g_tracks);
              if (!t.id.empty()) playlist_manager.add_track(new_pl_id, t);
            }
            ctx.playlists = playlist_manager.get_playlists();
          }
        } else { // Add to existing playlist
          std::string pl_id = ctx.playlists[ctx.popup_selected_index - 1].id;
          {
            Track t;
            auto f = [&](const std::vector<Track> &v) {
              for (const auto &tr : v)
                if (tr.id == ctx.selected_track_id) {
                  t = tr;
                  return true;
                }
              return false;
            };
            f(ctx.playing_tracks) || f(ctx.search_tracks) || f(ctx.g_tracks);
            if (!t.id.empty()) playlist_manager.add_track(pl_id, t);
          }
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
            const std::string &csv = ctx.config.quick_access_ids;
            size_t s = 0;
            while (s < csv.size()) {
              size_t p = csv.find(',', s);
              std::string id = (p == std::string::npos) ? csv.substr(s)
                                                        : csv.substr(s, p - s);
              if (!id.empty()) qa_ids.push_back(id);
              if (p == std::string::npos) break;
              s = p + 1;
            }
          }
          bool in_qa = false;
          for (size_t i = 0; i < qa_ids.size(); ++i) {
            if (qa_ids[i] == ctx.selected_playlist_id) {
              in_qa = true;
              qa_ids.erase(qa_ids.begin() + i);
              break;
            }
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
            const std::string &csv = ctx.config.quick_access_ids;
            size_t s = 0;
            while (s < csv.size()) {
              size_t p = csv.find(',', s);
              std::string id = (p == std::string::npos) ? csv.substr(s)
                                                        : csv.substr(s, p - s);
              if (!id.empty() && id != ctx.selected_playlist_id)
                qa_ids.push_back(id);
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
      // Search: [Details, Add] = 2 items / PL/Playing: [Details, Rename,
      // Remove, Add] = 4 items
      bool has_delete = (ctx.previous_state == STATE_PLAYLIST_DETAIL ||
                         ctx.previous_state == STATE_PLAYING_UI);
      int item_count = has_delete ? 4 : 2;
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
        } else if (has_delete && ctx.popup_selected_index == 1) { // Rename
          std::string current_title = "";
          LightLock_Lock(&ctx.lock);
          for (const auto &t : ctx.g_tracks) {
            if (t.id == ctx.selected_track_id) {
              current_title = t.title;
              break;
            }
          }
          if (current_title.empty()) {
            for (const auto &t : ctx.playing_tracks) {
              if (t.id == ctx.selected_track_id) {
                current_title = t.title;
                break;
              }
            }
          }
          LightLock_Unlock(&ctx.lock);
          SwkbdState swkbd;
          char mybuf[256] = "";
          swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
          swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT);
          swkbdSetHintText(&swkbd, "Track Name");
          if (!current_title.empty())
            swkbdSetInitialText(&swkbd, current_title.c_str());
          if (swkbdInputText(&swkbd, mybuf, sizeof(mybuf)) ==
                  SWKBD_BUTTON_CONFIRM &&
              strlen(mybuf) > 0) {
            std::string new_title(mybuf);
            std::string pl_id = (ctx.previous_state == STATE_PLAYING_UI)
                                    ? ctx.active_playlist_id
                                    : ctx.selected_playlist_id;
            playlist_manager.rename_track(pl_id, ctx.selected_track_id,
                                          new_title);
            ctx.playlists = playlist_manager.get_playlists();
            LightLock_Lock(&ctx.lock);
            for (auto &t : ctx.g_tracks) {
              if (t.id == ctx.selected_track_id) {
                t.title = new_title;
                break;
              }
            }
            for (auto &t : ctx.playing_tracks) {
              if (t.id == ctx.selected_track_id) {
                t.title = new_title;
                break;
              }
            }
            bool need_title_update = (ctx.playing_id == ctx.selected_track_id);
            if (need_title_update) ctx.playing_title = new_title;
            LightLock_Unlock(&ctx.lock);
            if (need_title_update)
              update_playing_title_lines(ui_mgr.get_text_buf());
          }
          ctx.current_state = ctx.previous_state;
        } else if (has_delete && ctx.popup_selected_index == 2) { // Remove
          if (ctx.previous_state == STATE_PLAYING_UI) {
            // Remove from PlayingScreen: use active_playlist_id
            playlist_manager.remove_track(ctx.active_playlist_id,
                                          ctx.selected_track_id);
            ctx.playlists = playlist_manager.get_playlists();
            // Remove from playing_tracks
            for (auto it = ctx.playing_tracks.begin();
                 it != ctx.playing_tracks.end(); ++it) {
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
              if (ctx.play_queue.size() > 1) {
                for (size_t i = ctx.play_queue.size() - 1; i > 0; --i) {
                  size_t j = rand() % (i + 1);
                  std::swap(ctx.play_queue[i], ctx.play_queue[j]);
                }
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
              ctx.current_track_idx =
                  cur_playing_idx >= 0 ? cur_playing_idx : 0;
            }
            // Adjust selected_index
            if (ctx.selected_index >= (int)ctx.playing_tracks.size())
              ctx.selected_index = (int)ctx.playing_tracks.size() - 1;
            if (ctx.selected_index < 0) ctx.selected_index = 0;
            // Stop if the currently playing track was removed
            if (ctx.playing_id == ctx.selected_track_id) {
              ctx.playing_id = "";
              reset_thumbnail_interference_log_state(&ctx);
              OpusPocPlayer::is_playing = false;
              ctx.active_audio_path = ctx.config.audio_path;
              ctx.webm_playback_stage = WebmPlaybackStage::Idle;
              clear_pending_webm_seek_runtime(&ctx);
              reset_webm_seek_track_state(&ctx);
              clear_active_webm_seek_runtime(&ctx);
            }
          } else {
            // Remove from PlaylistDetailScreen (existing logic)
            playlist_manager.remove_track(ctx.selected_playlist_id,
                                          ctx.selected_track_id);
            ctx.playlists = playlist_manager.get_playlists();
            for (const auto &p : ctx.playlists) {
              if (p.id == ctx.selected_playlist_id) {
                ctx.g_tracks = p.tracks;
                break;
              }
            }
            if (ctx.selected_index >= (int)ctx.g_tracks.size())
              ctx.selected_index = ctx.g_tracks.size() - 1;
            if (ctx.selected_index < 0) ctx.selected_index = 0;
          }
          ctx.current_state = ctx.previous_state;
        } else { // Add to playlist (Search:idx1, PL/Playing:idx3)
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
        if (ctx.popup_selected_index == 2)
          is_grayed = (ctx.previous_state == STATE_PLAYLISTS);
        if (ctx.popup_selected_index == 3)
          is_grayed = (ctx.previous_state == STATE_SETTINGS);

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
        const std::string &csv = ctx.config.quick_access_ids;
        size_t s = 0;
        while (s < csv.size()) {
          size_t p = csv.find(',', s);
          std::string id =
              (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
          if (!id.empty()) qa_ids.push_back(id);
          if (p == std::string::npos) break;
          s = p + 1;
        }
      }
      std::vector<int> filtered_indices;
      for (int i = 0; i < (int)ctx.playlists.size(); ++i) {
        bool already = false;
        for (const auto &qid : qa_ids) {
          if (qid == ctx.playlists[i].id) {
            already = true;
            break;
          }
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
        if (item_count > 0 && ctx.popup_selected_index >= 0 &&
            ctx.popup_selected_index < item_count) {
          std::string pl_id =
              ctx.playlists[filtered_indices[ctx.popup_selected_index]].id;
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
          const std::string &csv = ctx.config.quick_access_ids;
          size_t s = 0;
          while (s < csv.size()) {
            size_t p = csv.find(',', s);
            std::string id =
                (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
            if (!id.empty() && id != ctx.selected_playlist_id)
              qa_ids.push_back(id);
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
      swkbdSetHintText(&swkbd, "Search music...");
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

        opus_player.stop();
        if (g_stream_buffer_ptr) g_stream_buffer_ptr->clear();
        YouTubeAPI::should_cancel = false;
        ctx.active_audio_path = ctx.config.audio_path;
        ctx.webm_playback_stage = WebmPlaybackStage::Idle;
        clear_pending_webm_seek_runtime(&ctx);
        reset_webm_seek_track_state(&ctx);
        clear_active_webm_seek_runtime(&ctx);
        ctx.playing_id = "";
        reset_thumbnail_interference_log_state(&ctx);
        ctx.playing_title = "";
        ctx.playing_title_lines.clear();

        ctx.search_tracks.clear();
        ctx.search_query = url_encode(std::string(mybuf));
        ctx.selected_index = 0;
        screen_mgr.change_screen(ctx, "SearchScreen");
      }
    }

    // --- Thumbnail async trigger ---
    {
      size_t stream_buffer_size = 0;
      bool stream_download_complete = true;
      LightLock_Lock(&stream_lock);
      if (g_stream_buffer_ptr) {
        stream_buffer_size = g_stream_buffer_ptr->size();
      }
      stream_download_complete = g_stream_download_complete;
      LightLock_Unlock(&stream_lock);

      bool should_log_thumb_fetch_interference = false;
      const char *thumb_fetch_event = nullptr;
      NonAudioInterferenceInput thumb_fetch_log_input = {};
      NonAudioInterferenceDecision thumb_fetch_log_decision = {};
      bool thumb_fetch_log_track_changed = false;
      bool thumb_fetch_log_loading = false;
      bool thumb_fetch_log_ready = false;
      u64 thumb_fetch_log_waited_ms = 0;
      LightLock_Lock(&ctx.lock);
      bool track_changed = !ctx.playing_id.empty() &&
                           ctx.playing_id != ctx.thumbnail_vid_id &&
                           !ctx.thumbnail_loading;
      const NonAudioInterferenceInput non_audio_input =
          make_non_audio_interference_input(
              stream_buffer_size, stream_download_complete, opus_player, ctx);
      const NonAudioInterferenceDecision fetch_decision =
          evaluate_thumbnail_fetch_interference(non_audio_input);
      const u64 now_ms = osGetTime();
      const bool base_delay_elapsed =
          now_ms - ctx.playback_start_time > 3000ULL;
      bool need_fetch =
          track_changed && !fetch_decision.defer && base_delay_elapsed;
      NonAudioInterferenceReason fetch_log_reason =
          NonAudioInterferenceReason::None;
      if (track_changed && fetch_decision.defer) {
        fetch_log_reason = fetch_decision.reason;
        thumb_fetch_event = "fetch_deferred";
      } else if (track_changed && !fetch_decision.defer && base_delay_elapsed) {
        thumb_fetch_event = "fetch_allowed";
      }
      if (thumb_fetch_event) {
        should_log_thumb_fetch_interference = should_log_thumbnail_interference(
            fetch_log_reason, &ctx.thumbnail_fetch_defer_reason,
            &ctx.thumbnail_fetch_defer_since_ms,
            &ctx.thumbnail_fetch_defer_last_log_ms, now_ms,
            &thumb_fetch_log_waited_ms);
        if (should_log_thumb_fetch_interference) {
          thumb_fetch_log_input = non_audio_input;
          thumb_fetch_log_decision = fetch_decision;
          thumb_fetch_log_track_changed = track_changed;
          thumb_fetch_log_loading = ctx.thumbnail_loading;
          thumb_fetch_log_ready = ctx.thumbnail_ready;
        }
      }
      if (need_fetch) {
        ctx.thumbnail_vid_id = ctx.playing_id;
        ctx.thumbnail_loading = true;
        ctx.thumbnail_ready = false;
        ctx.compare_thumbnail_fetch_logged = false;
        ctx.compare_thumbnail_done_logged = false;
        ctx.compare_thumbnail_upload_logged = false;
      }
      LightLock_Unlock(&ctx.lock);

      if (should_log_thumb_fetch_interference) {
        append_webm_thumbnail_interference_log(
            thumb_fetch_event, thumb_fetch_log_input, thumb_fetch_log_decision,
            thumb_fetch_log_track_changed, thumb_fetch_log_loading,
            thumb_fetch_log_ready, thumb_fetch_log_waited_ms);
      }

      if (track_changed)
        ctx.thumbnail_tex
            .unload(); // immediately hide stale thumbnail on track change

      if (need_fetch) {
        if (!ctx.compare_thumbnail_fetch_logged) {
          log_playback_compare_event(ctx.active_stream_mode,
                                     PlaybackCompareEvent::ThumbnailFetchStart,
                                     OpusPlayerUpdateStats{});
          LightLock_Lock(&ctx.lock);
          ctx.compare_thumbnail_fetch_logged = true;
          LightLock_Unlock(&ctx.lock);
        }
        Thread t =
            threadCreate(thumbnail_dl_thread, &ctx, 0x10000, 0x3F, -2, true);
        if (!t) {
          LightLock_Lock(&ctx.lock);
          ctx.thumbnail_loading = false;
          ctx.thumbnail_vid_id.clear();
          LightLock_Unlock(&ctx.lock);
        }
      }
    }

    // --- Thumbnail GPU upload (main thread only) ---
    {
      size_t stream_buffer_size = 0;
      bool stream_download_complete = true;
      LightLock_Lock(&stream_lock);
      if (g_stream_buffer_ptr) {
        stream_buffer_size = g_stream_buffer_ptr->size();
      }
      stream_download_complete = g_stream_download_complete;
      LightLock_Unlock(&stream_lock);

      bool allow_upload = false;
      bool should_log_thumb_upload_interference = false;
      const char *thumb_upload_event = nullptr;
      NonAudioInterferenceInput thumb_upload_log_input = {};
      NonAudioInterferenceDecision thumb_upload_log_decision = {};
      bool thumb_upload_log_loading = false;
      bool thumb_upload_log_ready = false;
      u64 thumb_upload_log_waited_ms = 0;
      LightLock_Lock(&ctx.lock);
      const NonAudioInterferenceInput non_audio_input =
          make_non_audio_interference_input(
              stream_buffer_size, stream_download_complete, opus_player, ctx);
      const NonAudioInterferenceDecision upload_decision =
          evaluate_thumbnail_upload_interference(non_audio_input);
      allow_upload = !upload_decision.defer;
      const bool upload_ready_for_log = ctx.thumbnail_ready;
      if (upload_ready_for_log && upload_decision.defer) {
        thumb_upload_event = "upload_deferred";
      } else if (upload_ready_for_log && !upload_decision.defer) {
        thumb_upload_event = "upload_allowed";
      }
      if (thumb_upload_event) {
        const NonAudioInterferenceReason upload_log_reason =
            upload_decision.defer ? upload_decision.reason
                                  : NonAudioInterferenceReason::None;
        should_log_thumb_upload_interference =
            should_log_thumbnail_interference(
                upload_log_reason, &ctx.thumbnail_upload_defer_reason,
                &ctx.thumbnail_upload_defer_since_ms,
                &ctx.thumbnail_upload_defer_last_log_ms, osGetTime(),
                &thumb_upload_log_waited_ms);
        if (should_log_thumb_upload_interference) {
          thumb_upload_log_input = non_audio_input;
          thumb_upload_log_decision = upload_decision;
          thumb_upload_log_loading = ctx.thumbnail_loading;
          thumb_upload_log_ready = ctx.thumbnail_ready;
        }
      }
      LightLock_Unlock(&ctx.lock);

      if (should_log_thumb_upload_interference) {
        append_webm_thumbnail_interference_log(
            thumb_upload_event, thumb_upload_log_input,
            thumb_upload_log_decision, false, thumb_upload_log_loading,
            thumb_upload_log_ready, thumb_upload_log_waited_ms);
      }

      LightLock_Lock(&ctx.lock);
      bool ready = ctx.thumbnail_ready;
      bool thumbnail_done_logged = ctx.compare_thumbnail_done_logged;
      if (ready && !ctx.compare_thumbnail_done_logged) {
        ctx.compare_thumbnail_done_logged = true;
      }
      std::vector<uint8_t> pixels;
      int crop_size = 0;
      if (ready && allow_upload) {
        pixels = std::move(ctx.thumbnail_pixels);
        crop_size = ctx.thumbnail_crop_size;
        ctx.thumbnail_ready = false;
      }
      LightLock_Unlock(&ctx.lock);

      if (ready && !thumbnail_done_logged) {
        log_playback_compare_event(ctx.active_stream_mode,
                                   PlaybackCompareEvent::ThumbnailFetchDone,
                                   OpusPlayerUpdateStats{});
      }
      if (ready && allow_upload) {
        ctx.thumbnail_tex.unload();
        ctx.thumbnail_tex.load_from_pixels(pixels.data(), crop_size, crop_size);
        bool should_log_upload = false;
        LightLock_Lock(&ctx.lock);
        if (!ctx.compare_thumbnail_upload_logged) {
          ctx.compare_thumbnail_upload_logged = true;
          should_log_upload = true;
        }
        LightLock_Unlock(&ctx.lock);
        if (should_log_upload) {
          log_playback_compare_event(ctx.active_stream_mode,
                                     PlaybackCompareEvent::ThumbnailUploadDone,
                                     OpusPlayerUpdateStats{});
        }
      }
    }

    bool should_start_opus_decoder = false;
    bool opus_download_complete = false;
    size_t opus_buffer_size = 0;
    bool use_webm_poc = false;
    StreamContainerMode compare_stream_mode = StreamContainerMode::ProxyOggOpus;
    WebmPlaybackStage playback_stage = WebmPlaybackStage::Idle;
    PlaybackCoreSnapshot core_snapshot = {};
    WebmPlaybackControllerInput controller_input = {};
    WebmPlaybackControllerDecision controller_decision = {};
    LightLock_Lock(&ctx.lock);
    const bool opus_decode_pending =
        ctx.webm_playback_stage == WebmPlaybackStage::WaitingForDecoderStart &&
        OpusPocPlayer::is_playing;
    use_webm_poc = ctx.active_stream_mode == StreamContainerMode::ProxyWebmOpus;
    compare_stream_mode = ctx.active_stream_mode;
    playback_stage = ctx.webm_playback_stage;
    LightLock_Unlock(&ctx.lock);
    if (opus_decode_pending) {
      core_snapshot = collect_playback_core_snapshot();
      opus_buffer_size = core_snapshot.stream_buffer_bytes;
      opus_download_complete = core_snapshot.download_complete;
      if (use_webm_poc) {
        bool seek_active = false;
        LightLock_Lock(&ctx.lock);
        seek_active = ctx.active_stream_seek_seq > 0;
        controller_input = make_webm_controller_input(
            core_snapshot, OpusPlayerUpdateStats{}, opus_player, ctx);
        LightLock_Unlock(&ctx.lock);
        const WebmPlaybackControllerConfig controller_config =
            controller_config_for_seek(seek_active);
        controller_decision = decide_webm_playback_step(
            playback_stage, controller_input, controller_config);
        should_start_opus_decoder = controller_decision.should_start_decoder;
      } else {
        should_start_opus_decoder =
            opus_buffer_size >= OPUS_STREAM_START_BYTES ||
            (opus_download_complete && opus_buffer_size > 0);
      }
    }

    if (should_start_opus_decoder) {
      bool started = false;
      int pending_seek_ms = 0;
      int pending_emit_start_ms = 0;
      uint64_t parser_prefetch_offset = 0;
      bool enable_parser_seek = false;
      bool parser_cluster_aligned = false;
      WebmSeekExecutionContext seek_execution_context = {};
      std::string parser_range_probe_base_url = "";
      uint64_t parser_range_filesize = 0;
      append_opus_playback_perf_log(OpusPerfEvent::DecoderOpenStart,
                                    opus_buffer_size);
      log_playback_compare_event(compare_stream_mode,
                                 PlaybackCompareEvent::DecoderOpenStart,
                                 OpusPlayerUpdateStats{});
      LightLock_Lock(&ctx.lock);
      pending_seek_ms = ctx.pending_stream_seek_ms;
      pending_emit_start_ms = ctx.pending_stream_emit_start_ms;
      parser_prefetch_offset = ctx.pending_stream_parser_prefetch_byte;
      parser_cluster_aligned = ctx.pending_stream_parser_cluster_aligned;
      seek_execution_context.seek_seq = ctx.pending_stream_seek_seq;
      seek_execution_context.target_ms = ctx.pending_stream_seek_ms;
      seek_execution_context.plan = ctx.pending_stream_seek_plan;
      seek_execution_context.repeated_seek = ctx.pending_stream_repeated_seek;
      seek_execution_context.reuse_class = ctx.pending_stream_reuse_class;
      enable_parser_seek = kWebmUseParserLevelSeek && pending_seek_ms > 0;
      if (enable_parser_seek && ctx.webm_seek_track_state.source_info_ready &&
          ctx.webm_seek_track_state.source_info.has_filesize) {
        parser_range_filesize = ctx.webm_seek_track_state.source_info.filesize;
      }
      if (enable_parser_seek) {
        parser_range_probe_base_url =
            api.build_webm_seek_probe_base_url(ctx.playing_id);
      }
      LightLock_Unlock(&ctx.lock);
      if (g_stream_buffer_ptr) {
        if (use_webm_poc) {
          started = opus_player.start_webm_streaming(
              g_stream_buffer_ptr.get(), &stream_lock,
              &g_stream_download_complete, seek_execution_context,
              pending_emit_start_ms, enable_parser_seek, parser_cluster_aligned,
              parser_range_probe_base_url, parser_range_filesize,
              parser_prefetch_offset);
        } else if (opus_download_complete) {
          LightLock_Lock(&stream_lock);
          started = opus_player.start(g_stream_buffer_ptr->data(),
                                      g_stream_buffer_ptr->size());
          LightLock_Unlock(&stream_lock);
        } else {
          started = opus_player.start_streaming(g_stream_buffer_ptr.get(),
                                                &stream_lock,
                                                &g_stream_download_complete);
        }
      }
      append_opus_playback_perf_log(started ? OpusPerfEvent::DecoderOpenOk
                                            : OpusPerfEvent::DecoderOpenFailed,
                                    opus_buffer_size);
      log_playback_compare_event(compare_stream_mode,
                                 started
                                     ? PlaybackCompareEvent::DecoderOpenOk
                                     : PlaybackCompareEvent::DecoderOpenFailed,
                                 OpusPlayerUpdateStats{});

      LightLock_Lock(&ctx.lock);
      const WebmRemuxError start_error =
          use_webm_poc ? opus_player.webm_remux_error() : WebmRemuxError::None;
      if (!started) {
        clear_pending_webm_seek_runtime(&ctx);
        ctx.webm_playback_stage = WebmPlaybackStage::Failed;
        ctx.pending_seek_backtrack_bytes = 0;
        ctx.pending_seek_retry_count = 0;
        reset_webm_seek_track_state(&ctx);
        clear_active_webm_seek_runtime(&ctx);
        ctx.opus_playback_failure = use_webm_poc
                                        ? webm_error_to_failure(start_error)
                                        : OpusPlaybackFailure::Decoder;
        if (ctx.opus_playback_failure == OpusPlaybackFailure::None) {
          ctx.opus_playback_failure = OpusPlaybackFailure::Decoder;
        }
        ctx.g_status_msg = playback_failure_message(ctx.opus_playback_failure);
        OpusPocPlayer::is_playing = false;
        ctx.playing_id = "";
        reset_thumbnail_interference_log_state(&ctx);
        playback_observer_end_session();
      } else {
        ctx.pending_seek_backtrack_bytes = 0;
        ctx.pending_seek_retry_count = 0;
        ctx.active_stream_seek_seq = ctx.pending_stream_seek_seq;
        ctx.active_stream_seek_plan = ctx.pending_stream_seek_plan;
        ctx.active_stream_repeated_seek = ctx.pending_stream_repeated_seek;
        ctx.active_stream_reuse_class = ctx.pending_stream_reuse_class;
        clear_pending_webm_seek_runtime(&ctx);
        ctx.webm_playback_stage = use_webm_poc ? WebmPlaybackStage::Prebuffering
                                               : WebmPlaybackStage::Steady;
        ndspChnSetPaused(0, use_webm_poc ? true : ctx.is_paused);
        ctx.g_status_msg =
            playback_status_message(ctx.is_paused, ctx.is_buffering);
      }
      LightLock_Unlock(&ctx.lock);
    }

    OpusPlayerUpdateStats opus_update_stats = {};
    const bool use_opus_poc_for_update = OpusPocPlayer::is_playing;
    if (use_opus_poc_for_update) {
      opus_update_stats = opus_player.update_with_stats();
      if (opus_update_stats.decoded_buffers >
          g_opus_observed_max_decoded_buffers) {
        g_opus_observed_max_decoded_buffers = opus_update_stats.decoded_buffers;
      }
      if (opus_update_stats.hit_decode_failure) {
        g_opus_observed_decode_failure = true;
      }
      const u64 now_ms = osGetTime();
      if (now_ms >= g_opus_last_frame_observe_ms + 1000ULL) {
        const OpusPipelineState observed_state = collect_opus_pipeline_state();
        OpusPlayerUpdateStats observed_update_stats = {};
        observed_update_stats.decoded_buffers =
            g_opus_observed_max_decoded_buffers;
        observed_update_stats.hit_decode_failure =
            g_opus_observed_decode_failure;
        const OpusPerfSnapshot observed_snapshot =
            make_opus_perf_snapshot(observed_state, observed_update_stats);
        append_opus_playback_perf_log(OpusPerfEvent::FrameObserve,
                                      observed_snapshot);
        if (observed_update_stats.decoded_buffers > 0 ||
            observed_update_stats.hit_decode_failure) {
          append_opus_playback_perf_log(OpusPerfEvent::DecodeLoopFinish,
                                        observed_snapshot);
        }
        g_opus_observed_max_decoded_buffers = 0;
        g_opus_observed_decode_failure = false;
        g_opus_last_frame_observe_ms = now_ms;
      }
    }

    if (use_opus_poc_for_update) {
      LightLock_Lock(&ctx.lock);
      use_webm_poc =
          ctx.active_stream_mode == StreamContainerMode::ProxyWebmOpus;
      playback_stage = ctx.webm_playback_stage;
      const bool seek_active = ctx.active_stream_seek_seq > 0;
      controller_input =
          make_webm_controller_input(collect_playback_core_snapshot(),
                                     opus_update_stats, opus_player, ctx);
      LightLock_Unlock(&ctx.lock);
      const WebmPlaybackControllerConfig controller_config =
          controller_config_for_seek(seek_active);
      controller_decision =
          use_webm_poc
              ? decide_webm_playback_step(playback_stage, controller_input,
                                          controller_config)
              : WebmPlaybackControllerDecision{};
      const WebmPlaybackStage next_stage =
          use_webm_poc
              ? next_webm_playback_stage(playback_stage, controller_input,
                                         controller_decision)
              : WebmPlaybackStage::Steady;
      const char *webm_update_pressure_event = NULL;
      if (use_webm_poc && should_log_webm_update_pressure(
                              opus_update_stats, controller_input, seek_active,
                              osGetTime(), &webm_update_pressure_event)) {
        append_webm_update_pressure_log(
            webm_update_pressure_event,
            playback_observer_current_session_kind(), playback_stage,
            controller_input, controller_decision, opus_update_stats,
            seek_active, controller_input.user_paused);
      }
      LightLock_Lock(&ctx.lock);
      if (use_webm_poc) {
        ctx.webm_playback_stage = next_stage;
      }
      const bool entering_steady_from_prebuffer =
          use_webm_poc && playback_stage == WebmPlaybackStage::Prebuffering &&
          next_stage == WebmPlaybackStage::Steady;
      const bool should_unpause = use_webm_poc &&
                                  (controller_decision.release_prebuffer ||
                                   entering_steady_from_prebuffer) &&
                                  !ctx.is_paused;
      LightLock_Unlock(&ctx.lock);
      if (should_unpause) {
        append_webm_playback_log("prebuffer_release", opus_buffer_size);
        append_webm_queue_state_log(
            "prebuffer_release", playback_observer_current_session_kind(),
            playback_stage, controller_input, controller_decision,
            opus_update_stats, seek_active, controller_input.user_paused);
        ndspChnSetPaused(0, false);
      }
      if (use_webm_poc && playback_stage == WebmPlaybackStage::Prebuffering &&
          opus_update_stats.decoded_buffers > 0 &&
          !controller_decision.release_prebuffer) {
        append_webm_queue_state_log("prebuffer_hold_after_pcm",
                                    playback_observer_current_session_kind(),
                                    playback_stage, controller_input,
                                    controller_decision, opus_update_stats,
                                    seek_active, controller_input.user_paused);
      }
      if (use_webm_poc && next_stage == WebmPlaybackStage::Steady) {
        bool keep_seek_tuning = false;
        LightLock_Lock(&ctx.lock);
        keep_seek_tuning = ctx.active_stream_seek_seq > 0;
        LightLock_Unlock(&ctx.lock);
        opus_player.set_decode_tuning(keep_seek_tuning
                                          ? webm_seek_decode_tuning()
                                          : webm_steady_decode_tuning());
      }
      if (opus_player.has_decode_failed()) {
        log_playback_compare_event(ctx.active_stream_mode,
                                   PlaybackCompareEvent::DecodeFailure,
                                   opus_update_stats);
        LightLock_Lock(&ctx.lock);
        const bool failed_during_seek = ctx.active_stream_seek_seq > 0;
        ctx.opus_playback_failure =
            ctx.active_stream_mode == StreamContainerMode::ProxyWebmOpus
                ? webm_error_to_failure(opus_player.webm_remux_error())
                : OpusPlaybackFailure::Decoder;
        if (ctx.opus_playback_failure == OpusPlaybackFailure::None) {
          ctx.opus_playback_failure = OpusPlaybackFailure::Decoder;
        }
        ctx.g_status_msg = playback_failure_message(ctx.opus_playback_failure);
        ctx.playing_id = "";
        reset_thumbnail_interference_log_state(&ctx);
        OpusPocPlayer::is_playing = false;
        ctx.webm_playback_stage = WebmPlaybackStage::Failed;
        if (failed_during_seek) {
          append_webm_queue_state_log("decode_failure_during_seek",
                                      playback_observer_current_session_kind(),
                                      playback_stage, controller_input,
                                      controller_decision, opus_update_stats,
                                      true, ctx.is_paused);
          clear_active_webm_seek_runtime(&ctx);
        }
        LightLock_Unlock(&ctx.lock);
        playback_observer_end_session();
      }
    }

    // Clear buffering flag once audio actually starts playing
    bool waiting_for_prebuffer_release = false;
    LightLock_Lock(&ctx.lock);
    waiting_for_prebuffer_release =
        ctx.webm_playback_stage == WebmPlaybackStage::Prebuffering;
    LightLock_Unlock(&ctx.lock);
    bool has_started_playing = use_opus_poc_for_update &&
                               opus_player.has_started_playing() &&
                               !waiting_for_prebuffer_release;
    const bool pcm_queued_this_update =
        use_opus_poc_for_update && opus_update_stats.decoded_buffers > 0;
    if (pcm_queued_this_update && !g_opus_first_pcm_queued_logged) {
      size_t logged_opus_buffer_size = 0;
      LightLock_Lock(&stream_lock);
      logged_opus_buffer_size =
          g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
      LightLock_Unlock(&stream_lock);
      append_opus_playback_perf_log(OpusPerfEvent::AudioStarted,
                                    logged_opus_buffer_size);
      g_opus_first_pcm_queued_logged = true;
      log_playback_compare_event(ctx.active_stream_mode,
                                 PlaybackCompareEvent::FirstPcmQueued,
                                 opus_update_stats);
    }
    if (use_opus_poc_for_update && has_started_playing &&
        !g_opus_audio_started_logged) {
      g_opus_audio_started_logged = true;
    }
    if (use_opus_poc_for_update && use_webm_poc && pcm_queued_this_update &&
        !g_webm_first_pcm_queued_logged) {
      append_webm_playback_log("first_pcm_queued", opus_buffer_size);
      append_webm_queue_state_log(
          "first_pcm_queued", playback_observer_current_session_kind(),
          playback_stage, controller_input, controller_decision,
          opus_update_stats, ctx.active_stream_seek_seq > 0, ctx.is_paused);
      PlaybackObserverEventTimes phase5a_times = {};
      if (playback_observer_get_event_times(&phase5a_times)) {
        append_webm_phase5a_summary_log(
            playback_observer_current_session_kind(), phase5a_times);
      }
      int active_seek_seq = 0;
      int active_seek_target_ms = 0;
      u64 active_seek_plan_ready_at_ms = 0;
      bool repeated_seek = false;
      WebmSeekReuseClass reuse_class = WebmSeekReuseClass::Cold;
      WebmSeekPlan active_seek_plan = {};
      LightLock_Lock(&ctx.lock);
      active_seek_seq = ctx.active_stream_seek_seq;
      active_seek_target_ms = ctx.active_stream_seek_target_ms;
      active_seek_plan_ready_at_ms = ctx.active_stream_seek_plan_ready_at_ms;
      repeated_seek = ctx.active_stream_repeated_seek;
      reuse_class = ctx.active_stream_reuse_class;
      active_seek_plan = ctx.active_stream_seek_plan;
      LightLock_Unlock(&ctx.lock);
      if (active_seek_seq > 0 && active_seek_plan_ready_at_ms > 0) {
        WebmSeekRequest seek_request = {};
        seek_request.target_ms = active_seek_target_ms;
        seek_request.seek_seq = active_seek_seq;
        PlaybackObserverEventTimes seek_times = {};
        if (playback_observer_get_event_times(&seek_times)) {
          append_webm_seek_stage_summary_log(
              "first_pcm_after_seek", active_seek_seq, active_seek_target_ms,
              osGetTime() - active_seek_plan_ready_at_ms,
              seek_times.first_pcm_queued_seen ? seek_times.first_pcm_queued_ms
                                               : 0,
              seek_times);
        }
        append_webm_seek_trace_log(
            "first_pcm_after_seek", active_seek_seq, seek_request,
            repeated_seek, reuse_class, &active_seek_plan, NULL,
            active_seek_plan.reconnect_start_byte,
            active_seek_plan.selected_cluster_ms, active_seek_plan.cache_hit,
            osGetTime() - active_seek_plan_ready_at_ms);
        uint64_t runtime_seek_start_byte = 0;
        int runtime_seek_timecode_ms = -1;
        if (opus_player.get_webm_last_seek_runtime_point(
                &runtime_seek_start_byte, &runtime_seek_timecode_ms)) {
          LightLock_Lock(&ctx.lock);
          const WebmSeekCacheStoreTrace store_trace =
              append_webm_seek_runtime_cache_point(&ctx.webm_seek_track_state,
                                                   runtime_seek_timecode_ms,
                                                   runtime_seek_start_byte);
          append_webm_seek_cache_store_log("seek_runtime_cache_harvest",
                                           active_seek_seq,
                                           active_seek_target_ms, store_trace);
          ctx.active_stream_runtime_cache_saved =
              store_trace.status != WebmSeekCacheStoreStatus::IgnoredInvalid;
          ctx.active_stream_first_pcm_queued_logged = true;
          ctx.active_stream_first_pcm_queued_at_ms = osGetTime();
          LightLock_Unlock(&ctx.lock);
        }
      }
      g_webm_first_pcm_queued_logged = true;
    }
    if (use_opus_poc_for_update && use_webm_poc &&
        ctx.active_stream_first_pcm_queued_logged &&
        ctx.active_stream_first_pcm_queued_at_ms > 0 && !ctx.is_paused &&
        !has_started_playing &&
        osGetTime() >= ctx.active_stream_first_pcm_queued_at_ms + 1000ULL) {
      append_webm_queue_state_log(
          "queue_to_playing_slow", playback_observer_current_session_kind(),
          playback_stage, controller_input, controller_decision,
          opus_update_stats, true, ctx.is_paused);
      LightLock_Lock(&ctx.lock);
      ctx.active_stream_first_pcm_queued_at_ms = osGetTime();
      LightLock_Unlock(&ctx.lock);
    }
    if (use_opus_poc_for_update && use_webm_poc && has_started_playing &&
        !g_webm_audio_started_logged) {
      int active_seek_seq = 0;
      int active_seek_target_ms = 0;
      u64 active_seek_plan_ready_at_ms = 0;
      LightLock_Lock(&ctx.lock);
      active_seek_seq = ctx.active_stream_seek_seq;
      active_seek_target_ms = ctx.active_stream_seek_target_ms;
      active_seek_plan_ready_at_ms = ctx.active_stream_seek_plan_ready_at_ms;
      LightLock_Unlock(&ctx.lock);
      if (active_seek_seq > 0 && active_seek_plan_ready_at_ms > 0) {
        PlaybackObserverEventTimes seek_times = {};
        if (playback_observer_get_event_times(&seek_times)) {
          append_webm_seek_stage_summary_log(
              "audio_started_after_seek", active_seek_seq,
              active_seek_target_ms, osGetTime() - active_seek_plan_ready_at_ms,
              seek_times.audio_audible_seen ? seek_times.audio_audible_ms : 0,
              seek_times);
        }
      }
      g_webm_audio_started_logged = true;
    }
    if (use_opus_poc_for_update && has_started_playing &&
        !ctx.compare_audio_audible_logged) {
      log_playback_compare_event(ctx.active_stream_mode,
                                 PlaybackCompareEvent::AudioAudible,
                                 opus_update_stats);
      LightLock_Lock(&ctx.lock);
      ctx.compare_audio_audible_logged = true;
      LightLock_Unlock(&ctx.lock);
    }
    if (use_opus_poc_for_update && use_webm_poc) {
      LightLock_Lock(&ctx.lock);
      if (ctx.active_stream_seek_seq > 0 &&
          ctx.active_stream_first_pcm_queued_logged &&
          (has_started_playing || ctx.is_paused)) {
        clear_active_webm_seek_runtime(&ctx);
      }
      LightLock_Unlock(&ctx.lock);
    }
    if (use_opus_poc_for_update && has_started_playing && !use_webm_poc) {
      LightLock_Lock(&ctx.lock);
      if (ctx.webm_playback_stage != WebmPlaybackStage::Failed) {
        ctx.webm_playback_stage = WebmPlaybackStage::Steady;
      }
      LightLock_Unlock(&ctx.lock);
    }
    if (ctx.is_buffering && has_started_playing) {
      LightLock_Lock(&ctx.lock);
      ctx.is_buffering = false;
      if (!ctx.is_paused && ctx.pause_started_at > 0) {
        ctx.pause_accumulated_ms += osGetTime() - ctx.pause_started_at;
        ctx.pause_started_at = 0;
      }
      LightLock_Unlock(&ctx.lock);
    }

    bool active_is_playing =
        use_opus_poc_for_update && OpusPocPlayer::is_playing;
    if (active_is_playing && has_started_playing && !ctx.is_paused) {
      LightLock_Lock(&ctx.lock);
      ctx.g_status_msg =
          playback_status_message(ctx.is_paused, ctx.is_buffering);
      LightLock_Unlock(&ctx.lock);
    }

    // --- Rendering (MVC: View) ---
    clamp_scroll_x_for_current_screen(ctx, ui_mgr);
    LightLock_Lock(&ctx.lock);
    RenderContext render_ctx = make_render_snapshot_locked(ctx);
    LightLock_Unlock(&ctx.lock);

    // Draw without holding the lock
    // If it's a popup, UIRenderer needs to draw the overlay on top of whatever
    // screen was behind it.
    bool is_popup = is_popup_state(render_ctx.current_state);

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
  // 1. Wait for thread to fully terminate (attached, so U64_MAX blocks
  // reliably)
  if (threadId) {
    threadJoin(threadId, U64_MAX);
    threadFree(threadId);
  }

  // 2. Safely stop player
  opus_player.stop();
  playback_observer_end_session();

  // 3. Clean up network library before socExit
  api.cleanup();

  // 4. Destroy C++ heap objects before hardware shutdown
  if (g_stream_buffer_ptr) {
    g_stream_buffer_ptr->clear();
    g_stream_buffer_ptr->shrink_to_fit();
    g_stream_buffer_ptr.reset();
  }

  g_opus_player_ptr.reset();
  g_playlist_manager_ptr.reset();
  // Wait for thumbnail thread to finish (if running)
  {
    bool still_loading;
    do {
      LightLock_Lock(&ctx.lock);
      still_loading = ctx.thumbnail_loading;
      LightLock_Unlock(&ctx.lock);
      if (still_loading) svcSleepThread(10 * 1000 * 1000);
    } while (still_loading);
  }
  // Unload GPU textures stored in ctx before destroying ctx
  ctx.thumbnail_tex.unload();
  g_ctx_ptr.reset();

  // 5. Destroy network class before socExit
  g_api_ptr.reset();

  // 6. Explicitly destroy local C++ objects before scope exit
  // (destructors would access already-exited hardware, causing crash)
  // Unload wallpaper texture before GPU shutdown (stack var, must unload
  // manually)
  g_wallpaper.unload();
  // Destroy ScreenManager first (HomeScreen holds TouchButton vector)
  screen_mgr.clear();
  cleanup_ui_icon_cache();
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

  if (soc_buffer) free(soc_buffer);

  return 0;
}
