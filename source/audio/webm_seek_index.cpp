#include "webm_seek_index.h"

#include <string.h>

namespace {

static const int kSeekProbeWarmupMs = 1200;
static const size_t kSeekProbeScanBytesPerCluster = 192U;
static const uint8_t kWebmClusterId[4] = {0x1F, 0x43, 0xB6, 0x75};
static const uint64_t kWebmTimecodeId = 0xE7ULL;
static const uint64_t kWebmSegmentId = 0x18538067ULL;
static const uint64_t kWebmInfoId = 0x1549A966ULL;
static const uint64_t kWebmSeekHeadId = 0x114D9B74ULL;
static const uint64_t kWebmSeekId = 0x4DBBULL;
static const uint64_t kWebmSeekIdId = 0x53ABULL;
static const uint64_t kWebmSeekPositionId = 0x53ACULL;
static const uint64_t kWebmTimestampScaleId = 0x2AD7B1ULL;
static const uint64_t kWebmCuesId = 0x1C53BB6BULL;
static const uint64_t kWebmCuePointId = 0xBBULL;
static const uint64_t kWebmCueTimeId = 0xB3ULL;
static const uint64_t kWebmCueTrackId = 0xF7ULL;
static const uint64_t kWebmCueTrackPositionsId = 0xB7ULL;
static const uint64_t kWebmCueClusterPositionId = 0xF1ULL;
static const uint64_t kWebmCueRelativePositionId = 0xF0ULL;
static const uint64_t kDefaultTimestampScaleNs = 1000000ULL;
static const size_t kMaxCuePoints = 128U;

struct ClusterCandidate {
  size_t offset = 0;
  int timecode_ms = 0;
};

struct CueCandidate {
  uint64_t cluster_offset = 0;
  int timecode_ms = 0;
  uint64_t cue_track = 0;
  uint64_t relative_position = 0;
};

struct ElementSpan {
  size_t id_offset = 0;
  size_t payload_offset = 0;
  size_t payload_size = 0;
  size_t next_offset = 0;
};

static size_t ebml_vint_length(uint8_t first_byte) {
  if (first_byte == 0U) {
    return 0U;
  }
  uint8_t mask = 0x80U;
  for (size_t len = 1; len <= 8U; ++len) {
    if ((first_byte & mask) != 0U) {
      return len;
    }
    mask >>= 1;
  }
  return 0U;
}

static bool read_ebml_id(const uint8_t *data, size_t size, size_t offset,
                         uint64_t *out_id, size_t *out_len) {
  if (!data || !out_id || !out_len || offset >= size) {
    return false;
  }
  const size_t len = ebml_vint_length(data[offset]);
  if (len == 0U || offset + len > size) {
    return false;
  }
  uint64_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data[offset + i]);
  }
  *out_id = value;
  *out_len = len;
  return true;
}

static bool read_ebml_size(const uint8_t *data, size_t size, size_t offset,
                           uint64_t *out_value, size_t *out_len,
                           bool *out_unknown) {
  if (!data || !out_value || !out_len || !out_unknown || offset >= size) {
    return false;
  }
  const size_t len = ebml_vint_length(data[offset]);
  if (len == 0U || offset + len > size) {
    return false;
  }
  uint64_t value =
      static_cast<uint64_t>(data[offset] & ((1U << (8U - len)) - 1U));
  bool all_ones = value == ((1ULL << (8U - len)) - 1ULL);
  for (size_t i = 1; i < len; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data[offset + i]);
    all_ones = all_ones && (data[offset + i] == 0xFFU);
  }
  *out_value = value;
  *out_len = len;
  *out_unknown = all_ones;
  return true;
}

static bool read_uint_be(const uint8_t *data, size_t size, size_t offset,
                         size_t value_len, uint64_t *out_value) {
  if (!data || !out_value || value_len == 0U || value_len > 8U ||
      offset + value_len > size) {
    return false;
  }
  uint64_t value = 0;
  for (size_t i = 0; i < value_len; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data[offset + i]);
  }
  *out_value = value;
  return true;
}

