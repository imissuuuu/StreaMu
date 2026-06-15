#include "webm_seek_runtime.h"

#include <3ds.h>

#include "network/youtube_api.h"
#include "webm_seek_index.h"
#include "webm_seek_planner.h"

namespace {

static const size_t kWebmSeekClusterCacheLimit = 128U;
static const size_t kWebmSeekProbeExtractedClusterLimit = 128U;
static const uint64_t kWebmSeekExtraProbeMinAdvanceBytes = 64ULL * 1024ULL;
static const uint64_t kWebmSeekExtraProbeFallbackAdvanceBytes =
    128ULL * 1024ULL;
static const bool kWebmSeekPreferColdCoarseFastPath = true;

static void clamp_seek_plan_start_byte(const WebmSeekSourceInfo &source_info,
                                       WebmSeekPlan *plan) {
  if (!plan) {
    return;
  }
  if (source_info.has_filesize &&
      plan->reconnect_start_byte > source_info.filesize) {
    plan->reconnect_start_byte = source_info.filesize;
  }
}

static WebmSeekReuseClass
classify_webm_seek_reuse(const WebmSeekTrackState &track_state) {
  if (!track_state.cluster_cache.empty()) {
    return WebmSeekReuseClass::ClusterCacheWarm;
  }
  if (track_state.cues_checked) {
    return WebmSeekReuseClass::CuesWarm;
  }
  if (track_state.source_info_ready) {
    return WebmSeekReuseClass::MetadataWarm;
  }
  return WebmSeekReuseClass::Cold;
}

static WebmSeekCacheStoreTrace
append_cluster_cache_point(WebmSeekTrackState *state,
                           const WebmSeekClusterPoint &point) {
  WebmSeekCacheStoreTrace trace = {};
  trace.timecode_ms = point.timecode_ms;
  trace.start_byte = point.start_byte;
  if (!state || point.timecode_ms < 0) {
    return trace;
  }
  for (size_t i = 0; i < state->cluster_cache.size(); ++i) {
    const int diff_ms =
        state->cluster_cache[i].timecode_ms > point.timecode_ms
            ? state->cluster_cache[i].timecode_ms - point.timecode_ms
            : point.timecode_ms - state->cluster_cache[i].timecode_ms;
    if (state->cluster_cache[i].start_byte == point.start_byte ||
        diff_ms <= 20) {
      if (point.timecode_ms > state->cluster_cache[i].timecode_ms) {
        state->cluster_cache[i] = point;
      }
      trace.status = WebmSeekCacheStoreStatus::UpdatedExisting;
      trace.cache_size_after = state->cluster_cache.size();
      return trace;
    }
  }
  if (state->cluster_cache.size() >= kWebmSeekClusterCacheLimit) {
    state->cluster_cache.erase(state->cluster_cache.begin());
    trace.status = WebmSeekCacheStoreStatus::EvictedOldest;
  } else {
    trace.status = WebmSeekCacheStoreStatus::Added;
  }
  state->cluster_cache.push_back(point);
  trace.cache_size_after = state->cluster_cache.size();
  return trace;
}

static void append_cluster_cache_points(WebmSeekTrackState *state,
                                        const WebmSeekClusterPoint *points,
                                        size_t point_count) {
  if (!state || !points || point_count == 0U) {
    return;
  }
  for (size_t i = 0; i < point_count; ++i) {
    (void)append_cluster_cache_point(state, points[i]);
  }
}

} // namespace

