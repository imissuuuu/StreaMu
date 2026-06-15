#include "webm_seek_planner.h"

namespace {

static const uint64_t kSeekBacktrackBytes = 128ULL * 1024ULL;
static const int kMaxSeekSeconds = 86400;
static const int kSeekProbeWarmupMs = 1200;
static const int kSeekCacheReuseSlackMs = 2000;

static int desired_cluster_time_ms(const WebmSeekSourceInfo &source_info,
                                   int target_ms) {
  const uint64_t desired_warmup_ms =
      source_info.preroll_ms > static_cast<uint64_t>(kSeekProbeWarmupMs)
          ? source_info.preroll_ms
          : static_cast<uint64_t>(kSeekProbeWarmupMs);
  return target_ms > static_cast<int>(desired_warmup_ms)
             ? (target_ms - static_cast<int>(desired_warmup_ms))
             : 0;
}

static bool build_coarse_seek_plan(int seek_seconds,
                                   const WebmSeekSourceInfo &source_info,
                                   WebmSeekPlan *out_plan) {
  if (!out_plan) {
    return false;
  }
  WebmSeekPlan plan = {};
  if (seek_seconds <= 0 || seek_seconds > kMaxSeekSeconds) {
    *out_plan = plan;
    return false;
  }
  if (!source_info.has_filesize || !source_info.has_duration_ms ||
      source_info.filesize == 0 || source_info.duration_ms == 0) {
    *out_plan = plan;
    return false;
  }

  const uint64_t seek_target_ms = static_cast<uint64_t>(seek_seconds) * 1000ULL;
  const uint64_t emit_start_ms = (seek_target_ms > source_info.preroll_ms)
                                     ? (seek_target_ms - source_info.preroll_ms)
                                     : 0ULL;
  uint64_t reconnect_start_byte =
      (emit_start_ms * source_info.filesize) / source_info.duration_ms;
  const uint64_t backtrack_bytes = source_info.backtrack_bytes > 0
                                       ? source_info.backtrack_bytes
                                       : kSeekBacktrackBytes;
  if (reconnect_start_byte > backtrack_bytes) {
    reconnect_start_byte -= backtrack_bytes;
  } else {
    reconnect_start_byte = 0;
  }
  if (reconnect_start_byte > source_info.filesize) {
    reconnect_start_byte = source_info.filesize;
  }

  plan.valid = true;
  plan.seek_target_ms = static_cast<int>(seek_target_ms);
  plan.emit_start_ms = static_cast<int>(emit_start_ms);
  plan.reconnect_start_byte = reconnect_start_byte;
  plan.selected_cluster_ms = static_cast<int>(emit_start_ms);
  plan.source = WebmSeekPlanSource::CoarseEstimate;
  plan.cluster_aligned = false;
  plan.cache_hit = false;
  *out_plan = plan;
  return true;
}

} // namespace

WebmSeekPlan build_webm_seek_plan(int seek_seconds,
                                  const WebmSeekSourceInfo &source_info) {
  WebmSeekPlan plan = {};
  build_coarse_seek_plan(seek_seconds, source_info, &plan);
  return plan;
}

bool choose_webm_seek_cluster_from_cache(const WebmSeekSourceInfo &source_info,
                                         const WebmSeekClusterPoint *points,
                                         size_t point_count,
                                         WebmSeekPlan *inout_plan,
                                         int *out_cluster_timecode_ms,
                                         WebmSeekCacheLookupTrace *out_trace) {
  if (out_trace) {
    out_trace->exact_status = WebmSeekCacheLookupStatus::NotChecked;
    out_trace->cache_size = point_count;
    out_trace->target_ms = inout_plan ? inout_plan->seek_target_ms : 0;
  }
  if (!points || point_count == 0U || !inout_plan || !inout_plan->valid) {
    if (out_trace) {
      out_trace->exact_status = WebmSeekCacheLookupStatus::Empty;
    }
    return false;
  }

  const uint64_t desired_warmup_ms =
      source_info.preroll_ms > static_cast<uint64_t>(kSeekProbeWarmupMs)
          ? source_info.preroll_ms
          : static_cast<uint64_t>(kSeekProbeWarmupMs);
  const int max_reuse_gap_ms =
      static_cast<int>(desired_warmup_ms) + kSeekCacheReuseSlackMs;
  if (out_trace) {
    out_trace->target_ms = inout_plan->seek_target_ms;
    out_trace->desired_cluster_ms =
        desired_cluster_time_ms(source_info, inout_plan->seek_target_ms);
    out_trace->max_reuse_gap_ms = max_reuse_gap_ms;
  }

  const WebmSeekClusterPoint *best = NULL;
  const WebmSeekClusterPoint *best_before_target = NULL;
  for (size_t i = 0; i < point_count; ++i) {
    const int reuse_gap_ms = inout_plan->seek_target_ms - points[i].timecode_ms;
    if (reuse_gap_ms >= 0) {
      if (!best_before_target ||
          points[i].timecode_ms > best_before_target->timecode_ms) {
        best_before_target = &points[i];
      }
    }
    if (reuse_gap_ms < 0 || reuse_gap_ms > max_reuse_gap_ms) {
      continue;
    }
    if (!best || points[i].timecode_ms > best->timecode_ms) {
      best = &points[i];
    }
  }
  if (!best) {
    if (out_trace) {
      if (best_before_target) {
        out_trace->exact_status = WebmSeekCacheLookupStatus::ExactRejectedGap;
        out_trace->best_cluster_ms = best_before_target->timecode_ms;
        out_trace->best_start_byte = best_before_target->start_byte;
        out_trace->best_gap_ms =
            inout_plan->seek_target_ms - best_before_target->timecode_ms;
      } else {
        out_trace->exact_status =
            WebmSeekCacheLookupStatus::ExactMissNoCandidate;
      }
    }
    return false;
  }

  inout_plan->reconnect_start_byte = best->start_byte;
  inout_plan->selected_cluster_ms = best->timecode_ms;
  inout_plan->source = WebmSeekPlanSource::ExactClusterCache;
  inout_plan->cluster_aligned = true;
  inout_plan->cache_hit = true;
  if (out_trace) {
    out_trace->exact_status = WebmSeekCacheLookupStatus::ExactHit;
    out_trace->best_cluster_ms = best->timecode_ms;
    out_trace->best_start_byte = best->start_byte;
    out_trace->best_gap_ms = inout_plan->seek_target_ms - best->timecode_ms;
  }
  if (out_cluster_timecode_ms) {
    *out_cluster_timecode_ms = best->timecode_ms;
  }
  return true;
}