static bool read_element_span(const uint8_t *data, size_t size, size_t offset,
                              ElementSpan *out_span, uint64_t *out_id) {
  if (!data || !out_span || !out_id || offset >= size) {
    return false;
  }
  uint64_t elem_id = 0;
  size_t elem_id_len = 0;
  if (!read_ebml_id(data, size, offset, &elem_id, &elem_id_len)) {
    return false;
  }
  uint64_t elem_size = 0;
  size_t elem_size_len = 0;
  bool elem_unknown_size = false;
  if (!read_ebml_size(data, size, offset + elem_id_len, &elem_size,
                      &elem_size_len, &elem_unknown_size)) {
    return false;
  }
  const size_t payload_offset = offset + elem_id_len + elem_size_len;
  if (payload_offset > size) {
    return false;
  }
  size_t payload_size = size - payload_offset;
  if (!elem_unknown_size) {
    if (elem_size > static_cast<uint64_t>(size - payload_offset)) {
      return false;
    }
    payload_size = static_cast<size_t>(elem_size);
  }
  out_span->id_offset = offset;
  out_span->payload_offset = payload_offset;
  out_span->payload_size = payload_size;
  out_span->next_offset = payload_offset + payload_size;
  *out_id = elem_id;
  return true;
}

static bool find_child_element(const uint8_t *data, size_t size,
                               size_t parent_payload_offset,
                               size_t parent_payload_size, uint64_t target_id,
                               ElementSpan *out_span) {
  if (!data || !out_span) {
    return false;
  }
  const size_t end = parent_payload_offset + parent_payload_size;
  size_t offset = parent_payload_offset;
  while (offset < end && offset < size) {
    ElementSpan span = {};
    uint64_t elem_id = 0;
    if (!read_element_span(data, end < size ? end : size, offset, &span,
                           &elem_id)) {
      return false;
    }
    if (elem_id == target_id) {
      *out_span = span;
      return true;
    }
    if (span.next_offset <= offset) {
      return false;
    }
    offset = span.next_offset;
  }
  return false;
}

static bool parse_timestamp_scale_ns(const uint8_t *data, size_t size,
                                     const ElementSpan &segment_span,
                                     uint64_t *out_timestamp_scale_ns) {
  if (!data || !out_timestamp_scale_ns) {
    return false;
  }
  *out_timestamp_scale_ns = kDefaultTimestampScaleNs;
  ElementSpan info_span = {};
  if (!find_child_element(data, size, segment_span.payload_offset,
                          segment_span.payload_size, kWebmInfoId, &info_span)) {
    return true;
  }
  ElementSpan scale_span = {};
  if (!find_child_element(data, size, info_span.payload_offset,
                          info_span.payload_size, kWebmTimestampScaleId,
                          &scale_span)) {
    return true;
  }
  if (scale_span.payload_size == 0U || scale_span.payload_size > 8U) {
    return false;
  }
  uint64_t value = 0;
  if (!read_uint_be(data, size, scale_span.payload_offset,
                    scale_span.payload_size, &value)) {
    return false;
  }
  if (value > 0ULL) {
    *out_timestamp_scale_ns = value;
  }
  return true;
}

static bool parse_seekhead_cues_offset(const uint8_t *data, size_t size,
                                       const ElementSpan &segment_span,
                                       uint64_t *out_cues_absolute_offset) {
  if (!data || !out_cues_absolute_offset) {
    return false;
  }
  ElementSpan seekhead_span = {};
  if (!find_child_element(data, size, segment_span.payload_offset,
                          segment_span.payload_size, kWebmSeekHeadId,
                          &seekhead_span)) {
    return false;
  }

  const size_t seekhead_end =
      seekhead_span.payload_offset + seekhead_span.payload_size;
  size_t offset = seekhead_span.payload_offset;
  while (offset < seekhead_end && offset < size) {
    ElementSpan seek_span = {};
    uint64_t elem_id = 0;
    if (!read_element_span(data, seekhead_end < size ? seekhead_end : size,
                           offset, &seek_span, &elem_id)) {
      return false;
    }
    if (elem_id != kWebmSeekId) {
      if (seek_span.next_offset <= offset) {
        return false;
      }
      offset = seek_span.next_offset;
      continue;
    }

    ElementSpan seek_id_span = {};
    ElementSpan seek_pos_span = {};
    if (!find_child_element(data, size, seek_span.payload_offset,
                            seek_span.payload_size, kWebmSeekIdId,
                            &seek_id_span) ||
        !find_child_element(data, size, seek_span.payload_offset,
                            seek_span.payload_size, kWebmSeekPositionId,
                            &seek_pos_span)) {
      offset = seek_span.next_offset;
      continue;
    }
    if (seek_id_span.payload_size == 0U || seek_id_span.payload_size > 8U ||
        seek_pos_span.payload_size == 0U || seek_pos_span.payload_size > 8U) {
      offset = seek_span.next_offset;
      continue;
    }

    uint64_t seek_target_id = 0;
    uint64_t seek_position = 0;
    if (!read_uint_be(data, size, seek_id_span.payload_offset,
                      seek_id_span.payload_size, &seek_target_id) ||
        !read_uint_be(data, size, seek_pos_span.payload_offset,
                      seek_pos_span.payload_size, &seek_position)) {
      offset = seek_span.next_offset;
      continue;
    }
    if (seek_target_id == kWebmCuesId) {
      *out_cues_absolute_offset =
          static_cast<uint64_t>(segment_span.payload_offset) + seek_position;
      return true;
    }
    offset = seek_span.next_offset;
  }
  return false;
}

