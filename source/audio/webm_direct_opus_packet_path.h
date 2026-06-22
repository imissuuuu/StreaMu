#ifndef WEBM_DIRECT_OPUS_PACKET_PATH_H
#define WEBM_DIRECT_OPUS_PACKET_PATH_H

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "opus_memory_decoder.h"
#include "webm_decode_types.h"
#include "webm_opus_packet_decoder.h"

enum class WebmDirectPathOpenStatus {
  Ready,
  FallbackAllowed,
  Fatal,
};

struct WebmDirectPathOpenResult {
  WebmDirectPathOpenStatus status = WebmDirectPathOpenStatus::FallbackAllowed;
  WebmRemuxError error = WebmRemuxError::None;
};

struct WebmDirectOpusDecodeStep {
  OpusDecodeResult decoded = {false, 0, 48000, 0, false};
  bool consumed_packet = false;
  bool has_output = false;
  bool failed = false;
  WebmRemuxError error = WebmRemuxError::None;
};

class WebmDirectOpusPacketPath {
public:
  WebmDirectOpusPacketPath();

  WebmDirectPathOpenResult open(const uint8_t *codec_private,
                                size_t codec_private_size);
  void reset();
  void release_storage();
  bool is_open() const;
  bool has_failed() const;
  WebmRemuxError error() const;
  bool is_eof() const;
  bool queue_full() const;
  bool enqueue_packet(const unsigned char *data, size_t length,
                      uint64_t packet_tstamp_ns, int packet_tstamp_ms);
  WebmDirectOpusDecodeStep decode_next(int16_t *pcm_out,
                                       size_t pcm_capacity_samples);
  void add_skip_samples_per_channel(int samples_per_channel);
  int pending_skip_samples_per_channel() const;
  void mark_complete();
  WebmDirectPacketQueueSnapshot snapshot() const;

private:
  static constexpr size_t kQueueLimit = 8U;

  struct Packet {
    std::vector<uint8_t> data;
    uint64_t tstamp_ns = 0;
    int tstamp_ms = -1;
  };

  bool push_packet(Packet *packet);
  bool pop_packet(Packet *out_packet);
  size_t active_count() const;
  bool empty() const;
  void compact_if_needed();
  void mark_failed(WebmRemuxError error);
  static WebmRemuxError map_packet_error(WebmOpusPacketDecodeError error);

  WebmOpusPacketDecoder decoder_;
  std::vector<Packet> queue_;
  size_t read_index_;
  bool complete_;
  bool failed_;
  WebmRemuxError error_;
};

#endif
