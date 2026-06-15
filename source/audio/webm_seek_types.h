#ifndef WEBM_SEEK_TYPES_H
#define WEBM_SEEK_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

enum class WebmSeekPlanSource {
  Invalid,
  ExactClusterCache,
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
  ExactMissNoCandidate,
  ExactRejectedGap,
  ProbeEstimateHit,
  ProbeEstimateMiss,
};

enum class WebmSeekCacheStoreStatus {
  IgnoredInvalid,
  Added,
  UpdatedExisting,
  EvictedOldest,
};

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
  size_t cache_size = 0;
  int target_ms = 0;
  int desired_cluster_ms = 0;
  int max_reuse_gap_ms = 0;
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
