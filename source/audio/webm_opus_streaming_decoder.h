#ifndef WEBM_OPUS_STREAMING_DECODER_H
#define WEBM_OPUS_STREAMING_DECODER_H

#include <3ds.h>
#include <curl/curl.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "opus_memory_decoder.h"
#include "webm_audible_start_policy.h"

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
  SeekPrerollInsufficient,
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

  bool open_streaming(std::vector<uint8_t> *webm_buffer, LightLock *webm_lock,
                      bool *webm_download_complete, int seek_start_ms,
                      int emit_start_ms, bool enable_parser_seek,
                      bool prefer_offset_seek,
                      const std::string &range_probe_base_url,
                      uint64_t range_filesize, uint64_t parser_prefetch_offset);
  void reset();
  OpusDecodeResult decode(int16_t *pcm_out, size_t pcm_capacity_samples);
  bool is_open() const;
  bool is_eof() const;
  bool has_failed() const;
  WebmRemuxError remux_error() const;
  bool get_last_seek_runtime_point(uint64_t *out_start_byte,
                                   int *out_timecode_ms) const;

private:
  struct RangeSegment {
    uint64_t start = 0;
    std::vector<uint8_t> data;
  };

  struct StreamSource {
    std::vector<uint8_t> *buffer;
    LightLock *lock;
    bool *download_complete;
    int64_t offset;
    uint64_t filesize;
    std::string range_probe_base_url;
    WebmOpusStreamingDecoder *owner;
  };

  static bool pump_callback(void *user_data);
  static void parser_range_prefetch_thread(void *user_data);
  static int64_t nestegg_read_cb(void *buffer, size_t length, void *userdata);
  static int nestegg_seek_cb(int64_t offset, int whence, void *userdata);
  static int64_t nestegg_tell_cb(void *userdata);

  bool init_nestegg();
  bool open_decoder_if_ready();
  bool pump_more_data();
  bool emit_headers();
  bool emit_packet(const unsigned char *data, size_t length,
                   uint64_t packet_tstamp_ns);
  bool flush_audio_page(uint8_t header_type);
  bool append_audio_packet(const OggPagePacket &packet);
  bool finalize_stream();
  bool init_seek_preroll();
  bool apply_parser_seek();
  bool should_emit_packet(uint64_t packet_tstamp_ns,
                          int *out_packet_tstamp_ms) const;
  size_t current_audio_page_target_bytes() const;
  void log_first_decoded_pcm_if_needed(const OpusDecodeResult &decoded);
  void log_first_audible_pcm_if_needed(const OpusDecodeResult &decoded);
  bool ensure_stream_bytes(size_t required_size);
  size_t copy_from_range_segments(uint64_t offset, uint8_t *dst,
                                  size_t length) const;
  uint64_t range_segment_end_for_offset(uint64_t offset) const;
  bool fetch_range_segment(uint64_t offset, size_t min_length,
                           CURL *curl_override = NULL,
                           bool background_prefetch = false);
  bool maybe_start_parser_range_prefetch(uint64_t offset);
  void run_parser_range_prefetch();
  void cleanup_parser_range_prefetch(bool cancel);
  void seed_initial_range_segment();

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
  int seek_start_ms_;
  int seek_emit_ms_;
  int requested_emit_start_ms_;
  bool parser_seek_enabled_;
  bool parser_offset_seek_preferred_;
  bool parser_range_fetch_failed_;
  uint32_t parser_range_fetch_count_;
  u64 parser_range_fetch_last_log_ms_;
  uint64_t parser_range_retry_offset_;
  u64 parser_range_retry_after_ms_;
  Thread parser_range_prefetch_thread_;
  bool parser_range_prefetch_active_;
  bool parser_range_prefetch_cancel_;
  uint64_t parser_range_prefetch_offset_;
  uint64_t parser_prefetch_offset_;
  std::vector<RangeSegment> range_segments_;
  int pcm_skip_samples_per_channel_;
  int first_emitted_packet_tstamp_ms_;
  bool pcm_skip_logged_;
  bool seek_decode_ready_logged_;
  uint64_t last_seek_runtime_start_byte_;
  int last_seek_runtime_timecode_ms_;
  WebmAudibleStartPolicyConfig audible_policy_;
  int discarded_packets_before_emit_;
  bool seek_packet_discard_logged_;
  bool first_decoded_pcm_logged_;
  bool first_audible_pcm_logged_;
  u64 parser_seek_done_elapsed_ms_;
  u64 first_decoded_pcm_elapsed_ms_;
  u64 first_audible_pcm_elapsed_ms_;
  bool seek_preroll_initialized_;
  CURL *range_fetch_curl_;
  OpusMemoryDecoder decoder_;
};

#endif
