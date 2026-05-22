#ifndef VORBIS_STREAM_DECODER_H
#define VORBIS_STREAM_DECODER_H

#include "stream_decoder.h"

extern "C" {
#include <tremor/ivorbisfile.h>
}

class VorbisStreamDecoder : public StreamDecoder {
public:
  VorbisStreamDecoder();
  ~VorbisStreamDecoder() override;
  void reset() override;
  StreamDecodeResult decode(const uint8_t *data, size_t size, int16_t *pcm_out,
                            size_t pcm_capacity_samples) override;

private:
  struct MemoryStream {
    const uint8_t *data;
    size_t size;
    size_t offset;
  };

  static size_t read_memory(void *ptr, size_t size, size_t nmemb,
                            void *datasource);
  static int close_memory(void *datasource);
  bool open_if_needed(const uint8_t *data, size_t size);
  void close();

  OggVorbis_File file_;
  MemoryStream stream_;
  bool opened_;
};

#endif
