#include "webm_ogg_bridge_path.h"

#include <string.h>

namespace {

static const uint32_t OGG_SERIAL = 0x5354524DU;
static const size_t MAX_OGG_PAGE_SEGMENTS = 255U;

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
    const uint8_t index = static_cast<uint8_t>((crc >> 24) ^ data[i]);
    crc = (crc << 8) ^ table[index];
  }
  return crc;
}

static std::vector<uint8_t> build_opus_tags() {
  const char vendor[] = "StreaMu";
  std::vector<uint8_t> tags;
  tags.insert(tags.end(), {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
  append_u32_le(&tags, sizeof(vendor) - 1U);
  tags.insert(tags.end(), vendor, vendor + sizeof(vendor) - 1U);
  append_u32_le(&tags, 0);
  return tags;
}

static std::vector<uint8_t>
packet_segments(const std::vector<uint8_t> &packet) {
  std::vector<uint8_t> segments;
  size_t remaining = packet.size();
  while (remaining >= 255U) {
    segments.push_back(255U);
    remaining -= 255U;
  }
  segments.push_back(static_cast<uint8_t>(remaining));
  return segments;
}

static int packet_segment_count(const std::vector<uint8_t> &packet) {
  return static_cast<int>((packet.size() / 255U) + 1U);
}

static bool build_ogg_page_from_packets(
    const std::vector<WebmOggBridgePath::Packet> &packets, uint8_t header_type,
    uint32_t sequence, std::vector<uint8_t> *page_out) {
  if (!page_out || packets.empty()) {
    return false;
  }

  std::vector<uint8_t> segments;
  size_t payload_size = 0;
  for (size_t i = 0; i < packets.size(); ++i) {
    const std::vector<uint8_t> packet_segment_bytes =
        packet_segments(packets[i].data);
    if (packet_segment_bytes.size() > MAX_OGG_PAGE_SEGMENTS ||
        segments.size() + packet_segment_bytes.size() > MAX_OGG_PAGE_SEGMENTS) {
      return false;
    }
    segments.insert(segments.end(), packet_segment_bytes.begin(),
                    packet_segment_bytes.end());
    payload_size += packets[i].data.size();
  }

  page_out->clear();
  page_out->reserve(27U + segments.size() + payload_size);
  page_out->insert(page_out->end(), {'O', 'g', 'g', 'S'});
  page_out->push_back(0);
  page_out->push_back(header_type);
  append_u64_le(page_out,
                static_cast<uint64_t>(packets.back().granule_position));
  append_u32_le(page_out, OGG_SERIAL);
  append_u32_le(page_out, sequence);
  append_u32_le(page_out, 0);
  page_out->push_back(static_cast<uint8_t>(segments.size()));
  page_out->insert(page_out->end(), segments.begin(), segments.end());
  for (size_t i = 0; i < packets.size(); ++i) {
    page_out->insert(page_out->end(), packets[i].data.begin(),
                     packets[i].data.end());
  }
  const uint32_t crc = ogg_crc(*page_out);
  (*page_out)[22] = static_cast<uint8_t>(crc & 0xFFU);
  (*page_out)[23] = static_cast<uint8_t>((crc >> 8) & 0xFFU);
  (*page_out)[24] = static_cast<uint8_t>((crc >> 16) & 0xFFU);
  (*page_out)[25] = static_cast<uint8_t>((crc >> 24) & 0xFFU);
  return true;
}

static bool build_ogg_page(const std::vector<uint8_t> &packet,
                           uint8_t header_type, int64_t granule_position,
                           uint32_t sequence, std::vector<uint8_t> *page_out) {
  std::vector<WebmOggBridgePath::Packet> packets;
  packets.push_back(WebmOggBridgePath::Packet{packet, granule_position});
  return build_ogg_page_from_packets(packets, header_type, sequence, page_out);
}

static void set_error(WebmRemuxError *out_error, WebmRemuxError error) {
  if (out_error) {
    *out_error = error;
  }
}

} // namespace

WebmOggBridgePath::WebmOggBridgePath()
    : complete_(false), sequence_(0), audio_page_bytes_(0),
      audio_page_segments_(0) {
  LightLock_Init(&lock_);
}

WebmOggBridgePath::~WebmOggBridgePath() { reset(); }

void WebmOggBridgePath::reset() {
  audio_page_packets_.clear();
  LightLock_Lock(&lock_);
  buffer_.clear();
  LightLock_Unlock(&lock_);
  complete_ = false;
  sequence_ = 0;
  audio_page_bytes_ = 0;
  audio_page_segments_ = 0;
}

void WebmOggBridgePath::release_storage() {
  audio_page_packets_.clear();
  std::vector<Packet>().swap(audio_page_packets_);
  LightLock_Lock(&lock_);
  std::vector<uint8_t>().swap(buffer_);
  LightLock_Unlock(&lock_);
  complete_ = false;
  sequence_ = 0;
  audio_page_bytes_ = 0;
  audio_page_segments_ = 0;
}

bool WebmOggBridgePath::emit_headers(const std::vector<uint8_t> &codec_private,
                                     WebmRemuxError *out_error) {
  std::vector<uint8_t> page;
  if (!build_ogg_page(codec_private, 0x02U, 0, sequence_, &page)) {
    set_error(out_error, WebmRemuxError::InvalidCodecPrivate);
    return false;
  }
  LightLock_Lock(&lock_);
  buffer_.insert(buffer_.end(), page.begin(), page.end());
  LightLock_Unlock(&lock_);
  ++sequence_;

  const std::vector<uint8_t> tags = build_opus_tags();
  if (!build_ogg_page(tags, 0x00U, 0, sequence_, &page)) {
    set_error(out_error, WebmRemuxError::InvalidCodecPrivate);
    return false;
  }
  LightLock_Lock(&lock_);
  buffer_.insert(buffer_.end(), page.begin(), page.end());
  LightLock_Unlock(&lock_);
  ++sequence_;
  return true;
}

bool WebmOggBridgePath::emit_packet(const unsigned char *data, size_t length,
                                    int64_t granule_position,
                                    size_t target_page_bytes,
                                    WebmRemuxError *out_error) {
  if (!data || length == 0U) {
    set_error(out_error, WebmRemuxError::InvalidBlock);
    return false;
  }

  Packet packet;
  packet.data.assign(data, data + length);
  packet.granule_position = granule_position;
  return append_audio_packet(packet, target_page_bytes, out_error);
}

bool WebmOggBridgePath::flush_audio_page(uint8_t header_type,
                                         WebmRemuxError *out_error) {
  if (audio_page_packets_.empty()) {
    return true;
  }
  std::vector<uint8_t> page;
  if (!build_ogg_page_from_packets(audio_page_packets_, header_type, sequence_,
                                   &page)) {
    set_error(out_error, WebmRemuxError::InvalidBlock);
    return false;
  }
  LightLock_Lock(&lock_);
  buffer_.insert(buffer_.end(), page.begin(), page.end());
  LightLock_Unlock(&lock_);
  ++sequence_;
  audio_page_packets_.clear();
  audio_page_bytes_ = 0;
  audio_page_segments_ = 0;
  return true;
}

bool WebmOggBridgePath::finalize(WebmRemuxError *out_error) {
  if (!flush_audio_page(0x04U, out_error)) {
    return false;
  }
  complete_ = true;
  return true;
}

bool WebmOggBridgePath::complete() const { return complete_; }

size_t WebmOggBridgePath::buffered_bytes() const {
  LightLock_Lock(&lock_);
  const size_t size = buffer_.size();
  LightLock_Unlock(&lock_);
  return size;
}

std::vector<uint8_t> *WebmOggBridgePath::buffer() { return &buffer_; }

LightLock *WebmOggBridgePath::lock() { return &lock_; }

bool *WebmOggBridgePath::complete_flag() { return &complete_; }

bool WebmOggBridgePath::append_audio_packet(const Packet &packet,
                                            size_t target_page_bytes,
                                            WebmRemuxError *out_error) {
  const int segment_count = packet_segment_count(packet.data);
  if (!audio_page_packets_.empty() &&
      audio_page_segments_ + static_cast<size_t>(segment_count) >
          MAX_OGG_PAGE_SEGMENTS) {
    if (!flush_audio_page(0x00U, out_error)) {
      return false;
    }
  }
  audio_page_packets_.push_back(packet);
  audio_page_bytes_ += packet.data.size();
  audio_page_segments_ += static_cast<size_t>(segment_count);
  if (audio_page_bytes_ >= target_page_bytes) {
    if (!flush_audio_page(0x00U, out_error)) {
      return false;
    }
  }
  return true;
}