bool estimate_webm_seek_probe_start_byte_from_cache(
    const WebmSeekSourceInfo &source_info, const WebmSeekClusterPoint *points,
    size_t point_count, int target_ms, uint64_t *out_start_byte,
    WebmSeekCacheLookupTrace *out_trace) {
  if (out_trace) {
    out_trace->probe_status = WebmSeekCacheLookupStatus::NotChecked;
    out_trace->cache_size = point_count;
    out_trace->target_ms = target_ms;
    out_trace->desired_cluster_ms =
        desired_cluster_time_ms(source_info, target_ms);
  }
  if (!points || point_count == 0U || !out_start_byte || target_ms <= 0 ||
      !source_info.has_filesize || source_info.filesize == 0U) {
    if (out_trace) {
      out_trace->probe_status = WebmSeekCacheLookupStatus::ProbeEstimateMiss;
    }
    return false;
  }

  const int desired_ms = desired_cluster_time_ms(source_info, target_ms);
  const uint64_t backtrack_bytes = source_info.backtrack_bytes > 0
                                       ? source_info.backtrack_bytes
                                       : kSeekBacktrackBytes;

  const WebmSeekClusterPoint *best_before = NULL;
  const WebmSeekClusterPoint *best_after = NULL;
  const WebmSeekClusterPoint *prev_before = NULL;

  for (size_t i = 0; i < point_count; ++i) {
    const WebmSeekClusterPoint *point = &points[i];
    if (point->timecode_ms <= desired_ms) {
      if (!best_before || point->timecode_ms > best_before->timecode_ms) {
        prev_before = best_before;
        best_before = point;
      } else if (!prev_before ||
                 point->timecode_ms > prev_before->timecode_ms) {
        prev_before = point;
      }
      continue;
    }
    if (!best_after || point->timecode_ms < best_after->timecode_ms) {
      best_after = point;
    }
  }

  uint64_t estimated_cluster_byte = 0U;
  bool estimated = false;
  if (best_before && best_after &&
      best_after->timecode_ms > best_before->timecode_ms &&
      best_after->start_byte > best_before->start_byte) {
    const uint64_t byte_span = best_after->start_byte - best_before->start_byte;
    const int time_span_ms = best_after->timecode_ms - best_before->timecode_ms;
    const int offset_ms = desired_ms - best_before->timecode_ms;
    estimated_cluster_byte = best_before->start_byte +
                             (byte_span * static_cast<uint64_t>(offset_ms)) /
                                 static_cast<uint64_t>(time_span_ms);
    estimated = true;
  } else if (best_before && prev_before &&
             best_before->timecode_ms > prev_before->timecode_ms &&
             best_before->start_byte > prev_before->start_byte) {
    const uint64_t byte_span =
        best_before->start_byte - prev_before->start_byte;
    const int time_span_ms =
        best_before->timecode_ms - prev_before->timecode_ms;
    const int offset_ms = desired_ms - best_before->timecode_ms;
    if (offset_ms >= 0) {
      estimated_cluster_byte = best_before->start_byte +
                               (byte_span * static_cast<uint64_t>(offset_ms)) /
                                   static_cast<uint64_t>(time_span_ms);
      estimated = true;
    }
  } else if (best_before) {
    if (source_info.has_duration_ms && source_info.duration_ms > 0U &&
        source_info.has_filesize) {
      const int offset_ms = desired_ms - best_before->timecode_ms;
      if (offset_ms > 0) {
        const uint64_t approx_bytes_per_ms =
            source_info.filesize / source_info.duration_ms;
        estimated_cluster_byte =
            best_before->start_byte +
            (static_cast<uint64_t>(offset_ms) * approx_bytes_per_ms);
        estimated = true;
      }
    }
    if (!estimated) {
      estimated_cluster_byte = best_before->start_byte;
      estimated = true;
    }
  }

  if (!estimated) {
    if (out_trace) {
      out_trace->probe_status = WebmSeekCacheLookupStatus::ProbeEstimateMiss;
    }
    return false;
  }

  uint64_t probe_start_byte = estimated_cluster_byte > backtrack_bytes
                                  ? (estimated_cluster_byte - backtrack_bytes)
                                  : 0U;
  if (probe_start_byte > source_info.filesize) {
    probe_start_byte = source_info.filesize;
  }
  *out_start_byte = probe_start_byte;
  if (out_trace) {
    out_trace->probe_status = WebmSeekCacheLookupStatus::ProbeEstimateHit;
    out_trace->estimated_probe_start_byte = probe_start_byte;
  }
  return true;
}
