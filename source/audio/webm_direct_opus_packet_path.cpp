#include "webm_direct_opus_packet_path.h"

#include <limits.h>
#include <string.h>

WebmDirectOpusPacketPath::WebmDirectOpusPacketPath()
    : read_index_(0), write_index_(0), queued_count_(0),
      max_packet_bytes_seen_(0), complete_(false), failed_(false),
      error_(WebmRemuxError::None) {}

WebmDirectPathOpenResult
WebmDirectOpusPacketPath::open(const uint8_t *codec_private,
                               size_t codec_private_size) {
  reset();

  WebmDirectPathOpenResult result = {};
  if (decoder_.open(codec_private, codec_private_size)) {
    result.status = WebmDirectPathOpenStatus::Ready;
    return result;
  }

  const WebmOpusPacketDecodeError packet_error = decoder_.error();
  if (packet_error == WebmOpusPacketDecodeError::DecoderCreateFailed) {
    result.status = WebmDirectPathOpenStatus::Fatal;
    result.error = WebmRemuxError::UnsupportedFeature;
    mark_failed(result.error);
    return result;
  }

  decoder_.reset();
  result.status = WebmDirectPathOpenStatus::FallbackAllowed;
  result.error = WebmRemuxError::None;
  return result;
}

void WebmDirectOpusPacketPath::reset() {
  decoder_.reset();
  ensure_storage();
  clear_slots();
  read_index_ = 0;
  write_index_ = 0;
  queued_count_ = 0;
  max_packet_bytes_seen_ = 0;
  complete_ = false;
  failed_ = false;
  error_ = WebmRemuxError::None;
}

void WebmDirectOpusPacketPath::release_storage() {
  decoder_.reset();
  std::vector<uint8_t>().swap(packet_storage_);
  clear_slots();
  read_index_ = 0;
  write_index_ = 0;
  queued_count_ = 0;
  max_packet_bytes_seen_ = 0;
  complete_ = false;
  failed_ = false;
  error_ = WebmRemuxError::None;
}

bool WebmDirectOpusPacketPath::is_open() const {
  return decoder_.is_open() && !failed_;
}

bool WebmDirectOpusPacketPath::has_failed() const { return failed_; }

WebmRemuxError WebmDirectOpusPacketPath::error() const { return error_; }

bool WebmDirectOpusPacketPath::is_eof() const { return complete_ && empty(); }

bool WebmDirectOpusPacketPath::queue_full() const {
  return active_count() >= kQueueLimit;
}

bool WebmDirectOpusPacketPath::enqueue_packet(const unsigned char *data,
                                              size_t length,
                                              uint64_t packet_tstamp_ns,
                                              int packet_tstamp_ms) {
  if (!data || length == 0U) {
    mark_failed(WebmRemuxError::InvalidBlock);
    return false;
  }
  if (length > kPacketSlotBytes) {
    mark_failed(WebmRemuxError::UnsupportedFeature);
    return false;
  }
  // Queue fullness is enforced before reading the next nestegg packet. Enqueue
  // still accepts chunks from an already-read packet so multi-chunk packets stay
  // intact, up to the fixed storage bound.
  if (queued_count_ >= kQueueSlotCount) {
    mark_failed(WebmRemuxError::InvalidBlock);
    return false;
  }
  ensure_storage();

  uint8_t *dst = slot_data(write_index_);
  if (!dst) {
    mark_failed(WebmRemuxError::InvalidBlock);
    return false;
  }
  memcpy(dst, data, length);
  PacketSlot &slot = slots_[write_index_];
  slot.length = length;
  slot.tstamp_ns = packet_tstamp_ns;
  slot.tstamp_ms = packet_tstamp_ms;
  write_index_ = (write_index_ + 1U) % kQueueSlotCount;
  ++queued_count_;
  if (length > max_packet_bytes_seen_) {
    max_packet_bytes_seen_ = length;
  }
  return true;
}