static bool parse_cue_candidates_from_cues_span(
    const uint8_t *data, size_t size, const ElementSpan &cues_span,
    uint64_t segment_data_offset, uint64_t timestamp_scale_ns,
    CueCandidate *out_candidates, size_t *out_count) {
  if (!data || !out_candidates || !out_count) {
    return false;
  }
  *out_count = 0U;

  const size_t cues_end = cues_span.payload_offset + cues_span.payload_size;
  size_t offset = cues_span.payload_offset;
  while (offset < cues_end && *out_count < kMaxCuePoints) {
    ElementSpan cue_point_span = {};
    uint64_t elem_id = 0;
    if (!read_element_span(data, cues_end < size ? cues_end : size, offset,
                           &cue_point_span, &elem_id)) {
      return false;
    }
    if (elem_id != kWebmCuePointId) {
      if (cue_point_span.next_offset <= offset) {
        return false;
      }
      offset = cue_point_span.next_offset;
      continue;
    }

    ElementSpan cue_time_span = {};
    ElementSpan cue_track_positions_span = {};
    if (!find_child_element(data, size, cue_point_span.payload_offset,
                            cue_point_span.payload_size, kWebmCueTimeId,
                            &cue_time_span) ||
        !find_child_element(data, size, cue_point_span.payload_offset,
                            cue_point_span.payload_size,
                            kWebmCueTrackPositionsId,
                            &cue_track_positions_span)) {
      offset = cue_point_span.next_offset;
      continue;
    }

    ElementSpan cue_cluster_pos_span = {};
    ElementSpan cue_track_span = {};
    ElementSpan cue_relative_pos_span = {};
    if (!find_child_element(data, size, cue_track_positions_span.payload_offset,
                            cue_track_positions_span.payload_size,
                            kWebmCueClusterPositionId, &cue_cluster_pos_span)) {
      offset = cue_point_span.next_offset;
      continue;
    }
    (void)find_child_element(data, size,
                             cue_track_positions_span.payload_offset,
                             cue_track_positions_span.payload_size,
                             kWebmCueTrackId, &cue_track_span);
    (void)find_child_element(
        data, size, cue_track_positions_span.payload_offset,
        cue_track_positions_span.payload_size, kWebmCueRelativePositionId,
        &cue_relative_pos_span);

    if (cue_time_span.payload_size == 0U || cue_time_span.payload_size > 8U ||
        cue_cluster_pos_span.payload_size == 0U ||
        cue_cluster_pos_span.payload_size > 8U) {
      offset = cue_point_span.next_offset;
      continue;
    }

    uint64_t cue_time = 0;
    uint64_t cue_cluster_pos = 0;
    uint64_t cue_track = 0;
    uint64_t cue_relative_pos = 0;
    if (!read_uint_be(data, size, cue_time_span.payload_offset,
                      cue_time_span.payload_size, &cue_time) ||
        !read_uint_be(data, size, cue_cluster_pos_span.payload_offset,
                      cue_cluster_pos_span.payload_size, &cue_cluster_pos)) {
      offset = cue_point_span.next_offset;
      continue;
    }
    if (cue_track_span.payload_size > 0U && cue_track_span.payload_size <= 8U) {
      (void)read_uint_be(data, size, cue_track_span.payload_offset,
                         cue_track_span.payload_size, &cue_track);
    }
    if (cue_relative_pos_span.payload_size > 0U &&
        cue_relative_pos_span.payload_size <= 8U) {
      (void)read_uint_be(data, size, cue_relative_pos_span.payload_offset,
                         cue_relative_pos_span.payload_size, &cue_relative_pos);
    }

    CueCandidate candidate = {};
    candidate.cluster_offset = segment_data_offset + cue_cluster_pos;
    candidate.timecode_ms =
        static_cast<int>((cue_time * timestamp_scale_ns) / 1000000ULL);
    candidate.cue_track = cue_track;
    candidate.relative_position = cue_relative_pos;
    out_candidates[*out_count] = candidate;
    *out_count += 1U;
    offset = cue_point_span.next_offset;
  }
  return *out_count > 0U;
}

