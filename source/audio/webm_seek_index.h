#ifndef WEBM_SEEK_INDEX_H
#define WEBM_SEEK_INDEX_H

#include "webm_seek_types.h"

bool refine_webm_seek_plan_with_probe(const WebmSeekSourceInfo &source_info,
                                      const WebmSeekProbe &probe,
                                      WebmSeekPlan *inout_plan,
                                      int *out_cluster_timecode_ms = NULL);

bool refine_webm_seek_plan_with_cues(const WebmSeekSourceInfo &source_info,
                                     const WebmSeekProbe &header_probe,
                                     WebmSeekPlan *inout_plan,
                                     int *out_cue_timecode_ms = NULL);

bool extract_webm_seek_clusters(const WebmSeekProbe &probe,
                                WebmSeekClusterPoint *out_points,
                                size_t max_points, size_t *out_count);

bool locate_webm_cues(const WebmSeekProbe &header_probe,
                      uint64_t *out_cues_absolute_offset,
                      uint64_t *out_segment_data_offset);

#endif
