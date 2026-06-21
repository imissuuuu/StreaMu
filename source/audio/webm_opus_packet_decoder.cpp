#include "webm_opus_packet_decoder.h"

#include <limits.h>
#include <opus.h>
#include <string.h>

namespace {

static constexpr int kOpusDecodeSampleRate = 48000;
static constexpr size_t kOpusHeadMinBytes = 19U;

static int read_u16_le(const uint8_t *data) {
  return static_cast<int>(data[0]) | (static_cast<int>(data[1]) << 8);
}

static int read_i16_le(const uint8_t *data) {
  const uint16_t value =
      static_cast<uint16_t>(data[0]) |
      static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
  return static_cast<int>(static_cast<int16_t>(value));
}

static int read_u32_le_as_int(const uint8_t *data) {
  const uint32_t value =
      static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) |
      (static_cast<uint32_t>(data[3]) << 24);
  return value > static_cast<uint32_t>(INT_MAX)
             ? INT_MAX
             : static_cast<int>(value);
}

} // namespace

WebmOpusPacketDecoder::WebmOpusPacketDecoder()
    : decoder_(NULL), error_(WebmOpusPacketDecodeError::None),
      pending_skip_samples_per_channel_(0), failed_(false) {}

WebmOpusPacketDecoder::~WebmOpusPacketDecoder() { reset(); }

bool WebmOpusPacketDecoder::open(const uint8_t *codec_private,
                                 size_t codec_private_size) {
  reset();

  WebmOpusHeadInfo info = {};
  if (!parse_opus_head(codec_private, codec_private_size, &info)) {
    return false;
  }

  int opus_error = OPUS_OK;
  decoder_ = opus_decoder_create(kOpusDecodeSampleRate, info.channels,
                                 &opus_error);
  if (!decoder_ || opus_error != OPUS_OK) {
    mark_failed(WebmOpusPacketDecodeError::DecoderCreateFailed);
    return false;
  }

  head_ = info;
  pending_skip_samples_per_channel_ = head_.pre_skip_samples_per_channel;
  failed_ = false;
  error_ = WebmOpusPacketDecodeError::None;
  return true;
}

void WebmOpusPacketDecoder::reset() {
  if (decoder_) {
    opus_decoder_destroy(decoder_);
    decoder_ = NULL;
  }
  head_ = WebmOpusHeadInfo{};
  error_ = WebmOpusPacketDecodeError::None;
  pending_skip_samples_per_channel_ = 0;
  failed_ = false;
}

bool WebmOpusPacketDecoder::is_open() const { return decoder_ != NULL; }

bool WebmOpusPacketDecoder::has_failed() const { return failed_; }

WebmOpusPacketDecodeError WebmOpusPacketDecoder::error() const {
  return error_;
}

const WebmOpusHeadInfo &WebmOpusPacketDecoder::head() const { return head_; }

int WebmOpusPacketDecoder::channels() const { return head_.channels; }

int WebmOpusPacketDecoder::pending_skip_samples_per_channel() const {
  return pending_skip_samples_per_channel_;
}

void WebmOpusPacketDecoder::add_skip_samples_per_channel(
    int samples_per_channel) {
  if (samples_per_channel <= 0) {
    return;
  }
  if (pending_skip_samples_per_channel_ >
      INT_MAX - samples_per_channel) {
    pending_skip_samples_per_channel_ = INT_MAX;
    return;
  }
  pending_skip_samples_per_channel_ += samples_per_channel;
}

