#ifndef WEBM_OGG_BRIDGE_PATH_H
#define WEBM_OGG_BRIDGE_PATH_H

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "webm_decode_types.h"

class WebmOggBridgePath {
public:
  struct Packet {
    std::vector<uint8_t> data;
    int64_t granule_position = 0;
  };

  WebmOggBridgePath();
  ~WebmOggBridgePath();

  void reset();
  bool emit_headers(const std::vector<uint8_t> &codec_private,
                    WebmRemuxError *out_error);
  bool emit_packet(const unsigned char *data, size_t length,
                   int64_t granule_position, size_t target_page_bytes,
                   WebmRemuxError *out_error);
  bool flush_audio_page(uint8_t header_type, WebmRemuxError *out_error);
  bool finalize(WebmRemuxError *out_error);
  bool complete() const;
  size_t buffered_bytes() const;
  std::vector<uint8_t> *buffer();
  LightLock *lock();
  bool *complete_flag();

private:
  bool append_audio_packet(const Packet &packet, size_t target_page_bytes,
                           WebmRemuxError *out_error);

  std::vector<uint8_t> buffer_;
  std::vector<Packet> audio_page_packets_;
  mutable LightLock lock_;
  bool complete_;
  uint32_t sequence_;
  size_t audio_page_bytes_;
  size_t audio_page_segments_;
};

#endif