static bool parse_cue_candidates(const uint8_t *data, size_t size,
                                 const ElementSpan &segment_span,
                                 uint64_t timestamp_scale_ns,
                                 CueCandidate *out_candidates,
                                 size_t *out_count) {
  ElementSpan cues_span = {};
  if (!find_child_element(data, size, segment_span.payload_offset,
                          segment_span.payload_size, kWebmCuesId, &cues_span)) {
    return false;
  }
  return parse_cue_candidates_from_cues_span(
      data, size, cues_span, static_cast<uint64_t>(segment_span.payload_offset),
      timestamp_scale_ns, out_candidates, out_count);
}

static bool parse_cluster_timecode(const uint8_t *data, size_t size,
                                   size_t cluster_offset,
                                   ClusterCandidate *out_candidate,
                                   size_t *out_next_offset) {
  if (!data || !out_candidate || cluster_offset + 4U > size ||
      memcmp(data + cluster_offset, kWebmClusterId, 4U) != 0) {
    return false;
  }

  uint64_t cluster_size = 0;
  size_t cluster_size_len = 0;
  bool cluster_unknown_size = false;
  if (!read_ebml_size(data, size, cluster_offset + 4U, &cluster_size,
                      &cluster_size_len, &cluster_unknown_size)) {
    return false;
  }

  const size_t payload_offset = cluster_offset + 4U + cluster_size_len;
  if (payload_offset >= size) {
    return false;
  }

  size_t cluster_end = size;
  if (!cluster_unknown_size) {
    const uint64_t requested_end =
        static_cast<uint64_t>(payload_offset) + cluster_size;
    if (requested_end < static_cast<uint64_t>(size)) {
      cluster_end = static_cast<size_t>(requested_end);
    }
  }

  size_t scan_end = payload_offset + kSeekProbeScanBytesPerCluster;
  if (scan_end > cluster_end) {
    scan_end = cluster_end;
  }
  if (scan_end > size) {
    scan_end = size;
  }

  size_t pos = payload_offset;
  while (pos < scan_end) {
    uint64_t elem_id = 0;
    size_t elem_id_len = 0;
    if (!read_ebml_id(data, scan_end, pos, &elem_id, &elem_id_len)) {
      break;
    }
    uint64_t elem_size = 0;
    size_t elem_size_len = 0;
    bool elem_unknown_size = false;
    if (!read_ebml_size(data, scan_end, pos + elem_id_len, &elem_size,
                        &elem_size_len, &elem_unknown_size)) {
      break;
    }
    const size_t elem_payload_offset = pos + elem_id_len + elem_size_len;
    if (elem_payload_offset > cluster_end || elem_payload_offset > size) {
      break;
    }

    if (elem_id == kWebmTimecodeId && !elem_unknown_size && elem_size > 0ULL &&
        elem_size <= 8ULL) {
      uint64_t timecode_value = 0;
      if (!read_uint_be(data, size, elem_payload_offset,
                        static_cast<size_t>(elem_size), &timecode_value)) {
        return false;
      }
      out_candidate->offset = cluster_offset;
      out_candidate->timecode_ms = static_cast<int>(timecode_value);
      if (out_next_offset) {
        *out_next_offset = cluster_offset + 4U;
      }
      return true;
    }

    if (elem_unknown_size) {
      break;
    }
    const uint64_t next_pos =
        static_cast<uint64_t>(elem_payload_offset) + elem_size;
    if (next_pos <= static_cast<uint64_t>(pos) ||
        next_pos > static_cast<uint64_t>(cluster_end)) {
      break;
    }
    pos = static_cast<size_t>(next_pos);
  }

  if (out_next_offset) {
    *out_next_offset = cluster_offset + 4U;
  }
  return false;
}

} // namespace

