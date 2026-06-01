#ifndef OPUS_PERF_METRICS_H
#define OPUS_PERF_METRICS_H

#include <3ds.h>
#include <stddef.h>

enum class OpusPerfEvent {
  PlaybackRequest,
  StreamRequestStart,
  FirstByte,
  Received16Kb,
  StreamBufferObserve,
  DecoderOpenStart,
  DecoderOpenOk,
  DecoderOpenFailed,
  AudioStarted,
  DecodeLoopFinish,
  FrameObserve,
  StreamComplete,
  StreamFailed,
};

struct OpusPerfSnapshot {
  size_t stream_buffer_bytes;
  int queued_wavebufs;
  int free_wavebufs;
  int decoded_chunks_in_frame;
};

static inline const char *opus_perf_event_name(OpusPerfEvent event) {
  switch (event) {
    case OpusPerfEvent::PlaybackRequest:
      return "playback-request";
    case OpusPerfEvent::StreamRequestStart:
      return "stream-request-start";
    case OpusPerfEvent::FirstByte:
      return "first-byte";
    case OpusPerfEvent::Received16Kb:
      return "received-16kb";
    case OpusPerfEvent::StreamBufferObserve:
      return "stream-buffer-observe";
    case OpusPerfEvent::DecoderOpenStart:
      return "decoder-open-start";
    case OpusPerfEvent::DecoderOpenOk:
      return "decoder-open-ok";
    case OpusPerfEvent::DecoderOpenFailed:
      return "decoder-open-failed";
    case OpusPerfEvent::AudioStarted:
      return "audio-started";
    case OpusPerfEvent::DecodeLoopFinish:
      return "decode-loop-finish";
    case OpusPerfEvent::FrameObserve:
      return "frame-observe";
    case OpusPerfEvent::StreamComplete:
      return "stream-complete";
    case OpusPerfEvent::StreamFailed:
      return "stream-failed";
  }
  return "unknown";
}

#endif
