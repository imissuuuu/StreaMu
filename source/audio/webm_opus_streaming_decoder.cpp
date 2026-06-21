#include "webm_opus_streaming_decoder.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <utility>

extern "C" {
#include <nestegg/nestegg.h>
}

namespace {

static const uint32_t OGG_SERIAL = 0x5354524DU;
static const size_t MAX_OGG_PAGE_SEGMENTS = 255U;
static constexpr size_t kDefaultWebmPacketPumpLimit = 8U;
static constexpr size_t kSeekStartupPacketPumpLimit = 48U;
static constexpr bool kPreferDirectOpusPacketDecode = true;
static constexpr size_t kDirectOpusPacketQueueLimit = 8U;
static constexpr size_t kParserRangeInitialSeekChunkBytes = 128U * 1024U;
static constexpr size_t kParserRangeFetchChunkBytes = 512U * 1024U;
static constexpr size_t kParserRangeMidplaybackFetchChunkBytes = 64U * 1024U;
static constexpr long kParserRangeFetchConnectTimeoutMs = 1500L;
static constexpr long kParserRangeFetchTimeoutMs = 2000L;
static constexpr long kParserRangeMidplaybackConnectTimeoutMs = 100L;
static constexpr long kParserRangeMidplaybackTimeoutMs = 120L;
static constexpr long kParserRangePrefetchConnectTimeoutMs = 500L;
static constexpr long kParserRangePrefetchTimeoutMs = 1000L;
static constexpr u64 kParserRangeMidplaybackRetryDelayMs = 100ULL;
static constexpr uint64_t kOpusSamplesPerMs = 48ULL;

static bool should_log_webm_perf_event(const char *event) {
  if (!event) {
    return false;
  }
  return strcmp(event, "decoder_session_start") == 0 ||
         strcmp(event, "nestegg_init_failed") == 0 ||
         strcmp(event, "parser_range_fetch_failed") == 0 ||
         strcmp(event, "parser_range_fetch_done") == 0 ||
         strcmp(event, "parser_range_fetch_slow") == 0 ||
         strcmp(event, "parser_range_fetch_midplayback") == 0 ||
         strcmp(event, "parser_offset_seek_done") == 0 ||
         strcmp(event, "parser_offset_seek_failed") == 0 ||
         strcmp(event, "parser_seek_done") == 0 ||
         strcmp(event, "parser_seek_failed") == 0 ||
         strcmp(event, "seek_packet_discard_done") == 0 ||
         strcmp(event, "seek_preroll_short") == 0 ||
         strcmp(event, "first_decoded_pcm") == 0 ||
         strcmp(event, "first_audible_pcm") == 0 ||
         strcmp(event, "first_audio_data") == 0 ||
         strcmp(event, "decoder_open_failed") == 0 ||
         strcmp(event, "decoder_open_ok") == 0 ||
         strcmp(event, "seek_pcm_skip_done") == 0 ||
         strcmp(event, "seek_decode_ready") == 0 ||
         strcmp(event, "remux_failed") == 0;
}

static void append_webm_perf_log(const char *event, size_t raw_bytes,
                                 size_t ogg_bytes, WebmRemuxError error,
                                 u64 start_ms) {
  if (!should_log_webm_perf_event(event)) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (start_ms > 0 && now_ms >= start_ms) ? now_ms - start_ms : 0;
  fprintf(f, "[webm-perf] +%llums %s raw=%lu ogg=%lu error=%d\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          static_cast<unsigned long>(raw_bytes),
          static_cast<unsigned long>(ogg_bytes), static_cast<int>(error));
  fclose(f);
}

static void append_webm_seek_execution_log(
    const char *event, const WebmSeekExecutionContext &context, uint64_t offset,
    int packet_ms, uint64_t elapsed_ms) {
  if (!event || context.seek_seq <= 0) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  fprintf(f,
          "[webm-seek-exec] %s seek_seq=%d target_ms=%d source=%s "
          "start_byte=%llu packet_ms=%d elapsed_ms=%llu\n",
          event, context.seek_seq, context.target_ms,
          webm_seek_plan_source_name(context.plan.source),
          static_cast<unsigned long long>(offset), packet_ms,
          static_cast<unsigned long long>(elapsed_ms));
  fclose(f);
}

static void
append_webm_parser_range_fetch_log(const char *event, uint64_t fetch_start,
                                   uint64_t fetch_size, size_t fetched_size,
                                   size_t min_length, bool seek_startup_fetch,
                                   bool midplayback_fetch, u64 duration_ms,
                                   uint32_t fetch_count, u64 start_ms) {
  if (!should_log_webm_perf_event(event)) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (start_ms > 0 && now_ms >= start_ms) ? now_ms - start_ms : 0;
  fprintf(f,
          "[webm-perf] +%llums %s start=%llu size=%llu got=%lu min=%lu "
          "startup=%d mid=%d duration_ms=%llu count=%lu\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          static_cast<unsigned long long>(fetch_start),
          static_cast<unsigned long long>(fetch_size),
          static_cast<unsigned long>(fetched_size),
          static_cast<unsigned long>(min_length), seek_startup_fetch ? 1 : 0,
          midplayback_fetch ? 1 : 0,
          static_cast<unsigned long long>(duration_ms),
          static_cast<unsigned long>(fetch_count));
  fclose(f);
}

static void append_webm_parser_range_fetch_failed_log(
    uint64_t fetch_start, uint64_t fetch_size, size_t fetched_size,
    size_t min_length, bool seek_startup_fetch, bool midplayback_fetch,
    u64 duration_ms, long response_code, CURLcode curl_code,
    WebmRemuxError error, u64 start_ms) {
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (start_ms > 0 && now_ms >= start_ms) ? now_ms - start_ms : 0;
  fprintf(f,
          "[webm-perf] +%llums parser_range_fetch_failed start=%llu "
          "size=%llu got=%lu min=%lu startup=%d mid=%d duration_ms=%llu "
          "response=%ld curl=%d error=%d\n",
          static_cast<unsigned long long>(elapsed_ms),
          static_cast<unsigned long long>(fetch_start),
          static_cast<unsigned long long>(fetch_size),
          static_cast<unsigned long>(fetched_size),
          static_cast<unsigned long>(min_length), seek_startup_fetch ? 1 : 0,
          midplayback_fetch ? 1 : 0,
          static_cast<unsigned long long>(duration_ms), response_code,
          static_cast<int>(curl_code), static_cast<int>(error));
  fclose(f);
}

static u64 webm_perf_elapsed_ms(u64 start_ms) {
  const u64 now_ms = osGetTime();
  return (start_ms > 0 && now_ms >= start_ms) ? (now_ms - start_ms) : 0;
}

static bool should_log_parser_range_fetch_success(bool seek_startup_fetch,
                                                  bool midplayback_fetch,
                                                  uint32_t fetch_count,
                                                  u64 duration_ms, u64 now_ms,
                                                  u64 last_log_ms) {
  if (seek_startup_fetch || midplayback_fetch || duration_ms >= 100ULL ||
      fetch_count <= 3U) {
    return true;
  }
  return last_log_ms == 0 || now_ms >= last_log_ms + 1000ULL;
}

static const char *parser_range_fetch_event_name(bool seek_startup_fetch,
                                                 bool midplayback_fetch,
                                                 u64 duration_ms) {
  if (midplayback_fetch) {
    return "parser_range_fetch_midplayback";
  }
  if (duration_ms >= 100ULL) {
    return "parser_range_fetch_slow";
  }
  (void)seek_startup_fetch;
  return "parser_range_fetch_done";
}

static bool is_transient_parser_range_fetch_failure(CURLcode curl_code,
                                                    long response_code) {
  return curl_code == CURLE_OPERATION_TIMEDOUT ||
         curl_code == CURLE_COULDNT_CONNECT ||
         curl_code == CURLE_COULDNT_RESOLVE_HOST || response_code == 0L;
}

static void
append_webm_seek_decode_breakdown_log(u64 parser_seek_done_elapsed_ms,
                                      u64 first_decoded_pcm_elapsed_ms,
                                      u64 first_audible_pcm_elapsed_ms) {
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const long long parser_to_decoded_ms =
      (parser_seek_done_elapsed_ms > 0 &&
       first_decoded_pcm_elapsed_ms >= parser_seek_done_elapsed_ms)
          ? static_cast<long long>(first_decoded_pcm_elapsed_ms -
                                   parser_seek_done_elapsed_ms)
          : -1LL;
  const long long decoded_to_audible_ms =
      (first_decoded_pcm_elapsed_ms > 0 &&
       first_audible_pcm_elapsed_ms >= first_decoded_pcm_elapsed_ms)
          ? static_cast<long long>(first_audible_pcm_elapsed_ms -
                                   first_decoded_pcm_elapsed_ms)
          : -1LL;
  const long long parser_to_audible_ms =
      (parser_seek_done_elapsed_ms > 0 &&
       first_audible_pcm_elapsed_ms >= parser_seek_done_elapsed_ms)
          ? static_cast<long long>(first_audible_pcm_elapsed_ms -
                                   parser_seek_done_elapsed_ms)
          : -1LL;
  fprintf(f,
          "[webm-seek-decode] parser_seek_done_ms=%llu "
          "first_decoded_pcm_ms=%llu first_audible_pcm_ms=%llu "
          "parser_to_decoded_ms=%lld decoded_to_audible_ms=%lld "
          "parser_to_audible_ms=%lld\n",
          static_cast<unsigned long long>(parser_seek_done_elapsed_ms),
          static_cast<unsigned long long>(first_decoded_pcm_elapsed_ms),
          static_cast<unsigned long long>(first_audible_pcm_elapsed_ms),
          parser_to_decoded_ms, decoded_to_audible_ms, parser_to_audible_ms);
  fclose(f);
}

static void append_u32_le(std::vector<uint8_t> *out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
}

static void append_u64_le(std::vector<uint8_t> *out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFU));
  }
}

