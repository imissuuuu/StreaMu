#ifndef OPUS_MEMORY_DECODER_H
#define OPUS_MEMORY_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include <opusfile.h>

struct OpusDecodeResult {
  bool ok;
  int samples_per_channel;
  int sample_rate;
  int channels;
  bool eof;
};

class OpusMemoryDecoder {
public:
  OpusMemoryDecoder();
  ~OpusMemoryDecoder();

  bool open(const uint8_t *data, size_t size);
  void reset();
  OpusDecodeResult decode(int16_t *pcm_out, size_t pcm_capacity_samples);
  bool is_open() const;
  bool is_eof() const;
  bool has_failed() const;

private:
  OggOpusFile *file_;
  bool eof_;
  bool failed_;
};

#endif
