#ifndef WEBM_SEEK_PLANNER_H
#define WEBM_SEEK_PLANNER_H

#include "webm_seek_types.h"

WebmSeekPlan build_webm_seek_plan(int seek_seconds,
                                  const WebmSeekSourceInfo &source_info);

bool choose_webm_seek_cluster_from_cache(
    const WebmSeekSourceInfo &source_info, const WebmSeekClusterPoint *points,
    size_t point_count, WebmSeekPlan *inout_plan,
    int *out_cluster_timecode_ms = NULL,
    WebmSeekCacheLookupTrace *out_trace = NULL);

bool estimate_webm_seek_probe_start_byte_from_cache(
    const WebmSeekSourceInfo &source_info, const WebmSeekClusterPoint *points,
    size_t point_count, int target_ms, uint64_t *out_start_byte,
    WebmSeekCacheLookupTrace *out_trace = NULL);

#endif
