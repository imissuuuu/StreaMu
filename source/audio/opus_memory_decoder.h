#ifndef OPUS_MEMORY_DECODER_H
#define OPUS_MEMORY_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include <3ds.h>
#include <opusfile.h>
#include <vector>

typedef bool (*OpusStreamPumpCallback)(void *user_data);

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
  bool open_streaming(const std::vector<uint8_t> *buffer, LightLock *lock,
                      const bool *download_complete,
                      OpusStreamPumpCallback pump_callback = NULL,
                      void *pump_user_data = NULL);
  void reset();
  OpusDecodeResult decode(int16_t *pcm_out, size_t pcm_capacity_samples);
  bool is_open() const;
  bool is_eof() const;
  bool has_failed() const;

  struct StreamSource {
    const std::vector<uint8_t> *buffer;
    LightLock *lock;
    const bool *download_complete;
    size_t offset;
    OpusStreamPumpCallback pump_callback;
    void *pump_user_data;
  };

private:
  OggOpusFile *file_;
  StreamSource stream_source_;
  bool eof_;
  bool failed_;
};

#endif
