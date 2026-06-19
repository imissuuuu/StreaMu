#ifndef WEBM_SEEK_RUNTIME_H
#define WEBM_SEEK_RUNTIME_H

#include <stdint.h>

#include <string>

#include "webm_seek_types.h"

class YouTubeAPI;

typedef uint64_t (*WebmSeekBacktrackBytesFn)(int retry_count);

typedef void (*WebmSeekTraceLogFn)(
    const char *event, int seek_seq, const WebmSeekRequest &request,
    bool repeated_seek, WebmSeekReuseClass reuse_class,
    const WebmSeekPlan *plan, const WebmSeekCacheLookupTrace *cache_trace,
    uint64_t start_byte, int cluster_ms, bool cache_hit, uint64_t elapsed_ms);

struct PreparedWebmSeekPlanResult {
  bool metadata_ok = false;
  bool plan_ok = false;
  bool repeated_seek = false;
  WebmSeekReuseClass reuse_class = WebmSeekReuseClass::Cold;
  bool parser_cluster_aligned = false;
  WebmSeekSourceInfo source_info;
  WebmSeekPlan seek_plan;
  WebmSeekCacheLookupTrace cache_trace;
  WebmSeekPlanningBreakdown planning_breakdown;
  std::string stream_url;
};

PreparedWebmSeekPlanResult prepare_webm_seek_plan(
    YouTubeAPI *api, const std::string &stream_video_id,
    const WebmSeekRequest &request, uint64_t pending_seek_backtrack_bytes,
    int pending_seek_retry_count, uint64_t seek_plan_preroll_ms,
    uint64_t header_probe_size_bytes, uint64_t probe_size_bytes,
    WebmSeekBacktrackBytesFn backtrack_bytes_for_retry,
    WebmSeekTraceLogFn trace_log, WebmSeekTrackState *track_state);

WebmSeekCacheStoreTrace
append_webm_seek_runtime_cache_point(WebmSeekTrackState *state, int timecode_ms,
                                     uint64_t start_byte);

#endif