static uint32_t ogg_crc_entry(int index) {
  uint32_t value = static_cast<uint32_t>(index) << 24;
  for (int i = 0; i < 8; ++i) {
    if ((value & 0x80000000U) != 0U) {
      value = (value << 1) ^ 0x04C11DB7U;
    } else {
      value <<= 1;
    }
  }
  return value;
}

static uint32_t ogg_crc(const std::vector<uint8_t> &data) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; ++i) {
      table[i] = ogg_crc_entry(i);
    }
    init = true;
  }

  uint32_t crc = 0;
  for (size_t i = 0; i < data.size(); ++i) {
    crc = (crc << 8) ^ table[((crc >> 24) & 0xFFU) ^ data[i]];
  }
  return crc;
}

static bool packet_segments(const std::vector<uint8_t> &packet,
                            std::vector<uint8_t> *segments) {
  if (!segments) {
    return false;
  }
  segments->clear();
  const size_t full_segments = packet.size() / 255U;
  const size_t last_segment = packet.size() % 255U;
  for (size_t i = 0; i < full_segments; ++i) {
    segments->push_back(255U);
  }
  segments->push_back(static_cast<uint8_t>(last_segment));
  return true;
}

static int packet_segment_count(const std::vector<uint8_t> &packet) {
  return static_cast<int>((packet.size() / 255U) + 1U);
}

static bool build_ogg_page_from_packets(
    const std::vector<WebmOpusStreamingDecoder::OggPagePacket> &packets,
    uint8_t header_type, uint32_t sequence, std::vector<uint8_t> *page_out) {
  if (!page_out || packets.empty()) {
    return false;
  }

  std::vector<uint8_t> segments;
  std::vector<uint8_t> body;
  for (size_t i = 0; i < packets.size(); ++i) {
    std::vector<uint8_t> packet_segment_bytes;
    if (!packet_segments(packets[i].data, &packet_segment_bytes)) {
      return false;
    }
    if (packet_segment_bytes.size() > MAX_OGG_PAGE_SEGMENTS ||
        segments.size() + packet_segment_bytes.size() > MAX_OGG_PAGE_SEGMENTS) {
      return false;
    }
    segments.insert(segments.end(), packet_segment_bytes.begin(),
                    packet_segment_bytes.end());
    body.insert(body.end(), packets[i].data.begin(), packets[i].data.end());
  }

  page_out->clear();
  page_out->push_back('O');
  page_out->push_back('g');
  page_out->push_back('g');
  page_out->push_back('S');
  page_out->push_back(0U);
  page_out->push_back(header_type);
  append_u64_le(page_out,
                static_cast<uint64_t>(packets.back().granule_position));
  append_u32_le(page_out, OGG_SERIAL);
  append_u32_le(page_out, sequence);
  append_u32_le(page_out, 0U);
  page_out->push_back(static_cast<uint8_t>(segments.size()));
  page_out->insert(page_out->end(), segments.begin(), segments.end());
  page_out->insert(page_out->end(), body.begin(), body.end());

  const uint32_t crc = ogg_crc(*page_out);
  (*page_out)[22] = static_cast<uint8_t>(crc & 0xFFU);
  (*page_out)[23] = static_cast<uint8_t>((crc >> 8) & 0xFFU);
  (*page_out)[24] = static_cast<uint8_t>((crc >> 16) & 0xFFU);
  (*page_out)[25] = static_cast<uint8_t>((crc >> 24) & 0xFFU);
  return true;
}

static std::vector<uint8_t> build_opus_tags() {
  static const char kVendor[] = "StreaMu";
  std::vector<uint8_t> tags;
  tags.push_back('O');
  tags.push_back('p');
  tags.push_back('u');
  tags.push_back('s');
  tags.push_back('T');
  tags.push_back('a');
  tags.push_back('g');
  tags.push_back('s');
  append_u32_le(&tags, static_cast<uint32_t>(sizeof(kVendor) - 1));
  tags.insert(tags.end(), kVendor, kVendor + sizeof(kVendor) - 1);
  append_u32_le(&tags, 0U);
  return tags;
}

static bool build_ogg_page(const std::vector<uint8_t> &packet,
                           uint8_t header_type, int64_t granule_position,
                           uint32_t sequence, std::vector<uint8_t> *page_out) {
  std::vector<WebmOpusStreamingDecoder::OggPagePacket> packets;
  packets.push_back(
      WebmOpusStreamingDecoder::OggPagePacket{packet, granule_position});
  return build_ogg_page_from_packets(packets, header_type, sequence, page_out);
}

static bool opus_packet_duration_samples(const unsigned char *packet,
                                         size_t length, int *duration_samples) {
  if (!packet || length == 0U || !duration_samples) {
    return false;
  }
  const uint8_t config = packet[0] >> 3;
  const uint8_t frame_count_code = packet[0] & 0x03U;

  int base_samples = 0;
  if (config < 12U) {
    base_samples = ((config & 0x03U) == 0U) ? 480 : 960;
    if ((config & 0x03U) == 2U) {
      base_samples = 1920;
    } else if ((config & 0x03U) == 3U) {
      base_samples = 2880;
    }
  } else if (config < 16U) {
    base_samples = ((config & 0x01U) == 0U) ? 480 : 960;
  } else {
    base_samples = 120 << (config & 0x03U);
  }

  int frames = 0;
  if (frame_count_code == 0U) {
    frames = 1;
  } else if (frame_count_code == 1U || frame_count_code == 2U) {
    frames = 2;
  } else {
    if (length < 2U) {
      return false;
    }
    frames = packet[1] & 0x3F;
    if (frames <= 0) {
      return false;
    }
  }

  const int duration = base_samples * frames;
  if (duration <= 0 || duration > 5760) {
    return false;
  }
  *duration_samples = duration;
  return true;
}

static size_t append_fetch_bytes_cb(char *ptr, size_t size, size_t nmemb,
                                    void *userdata) {
  std::vector<uint8_t> *out = static_cast<std::vector<uint8_t> *>(userdata);
  if (!ptr || !out) {
    return 0;
  }
  const size_t total = size * nmemb;
  out->insert(out->end(), reinterpret_cast<uint8_t *>(ptr),
              reinterpret_cast<uint8_t *>(ptr) + total);
  return total;
}

} // namespace

WebmOpusStreamingDecoder::WebmOpusStreamingDecoder()
    : stream_source_{NULL, NULL, NULL, 0, 0, "", NULL},
      cancel_requested_(false), nestegg_ctx_(NULL), nestegg_inited_(false),
      opus_track_(0), ogg_complete_(false), decoder_open_(false),
      remux_failed_(false), logged_init_(false), logged_track_(false),
      logged_headers_(false), logged_audio_(false), logged_decoder_open_(false),
      ogg_sequence_(0), granule_position_(0), audio_page_bytes_(0),
      audio_page_segments_(0), last_error_(WebmRemuxError::None),
      perf_start_ms_(0), seek_context_(WebmSeekExecutionContext{}),
      seek_start_ms_(0), seek_emit_ms_(0), requested_emit_start_ms_(0),
      parser_seek_enabled_(false), parser_offset_seek_preferred_(false),
      parser_range_fetch_failed_(false), parser_range_fetch_count_(0),
      parser_range_fetch_last_log_ms_(0), parser_range_retry_offset_(0),
      parser_range_retry_after_ms_(0), parser_range_prefetch_thread_(NULL),
      parser_range_prefetch_active_(false),
      parser_range_prefetch_cancel_(false), parser_range_prefetch_offset_(0),
      parser_prefetch_offset_(0), pcm_skip_samples_per_channel_(0),
      first_emitted_packet_tstamp_ms_(-1), pcm_skip_logged_(false),
      seek_decode_ready_logged_(false), last_seek_runtime_start_byte_(0),
      last_seek_runtime_timecode_ms_(-1), discarded_packets_before_emit_(0),
      seek_packet_discard_logged_(false), first_decoded_pcm_logged_(false),
      first_audible_pcm_logged_(false), parser_seek_done_elapsed_ms_(0),
      first_decoded_pcm_elapsed_ms_(0), first_audible_pcm_elapsed_ms_(0),
      seek_preroll_initialized_(false), range_fetch_curl_(NULL),
      decode_backend_(WebmDecodeBackend::OggBridge),
      direct_packets_complete_(false) {
  LightLock_Init(&cancel_lock_);
  LightLock_Init(&ogg_lock_);
}

WebmOpusStreamingDecoder::~WebmOpusStreamingDecoder() {
  reset();
  if (range_fetch_curl_) {
    curl_easy_cleanup(range_fetch_curl_);
    range_fetch_curl_ = NULL;
  }
}