bool refine_webm_seek_plan_with_probe(const WebmSeekSourceInfo &source_info,
                                      const WebmSeekProbe &probe,
                                      WebmSeekPlan *inout_plan,
                                      int *out_cluster_timecode_ms) {
  if (!inout_plan || !inout_plan->valid || !probe.data || probe.size < 4U) {
    return false;
  }
  size_t offset = 0;
  ClusterCandidate first_candidate = {};
  bool have_first_candidate = false;
  ClusterCandidate best_before_warmup = {};
  bool have_best_before_warmup = false;
  ClusterCandidate best_before_target = {};
  bool have_best_before_target = false;

  const uint64_t desired_warmup_ms =
      source_info.preroll_ms > static_cast<uint64_t>(kSeekProbeWarmupMs)
          ? source_info.preroll_ms
          : static_cast<uint64_t>(kSeekProbeWarmupMs);
  const int desired_cluster_ms =
      inout_plan->seek_target_ms > static_cast<int>(desired_warmup_ms)
          ? (inout_plan->seek_target_ms - static_cast<int>(desired_warmup_ms))
          : 0;

  while (offset + 4U <= probe.size) {
    if (memcmp(probe.data + offset, kWebmClusterId, 4U) != 0) {
      ++offset;
      continue;
    }
    ClusterCandidate candidate = {};
    size_t next_offset = offset + 4U;
    if (parse_cluster_timecode(probe.data, probe.size, offset, &candidate,
                               &next_offset)) {
      if (!have_first_candidate) {
        first_candidate = candidate;
        have_first_candidate = true;
      }
      if (candidate.timecode_ms <= desired_cluster_ms) {
        if (!have_best_before_warmup ||
            candidate.timecode_ms > best_before_warmup.timecode_ms) {
          best_before_warmup = candidate;
          have_best_before_warmup = true;
        }
      }
      if (candidate.timecode_ms <= inout_plan->seek_target_ms) {
        if (!have_best_before_target ||
            candidate.timecode_ms > best_before_target.timecode_ms) {
          best_before_target = candidate;
          have_best_before_target = true;
        }
      }
    }
    offset = next_offset;
  }

  if (!have_first_candidate) {
    return false;
  }

  const ClusterCandidate *best =
      have_best_before_warmup
          ? &best_before_warmup
          : (have_best_before_target ? &best_before_target : &first_candidate);

  inout_plan->reconnect_start_byte = probe.start_byte + best->offset;
  inout_plan->selected_cluster_ms = best->timecode_ms;
  inout_plan->source = WebmSeekPlanSource::ProbeCluster;
  inout_plan->cluster_aligned = true;
  inout_plan->cache_hit = false;
  if (out_cluster_timecode_ms) {
    *out_cluster_timecode_ms = best->timecode_ms;
  }
  return true;
}

bool extract_webm_seek_clusters(const WebmSeekProbe &probe,
                                WebmSeekClusterPoint *out_points,
                                size_t max_points, size_t *out_count) {
  if (!out_count) {
    return false;
  }
  *out_count = 0U;
  if (!probe.data || probe.size < 4U || !out_points || max_points == 0U) {
    return false;
  }

  size_t offset = 0;
  while (offset + 4U <= probe.size && *out_count < max_points) {
    if (memcmp(probe.data + offset, kWebmClusterId, 4U) != 0) {
      ++offset;
      continue;
    }
    ClusterCandidate candidate = {};
    size_t next_offset = offset + 4U;
    if (parse_cluster_timecode(probe.data, probe.size, offset, &candidate,
                               &next_offset)) {
      WebmSeekClusterPoint point = {};
      point.timecode_ms = candidate.timecode_ms;
      point.start_byte = probe.start_byte + candidate.offset;
      out_points[*out_count] = point;
      *out_count += 1U;
    }
    offset = next_offset;
  }
  return *out_count > 0U;
}

