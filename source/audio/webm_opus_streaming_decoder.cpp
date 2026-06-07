#include "webm_opus_streaming_decoder.h"

#include <stdio.h>
#include <string.h>

extern "C" {
#include <nestegg/nestegg.h>
}

namespace {

static const uint32_t OGG_SERIAL = 0x5354524DU;
static const size_t MAX_OGG_PAGE_SEGMENTS = 255U;
static constexpr size_t kDefaultOggAudioPageTargetBytes = 2048U;
static constexpr size_t kDefaultWebmPacketPumpLimit = 8U;

static void append_webm_perf_log(const char *event, size_t raw_bytes,
                                 size_t ogg_bytes, WebmRemuxError error,
                                 u64 start_ms) {
  if (!event) {
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

static bool build_ogg_page(const std::vector<uint8_t> &packet, uint8_t header_type,
                           int64_t granule_position, uint32_t sequence,
                           std::vector<uint8_t> *page_out) {
  std::vector<WebmOpusStreamingDecoder::OggPagePacket> packets;
  packets.push_back(
      WebmOpusStreamingDecoder::OggPagePacket{packet, granule_position});
  return build_ogg_page_from_packets(packets, header_type, sequence, page_out);
}

static bool opus_packet_duration_samples(const unsigned char *packet, size_t length,
                                         int *duration_samples) {
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

} // namespace

WebmOpusStreamingDecoder::WebmOpusStreamingDecoder()
    : stream_source_{NULL, NULL, NULL, 0}, nestegg_ctx_(NULL),
      nestegg_inited_(false), opus_track_(0), ogg_complete_(false),
      decoder_open_(false), remux_failed_(false), logged_init_(false),
      logged_track_(false), logged_headers_(false),
      logged_audio_(false), logged_decoder_open_(false),
      ogg_sequence_(0), granule_position_(0), audio_page_bytes_(0),
      audio_page_segments_(0),
      last_error_(WebmRemuxError::None), perf_start_ms_(0) {
  LightLock_Init(&ogg_lock_);
}

WebmOpusStreamingDecoder::~WebmOpusStreamingDecoder() { reset(); }

int64_t WebmOpusStreamingDecoder::nestegg_read_cb(void *buffer, size_t length,
                                                  void *userdata) {
  StreamSource *source = static_cast<StreamSource *>(userdata);
  if (!source || !buffer || !source->buffer || !source->lock ||
      !source->download_complete) {
    return -1;
  }

  while (true) {
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
    svcSleepThread(10 * 1000 * 1000);
  }
}

int WebmOpusStreamingDecoder::nestegg_seek_cb(int64_t offset, int whence,
                                              void *userdata) {
  StreamSource *source = static_cast<StreamSource *>(userdata);
  if (!source || !source->buffer || !source->lock || !source->download_complete) {
    return -1;
  }

  while (true) {
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
    const std::vector<uint8_t> *webm_buffer, LightLock *webm_lock,
    const bool *webm_download_complete) {
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
  stream_source_ = {NULL, NULL, NULL, 0};
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
  append_webm_perf_log("segment_found", static_cast<size_t>(stream_source_.offset),
                       ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);

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
  append_webm_perf_log("tracks_found", static_cast<size_t>(stream_source_.offset),
                       ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);

  unsigned int codec_chunks = 0;
  if (nestegg_track_codec_data_count(nestegg_ctx_, opus_track_, &codec_chunks) != 0 ||
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
  {
    FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
    if (f) {
      const u64 now_ms = osGetTime();
      const u64 elapsed_ms =
          (perf_start_ms_ > 0 && now_ms >= perf_start_ms_) ? now_ms - perf_start_ms_
                                                           : 0;
      fprintf(f, "[webm-perf] +%llums codec_private_info length=%lu",
              static_cast<unsigned long long>(elapsed_ms),
              static_cast<unsigned long>(codec_private_.size()));
      const size_t preview =
          codec_private_.size() < 16U ? codec_private_.size() : 16U;
      for (size_t i = 0; i < preview; ++i) {
        fprintf(f, " %02X", static_cast<unsigned int>(codec_private_[i]));
      }
      fputc('\n', f);
      fclose(f);
    }
  }
  if (memcmp(codec_private_.data(), "OpusHead", 8U) != 0) {
    remux_failed_ = true;
    last_error_ = WebmRemuxError::InvalidCodecPrivate;
    return false;
  }

  if (!emit_headers()) {
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
  append_webm_perf_log("headers_emitted", static_cast<size_t>(stream_source_.offset),
                       ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
  return true;
}

bool WebmOpusStreamingDecoder::flush_audio_page(uint8_t header_type) {
  if (audio_page_packets_.empty()) {
    return true;
  }
  std::vector<uint8_t> page;
  if (!build_ogg_page_from_packets(audio_page_packets_, header_type, ogg_sequence_,
                                   &page)) {
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

bool WebmOpusStreamingDecoder::append_audio_packet(const OggPagePacket &packet) {
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
  if (audio_page_bytes_ >= kDefaultOggAudioPageTargetBytes) {
    if (!flush_audio_page(0x00U)) {
      return false;
    }
  }
  return true;
}

bool WebmOpusStreamingDecoder::emit_packet(const unsigned char *data, size_t length) {
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
    append_webm_perf_log("first_audio_data",
                         static_cast<size_t>(stream_source_.offset),
                         ogg_buffer_.size(), WebmRemuxError::None,
                         perf_start_ms_);
  }
  return true;
}

bool WebmOpusStreamingDecoder::finalize_stream() {
  if (!flush_audio_page(0x04U)) {
    return false;
  }
  ogg_complete_ = true;
  append_webm_perf_log("remux_done", static_cast<size_t>(stream_source_.offset),
                       ogg_buffer_.size(), WebmRemuxError::None, perf_start_ms_);
  return true;
}

bool WebmOpusStreamingDecoder::open_decoder_if_ready() {
  if (decoder_open_ || remux_failed_ || !logged_headers_ || !logged_audio_) {
    return decoder_open_;
  }
  if (!decoder_.open_streaming(&ogg_buffer_, &ogg_lock_, &ogg_complete_,
                               &WebmOpusStreamingDecoder::pump_callback, this)) {
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
    append_webm_perf_log("decoder_open_ok",
                         static_cast<size_t>(stream_source_.offset),
                         ogg_buffer_.size(), WebmRemuxError::None,
                         perf_start_ms_);
  }
  return true;
}

bool WebmOpusStreamingDecoder::pump_more_data() {
  if (remux_failed_ || !nestegg_inited_) {
    return false;
  }

  size_t processed_packets = 0;
  while (processed_packets < kDefaultWebmPacketPumpLimit) {
    nestegg_packet *packet = NULL;
    const int result = nestegg_read_packet(nestegg_ctx_, &packet);
    if (result == 1) {
      if (packet) {
        unsigned int track = 0;
        if (nestegg_packet_track(packet, &track) == 0 && track == opus_track_) {
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
                !emit_packet(data, length)) {
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

    if (stream_source_.download_complete && *stream_source_.download_complete) {
      remux_failed_ = true;
      last_error_ = WebmRemuxError::InvalidBlock;
      append_webm_perf_log("remux_failed", static_cast<size_t>(stream_source_.offset),
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
  return decoder_.decode(pcm_out, pcm_capacity_samples);
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