int64_t WebmOpusStreamingDecoder::nestegg_read_cb(void *buffer, size_t length,
                                                  void *userdata) {
  StreamSource *source = static_cast<StreamSource *>(userdata);
  if (!source || !buffer || !source->buffer || !source->lock ||
      !source->download_complete) {
    return -1;
  }

  while (true) {
    if (source->owner && source->owner->is_cancel_requested()) {
      return -1;
    }
    if (source->owner && source->owner->parser_seek_enabled_) {
      const size_t copied = source->owner->copy_from_range_segments(
          static_cast<uint64_t>(source->offset), static_cast<uint8_t *>(buffer),
          length);
      if (copied > 0U) {
        const uint64_t read_start = static_cast<uint64_t>(source->offset);
        const uint64_t segment_end =
            source->owner->range_segment_end_for_offset(read_start);
        source->offset += static_cast<int64_t>(copied);
        if (segment_end > static_cast<uint64_t>(source->offset)) {
          (void)source->owner->maybe_start_parser_range_prefetch(segment_end);
        }
        return static_cast<int64_t>(copied);
      }
      if (source->owner->fetch_range_segment(
              static_cast<uint64_t>(source->offset), length)) {
        continue;
      }
      if (source->owner->parser_range_fetch_failed_) {
        return -1;
      }
      if (*source->download_complete) {
        return 0;
      }
      return -1;
    }

    LightLock_Lock(source->lock);
    const size_t available = source->buffer->size();
    const bool complete = *source->download_complete;
    if (source->offset < static_cast<int64_t>(available)) {
      size_t to_copy = available - static_cast<size_t>(source->offset);
      if (to_copy > length) {
        to_copy = length;
      }
      memcpy(buffer, source->buffer->data() + source->offset, to_copy);
      source->offset += static_cast<int64_t>(to_copy);
      LightLock_Unlock(source->lock);
      return static_cast<int64_t>(to_copy);
    }
    LightLock_Unlock(source->lock);
    if (complete) {
      return 0;
    }
    if (source->owner && source->owner->is_cancel_requested()) {
      return -1;
    }
    if (source->owner && source->owner->ensure_stream_bytes(
                             static_cast<size_t>(source->offset) + length)) {
      continue;
    }
    if (source->owner && source->owner->parser_range_fetch_failed_) {
      return -1;
    }
    if (source->owner && source->owner->logged_audio_) {
      return -1;
    }
    svcSleepThread(10 * 1000 * 1000);
  }
}

int WebmOpusStreamingDecoder::nestegg_seek_cb(int64_t offset, int whence,
                                              void *userdata) {
  StreamSource *source = static_cast<StreamSource *>(userdata);
  if (!source || !source->buffer || !source->lock ||
      !source->download_complete) {
    return -1;
  }

  while (true) {
    if (source->owner && source->owner->is_cancel_requested()) {
      return -1;
    }
    if (source->owner && source->owner->parser_seek_enabled_) {
      int64_t target = 0;
      if (whence == NESTEGG_SEEK_SET) {
        target = offset;
      } else if (whence == NESTEGG_SEEK_CUR) {
        target = source->offset + offset;
      } else if (whence == NESTEGG_SEEK_END) {
        if (source->filesize == 0U) {
          return -1;
        }
        target = static_cast<int64_t>(source->filesize) + offset;
      } else {
        return -1;
      }
      if (target < 0) {
        return -1;
      }
      source->offset = target;
      return 0;
    }

    LightLock_Lock(source->lock);
    const size_t available = source->buffer->size();
    const bool complete = *source->download_complete;

    int64_t target = 0;
    if (whence == NESTEGG_SEEK_SET) {
      target = offset;
    } else if (whence == NESTEGG_SEEK_CUR) {
      target = source->offset + offset;
    } else if (whence == NESTEGG_SEEK_END) {
      if (!complete) {
        LightLock_Unlock(source->lock);
        if (source->owner && source->owner->is_cancel_requested()) {
          return -1;
        }
        if (source->owner && source->owner->logged_audio_) {
          return -1;
        }
        svcSleepThread(10 * 1000 * 1000);
        continue;
      }
      target = static_cast<int64_t>(available) + offset;
    } else {
      LightLock_Unlock(source->lock);
      return -1;
    }

    if (target < 0) {
      LightLock_Unlock(source->lock);
      return -1;
    }
    if (target <= static_cast<int64_t>(available)) {
      source->offset = target;
      LightLock_Unlock(source->lock);
      return 0;
    }
    LightLock_Unlock(source->lock);
    if (source->owner &&
        source->owner->ensure_stream_bytes(static_cast<size_t>(target) + 1U)) {
      continue;
    }
    if (complete) {
      return -1;
    }
    if (source->owner && source->owner->is_cancel_requested()) {
      return -1;
    }
    if (source->owner && source->owner->logged_audio_) {
      return -1;
    }
    svcSleepThread(10 * 1000 * 1000);
  }
}

int64_t WebmOpusStreamingDecoder::nestegg_tell_cb(void *userdata) {
  StreamSource *source = static_cast<StreamSource *>(userdata);
  return source ? source->offset : -1;
}

bool WebmOpusStreamingDecoder::open_streaming(
    std::vector<uint8_t> *webm_buffer, LightLock *webm_lock,
    bool *webm_download_complete, const WebmSeekExecutionContext &seek_context,
    int emit_start_ms, bool enable_parser_seek, bool prefer_offset_seek,
    const std::string &range_probe_base_url, uint64_t range_filesize,
    uint64_t parser_prefetch_offset) {
  reset();
  if (!webm_buffer || !webm_lock || !webm_download_complete) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::InvalidEbml;
    return false;
  }

  stream_source_.buffer = webm_buffer;
  stream_source_.lock = webm_lock;
  stream_source_.download_complete = webm_download_complete;
  stream_source_.offset = 0;
  stream_source_.filesize = range_filesize;
  stream_source_.range_probe_base_url = range_probe_base_url;
  stream_source_.owner = this;
  seek_context_ = seek_context;
  seek_start_ms_ = seek_context.target_ms > 0 ? seek_context.target_ms : 0;
  seek_emit_ms_ = 0;
  requested_emit_start_ms_ = emit_start_ms > 0 ? emit_start_ms : 0;
  parser_seek_enabled_ = enable_parser_seek;
  parser_offset_seek_preferred_ = prefer_offset_seek;
  parser_range_fetch_failed_ = false;
  LightLock_Lock(&cancel_lock_);
  cancel_requested_ = false;
  LightLock_Unlock(&cancel_lock_);
  parser_range_fetch_count_ = 0;
  parser_range_fetch_last_log_ms_ = 0;
  parser_range_retry_offset_ = 0;
  parser_range_retry_after_ms_ = 0;
  parser_range_prefetch_active_ = false;
  parser_range_prefetch_cancel_ = false;
  parser_range_prefetch_offset_ = 0;
  parser_prefetch_offset_ = parser_prefetch_offset;
  seek_preroll_initialized_ = false;
  seed_initial_range_segment();
  perf_start_ms_ = osGetTime();
  append_webm_perf_log("decoder_session_start", 0, 0, WebmRemuxError::None,
                       perf_start_ms_);
  return init_nestegg();
}

void WebmOpusStreamingDecoder::reset() {
  request_cancel();
  cleanup_parser_range_prefetch(true);
  decoder_.reset();
  packet_decoder_.reset();
  if (nestegg_ctx_) {
    nestegg_destroy(nestegg_ctx_);
    nestegg_ctx_ = NULL;
  }
  nestegg_inited_ = false;
  stream_source_ = {NULL, NULL, NULL, 0, 0, "", NULL};
  codec_private_.clear();
  audio_page_packets_.clear();
  clear_direct_packet_queue();
  LightLock_Lock(&ogg_lock_);
  ogg_buffer_.clear();
  LightLock_Unlock(&ogg_lock_);
  ogg_complete_ = false;
  decoder_open_ = false;
  remux_failed_ = false;
  logged_init_ = false;
  logged_track_ = false;
  logged_headers_ = false;
  logged_audio_ = false;
  logged_decoder_open_ = false;
  ogg_sequence_ = 0;
  granule_position_ = 0;
  audio_page_bytes_ = 0;
  audio_page_segments_ = 0;
  last_error_ = WebmRemuxError::None;
  LightLock_Lock(&cancel_lock_);
  cancel_requested_ = false;
  LightLock_Unlock(&cancel_lock_);
  perf_start_ms_ = 0;
  seek_context_ = WebmSeekExecutionContext{};
  seek_start_ms_ = 0;
  seek_emit_ms_ = 0;
  requested_emit_start_ms_ = 0;
  parser_offset_seek_preferred_ = false;
  parser_range_fetch_failed_ = false;
  parser_range_fetch_count_ = 0;
  parser_range_fetch_last_log_ms_ = 0;
  parser_range_retry_offset_ = 0;
  parser_range_retry_after_ms_ = 0;
  parser_range_prefetch_thread_ = NULL;
  parser_range_prefetch_active_ = false;
  parser_range_prefetch_cancel_ = false;
  parser_range_prefetch_offset_ = 0;
  parser_prefetch_offset_ = 0;
  range_segments_.clear();
  pcm_skip_samples_per_channel_ = 0;
  first_emitted_packet_tstamp_ms_ = -1;
  pcm_skip_logged_ = false;
  seek_decode_ready_logged_ = false;
  last_seek_runtime_start_byte_ = 0;
  last_seek_runtime_timecode_ms_ = -1;
  discarded_packets_before_emit_ = 0;
  seek_packet_discard_logged_ = false;
  first_decoded_pcm_logged_ = false;
  first_audible_pcm_logged_ = false;
  parser_seek_done_elapsed_ms_ = 0;
  first_decoded_pcm_elapsed_ms_ = 0;
  first_audible_pcm_elapsed_ms_ = 0;
  seek_preroll_initialized_ = false;
  decode_backend_ = WebmDecodeBackend::OggBridge;
  direct_packets_complete_ = false;
}

