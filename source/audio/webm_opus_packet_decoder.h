#ifndef WEBM_OPUS_PACKET_DECODER_H
#define WEBM_OPUS_PACKET_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "opus_memory_decoder.h"

struct OpusDecoder;

enum class WebmOpusPacketDecodeError {
  None,
  InvalidCodecPrivate,
  UnsupportedChannels,
  UnsupportedMappingFamily,
  DecoderCreateFailed,
  PacketTooLarge,
  DecodeFailed,
};

struct WebmOpusHeadInfo {
  int channels = 0;
  int pre_skip_samples_per_channel = 0;
  int input_sample_rate = 48000;
  int output_gain_q8 = 0;
  int mapping_family = 0;
};

struct WebmOpusPacketDecodeResult {
  OpusDecodeResult decoded = {false, 0, 48000, 0, false};
  bool consumed_packet = false;
  bool has_output = false;
};

class WebmOpusPacketDecoder {
public:
  WebmOpusPacketDecoder();
  ~WebmOpusPacketDecoder();

  bool open(const uint8_t *codec_private, size_t codec_private_size);
  void reset();
  bool is_open() const;
  bool has_failed() const;
  WebmOpusPacketDecodeError error() const;
  const WebmOpusHeadInfo &head() const;
  int channels() const;
  int pending_skip_samples_per_channel() const;
  void add_skip_samples_per_channel(int samples_per_channel);
  WebmOpusPacketDecodeResult decode_packet(const uint8_t *packet,
                                           size_t packet_size,
                                           int16_t *pcm_out,
                                           size_t pcm_capacity_samples);

private:
  bool parse_opus_head(const uint8_t *codec_private,
                       size_t codec_private_size,
                       WebmOpusHeadInfo *out_info);
  WebmOpusPacketDecodeResult apply_pending_skip(int16_t *pcm_out,
                                                int samples_per_channel);
  void mark_failed(WebmOpusPacketDecodeError error);

  OpusDecoder *decoder_;
  WebmOpusHeadInfo head_;
  WebmOpusPacketDecodeError error_;
  int pending_skip_samples_per_channel_;
  bool failed_;
};

#endif
