#ifndef WEBM_OPUS_STREAMING_DECODER_H
#define WEBM_OPUS_STREAMING_DECODER_H

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "opus_memory_decoder.h"

struct nestegg;

enum class WebmRemuxError {
  None,
  SegmentNotFound,
  OpusTrackNotFound,
  UnsupportedTrackCount,
  UnsupportedChannels,
  InvalidCodecPrivate,
  InvalidEbml,
  InvalidBlock,
  UnsupportedFeature,
};

class WebmOpusStreamingDecoder {
public:
  struct OggPagePacket {
    std::vector<uint8_t> data;
    int64_t granule_position;
  };

  WebmOpusStreamingDecoder();
  ~WebmOpusStreamingDecoder();

  bool open_streaming(const std::vector<uint8_t> *webm_buffer, LightLock *webm_lock,
                      const bool *webm_download_complete);
  void reset();
  OpusDecodeResult decode(int16_t *pcm_out, size_t pcm_capacity_samples);
  bool is_open() const;
  bool is_eof() const;
  bool has_failed() const;
  WebmRemuxError remux_error() const;

private:
  struct StreamSource {
    const std::vector<uint8_t> *buffer;
    LightLock *lock;
    const bool *download_complete;
    int64_t offset;
  };

  static bool pump_callback(void *user_data);
  static int64_t nestegg_read_cb(void *buffer, size_t length, void *userdata);
  static int nestegg_seek_cb(int64_t offset, int whence, void *userdata);
  static int64_t nestegg_tell_cb(void *userdata);

  bool init_nestegg();
  bool open_decoder_if_ready();
  bool pump_more_data();
  bool emit_headers();
  bool emit_packet(const unsigned char *data, size_t length);
  bool flush_audio_page(uint8_t header_type);
  bool append_audio_packet(const OggPagePacket &packet);
  bool finalize_stream();

  StreamSource stream_source_;
  nestegg *nestegg_ctx_;
  bool nestegg_inited_;
  unsigned int opus_track_;
  std::vector<uint8_t> codec_private_;
  std::vector<uint8_t> ogg_buffer_;
  std::vector<OggPagePacket> audio_page_packets_;
  LightLock ogg_lock_;
  bool ogg_complete_;
  bool decoder_open_;
  bool remux_failed_;
  bool logged_init_;
  bool logged_track_;
  bool logged_headers_;
  bool logged_audio_;
  bool logged_decoder_open_;
  uint32_t ogg_sequence_;
  int64_t granule_position_;
  size_t audio_page_bytes_;
  size_t audio_page_segments_;
  WebmRemuxError last_error_;
  u64 perf_start_ms_;
  OpusMemoryDecoder decoder_;
};

#endif