void WebmOpusStreamingDecoder::clear_direct_packet_queue() {
  direct_packet_queue_.clear();
}

void WebmOpusStreamingDecoder::request_cancel() {
  LightLock_Lock(&cancel_lock_);
  cancel_requested_ = true;
  LightLock_Unlock(&cancel_lock_);
}

bool WebmOpusStreamingDecoder::ensure_stream_bytes(size_t required_size) {
  if (!parser_seek_enabled_) {
    return false;
  }
  return fetch_range_segment(0, required_size);
}

size_t WebmOpusStreamingDecoder::copy_from_range_segments(uint64_t offset,
                                                          uint8_t *dst,
                                                          size_t length) const {
  if (!dst || length == 0U || !stream_source_.lock) {
    return 0U;
  }
  LightLock_Lock(stream_source_.lock);
  for (size_t i = 0; i < range_segments_.size(); ++i) {
    const RangeSegment &segment = range_segments_[i];
    const uint64_t seg_start = segment.start;
    const uint64_t seg_end = seg_start + segment.data.size();
    if (offset < seg_start || offset >= seg_end) {
      continue;
    }
    const size_t seg_offset = static_cast<size_t>(offset - seg_start);
    size_t to_copy = segment.data.size() - seg_offset;
    if (to_copy > length) {
      to_copy = length;
    }
    memcpy(dst, segment.data.data() + seg_offset, to_copy);
    LightLock_Unlock(stream_source_.lock);
    return to_copy;
  }
  LightLock_Unlock(stream_source_.lock);
  return 0U;
}

uint64_t
WebmOpusStreamingDecoder::range_segment_end_for_offset(uint64_t offset) const {
  if (!stream_source_.lock) {
    return 0U;
  }
  LightLock_Lock(stream_source_.lock);
  for (size_t i = 0; i < range_segments_.size(); ++i) {
    const RangeSegment &segment = range_segments_[i];
    const uint64_t seg_start = segment.start;
    const uint64_t seg_end = seg_start + segment.data.size();
    if (offset >= seg_start && offset < seg_end) {
      LightLock_Unlock(stream_source_.lock);
      return seg_end;
    }
  }
  LightLock_Unlock(stream_source_.lock);
  return 0U;
}

void WebmOpusStreamingDecoder::merge_range_segments_locked() {
  if (range_segments_.size() < 2U) {
    return;
  }

  for (size_t i = 1; i < range_segments_.size(); ++i) {
    RangeSegment segment = std::move(range_segments_[i]);
    size_t j = i;
    while (j > 0U && segment.start < range_segments_[j - 1U].start) {
      range_segments_[j] = std::move(range_segments_[j - 1U]);
      --j;
    }
    range_segments_[j] = std::move(segment);
  }

  std::vector<RangeSegment> merged;
  merged.reserve(range_segments_.size());
  RangeSegment current = std::move(range_segments_[0]);
  for (size_t i = 1; i < range_segments_.size(); ++i) {
    RangeSegment &next = range_segments_[i];
    const uint64_t current_end = current.start + current.data.size();
    const uint64_t next_end = next.start + next.data.size();
    if (current_end < next.start) {
      merged.push_back(std::move(current));
      current = std::move(next);
      continue;
    }
    if (next_end > current_end) {
      const size_t append_offset =
          static_cast<size_t>(current_end - next.start);
      current.data.insert(current.data.end(), next.data.begin() + append_offset,
                          next.data.end());
    }
  }
  merged.push_back(std::move(current));
  range_segments_.swap(merged);
}

void WebmOpusStreamingDecoder::parser_range_prefetch_thread(void *user_data) {
  WebmOpusStreamingDecoder *self =
      static_cast<WebmOpusStreamingDecoder *>(user_data);
  if (self) {
    self->run_parser_range_prefetch();
  }
}

void WebmOpusStreamingDecoder::clear_parser_range_prefetch_state_locked() {
  parser_range_prefetch_active_ = false;
  parser_range_prefetch_cancel_ = false;
  parser_range_prefetch_offset_ = 0;
}

bool WebmOpusStreamingDecoder::maybe_start_parser_range_prefetch(
    uint64_t offset) {
  if (!parser_seek_enabled_ || offset == 0U || !stream_source_.lock ||
      stream_source_.range_probe_base_url.empty()) {
    return false;
  }
  if (!logged_audio_ || !seek_decode_ready_logged_) {
    return false;
  }
  cleanup_parser_range_prefetch(false);

  LightLock_Lock(stream_source_.lock);
  for (size_t i = 0; i < range_segments_.size(); ++i) {
    const RangeSegment &segment = range_segments_[i];
    const uint64_t seg_start = segment.start;
    const uint64_t seg_end = seg_start + segment.data.size();
    if (offset >= seg_start && offset < seg_end) {
      LightLock_Unlock(stream_source_.lock);
      return false;
    }
  }
  if (parser_range_prefetch_active_) {
    LightLock_Unlock(stream_source_.lock);
    return false;
  }
  parser_range_prefetch_active_ = true;
  parser_range_prefetch_cancel_ = false;
  parser_range_prefetch_offset_ = offset;
  LightLock_Unlock(stream_source_.lock);

  Thread thread = threadCreate(parser_range_prefetch_thread, this, 0x10000,
                               0x3F, -2, false);
  if (!thread) {
    LightLock_Lock(stream_source_.lock);
    clear_parser_range_prefetch_state_locked();
    LightLock_Unlock(stream_source_.lock);
    return false;
  }

  parser_range_prefetch_thread_ = thread;
  return true;
}

void WebmOpusStreamingDecoder::run_parser_range_prefetch() {
  uint64_t offset = 0;
  LightLock_Lock(stream_source_.lock);
  offset = parser_range_prefetch_offset_;
  const bool cancel = parser_range_prefetch_cancel_;
  LightLock_Unlock(stream_source_.lock);

  if (!cancel && offset > 0U) {
    CURL *curl = curl_easy_init();
    if (curl) {
      (void)fetch_range_segment(offset, kParserRangeMidplaybackFetchChunkBytes,
                                curl, true);
      curl_easy_cleanup(curl);
    }
  }

  LightLock_Lock(stream_source_.lock);
  parser_range_prefetch_active_ = false;
  parser_range_prefetch_offset_ = 0;
  LightLock_Unlock(stream_source_.lock);
}

void WebmOpusStreamingDecoder::cleanup_parser_range_prefetch(bool cancel) {
  Thread thread = parser_range_prefetch_thread_;
  if (!thread) {
    if (cancel && stream_source_.lock) {
      LightLock_Lock(stream_source_.lock);
      clear_parser_range_prefetch_state_locked();
      LightLock_Unlock(stream_source_.lock);
    }
    return;
  }

  bool active = false;
  if (stream_source_.lock) {
    LightLock_Lock(stream_source_.lock);
    if (cancel) {
      parser_range_prefetch_cancel_ = true;
    }
    active = parser_range_prefetch_active_;
    LightLock_Unlock(stream_source_.lock);
  }
  if (cancel || !active) {
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    parser_range_prefetch_thread_ = NULL;
    if (cancel && stream_source_.lock) {
      LightLock_Lock(stream_source_.lock);
      clear_parser_range_prefetch_state_locked();
      LightLock_Unlock(stream_source_.lock);
    }
  }
}

