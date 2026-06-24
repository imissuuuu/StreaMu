#ifndef WEBM_DECODE_TYPES_H
#define WEBM_DECODE_TYPES_H

#include <stddef.h>

enum class WebmRemuxError {
  None,
  SegmentNotFound,
  OpusTrackNotFound,
  UnsupportedTrackCount,
  UnsupportedChannels,
  InvalidCodecPrivate,
  InvalidEbml,
  InvalidBlock,
  SeekPrerollInsufficient,
  UnsupportedFeature,
};

enum class WebmDecodeBackend {
  OggBridge,
  DirectOpusPacket,
};

struct WebmDirectPacketQueueSnapshot {
  size_t queued_packets = 0;
  size_t read_index = 0;
  bool complete = false;
  size_t packet_slot_bytes = 0;
  size_t max_packet_bytes_seen = 0;
};

#endif
