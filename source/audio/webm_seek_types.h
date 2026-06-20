#ifndef WEBM_SEEK_TYPES_H
#define WEBM_SEEK_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

enum class WebmSeekPlanSource {
  Invalid,
  ExactClusterCache,
  RuntimeClusterCacheWarmStart,
  CueIndex,
  ProbeCluster,
  CoarseEstimate,
};

enum class WebmSeekReuseClass {
  Cold,
  MetadataWarm,
  CuesWarm,
  ClusterCacheWarm,
};

enum class WebmSeekCacheLookupStatus {
  NotChecked,
  Empty,
  ExactHit,
  WarmStartHit,
  ExactMissNoCandidate,
  ExactRejectedGap,
  WarmStartRejectedGap,
  RejectedFuture,
  RejectedInvalid,
  ProbeEstimateHit,
  ProbeEstimateMiss,
};

enum class WebmSeekCacheStoreStatus {
  IgnoredInvalid,
  Added,
  UpdatedExisting,
  EvictedOldest,
};

inline const char *webm_seek_plan_source_name(WebmSeekPlanSource source) {
  switch (source) {
    case WebmSeekPlanSource::Invalid:
      return "invalid";
    case WebmSeekPlanSource::ExactClusterCache:
      return "cache";
    case WebmSeekPlanSource::RuntimeClusterCacheWarmStart:
      return "cache_warm_start";
    case WebmSeekPlanSource::CueIndex:
      return "cues";
    case WebmSeekPlanSource::ProbeCluster:
      return "probe";
    case WebmSeekPlanSource::CoarseEstimate:
      return "coarse";
  }
  return "unknown";
}

inline const char *webm_seek_reuse_class_name(WebmSeekReuseClass value) {
  switch (value) {
    case WebmSeekReuseClass::Cold:
      return "cold";
    case WebmSeekReuseClass::MetadataWarm:
      return "metadata";
    case WebmSeekReuseClass::CuesWarm:
      return "cues";
    case WebmSeekReuseClass::ClusterCacheWarm:
      return "cluster_cache";
  }
  return "unknown";
}

struct WebmSeekSourceInfo {
  bool has_filesize = false;
  uint64_t filesize = 0;
  bool has_duration_ms = false;
  uint64_t duration_ms = 0;
  uint64_t preroll_ms = 0;
  uint64_t backtrack_bytes = 0;
};

struct WebmSeekPlan {
  bool valid = false;
  int seek_target_ms = 0;
  int emit_start_ms = 0;
  uint64_t reconnect_start_byte = 0;
  int selected_cluster_ms = -1;
  WebmSeekPlanSource source = WebmSeekPlanSource::Invalid;
  bool cluster_aligned = false;
  bool cache_hit = false;
};

struct WebmSeekProbe {
  const uint8_t *data = NULL;
  size_t size = 0;
  uint64_t start_byte = 0;
  uint64_t segment_data_offset = 0;
};

struct WebmSeekCacheLookupTrace {
  WebmSeekCacheLookupStatus exact_status =
      WebmSeekCacheLookupStatus::NotChecked;
  WebmSeekCacheLookupStatus probe_status =
      WebmSeekCacheLookupStatus::NotChecked;
  // Candidate counts describe the last cache lookup that populated this trace:
  // exact/warm-start cluster selection, or probe-byte estimation on fallback.
  size_t cache_size = 0;
  size_t valid_point_count = 0;
  size_t invalid_point_count = 0;
  size_t future_point_count = 0;
  int target_ms = 0;
  int desired_cluster_ms = 0;
  int max_reuse_gap_ms = 0;
  // This is the configured warm-start limit, not an observed maximum gap.
  int max_warm_start_gap_ms = 0;
  int best_cluster_ms = -1;
  int best_gap_ms = -1;
  uint64_t best_start_byte = 0;
  uint64_t estimated_probe_start_byte = 0;
};

struct WebmSeekCacheStoreTrace {
  WebmSeekCacheStoreStatus status = WebmSeekCacheStoreStatus::IgnoredInvalid;
  int timecode_ms = -1;
  uint64_t start_byte = 0;
  size_t cache_size_after = 0;
};

struct WebmSeekClusterPoint {
  int timecode_ms = 0;
  uint64_t start_byte = 0;
};

struct WebmSeekRequest {
  int target_ms = 0;
  int seek_seq = 0;
};

struct WebmSeekPlanningBreakdown {
  uint64_t request_to_metadata_ms = 0;
  uint64_t metadata_to_cache_ms = 0;
  uint64_t cache_to_probe_ms = 0;
  uint64_t probe_to_ready_ms = 0;
  uint64_t request_to_ready_ms = 0;
  uint32_t header_probe_count = 0;
  uint32_t cues_probe_count = 0;
  uint32_t cluster_probe_count = 0;
  uint32_t extra_probe_count = 0;
  bool used_cached_metadata = false;
  bool checked_cues = false;
  bool used_cues = false;
  bool used_cluster_probe = false;
  bool used_extra_probe = false;
};

struct WebmSeekExecutionContext {
  int seek_seq = 0;
  int target_ms = 0;
  WebmSeekPlan plan;
  bool repeated_seek = false;
  WebmSeekReuseClass reuse_class = WebmSeekReuseClass::Cold;
};

struct WebmSeekTrackState {
  WebmSeekSourceInfo source_info;
  bool source_info_ready = false;
  bool cues_checked = false;
  bool cues_available = false;
  uint64_t cues_absolute_offset = 0;
  uint64_t segment_data_offset = 0;
  std::vector<WebmSeekClusterPoint> cluster_cache;
};

#endif