bool WebmOpusStreamingDecoder::fetch_range_segment(uint64_t offset,
                                                   size_t min_length,
                                                   CURL *curl_override,
                                                   bool background_prefetch) {
  if (!parser_seek_enabled_ || stream_source_.range_probe_base_url.empty()) {
    return false;
  }
  if (is_cancel_requested()) {
    return false;
  }

  const uint64_t fetch_start = offset;
  const bool seek_startup_fetch =
      seek_start_ms_ > 0 &&
      (!logged_audio_ || first_emitted_packet_tstamp_ms_ < 0 ||
       !seek_decode_ready_logged_);
  const bool midplayback_fetch =
      background_prefetch ||
      (!seek_startup_fetch && logged_audio_ && seek_decode_ready_logged_);
  const u64 retry_now_ms = osGetTime();
  if (midplayback_fetch && stream_source_.lock) {
    LightLock_Lock(stream_source_.lock);
    const bool retry_deferred = parser_range_retry_after_ms_ > retry_now_ms &&
                                parser_range_retry_offset_ == fetch_start;
    LightLock_Unlock(stream_source_.lock);
    if (retry_deferred) {
      return false;
    }
  }
  const uint64_t preferred_fetch_size =
      seek_startup_fetch
          ? static_cast<uint64_t>(kParserRangeInitialSeekChunkBytes)
          : (midplayback_fetch
                 ? static_cast<uint64_t>(kParserRangeMidplaybackFetchChunkBytes)
                 : static_cast<uint64_t>(kParserRangeFetchChunkBytes));
  uint64_t fetch_size = preferred_fetch_size;
  if (!seek_startup_fetch && !midplayback_fetch &&
      min_length > preferred_fetch_size) {
    fetch_size = static_cast<uint64_t>(min_length);
  }
  if (stream_source_.filesize > 0U) {
    if (fetch_start >= stream_source_.filesize) {
      if (stream_source_.download_complete) {
        *stream_source_.download_complete = true;
      }
      return false;
    }
    const uint64_t remaining = stream_source_.filesize - fetch_start;
    if (fetch_size > remaining) {
      fetch_size = remaining;
    }
  }
  if (fetch_size == 0U) {
    return false;
  }

  {
    LightLock_Lock(stream_source_.lock);
    for (size_t i = 0; i < range_segments_.size(); ++i) {
      const RangeSegment &segment = range_segments_[i];
      const uint64_t seg_start = segment.start;
      const uint64_t seg_end = seg_start + segment.data.size();
      if (fetch_start >= seg_start && fetch_start + fetch_size <= seg_end) {
        LightLock_Unlock(stream_source_.lock);
        return true;
      }
    }
    LightLock_Unlock(stream_source_.lock);
  }

  std::string url = stream_source_.range_probe_base_url +
                    "&start=" + std::to_string(fetch_start) +
                    "&size=" + std::to_string(fetch_size);
  if (!curl_override && !range_fetch_curl_) {
    range_fetch_curl_ = curl_easy_init();
  }
  CURL *curl = curl_override ? curl_override : range_fetch_curl_;
  if (!curl) {
    return false;
  }

  std::vector<uint8_t> fetched;
  long response_code = 0;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_fetch_bytes_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fetched);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  const long connect_timeout_ms =
      background_prefetch
          ? kParserRangePrefetchConnectTimeoutMs
          : (midplayback_fetch ? kParserRangeMidplaybackConnectTimeoutMs
                               : kParserRangeFetchConnectTimeoutMs);
  const long timeout_ms =
      background_prefetch
          ? kParserRangePrefetchTimeoutMs
          : (midplayback_fetch ? kParserRangeMidplaybackTimeoutMs
                               : kParserRangeFetchTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  const u64 fetch_begin_ms = osGetTime();
  const CURLcode res = curl_easy_perform(curl);
  const u64 fetch_end_ms = osGetTime();
  const u64 fetch_duration_ms =
      fetch_end_ms >= fetch_begin_ms ? fetch_end_ms - fetch_begin_ms : 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  uint32_t fetch_count_for_log = 0;
  LightLock_Lock(stream_source_.lock);
  ++parser_range_fetch_count_;
  fetch_count_for_log = parser_range_fetch_count_;
  LightLock_Unlock(stream_source_.lock);
  const bool usable_response = response_code < 400L;
  const bool usable_fetch =
      usable_response &&
      (res == CURLE_OK ||
       (res == CURLE_OPERATION_TIMEDOUT && !fetched.empty())) &&
      !fetched.empty();
  if (!usable_fetch) {
    const bool transient_midplayback_failure =
        !background_prefetch && midplayback_fetch &&
        is_transient_parser_range_fetch_failure(res, response_code);
    if (transient_midplayback_failure) {
      LightLock_Lock(stream_source_.lock);
      parser_range_retry_offset_ = fetch_start;
      parser_range_retry_after_ms_ =
          fetch_end_ms + kParserRangeMidplaybackRetryDelayMs;
      LightLock_Unlock(stream_source_.lock);
    } else if (!background_prefetch) {
      parser_range_fetch_failed_ = true;
      last_error_ = WebmRemuxError::InvalidBlock;
    }
    append_webm_parser_range_fetch_failed_log(
        fetch_start, fetch_size, fetched.size(), min_length, seek_startup_fetch,
        midplayback_fetch, fetch_duration_ms, response_code, res, last_error_,
        perf_start_ms_);
    return false;
  }
  bool should_log_fetch = false;
  LightLock_Lock(stream_source_.lock);
  if (parser_range_retry_offset_ == fetch_start) {
    parser_range_retry_offset_ = 0;
    parser_range_retry_after_ms_ = 0;
  }
  should_log_fetch = should_log_parser_range_fetch_success(
      seek_startup_fetch, midplayback_fetch, fetch_count_for_log,
      fetch_duration_ms, fetch_end_ms, parser_range_fetch_last_log_ms_);
  if (should_log_fetch) {
    parser_range_fetch_last_log_ms_ = fetch_end_ms;
  }
  LightLock_Unlock(stream_source_.lock);
  if (should_log_fetch) {
    append_webm_parser_range_fetch_log(
        parser_range_fetch_event_name(seek_startup_fetch, midplayback_fetch,
                                      fetch_duration_ms),
        fetch_start, fetch_size, fetched.size(), min_length, seek_startup_fetch,
        midplayback_fetch, fetch_duration_ms, fetch_count_for_log,
        perf_start_ms_);
  }

  const uint64_t received_end = fetch_start + fetched.size();
  LightLock_Lock(stream_source_.lock);
  bool covered = false;
  for (size_t i = 0; i < range_segments_.size(); ++i) {
    const RangeSegment &segment = range_segments_[i];
    const uint64_t seg_start = segment.start;
    const uint64_t seg_end = seg_start + segment.data.size();
    if (fetch_start >= seg_start && fetch_start + fetched.size() <= seg_end) {
      covered = true;
      break;
    }
  }
  if (!covered) {
    RangeSegment segment = {};
    segment.start = fetch_start;
    segment.data.swap(fetched);
    range_segments_.push_back(segment);
    merge_range_segments_locked();
  }
  if (stream_source_.filesize > 0U && received_end >= stream_source_.filesize &&
      stream_source_.download_complete) {
    *stream_source_.download_complete = true;
  }
  LightLock_Unlock(stream_source_.lock);
  return true;
}

bool WebmOpusStreamingDecoder::is_cancel_requested() const {
  LightLock_Lock(&cancel_lock_);
  const bool value = cancel_requested_;
  LightLock_Unlock(&cancel_lock_);
  return value;
}

void WebmOpusStreamingDecoder::seed_initial_range_segment() {
  range_segments_.clear();
  if (!parser_seek_enabled_ || !stream_source_.buffer || !stream_source_.lock) {
    return;
  }
  LightLock_Lock(stream_source_.lock);
  if (!stream_source_.buffer->empty()) {
    RangeSegment segment = {};
    segment.start = 0;
    segment.data = *stream_source_.buffer;
    range_segments_.push_back(segment);
  }
  LightLock_Unlock(stream_source_.lock);
}

bool WebmOpusStreamingDecoder::pump_callback(void *user_data) {
  WebmOpusStreamingDecoder *self =
      static_cast<WebmOpusStreamingDecoder *>(user_data);
  return self ? self->pump_more_data() : false;
}

bool WebmOpusStreamingDecoder::init_nestegg() {
  if (nestegg_inited_) {
    return true;
  }

  nestegg_io io = {};
  io.read = &WebmOpusStreamingDecoder::nestegg_read_cb;
  io.seek = &WebmOpusStreamingDecoder::nestegg_seek_cb;
  io.tell = &WebmOpusStreamingDecoder::nestegg_tell_cb;
  io.userdata = &stream_source_;

  if (nestegg_init(&nestegg_ctx_, io, NULL, -1) != 0 || !nestegg_ctx_) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::InvalidEbml;
    append_webm_perf_log("nestegg_init_failed",
                         static_cast<size_t>(stream_source_.offset), 0,
                         last_error_, perf_start_ms_);
    return false;
  }
  nestegg_inited_ = true;
  logged_init_ = true;
  unsigned int tracks = 0;
  if (nestegg_track_count(nestegg_ctx_, &tracks) != 0) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::OpusTrackNotFound;
    return false;
  }
  bool found_track = false;
  for (unsigned int track = 0; track < tracks; ++track) {
    if (nestegg_track_type(nestegg_ctx_, track) == NESTEGG_TRACK_AUDIO &&
        nestegg_track_codec_id(nestegg_ctx_, track) == NESTEGG_CODEC_OPUS) {
      opus_track_ = track;
      found_track = true;
      break;
    }
  }
  if (!found_track) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::OpusTrackNotFound;
    return false;
  }
  logged_track_ = true;
  if (!init_seek_preroll()) {
    remux_failed_ = true;
    return false;
  }

  unsigned int codec_chunks = 0;
  if (nestegg_track_codec_data_count(nestegg_ctx_, opus_track_,
                                     &codec_chunks) != 0 ||
      codec_chunks == 0U) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::InvalidCodecPrivate;
    return false;
  }

  unsigned char *codec_data = NULL;
  size_t codec_length = 0;
  if (nestegg_track_codec_data(nestegg_ctx_, opus_track_, 0, &codec_data,
                               &codec_length) != 0 ||
      !codec_data || codec_length < 8U) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::InvalidCodecPrivate;
    return false;
  }
  codec_private_.assign(codec_data, codec_data + codec_length);
  if (memcmp(codec_private_.data(), "OpusHead", 8U) != 0) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::InvalidCodecPrivate;
    return false;
  }
  if (!choose_decode_backend()) {
    remux_failed_ = true;
    return false;
  }
  if (decode_backend_ == WebmDecodeBackend::OggBridge) {
    if (!emit_headers()) {
      remux_failed_ = true;
      return false;
    }
  }
  if (parser_seek_enabled_ && parser_prefetch_offset_ > 0) {
    (void)fetch_range_segment(parser_prefetch_offset_,
                              kParserRangeInitialSeekChunkBytes);
  }
  if (!apply_parser_seek()) {
    remux_failed_ = true;
    return false;
  }
  return pump_more_data();
}