WebmDirectOpusDecodeStep
WebmDirectOpusPacketPath::decode_next(int16_t *pcm_out,
                                      size_t pcm_capacity_samples) {
  WebmDirectOpusDecodeStep step = {};
  if (failed_) {
    step.failed = true;
    step.error = error_;
    return step;
  }
  if (empty()) {
    step.decoded.eof = complete_;
    return step;
  }
  if (read_index_ >= kQueueSlotCount || queued_count_ > kQueueSlotCount) {
    mark_failed(WebmRemuxError::InvalidBlock);
    step.failed = true;
    step.error = error_;
    return step;
  }

  const PacketSlot &slot = slots_[read_index_];
  const uint8_t *packet_data = slot_data(read_index_);
  const size_t packet_length = slot.length;
  if (!packet_data || packet_length == 0U || packet_length > kPacketSlotBytes) {
    clear_slot(read_index_);
    read_index_ = (read_index_ + 1U) % kQueueSlotCount;
    --queued_count_;
    mark_failed(WebmRemuxError::InvalidBlock);
    step.failed = true;
    step.error = error_;
    return step;
  }

  const WebmOpusPacketDecodeResult decoded = decoder_.decode_packet(
      packet_data, packet_length, pcm_out, pcm_capacity_samples);
  clear_slot(read_index_);
  read_index_ = (read_index_ + 1U) % kQueueSlotCount;
  --queued_count_;

  step.decoded = decoded.decoded;
  step.consumed_packet = decoded.consumed_packet;
  step.has_output = decoded.has_output;
  if (decoder_.has_failed()) {
    mark_failed(map_packet_error(decoder_.error()));
    step.failed = true;
    step.error = error_;
  }
  return step;
}

void WebmDirectOpusPacketPath::add_skip_samples_per_channel(
    int samples_per_channel) {
  decoder_.add_skip_samples_per_channel(samples_per_channel);
}

int WebmDirectOpusPacketPath::pending_skip_samples_per_channel() const {
  return decoder_.pending_skip_samples_per_channel();
}

void WebmDirectOpusPacketPath::mark_complete() { complete_ = true; }

WebmDirectPacketQueueSnapshot WebmDirectOpusPacketPath::snapshot() const {
  WebmDirectPacketQueueSnapshot snapshot = {};
  snapshot.queued_packets = active_count();
  snapshot.read_index = read_index_;
  snapshot.complete = complete_;
  snapshot.packet_slot_bytes = packet_storage_.empty() ? 0U : kPacketSlotBytes;
  snapshot.max_packet_bytes_seen = max_packet_bytes_seen_;
  return snapshot;
}

void WebmDirectOpusPacketPath::ensure_storage() {
  const size_t required_size = kQueueSlotCount * kPacketSlotBytes;
  if (packet_storage_.size() == required_size) {
    return;
  }
  // resize() has no recoverable false path here: allocation failure is handled
  // by the runtime before this function returns.
  packet_storage_.resize(required_size);
}

uint8_t *WebmDirectOpusPacketPath::slot_data(size_t slot_index) {
  if (slot_index >= kQueueSlotCount ||
      packet_storage_.size() < (kQueueSlotCount * kPacketSlotBytes)) {
    return NULL;
  }
  return packet_storage_.data() + (slot_index * kPacketSlotBytes);
}

const uint8_t *WebmDirectOpusPacketPath::slot_data(size_t slot_index) const {
  if (slot_index >= kQueueSlotCount ||
      packet_storage_.size() < (kQueueSlotCount * kPacketSlotBytes)) {
    return NULL;
  }
  return packet_storage_.data() + (slot_index * kPacketSlotBytes);
}

void WebmDirectOpusPacketPath::clear_slot(size_t slot_index) {
  if (slot_index >= kQueueSlotCount) {
    return;
  }
  slots_[slot_index].length = 0;
  slots_[slot_index].tstamp_ns = 0;
  slots_[slot_index].tstamp_ms = -1;
}

void WebmDirectOpusPacketPath::clear_slots() {
  for (size_t i = 0; i < kQueueSlotCount; ++i) {
    clear_slot(i);
  }
}

size_t WebmDirectOpusPacketPath::active_count() const { return queued_count_; }

bool WebmDirectOpusPacketPath::empty() const { return active_count() == 0U; }

void WebmDirectOpusPacketPath::mark_failed(WebmRemuxError error) {
  failed_ = true;
  error_ = error == WebmRemuxError::None ? WebmRemuxError::InvalidBlock : error;
}

WebmRemuxError
WebmDirectOpusPacketPath::map_packet_error(WebmOpusPacketDecodeError error) {
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