PreparedWebmSeekPlanResult prepare_webm_seek_plan(
    YouTubeAPI *api, const std::string &stream_video_id,
    const WebmSeekRequest &request, uint64_t pending_seek_backtrack_bytes,
    int pending_seek_retry_count, uint64_t seek_plan_preroll_ms,
    uint64_t header_probe_size_bytes, uint64_t probe_size_bytes,
    WebmSeekBacktrackBytesFn backtrack_bytes_for_retry,
    WebmSeekTraceLogFn trace_log, WebmSeekTrackState *track_state) {
  PreparedWebmSeekPlanResult result = {};
  if (!api || stream_video_id.empty() || !track_state ||
      request.target_ms <= 0 || !backtrack_bytes_for_retry || !trace_log) {
    return result;
  }

  result.reuse_class = classify_webm_seek_reuse(*track_state);
  const bool repeated_seek = result.reuse_class != WebmSeekReuseClass::Cold;
  result.repeated_seek = repeated_seek;
  const uint64_t request_started_at_ms = osGetTime();
  trace_log("seek_request", request.seek_seq, request, repeated_seek,
            result.reuse_class, NULL, &result.cache_trace, 0, -1, false, 0);

  if (track_state->source_info_ready && track_state->source_info.has_filesize &&
      track_state->source_info.has_duration_ms &&
      track_state->source_info.filesize > 0 &&
      track_state->source_info.duration_ms > 0) {
    result.metadata_ok = true;
    result.source_info = track_state->source_info;
  } else {
    WebmSeekStreamInfo seek_info = {};
    if (!api->get_webm_seek_stream_info(stream_video_id, &seek_info)) {
      return result;
    }
    result.metadata_ok = true;
    result.source_info.has_filesize = seek_info.has_filesize;
    result.source_info.filesize = seek_info.filesize;
    result.source_info.has_duration_ms = seek_info.has_duration_ms;
    result.source_info.duration_ms = seek_info.duration_ms;
  }
  result.source_info.preroll_ms = seek_plan_preroll_ms;
  result.source_info.backtrack_bytes =
      pending_seek_backtrack_bytes > 0
          ? pending_seek_backtrack_bytes
          : backtrack_bytes_for_retry(pending_seek_retry_count);
  track_state->source_info = result.source_info;
  track_state->source_info_ready = true;
  trace_log("seek_info_done", request.seek_seq, request, repeated_seek,
            result.reuse_class, NULL, &result.cache_trace, 0, -1, false,
            osGetTime() - request_started_at_ms);

  result.seek_plan =
      build_webm_seek_plan(request.target_ms / 1000, result.source_info);
  if (!result.seek_plan.valid) {
    return result;
  }

  if (!track_state->cluster_cache.empty() && !result.seek_plan.cache_hit) {
    uint64_t cached_probe_start_byte = 0;
    if (estimate_webm_seek_probe_start_byte_from_cache(
            result.source_info, track_state->cluster_cache.data(),
            track_state->cluster_cache.size(), request.target_ms,
            &cached_probe_start_byte, &result.cache_trace)) {
      result.seek_plan.reconnect_start_byte = cached_probe_start_byte;
      clamp_seek_plan_start_byte(result.source_info, &result.seek_plan);
    }
  }

  if (!track_state->cluster_cache.empty() &&
      choose_webm_seek_cluster_from_cache(
          result.source_info, track_state->cluster_cache.data(),
          track_state->cluster_cache.size(), &result.seek_plan, NULL,
          &result.cache_trace)) {
    result.parser_cluster_aligned = true;
  }
  trace_log("seek_index_lookup_done", request.seek_seq, request, repeated_seek,
            result.reuse_class, &result.seek_plan, &result.cache_trace,
            result.seek_plan.reconnect_start_byte,
            result.seek_plan.selected_cluster_ms, result.seek_plan.cache_hit,
            osGetTime() - request_started_at_ms);

  if (kWebmSeekPreferColdCoarseFastPath && !result.parser_cluster_aligned &&
      result.seek_plan.source == WebmSeekPlanSource::CoarseEstimate &&
      !track_state->cues_checked) {
    // In parser-level seek mode, a coarse byte estimate is often good enough
    // when we do not yet have a usable cache/cue plan, and it avoids a slow
    // extra probe round-trip before parser seek can refine the position.
    result.parser_cluster_aligned = true;
  }

  if (!result.parser_cluster_aligned && !track_state->cues_checked) {
    const std::string header_probe_bytes =
        api->get_webm_seek_probe(stream_video_id, 0, header_probe_size_bytes);
    if (!header_probe_bytes.empty()) {
      WebmSeekProbe header_probe = {};
      header_probe.data =
          reinterpret_cast<const uint8_t *>(header_probe_bytes.data());
      header_probe.size = header_probe_bytes.size();
      header_probe.start_byte = 0;
      if (locate_webm_cues(header_probe, &track_state->cues_absolute_offset,
                           &track_state->segment_data_offset)) {
        track_state->cues_checked = true;
        track_state->cues_available = true;
      } else {
        track_state->cues_checked = true;
        track_state->cues_available = false;
        track_state->cues_absolute_offset = 0;
        track_state->segment_data_offset = 0;
      }
    }
  }

  if (!result.parser_cluster_aligned && track_state->cues_available &&
      track_state->segment_data_offset > 0) {
    const std::string cues_probe_bytes = api->get_webm_seek_probe(
        stream_video_id, track_state->cues_absolute_offset,
        header_probe_size_bytes);
    if (!cues_probe_bytes.empty()) {
      WebmSeekProbe cues_probe = {};
      cues_probe.data =
          reinterpret_cast<const uint8_t *>(cues_probe_bytes.data());
      cues_probe.size = cues_probe_bytes.size();
      cues_probe.start_byte = track_state->cues_absolute_offset;
      cues_probe.segment_data_offset = track_state->segment_data_offset;
      if (refine_webm_seek_plan_with_cues(result.source_info, cues_probe,
                                          &result.seek_plan, NULL)) {
        result.parser_cluster_aligned = true;
        WebmSeekClusterPoint cue_point = {};
        cue_point.timecode_ms = result.seek_plan.selected_cluster_ms;
        cue_point.start_byte = result.seek_plan.reconnect_start_byte;
        (void)append_cluster_cache_point(track_state, cue_point);
      }
    }
  }

  if (!result.parser_cluster_aligned &&
      result.seek_plan.source != WebmSeekPlanSource::CueIndex) {
    uint64_t coarse_start_byte = result.seek_plan.reconnect_start_byte;
    const std::string probe_bytes = api->get_webm_seek_probe(
        stream_video_id, coarse_start_byte, probe_size_bytes);
    if (!probe_bytes.empty()) {
      WebmSeekProbe probe = {};
      probe.data = reinterpret_cast<const uint8_t *>(probe_bytes.data());
      probe.size = probe_bytes.size();
      probe.start_byte = coarse_start_byte;
      if (refine_webm_seek_plan_with_probe(result.source_info, probe,
                                           &result.seek_plan, NULL)) {
        result.parser_cluster_aligned = true;
      }
      WebmSeekClusterPoint
          extracted_points[kWebmSeekProbeExtractedClusterLimit];
      size_t extracted_count = 0;
      if (extract_webm_seek_clusters(probe, extracted_points,
                                     kWebmSeekProbeExtractedClusterLimit,
                                     &extracted_count)) {
        append_cluster_cache_points(track_state, extracted_points,
                                    extracted_count);
      }

      const int initial_gap_ms =
          result.seek_plan.selected_cluster_ms >= 0
              ? (request.target_ms - result.seek_plan.selected_cluster_ms)
              : -1;
      if (result.parser_cluster_aligned &&
          result.seek_plan.source == WebmSeekPlanSource::ProbeCluster &&
          result.cache_trace.max_reuse_gap_ms > 0 &&
          initial_gap_ms > result.cache_trace.max_reuse_gap_ms) {
        uint64_t extra_probe_start_byte = 0;
        bool have_extra_probe_start =
            estimate_webm_seek_probe_start_byte_from_cache(
                result.source_info, track_state->cluster_cache.data(),
                track_state->cluster_cache.size(), request.target_ms,
                &extra_probe_start_byte, &result.cache_trace);
        const uint64_t fallback_probe_start_byte =
            result.seek_plan.reconnect_start_byte +
            kWebmSeekExtraProbeFallbackAdvanceBytes;
        if (!have_extra_probe_start ||
            extra_probe_start_byte <= coarse_start_byte ||
            extra_probe_start_byte - coarse_start_byte <
                kWebmSeekExtraProbeMinAdvanceBytes) {
          extra_probe_start_byte = fallback_probe_start_byte;
          have_extra_probe_start = true;
        }
        if (have_extra_probe_start) {
          if (result.source_info.has_filesize &&
              extra_probe_start_byte > result.source_info.filesize) {
            extra_probe_start_byte = result.source_info.filesize;
          }
        }
        if (have_extra_probe_start &&
            extra_probe_start_byte > coarse_start_byte &&
            extra_probe_start_byte - coarse_start_byte >=
                kWebmSeekExtraProbeMinAdvanceBytes) {
          const std::string extra_probe_bytes = api->get_webm_seek_probe(
              stream_video_id, extra_probe_start_byte, probe_size_bytes);
          if (!extra_probe_bytes.empty()) {
            WebmSeekProbe extra_probe = {};
            extra_probe.data =
                reinterpret_cast<const uint8_t *>(extra_probe_bytes.data());
            extra_probe.size = extra_probe_bytes.size();
            extra_probe.start_byte = extra_probe_start_byte;

            WebmSeekPlan refined_plan = result.seek_plan;
            if (refine_webm_seek_plan_with_probe(
                    result.source_info, extra_probe, &refined_plan, NULL)) {
              const int refined_gap_ms =
                  request.target_ms - refined_plan.selected_cluster_ms;
              if (refined_gap_ms >= 0 && refined_gap_ms < initial_gap_ms) {
                result.seek_plan = refined_plan;
                result.parser_cluster_aligned = true;
              }
            }

            WebmSeekClusterPoint
                extra_points[kWebmSeekProbeExtractedClusterLimit];
            size_t extra_count = 0;
            if (extract_webm_seek_clusters(extra_probe, extra_points,
                                           kWebmSeekProbeExtractedClusterLimit,
                                           &extra_count)) {
              append_cluster_cache_points(track_state, extra_points,
                                          extra_count);
            }
          }
        }
      }
    }
  }

  if (result.seek_plan.valid && result.seek_plan.cluster_aligned &&
      result.seek_plan.selected_cluster_ms >= 0) {
    WebmSeekClusterPoint planned_point = {};
    planned_point.timecode_ms = result.seek_plan.selected_cluster_ms;
    planned_point.start_byte = result.seek_plan.reconnect_start_byte;
    (void)append_cluster_cache_point(track_state, planned_point);
  }

  trace_log("seek_probe_done", request.seek_seq, request, repeated_seek,
            result.reuse_class, &result.seek_plan, &result.cache_trace,
            result.seek_plan.reconnect_start_byte,
            result.seek_plan.selected_cluster_ms, result.seek_plan.cache_hit,
            osGetTime() - request_started_at_ms);

  result.stream_url = api->build_webm_stream_url(
      stream_video_id, result.seek_plan.reconnect_start_byte);
  result.plan_ok = true;
  trace_log("seek_plan_ready", request.seek_seq, request, repeated_seek,
            result.reuse_class, &result.seek_plan, &result.cache_trace,
            result.seek_plan.reconnect_start_byte,
            result.seek_plan.selected_cluster_ms, result.seek_plan.cache_hit,
            osGetTime() - request_started_at_ms);
  return result;
}

WebmSeekCacheStoreTrace
append_webm_seek_runtime_cache_point(WebmSeekTrackState *state, int timecode_ms,
                                     uint64_t start_byte) {
  if (!state || timecode_ms < 0) {
    return WebmSeekCacheStoreTrace{};
  }
  WebmSeekClusterPoint point = {};
  point.timecode_ms = timecode_ms;
  point.start_byte = start_byte;
  return append_cluster_cache_point(state, point);
}