bool WebmOpusStreamingDecoder::choose_decode_backend() {
  decode_backend_ = WebmDecodeBackend::OggBridge;
  packet_decoder_.reset();
  clear_direct_packet_queue();
  direct_packets_complete_ = false;

  if (!kPreferDirectOpusPacketDecode) {
    return true;
  }

  if (packet_decoder_.open(codec_private_.data(), codec_private_.size())) {
    decode_backend_ = WebmDecodeBackend::DirectOpusPacket;
    decoder_open_ = true;
    logged_headers_ = true;
    if (!logged_decoder_open_) {
      logged_decoder_open_ = true;
      append_webm_perf_log("decoder_open_ok",
                           static_cast<size_t>(stream_source_.offset), 0,
                           WebmRemuxError::None, perf_start_ms_);
    }
    return true;
  }

  const WebmOpusPacketDecodeError packet_error = packet_decoder_.error();
  if (packet_error == WebmOpusPacketDecodeError::DecoderCreateFailed) {
    last_error_ = WebmRemuxError::UnsupportedFeature;
    return false;
  }

  packet_decoder_.reset();
  decode_backend_ = WebmDecodeBackend::OggBridge;
  return true;
}

bool WebmOpusStreamingDecoder::emit_headers() {
  if (logged_headers_) {
    return true;
  }
  std::vector<uint8_t> page;
  if (!build_ogg_page(codec_private_, 0x02U, 0, ogg_sequence_, &page)) {
    last_error_ = WebmRemuxError::InvalidCodecPrivate;
    return false;
  }
  LightLock_Lock(&ogg_lock_);
  ogg_buffer_.insert(ogg_buffer_.end(), page.begin(), page.end());
  LightLock_Unlock(&ogg_lock_);
  ++ogg_sequence_;

  const std::vector<uint8_t> tags = build_opus_tags();
  if (!build_ogg_page(tags, 0x00U, 0, ogg_sequence_, &page)) {
    last_error_ = WebmRemuxError::InvalidCodecPrivate;
    return false;
  }
  LightLock_Lock(&ogg_lock_);
  ogg_buffer_.insert(ogg_buffer_.end(), page.begin(), page.end());
  LightLock_Unlock(&ogg_lock_);
  ++ogg_sequence_;

  logged_headers_ = true;
  return true;
}

bool WebmOpusStreamingDecoder::flush_audio_page(uint8_t header_type) {
  if (audio_page_packets_.empty()) {
    return true;
  }
  std::vector<uint8_t> page;
  if (!build_ogg_page_from_packets(audio_page_packets_, header_type,
                                   ogg_sequence_, &page)) {
    last_error_ = WebmRemuxError::InvalidBlock;
    return false;
  }
  LightLock_Lock(&ogg_lock_);
  ogg_buffer_.insert(ogg_buffer_.end(), page.begin(), page.end());
  LightLock_Unlock(&ogg_lock_);
  ++ogg_sequence_;
  audio_page_packets_.clear();
  audio_page_bytes_ = 0;
  audio_page_segments_ = 0;
  return true;
}

bool WebmOpusStreamingDecoder::append_audio_packet(
    const OggPagePacket &packet) {
  const int segment_count = packet_segment_count(packet.data);
  if (!audio_page_packets_.empty() &&
      audio_page_segments_ + static_cast<size_t>(segment_count) >
          MAX_OGG_PAGE_SEGMENTS) {
    if (!flush_audio_page(0x00U)) {
      return false;
    }
  }
  audio_page_packets_.push_back(packet);
  audio_page_bytes_ += packet.data.size();
  audio_page_segments_ += static_cast<size_t>(segment_count);
  if (audio_page_bytes_ >= current_audio_page_target_bytes()) {
    if (!flush_audio_page(0x00U)) {
      return false;
    }
  }
  return true;
}

size_t WebmOpusStreamingDecoder::current_audio_page_target_bytes() const {
  return webm_audio_page_target_bytes(seek_start_ms_ > 0, !logged_audio_,
                                      audible_policy_);
}

bool WebmOpusStreamingDecoder::init_seek_preroll() {
  seek_emit_ms_ = requested_emit_start_ms_;
  seek_preroll_initialized_ = true;
  pcm_skip_samples_per_channel_ = 0;
  first_emitted_packet_tstamp_ms_ = -1;
  if (seek_start_ms_ <= 0) {
    return true;
  }
  if (seek_emit_ms_ > 0) {
    if (seek_emit_ms_ > seek_start_ms_) {
      seek_emit_ms_ = seek_start_ms_;
    }
  } else {
    nestegg_audio_params audio_params = {};
    if (nestegg_track_audio_params(nestegg_ctx_, opus_track_, &audio_params) !=
        0) {
      last_error_ = WebmRemuxError::UnsupportedFeature;
      return false;
    }
    const int preroll_ms =
        static_cast<int>(audio_params.seek_preroll / 1000000ULL);
    seek_emit_ms_ = seek_start_ms_ - preroll_ms;
    if (seek_emit_ms_ < 0) {
      seek_emit_ms_ = 0;
    }
  }

  return true;
}