WebmOpusPacketDecodeResult WebmOpusPacketDecoder::decode_packet(
    const uint8_t *packet, size_t packet_size, int16_t *pcm_out,
    size_t pcm_capacity_samples) {
  WebmOpusPacketDecodeResult result = {};
  if (!decoder_ || failed_ || !packet || packet_size == 0U || !pcm_out ||
      pcm_capacity_samples == 0U || packet_size > static_cast<size_t>(INT_MAX)) {
    return result;
  }

  const int samples_per_channel = opus_packet_get_nb_samples(
      packet, static_cast<opus_int32>(packet_size), kOpusDecodeSampleRate);
  if (samples_per_channel <= 0) {
    mark_failed(WebmOpusPacketDecodeError::DecodeFailed);
    return result;
  }

  const size_t required_samples =
      static_cast<size_t>(samples_per_channel) *
      static_cast<size_t>(head_.channels);
  if (required_samples > pcm_capacity_samples) {
    mark_failed(WebmOpusPacketDecodeError::PacketTooLarge);
    return result;
  }

  const int decoded =
      opus_decode(decoder_, packet, static_cast<opus_int32>(packet_size),
                  pcm_out, samples_per_channel, 0);
  if (decoded < 0) {
    mark_failed(WebmOpusPacketDecodeError::DecodeFailed);
    return result;
  }
  if (decoded == 0) {
    result.consumed_packet = true;
    return result;
  }

  result = apply_pending_skip(pcm_out, decoded);
  result.consumed_packet = true;
  return result;
}

bool WebmOpusPacketDecoder::parse_opus_head(
    const uint8_t *codec_private, size_t codec_private_size,
    WebmOpusHeadInfo *out_info) {
  if (!codec_private || codec_private_size < kOpusHeadMinBytes || !out_info) {
    mark_failed(WebmOpusPacketDecodeError::InvalidCodecPrivate);
    return false;
  }
  if (memcmp(codec_private, "OpusHead", 8U) != 0) {
    mark_failed(WebmOpusPacketDecodeError::InvalidCodecPrivate);
    return false;
  }
  const uint8_t version = codec_private[8];
  if ((version & 0xF0U) != 0U) {
    mark_failed(WebmOpusPacketDecodeError::InvalidCodecPrivate);
    return false;
  }

  WebmOpusHeadInfo info = {};
  info.channels = static_cast<int>(codec_private[9]);
  if (info.channels < 1 || info.channels > 2) {
    mark_failed(WebmOpusPacketDecodeError::UnsupportedChannels);
    return false;
  }

  info.pre_skip_samples_per_channel = read_u16_le(codec_private + 10);
  info.input_sample_rate = read_u32_le_as_int(codec_private + 12);
  if (info.input_sample_rate <= 0) {
    info.input_sample_rate = kOpusDecodeSampleRate;
  }
  info.output_gain_q8 = read_i16_le(codec_private + 16);
  info.mapping_family = static_cast<int>(codec_private[18]);
  if (info.mapping_family != 0) {
    mark_failed(WebmOpusPacketDecodeError::UnsupportedMappingFamily);
    return false;
  }

  *out_info = info;
  return true;
}

WebmOpusPacketDecodeResult WebmOpusPacketDecoder::apply_pending_skip(
    int16_t *pcm_out, int samples_per_channel) {
  WebmOpusPacketDecodeResult result = {};
  result.consumed_packet = true;

  if (!pcm_out || samples_per_channel <= 0 || head_.channels <= 0) {
    return result;
  }

  int output_samples_per_channel = samples_per_channel;
  result.decoded.ok = true;
  result.decoded.samples_per_channel = samples_per_channel;
  result.decoded.sample_rate = kOpusDecodeSampleRate;
  result.decoded.channels = head_.channels;
  result.decoded.eof = false;

  if (pending_skip_samples_per_channel_ > 0) {
    const int skip_samples =
        output_samples_per_channel < pending_skip_samples_per_channel_
            ? output_samples_per_channel
            : pending_skip_samples_per_channel_;
    pending_skip_samples_per_channel_ -= skip_samples;
    output_samples_per_channel -= skip_samples;
    if (output_samples_per_channel <= 0) {
      return result;
    }
    memmove(pcm_out, pcm_out + (skip_samples * head_.channels),
            static_cast<size_t>(output_samples_per_channel) *
                static_cast<size_t>(head_.channels) * sizeof(int16_t));
  }

  result.decoded.samples_per_channel = output_samples_per_channel;
  result.has_output = true;
  return result;
}

void WebmOpusPacketDecoder::mark_failed(WebmOpusPacketDecodeError error) {
  failed_ = true;
  error_ = error;
}
