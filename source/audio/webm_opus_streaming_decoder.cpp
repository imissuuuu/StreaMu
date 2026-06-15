#include "webm_opus_streaming_decoder.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <nestegg/nestegg.h>
}

namespace {

static const uint32_t OGG_SERIAL = 0x5354524DU;
static const size_t MAX_OGG_PAGE_SEGMENTS = 255U;
static constexpr size_t kDefaultWebmPacketPumpLimit = 8U;
static constexpr size_t kSeekStartupPacketPumpLimit = 48U;
static constexpr size_t kParserRangeInitialSeekChunkBytes = 128U * 1024U;
static constexpr size_t kParserRangeFetchChunkBytes = 512U * 1024U;
static constexpr long kParserRangeFetchTimeoutMs = 15000L;

static bool should_log_webm_perf_event(const char *event) {
  if (!event) {
    return false;
  }
  return strcmp(event, "decoder_session_start") == 0 ||
         strcmp(event, "nestegg_init_failed") == 0 ||
         strcmp(event, "parser_range_fetch_failed") == 0 ||
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
    : stream_source_{NULL, NULL, NULL, 0, 0, "", NULL}, nestegg_ctx_(NULL),
      nestegg_inited_(false), opus_track_(0), ogg_complete_(false),
      decoder_open_(false), remux_failed_(false), logged_init_(false),
      logged_track_(false), logged_headers_(false), logged_audio_(false),
      logged_decoder_open_(false), ogg_sequence_(0), granule_position_(0),
      audio_page_bytes_(0), audio_page_segments_(0),
      last_error_(WebmRemuxError::None), perf_start_ms_(0), seek_start_ms_(0),
      seek_emit_ms_(0), requested_emit_start_ms_(0),
      parser_seek_enabled_(false), parser_offset_seek_preferred_(false),
      parser_range_fetch_failed_(false), parser_prefetch_offset_(0),
      pcm_skip_samples_per_channel_(0), first_emitted_packet_tstamp_ms_(-1),
      pcm_skip_logged_(false), seek_decode_ready_logged_(false),
      last_seek_runtime_start_byte_(0), last_seek_runtime_timecode_ms_(-1),
      discarded_packets_before_emit_(0), seek_packet_discard_logged_(false),
      first_decoded_pcm_logged_(false), first_audible_pcm_logged_(false),
      seek_preroll_initialized_(false), range_fetch_curl_(NULL) {
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
    if (source->owner && source->owner->parser_seek_enabled_) {
      const size_t copied = source->owner->copy_from_range_segments(
          static_cast<uint64_t>(source->offset), static_cast<uint8_t *>(buffer),
          length);
      if (copied > 0U) {
        source->offset += static_cast<int64_t>(copied);
        return static_cast<int64_t>(copied);
      }
      if (source->owner->fetch_range_segment(
              static_cast<uint64_t>(source->offset), length)) {
        continue;
      }
      if (source->owner->parser_range_fetch_failed_) {
        return -1;
      }
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
    if (source->owner && source->owner->ensure_stream_bytes(
                             static_cast<size_t>(source->offset) + length)) {
      continue;
    }
    if (source->owner && source->owner->parser_range_fetch_failed_) {
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
    svcSleepThread(10 * 1000 * 1000);
  }
}

int64_t WebmOpusStreamingDecoder::nestegg_tell_cb(void *userdata) {
  StreamSource *source = static_cast<StreamSource *>(userdata);
  return source ? source->offset : -1;
}

bool WebmOpusStreamingDecoder::open_streaming(
    std::vector<uint8_t> *webm_buffer, LightLock *webm_lock,
    bool *webm_download_complete, int seek_start_ms, int emit_start_ms,
    bool enable_parser_seek, bool prefer_offset_seek,
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
  seek_start_ms_ = seek_start_ms > 0 ? seek_start_ms : 0;
  seek_emit_ms_ = 0;
  requested_emit_start_ms_ = emit_start_ms > 0 ? emit_start_ms : 0;
  parser_seek_enabled_ = enable_parser_seek;
  parser_offset_seek_preferred_ = prefer_offset_seek;
  parser_prefetch_offset_ = parser_prefetch_offset;
  seek_preroll_initialized_ = false;
  seed_initial_range_segment();
  perf_start_ms_ = osGetTime();
  append_webm_perf_log("decoder_session_start", 0, 0, WebmRemuxError::None,
                       perf_start_ms_);
  return init_nestegg();
}

void WebmOpusStreamingDecoder::reset() {
  decoder_.reset();
  if (nestegg_ctx_) {
    nestegg_destroy(nestegg_ctx_);
    nestegg_ctx_ = NULL;
  }
  nestegg_inited_ = false;
  stream_source_ = {NULL, NULL, NULL, 0, 0, "", NULL};
  codec_private_.clear();
  audio_page_packets_.clear();
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
  perf_start_ms_ = 0;
  seek_start_ms_ = 0;
  seek_emit_ms_ = 0;
  requested_emit_start_ms_ = 0;
  parser_offset_seek_preferred_ = false;
  parser_range_fetch_failed_ = false;
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
  seek_preroll_initialized_ = false;
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

bool WebmOpusStreamingDecoder::fetch_range_segment(uint64_t offset,
                                                   size_t min_length) {
  if (!parser_seek_enabled_ || stream_source_.range_probe_base_url.empty()) {
    return false;
  }

  const uint64_t fetch_start = offset;
  const bool seek_startup_fetch =
      seek_start_ms_ > 0 &&
      (!logged_audio_ || first_emitted_packet_tstamp_ms_ < 0 ||
       !seek_decode_ready_logged_);
  const uint64_t preferred_fetch_size =
      seek_startup_fetch
          ? static_cast<uint64_t>(kParserRangeInitialSeekChunkBytes)
          : static_cast<uint64_t>(kParserRangeFetchChunkBytes);
  uint64_t fetch_size = preferred_fetch_size;
  if (!seek_startup_fetch && min_length > preferred_fetch_size) {
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
  if (!range_fetch_curl_) {
    range_fetch_curl_ = curl_easy_init();
  }
  CURL *curl = range_fetch_curl_;
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
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kParserRangeFetchTimeoutMs);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kParserRangeFetchTimeoutMs);
  const CURLcode res = curl_easy_perform(curl);
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  if (res != CURLE_OK || response_code >= 400L || fetched.empty()) {
    parser_range_fetch_failed_ = true;
    last_error_ = WebmRemuxError::InvalidBlock;
    append_webm_perf_log("parser_range_fetch_failed",
                         static_cast<size_t>(fetch_start), fetched.size(),
                         last_error_, perf_start_ms_);
    return false;
  }

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
  }
  if (stream_source_.filesize > 0U &&
      fetch_start + fetch_size >= stream_source_.filesize &&
      stream_source_.download_complete) {
    *stream_source_.download_complete = true;
  }
  LightLock_Unlock(stream_source_.lock);
  return true;
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

  if (!emit_headers()) {
    remux_failed_ = true;
    return false;
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
      offset_seek_applied = true;
    }
    if (!offset_seek_applied) {
      append_webm_perf_log("parser_offset_seek_failed",
                           static_cast<size_t>(stream_source_.offset),
                           ogg_buffer_.size(), WebmRemuxError::None,
                           perf_start_ms_);
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
      return true;
    }
    last_error_ = WebmRemuxError::InvalidBlock;
    append_webm_perf_log("parser_seek_failed",
                         static_cast<size_t>(stream_source_.offset),
                         ogg_buffer_.size(), last_error_, perf_start_ms_);
    return false;
  }
  append_webm_perf_log(
      "parser_seek_done", static_cast<size_t>(stream_source_.offset),
      ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
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
  granule_position_ += duration_samples;
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
  append_webm_perf_log(
      "first_decoded_pcm", static_cast<size_t>(stream_source_.offset),
      ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
}

void WebmOpusStreamingDecoder::log_first_audible_pcm_if_needed(
    const OpusDecodeResult &decoded) {
  if (first_audible_pcm_logged_ || !decoded.ok || decoded.eof ||
      decoded.samples_per_channel <= 0) {
    return;
  }
  first_audible_pcm_logged_ = true;
  append_webm_perf_log(
      "first_audible_pcm", static_cast<size_t>(stream_source_.offset),
      ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
}

bool WebmOpusStreamingDecoder::finalize_stream() {
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
                               &WebmOpusStreamingDecoder::pump_callback,
                               this)) {
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

bool WebmOpusStreamingDecoder::is_open() const { return decoder_open_; }

bool WebmOpusStreamingDecoder::is_eof() const {
  return decoder_open_ && decoder_.is_eof();
}

bool WebmOpusStreamingDecoder::has_failed() const {
  return remux_failed_ || (decoder_open_ && decoder_.has_failed());
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
