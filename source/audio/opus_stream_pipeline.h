#ifndef OPUS_STREAM_PIPELINE_H
#define OPUS_STREAM_PIPELINE_H

#include <3ds.h>
#include <stddef.h>

struct OpusPipelineState {
  size_t stream_buffer_bytes;
  bool download_complete;
  int queued_wavebufs;
  int free_wavebufs;
};

OpusPipelineState collect_opus_pipeline_state();

#endif