bool refine_webm_seek_plan_with_cues(const WebmSeekSourceInfo &source_info,
                                     const WebmSeekProbe &header_probe,
                                     WebmSeekPlan *inout_plan,
                                     int *out_cue_timecode_ms) {
  if (!inout_plan || !inout_plan->valid || !header_probe.data ||
      header_probe.size < 4U) {
    return false;
  }

  uint64_t timestamp_scale_ns = kDefaultTimestampScaleNs;
  CueCandidate candidates[kMaxCuePoints];
  size_t candidate_count = 0;
  bool parsed = false;

  ElementSpan segment_span = {};
  uint64_t segment_id = 0;
  size_t offset = 0;
  while (offset < header_probe.size) {
    if (!read_element_span(header_probe.data, header_probe.size, offset,
                           &segment_span, &segment_id)) {
      break;
    }
    if (segment_id == kWebmSegmentId) {
      if (!parse_timestamp_scale_ns(header_probe.data, header_probe.size,
                                    segment_span, &timestamp_scale_ns)) {
        return false;
      }
      parsed = parse_cue_candidates(header_probe.data, header_probe.size,
                                    segment_span, timestamp_scale_ns,
                                    candidates, &candidate_count);
      break;
    }
    if (segment_span.next_offset <= offset) {
      break;
    }
    offset = segment_span.next_offset;
  }

  if (!parsed && header_probe.segment_data_offset > 0) {
    ElementSpan cues_span = {};
    uint64_t cues_id = 0;
    size_t cues_offset = 0;
    while (cues_offset < header_probe.size) {
      if (!read_element_span(header_probe.data, header_probe.size, cues_offset,
                             &cues_span, &cues_id)) {
        break;
      }
      if (cues_id == kWebmCuesId) {
        parsed = parse_cue_candidates_from_cues_span(
            header_probe.data, header_probe.size, cues_span,
            header_probe.segment_data_offset, timestamp_scale_ns, candidates,
            &candidate_count);
        break;
      }
      if (cues_span.next_offset <= cues_offset) {
        break;
      }
      cues_offset = cues_span.next_offset;
    }
  }

  if (!parsed) {
    return false;
  }

  const uint64_t desired_warmup_ms =
      source_info.preroll_ms > static_cast<uint64_t>(kSeekProbeWarmupMs)
          ? source_info.preroll_ms
          : static_cast<uint64_t>(kSeekProbeWarmupMs);
  const int desired_cue_ms =
      inout_plan->seek_target_ms > static_cast<int>(desired_warmup_ms)
          ? (inout_plan->seek_target_ms - static_cast<int>(desired_warmup_ms))
          : 0;

  const CueCandidate *best = NULL;
  for (size_t i = 0; i < candidate_count; ++i) {
    if (candidates[i].timecode_ms <= desired_cue_ms) {
      best = &candidates[i];
    }
  }
  if (!best) {
    for (size_t i = 0; i < candidate_count; ++i) {
      if (candidates[i].timecode_ms <= inout_plan->seek_target_ms) {
        best = &candidates[i];
      }
    }
  }
  if (!best) {
    best = &candidates[0];
  }

  inout_plan->reconnect_start_byte = best->cluster_offset;
  inout_plan->selected_cluster_ms = best->timecode_ms;
  inout_plan->source = WebmSeekPlanSource::CueIndex;
  inout_plan->cluster_aligned = true;
  inout_plan->cache_hit = false;
  if (out_cue_timecode_ms) {
    *out_cue_timecode_ms = best->timecode_ms;
  }
  return true;
}

bool locate_webm_cues(const WebmSeekProbe &header_probe,
                      uint64_t *out_cues_absolute_offset,
                      uint64_t *out_segment_data_offset) {
  if (!header_probe.data || header_probe.size < 4U ||
      !out_cues_absolute_offset || !out_segment_data_offset) {
    return false;
  }

  ElementSpan segment_span = {};
  uint64_t segment_id = 0;
  size_t offset = 0;
  while (offset < header_probe.size) {
    if (!read_element_span(header_probe.data, header_probe.size, offset,
                           &segment_span, &segment_id)) {
      return false;
    }
    if (segment_id == kWebmSegmentId) {
      *out_segment_data_offset =
          static_cast<uint64_t>(segment_span.payload_offset);
      ElementSpan cues_span = {};
      if (find_child_element(
              header_probe.data, header_probe.size, segment_span.payload_offset,
              segment_span.payload_size, kWebmCuesId, &cues_span)) {
        *out_cues_absolute_offset = static_cast<uint64_t>(cues_span.id_offset);
        return true;
      }
      return parse_seekhead_cues_offset(header_probe.data, header_probe.size,
                                        segment_span, out_cues_absolute_offset);
    }
    if (segment_span.next_offset <= offset) {
      return false;
    }
    offset = segment_span.next_offset;
  }
  return false;
}
