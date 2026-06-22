#include "webm_direct_opus_packet_path.h"

#include <limits.h>
#include <utility>

WebmDirectOpusPacketPath::WebmDirectOpusPacketPath()
    : read_index_(0), complete_(false), failed_(false),
      error_(WebmRemuxError::None) {
  queue_.reserve(kQueueLimit);
}

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
  queue_.clear();
  queue_.reserve(kQueueLimit);
  read_index_ = 0;
  complete_ = false;
  failed_ = false;
  error_ = WebmRemuxError::None;
}

bool WebmDirectOpusPacketPath::is_open() const {
  return decoder_.is_open() && !failed_;
}

bool WebmDirectOpusPacketPath::has_failed() const { return failed_; }

WebmRemuxError WebmDirectOpusPacketPath::error() const { return error_; }

bool WebmDirectOpusPacketPath::is_eof() const {
  return complete_ && empty();
}

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

  Packet packet;
  packet.data.assign(data, data + length);
  packet.tstamp_ns = packet_tstamp_ns;
  packet.tstamp_ms = packet_tstamp_ms;
  if (!push_packet(&packet)) {
    mark_failed(WebmRemuxError::InvalidBlock);
    return false;
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

  Packet packet;
  if (!pop_packet(&packet)) {
    step.decoded.eof = complete_;
    return step;
  }
  if (packet.data.empty()) {
    mark_failed(WebmRemuxError::InvalidBlock);
    step.failed = true;
    step.error = error_;
    return step;
  }

  const WebmOpusPacketDecodeResult decoded = decoder_.decode_packet(
      packet.data.data(), packet.data.size(), pcm_out, pcm_capacity_samples);
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
  return snapshot;
}

bool WebmDirectOpusPacketPath::push_packet(Packet *packet) {
  if (!packet || packet->data.empty()) {
    return false;
  }
  // Queue fullness is enforced before reading the next nestegg packet. Push
  // still accepts chunks from an already-read packet so multi-chunk packets stay
  // intact.
  queue_.push_back(std::move(*packet));
  return true;
}

bool WebmDirectOpusPacketPath::pop_packet(Packet *out_packet) {
  if (!out_packet || empty()) {
    return false;
  }
  Packet &slot = queue_[read_index_];
  *out_packet = std::move(slot);
  slot.data.clear();
  slot.tstamp_ns = 0;
  slot.tstamp_ms = -1;
  ++read_index_;
  compact_if_needed();
  return true;
}

size_t WebmDirectOpusPacketPath::active_count() const {
  if (read_index_ >= queue_.size()) {
    return 0U;
  }
  return queue_.size() - read_index_;
}

bool WebmDirectOpusPacketPath::empty() const {
  return active_count() == 0U;
}

void WebmDirectOpusPacketPath::compact_if_needed() {
  if (read_index_ == 0U) {
    return;
  }
  const size_t current_active_count = active_count();
  if (current_active_count == 0U) {
    queue_.clear();
    read_index_ = 0;
    return;
  }
  if (read_index_ < kQueueLimit) {
    return;
  }
  for (size_t i = 0; i < current_active_count; ++i) {
    queue_[i] = std::move(queue_[read_index_ + i]);
  }
  queue_.resize(current_active_count);
  read_index_ = 0;
}

void WebmDirectOpusPacketPath::mark_failed(WebmRemuxError error) {
  failed_ = true;
  error_ = error == WebmRemuxError::None ? WebmRemuxError::InvalidBlock : error;
}

WebmRemuxError WebmDirectOpusPacketPath::map_packet_error(
    WebmOpusPacketDecodeError error) {
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