bool WebmOpusStreamingDecoder::apply_parser_seek() {
  if (!parser_seek_enabled_ || !nestegg_ctx_ || seek_start_ms_ <= 0) {
    return true;
  }

  bool offset_seek_applied = false;
  if (parser_offset_seek_preferred_ && parser_prefetch_offset_ > 0U) {
    if (nestegg_offset_seek(nestegg_ctx_, parser_prefetch_offset_) == 0) {
      append_webm_perf_log(
          "parser_offset_seek_done", static_cast<size_t>(stream_source_.offset),
          ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
      append_webm_seek_execution_log(
          "parser_offset_seek_done", seek_context_,
          static_cast<uint64_t>(stream_source_.offset), -1,
          webm_perf_elapsed_ms(perf_start_ms_));
      offset_seek_applied = true;
    }
    if (!offset_seek_applied) {
      append_webm_perf_log("parser_offset_seek_failed",
                           static_cast<size_t>(stream_source_.offset),
                           ogg_buffer_.size(), WebmRemuxError::None,
                           perf_start_ms_);
      append_webm_seek_execution_log(
          "parser_offset_seek_failed", seek_context_,
          static_cast<uint64_t>(stream_source_.offset), -1,
          webm_perf_elapsed_ms(perf_start_ms_));
    }
  }

  const uint64_t seek_tstamp_ns =
      static_cast<uint64_t>(seek_emit_ms_ > 0 ? seek_emit_ms_
                                              : seek_start_ms_) *
      1000000ULL;
  if (nestegg_track_seek(nestegg_ctx_, opus_track_, seek_tstamp_ns) != 0) {
    if (offset_seek_applied) {
      append_webm_perf_log(
          "parser_seek_failed", static_cast<size_t>(stream_source_.offset),
          ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
      append_webm_seek_execution_log(
          "parser_seek_failed", seek_context_,
          static_cast<uint64_t>(stream_source_.offset), -1,
          webm_perf_elapsed_ms(perf_start_ms_));
      return true;
    }
    last_error_ = WebmRemuxError::InvalidBlock;
    append_webm_perf_log("parser_seek_failed",
                         static_cast<size_t>(stream_source_.offset),
                         ogg_buffer_.size(), last_error_, perf_start_ms_);
    append_webm_seek_execution_log("parser_seek_failed", seek_context_,
                                   static_cast<uint64_t>(stream_source_.offset),
                                   -1, webm_perf_elapsed_ms(perf_start_ms_));
    return false;
  }
  append_webm_perf_log(
      "parser_seek_done", static_cast<size_t>(stream_source_.offset),
      ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
  parser_seek_done_elapsed_ms_ = webm_perf_elapsed_ms(perf_start_ms_);
  append_webm_seek_execution_log("parser_seek_done", seek_context_,
                                 static_cast<uint64_t>(stream_source_.offset),
                                 -1, parser_seek_done_elapsed_ms_);
  last_seek_runtime_start_byte_ = static_cast<uint64_t>(stream_source_.offset);
  last_seek_runtime_timecode_ms_ = -1;
  return true;
}

bool WebmOpusStreamingDecoder::should_emit_packet(
    uint64_t packet_tstamp_ns, int *out_packet_tstamp_ms) const {
  const int packet_ms = static_cast<int>(packet_tstamp_ns / 1000000ULL);
  if (out_packet_tstamp_ms) {
    *out_packet_tstamp_ms = packet_ms;
  }
  return webm_should_emit_packet_for_audible_start(seek_start_ms_,
                                                   seek_emit_ms_, packet_ms);
}

bool WebmOpusStreamingDecoder::emit_packet(const unsigned char *data,
                                           size_t length,
                                           uint64_t packet_tstamp_ns) {
  if (!seek_preroll_initialized_) {
    last_error_ = WebmRemuxError::UnsupportedFeature;
    return false;
  }
  int packet_tstamp_ms = -1;
  if (!should_emit_packet(packet_tstamp_ns, &packet_tstamp_ms)) {
    ++discarded_packets_before_emit_;
    return true;
  }
  if (seek_start_ms_ > 0 && first_emitted_packet_tstamp_ms_ < 0) {
    first_emitted_packet_tstamp_ms_ = packet_tstamp_ms;
    if (last_seek_runtime_start_byte_ > 0U) {
      last_seek_runtime_timecode_ms_ = packet_tstamp_ms;
    }
    if (discarded_packets_before_emit_ > 0 && !seek_packet_discard_logged_) {
      seek_packet_discard_logged_ = true;
      append_webm_perf_log("seek_packet_discard_done",
                           static_cast<size_t>(stream_source_.offset),
                           ogg_buffer_.size(), WebmRemuxError::None,
                           perf_start_ms_);
    }
    const WebmFirstEmitDecision first_emit = webm_first_emit_decision(
        seek_start_ms_, first_emitted_packet_tstamp_ms_, audible_policy_);
    if (first_emit.preroll_short) {
      append_webm_perf_log(
          "seek_preroll_short", static_cast<size_t>(stream_source_.offset),
          ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
    }
    pcm_skip_samples_per_channel_ = first_emit.pcm_skip_samples_per_channel;
  }
  int duration_samples = 0;
  if (!opus_packet_duration_samples(data, length, &duration_samples)) {
    last_error_ = WebmRemuxError::InvalidBlock;
    return false;
  }
  if (seek_start_ms_ > 0 && packet_tstamp_ns > 0U) {
    const uint64_t packet_time_ms = packet_tstamp_ns / 1000000ULL;
    granule_position_ =
        static_cast<int64_t>(packet_time_ms * kOpusSamplesPerMs +
                             static_cast<uint64_t>(duration_samples));
  } else {
    granule_position_ += duration_samples;
  }

  if (decode_backend_ == WebmDecodeBackend::DirectOpusPacket) {
    if (pcm_skip_samples_per_channel_ > 0) {
      packet_decoder_.add_skip_samples_per_channel(
          pcm_skip_samples_per_channel_);
      pcm_skip_samples_per_channel_ = 0;
    }
    return enqueue_direct_packet(data, length, packet_tstamp_ns,
                                 packet_tstamp_ms);
  }
  return emit_packet_to_ogg_bridge(data, length, packet_tstamp_ns,
                                   packet_tstamp_ms);
}

bool WebmOpusStreamingDecoder::emit_packet_to_ogg_bridge(
    const unsigned char *data, size_t length, uint64_t packet_tstamp_ns,
    int packet_tstamp_ms) {
  OggPagePacket packet;
  packet.data.assign(data, data + length);
  packet.granule_position = granule_position_;
  if (!append_audio_packet(packet)) {
    return false;
  }
  if (!logged_audio_) {
    if (!flush_audio_page(0x00U)) {
      return false;
    }
  }
  if (!logged_audio_) {
    logged_audio_ = true;
    append_webm_perf_log(
        "first_audio_data", static_cast<size_t>(stream_source_.offset),
        ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
    append_webm_seek_execution_log("first_audio_data", seek_context_,
                                   static_cast<uint64_t>(stream_source_.offset),
                                   packet_tstamp_ms,
                                   webm_perf_elapsed_ms(perf_start_ms_));
  }
  return true;
}

bool WebmOpusStreamingDecoder::enqueue_direct_packet(const unsigned char *data,
                                                     size_t length,
                                                     uint64_t packet_tstamp_ns,
                                                     int packet_tstamp_ms) {
  if (!data || length == 0U) {
    last_error_ = WebmRemuxError::InvalidBlock;
    return false;
  }

  DirectOpusPacket packet;
  packet.data.assign(data, data + length);
  packet.tstamp_ns = packet_tstamp_ns;
  packet.tstamp_ms = packet_tstamp_ms;
  direct_packet_queue_.push_back(std::move(packet));

  if (!logged_audio_) {
    logged_audio_ = true;
    append_webm_perf_log("first_audio_data",
                         static_cast<size_t>(stream_source_.offset), 0,
                         WebmRemuxError::None, perf_start_ms_);
    append_webm_seek_execution_log("first_audio_data", seek_context_,
                                   static_cast<uint64_t>(stream_source_.offset),
                                   packet_tstamp_ms,
                                   webm_perf_elapsed_ms(perf_start_ms_));
  }
  return true;
}

void WebmOpusStreamingDecoder::log_first_decoded_pcm_if_needed(
    const OpusDecodeResult &decoded) {
  if (first_decoded_pcm_logged_ || !decoded.ok || decoded.eof ||
      decoded.samples_per_channel <= 0) {
    return;
  }
  first_decoded_pcm_logged_ = true;
  first_decoded_pcm_elapsed_ms_ = webm_perf_elapsed_ms(perf_start_ms_);
  append_webm_perf_log(
      "first_decoded_pcm", static_cast<size_t>(stream_source_.offset),
      ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
  append_webm_seek_execution_log("first_decoded_pcm", seek_context_,
                                 static_cast<uint64_t>(stream_source_.offset),
                                 -1, first_decoded_pcm_elapsed_ms_);
}

void WebmOpusStreamingDecoder::log_first_audible_pcm_if_needed(
    const OpusDecodeResult &decoded) {
  if (first_audible_pcm_logged_ || !decoded.ok || decoded.eof ||
      decoded.samples_per_channel <= 0) {
    return;
  }
  first_audible_pcm_logged_ = true;
  first_audible_pcm_elapsed_ms_ = webm_perf_elapsed_ms(perf_start_ms_);
  append_webm_perf_log(
      "first_audible_pcm", static_cast<size_t>(stream_source_.offset),
      ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
  append_webm_seek_execution_log("first_audible_pcm", seek_context_,
                                 static_cast<uint64_t>(stream_source_.offset),
                                 -1, first_audible_pcm_elapsed_ms_);
  if (seek_start_ms_ > 0 && parser_seek_done_elapsed_ms_ > 0 &&
      first_decoded_pcm_elapsed_ms_ > 0) {
    append_webm_seek_decode_breakdown_log(parser_seek_done_elapsed_ms_,
                                          first_decoded_pcm_elapsed_ms_,
                                          first_audible_pcm_elapsed_ms_);
  }
}

bool WebmOpusStreamingDecoder::finalize_stream() {
  if (decode_backend_ == WebmDecodeBackend::DirectOpusPacket) {
    direct_packets_complete_ = true;
    append_webm_perf_log("remux_done",
                         static_cast<size_t>(stream_source_.offset), 0,
                         WebmRemuxError::None, perf_start_ms_);
    return true;
  }
  if (!flush_audio_page(0x04U)) {
    return false;
  }
  ogg_complete_ = true;
  append_webm_perf_log("remux_done", static_cast<size_t>(stream_source_.offset),
                       ogg_buffer_.size(), WebmRemuxError::None,
                       perf_start_ms_);
  return true;
}

bool WebmOpusStreamingDecoder::open_decoder_if_ready() {
  if (decoder_open_ || remux_failed_ || !logged_headers_ || !logged_audio_) {
    return decoder_open_;
  }
  if (!decoder_.open_streaming(&ogg_buffer_, &ogg_lock_, &ogg_complete_,
                               &WebmOpusStreamingDecoder::pump_callback, this,
                               true)) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::InvalidBlock;
    append_webm_perf_log("decoder_open_failed",
                         static_cast<size_t>(stream_source_.offset),
                         ogg_buffer_.size(), last_error_, perf_start_ms_);
    return false;
  }
  decoder_open_ = true;
  if (!logged_decoder_open_) {
    logged_decoder_open_ = true;
    append_webm_perf_log(
        "decoder_open_ok", static_cast<size_t>(stream_source_.offset),
        ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
  }
  return true;
}

bool WebmOpusStreamingDecoder::pump_more_data() {
  if (remux_failed_ || !nestegg_inited_) {
    return false;
  }

  const size_t packet_pump_limit =
      (seek_start_ms_ > 0 &&
       (!logged_audio_ || first_emitted_packet_tstamp_ms_ < 0 ||
        !seek_decode_ready_logged_))
          ? kSeekStartupPacketPumpLimit
          : kDefaultWebmPacketPumpLimit;
  size_t processed_packets = 0;
  while (processed_packets < packet_pump_limit) {
    // Backpressure is applied before reading the next nestegg packet. Once a
    // packet is read, all of its chunks must be preserved even if that nudges
    // the direct queue over the soft limit until decode drains it.
    if (decode_backend_ == WebmDecodeBackend::DirectOpusPacket &&
        direct_packet_queue_full()) {
      break;
    }

    nestegg_packet *packet = NULL;
    const int result = nestegg_read_packet(nestegg_ctx_, &packet);
    if (result == 1) {
      if (packet) {
        unsigned int track = 0;
        if (nestegg_packet_track(packet, &track) == 0 && track == opus_track_) {
          uint64_t packet_tstamp_ns = 0;
          if (seek_start_ms_ > 0 &&
              nestegg_packet_tstamp(packet, &packet_tstamp_ns) != 0) {
            nestegg_free_packet(packet);
            remux_failed_ = true;
            last_error_ = WebmRemuxError::InvalidBlock;
            return false;
          }
          unsigned int chunk_count = 0;
          if (nestegg_packet_count(packet, &chunk_count) != 0) {
            nestegg_free_packet(packet);
            remux_failed_ = true;
            last_error_ = WebmRemuxError::InvalidBlock;
            return false;
          }
          for (unsigned int chunk = 0; chunk < chunk_count; ++chunk) {
            unsigned char *data = NULL;
            size_t length = 0;
            if (nestegg_packet_data(packet, chunk, &data, &length) != 0 ||
                !emit_packet(data, length, packet_tstamp_ns)) {
              nestegg_free_packet(packet);
              remux_failed_ = true;
              return false;
            }
          }
        }
        nestegg_free_packet(packet);
      }
      ++processed_packets;
      continue;
    }

    if (result == 0) {
      return finalize_stream();
    }

    if (parser_range_fetch_failed_) {
      remux_failed_ = true;
      if (last_error_ == WebmRemuxError::None) {
        last_error_ = WebmRemuxError::InvalidBlock;
      }
      append_webm_perf_log("remux_failed",
                           static_cast<size_t>(stream_source_.offset),
                           ogg_buffer_.size(), last_error_, perf_start_ms_);
      return false;
    }

    if (stream_source_.download_complete && *stream_source_.download_complete) {
      remux_failed_ = true;
      last_error_ = WebmRemuxError::InvalidBlock;
      append_webm_perf_log("remux_failed",
                           static_cast<size_t>(stream_source_.offset),
                           ogg_buffer_.size(), last_error_, perf_start_ms_);
      return false;
    }
    if (nestegg_read_reset(nestegg_ctx_) != 0) {
      remux_failed_ = true;
      last_error_ = WebmRemuxError::InvalidBlock;
      return false;
    }
    break;
  }

  if (!decoder_open_) {
    open_decoder_if_ready();
  }
  return processed_packets > 0U;
}

OpusDecodeResult WebmOpusStreamingDecoder::decode(int16_t *pcm_out,
                                                  size_t pcm_capacity_samples) {
  if (decode_backend_ == WebmDecodeBackend::DirectOpusPacket) {
    return decode_direct_packet(pcm_out, pcm_capacity_samples);
  }

  for (int attempt = 0; attempt < 8; ++attempt) {
    if (!decoder_open_ && !pump_more_data()) {
      OpusDecodeResult failed = {false, 0, 48000, 0, false};
      if (remux_failed_) {
        return failed;
      }
    }
    if (!decoder_open_) {
      OpusDecodeResult waiting = {false, 0, 48000, 0, false};
      return waiting;
    }

    OpusDecodeResult decoded = decoder_.decode(pcm_out, pcm_capacity_samples);
    log_first_decoded_pcm_if_needed(decoded);
    if (decoded.ok && !decoded.eof && pcm_skip_samples_per_channel_ <= 0 &&
        seek_start_ms_ > 0 && !seek_decode_ready_logged_ &&
        decoded.samples_per_channel > 0) {
      seek_decode_ready_logged_ = true;
      append_webm_perf_log(
          "seek_decode_ready", static_cast<size_t>(stream_source_.offset),
          ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
    }
    if (!decoded.ok || decoded.eof || pcm_skip_samples_per_channel_ <= 0) {
      log_first_audible_pcm_if_needed(decoded);
      return decoded;
    }

    const int skip_samples =
        decoded.samples_per_channel < pcm_skip_samples_per_channel_
            ? decoded.samples_per_channel
            : pcm_skip_samples_per_channel_;
    pcm_skip_samples_per_channel_ -= skip_samples;
    if (!pcm_skip_logged_ && pcm_skip_samples_per_channel_ <= 0) {
      pcm_skip_logged_ = true;
      append_webm_perf_log(
          "seek_pcm_skip_done", static_cast<size_t>(stream_source_.offset),
          ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
    }
    if (skip_samples == decoded.samples_per_channel) {
      continue;
    }

    const int remaining_samples = decoded.samples_per_channel - skip_samples;
    const size_t channel_count = decoded.channels > 0 ? decoded.channels : 2;
    memmove(pcm_out, pcm_out + (skip_samples * channel_count),
            static_cast<size_t>(remaining_samples) * channel_count *
                sizeof(int16_t));
    decoded.samples_per_channel = remaining_samples;
    if (seek_start_ms_ > 0 && !seek_decode_ready_logged_) {
      seek_decode_ready_logged_ = true;
      append_webm_perf_log(
          "seek_decode_ready", static_cast<size_t>(stream_source_.offset),
          ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
    }
    log_first_audible_pcm_if_needed(decoded);
    return decoded;
  }

  OpusDecodeResult waiting = {false, 0, 48000, 0, false};
  return waiting;
}

OpusDecodeResult
WebmOpusStreamingDecoder::decode_direct_packet(int16_t *pcm_out,
                                               size_t pcm_capacity_samples) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (direct_packet_queue_.empty() && !direct_packets_complete_) {
      if (!pump_more_data() && remux_failed_) {
        OpusDecodeResult failed = {false, 0, 48000, 0, false};
        return failed;
      }
    }

    if (direct_packet_queue_.empty()) {
      OpusDecodeResult result = {false, 0, 48000, 0, direct_packets_complete_};
      return result;
    }

    const DirectOpusPacket packet = direct_packet_queue_.front();
    direct_packet_queue_.erase(direct_packet_queue_.begin());
    if (packet.data.empty()) {
      remux_failed_ = true;
      last_error_ = WebmRemuxError::InvalidBlock;
      OpusDecodeResult failed = {false, 0, 48000, 0, false};
      return failed;
    }

    WebmOpusPacketDecodeResult decoded = packet_decoder_.decode_packet(
        packet.data.data(), packet.data.size(), pcm_out, pcm_capacity_samples);
    if (packet_decoder_.has_failed()) {
      remux_failed_ = true;
      last_error_ = packet_decode_error_to_remux_error(packet_decoder_.error());
      OpusDecodeResult failed = {false, 0, 48000, 0, false};
      return failed;
    }
    if (decoded.consumed_packet && !decoded.has_output) {
      log_first_decoded_pcm_if_needed(decoded.decoded);
      if (seek_start_ms_ > 0 && !pcm_skip_logged_ &&
          packet_decoder_.pending_skip_samples_per_channel() <= 0) {
        pcm_skip_logged_ = true;
        append_webm_perf_log("seek_pcm_skip_done",
                             static_cast<size_t>(stream_source_.offset), 0,
                             WebmRemuxError::None, perf_start_ms_);
      }
      continue;
    }
    if (!decoded.has_output) {
      OpusDecodeResult waiting = {false, 0, 48000, 0, false};
      return waiting;
    }

    log_first_decoded_pcm_if_needed(decoded.decoded);
    if (seek_start_ms_ > 0 && !pcm_skip_logged_ &&
        packet_decoder_.pending_skip_samples_per_channel() <= 0) {
      pcm_skip_logged_ = true;
      append_webm_perf_log("seek_pcm_skip_done",
                           static_cast<size_t>(stream_source_.offset), 0,
                           WebmRemuxError::None, perf_start_ms_);
    }
    if (decoded.decoded.ok && !decoded.decoded.eof && seek_start_ms_ > 0 &&
        !seek_decode_ready_logged_ && decoded.decoded.samples_per_channel > 0) {
      seek_decode_ready_logged_ = true;
      append_webm_perf_log("seek_decode_ready",
                           static_cast<size_t>(stream_source_.offset), 0,
                           WebmRemuxError::None, perf_start_ms_);
    }
    log_first_audible_pcm_if_needed(decoded.decoded);
    return decoded.decoded;
  }

  OpusDecodeResult waiting = {false, 0, 48000, 0, false};
  return waiting;
}

bool WebmOpusStreamingDecoder::direct_packet_queue_full() const {
  return direct_packet_queue_.size() >= kDirectOpusPacketQueueLimit;
}

WebmRemuxError WebmOpusStreamingDecoder::packet_decode_error_to_remux_error(
    WebmOpusPacketDecodeError error) const {
  switch (error) {
    case WebmOpusPacketDecodeError::InvalidCodecPrivate:
      return WebmRemuxError::InvalidCodecPrivate;
    case WebmOpusPacketDecodeError::UnsupportedChannels:
    case WebmOpusPacketDecodeError::UnsupportedMappingFamily:
    case WebmOpusPacketDecodeError::DecoderCreateFailed:
    case WebmOpusPacketDecodeError::PacketTooLarge:
      return WebmRemuxError::UnsupportedFeature;
    case WebmOpusPacketDecodeError::DecodeFailed:
      return WebmRemuxError::InvalidBlock;
    case WebmOpusPacketDecodeError::None:
      return WebmRemuxError::None;
  }
  return WebmRemuxError::InvalidBlock;
}

bool WebmOpusStreamingDecoder::is_open() const { return decoder_open_; }

bool WebmOpusStreamingDecoder::is_eof() const {
  if (decode_backend_ == WebmDecodeBackend::DirectOpusPacket) {
    return direct_packets_complete_ && direct_packet_queue_.empty();
  }
  return decoder_open_ && decoder_.is_eof();
}

bool WebmOpusStreamingDecoder::has_failed() const {
  return remux_failed_ ||
         (decode_backend_ == WebmDecodeBackend::OggBridge && decoder_open_ &&
          decoder_.has_failed()) ||
         (decode_backend_ == WebmDecodeBackend::DirectOpusPacket &&
          packet_decoder_.has_failed());
}

WebmRemuxError WebmOpusStreamingDecoder::remux_error() const {
  return last_error_;
}

bool WebmOpusStreamingDecoder::get_last_seek_runtime_point(
    uint64_t *out_start_byte, int *out_timecode_ms) const {
  if (!out_start_byte || !out_timecode_ms ||
      last_seek_runtime_start_byte_ == 0U ||
      last_seek_runtime_timecode_ms_ < 0) {
    return false;
  }
  *out_start_byte = last_seek_runtime_start_byte_;
  *out_timecode_ms = last_seek_runtime_timecode_ms_;
  return true;
}
