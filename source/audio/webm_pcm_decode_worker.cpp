#include "webm_pcm_decode_worker.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

namespace {

static constexpr int kWebmWorkerQueueCapacity = 4;
static constexpr size_t kWebmPcmCapacitySamples = 8192;
static constexpr u64 kWebmWorkerQueueFullLogIntervalMs = 1000ULL;
static constexpr u64 kWebmWorkerDecodeSlowMs = 100ULL;

static size_t decoded_sample_count(const OpusDecodeResult &decoded) {
  if (!decoded.ok || decoded.samples_per_channel <= 0 ||
      decoded.channels <= 0) {
    return 0U;
  }
  return static_cast<size_t>(decoded.samples_per_channel) *
         static_cast<size_t>(decoded.channels);
}

static u64 elapsed_ms_since(u64 start_ms) {
  const u64 now_ms = osGetTime();
  return (start_ms > 0 && now_ms >= start_ms) ? (now_ms - start_ms) : 0;
}

} // namespace

WebmPcmDecodeWorker::WebmPcmDecodeWorker()
    : thread_(NULL), running_(false), stop_requested_(false), failed_(false),
      eof_(false), error_(WebmRemuxError::None), read_index_(0),
      write_index_(0), queued_chunks_(0), produced_chunks_(0),
      consumed_chunks_(0), queue_full_count_(0), last_decode_ticks_(0),
      last_queue_full_log_ms_(0), last_seek_runtime_start_byte_(0),
      last_seek_runtime_timecode_ms_(-1), decode_scratch_(NULL) {
  LightLock_Init(&lock_);
}

WebmPcmDecodeWorker::~WebmPcmDecodeWorker() {
  stop();
  release_buffers();
}

bool WebmPcmDecodeWorker::allocate_buffers() {
  if (!decode_scratch_) {
    decode_scratch_ = static_cast<int16_t *>(
        linearAlloc(kWebmPcmCapacitySamples * sizeof(int16_t)));
    if (!decode_scratch_) {
      return false;
    }
  }
  for (int i = 0; i < kWebmWorkerQueueCapacity; ++i) {
    if (!queue_[i].samples) {
      queue_[i].samples = static_cast<int16_t *>(
          linearAlloc(kWebmPcmCapacitySamples * sizeof(int16_t)));
      if (!queue_[i].samples) {
        return false;
      }
    }
    queue_[i].samples_per_channel = 0;
    queue_[i].sample_rate = 48000;
    queue_[i].channels = 2;
  }
  return true;
}

void WebmPcmDecodeWorker::release_buffers() {
  if (decode_scratch_) {
    linearFree(decode_scratch_);
    decode_scratch_ = NULL;
  }
  for (int i = 0; i < kWebmWorkerQueueCapacity; ++i) {
    if (queue_[i].samples) {
      linearFree(queue_[i].samples);
      queue_[i].samples = NULL;
    }
    queue_[i].samples_per_channel = 0;
    queue_[i].sample_rate = 48000;
    queue_[i].channels = 2;
  }
}

bool WebmPcmDecodeWorker::start(const WebmPcmDecodeWorkerConfig &config) {
  stop();
  if (!config.webm_buffer || !config.webm_lock ||
      !config.webm_download_complete) {
    return false;
  }
  if (!allocate_buffers()) {
    release_buffers();
    return false;
  }

  config_ = config;
  LightLock_Lock(&lock_);
  stop_requested_ = false;
  running_ = false;
  failed_ = false;
  eof_ = false;
  error_ = WebmRemuxError::None;
  clear_queue_locked();
  produced_chunks_ = 0;
  consumed_chunks_ = 0;
  queue_full_count_ = 0;
  last_decode_ticks_ = 0;
  last_queue_full_log_ms_ = 0;
  last_seek_runtime_start_byte_ = 0;
  last_seek_runtime_timecode_ms_ = -1;
  LightLock_Unlock(&lock_);

  if (!decoder_.open_streaming(
          config.webm_buffer, config.webm_lock, config.webm_download_complete,
          config.seek_context, config.emit_start_ms, config.enable_parser_seek,
          config.prefer_offset_seek, config.range_probe_base_url,
          config.range_filesize, config.parser_prefetch_offset)) {
    mark_failed(decoder_.remux_error());
    release_buffers();
    return false;
  }

  thread_ = threadCreate(thread_entry, this, 0x10000, 0x3F, -2, false);
  if (!thread_) {
    decoder_.reset();
    mark_failed(WebmRemuxError::InvalidBlock);
    release_buffers();
    return false;
  }

  LightLock_Lock(&lock_);
  running_ = true;
  LightLock_Unlock(&lock_);
  append_worker_log("worker_start", snapshot());
  return true;
}

void WebmPcmDecodeWorker::stop() {
  Thread thread = NULL;
  LightLock_Lock(&lock_);
  if (thread_) {
    stop_requested_ = true;
    thread = thread_;
  }
  LightLock_Unlock(&lock_);

  if (thread) {
    decoder_.request_cancel();
    threadJoin(thread, U64_MAX);
    threadFree(thread);
  }

  decoder_.reset();

  bool should_log_stop = false;
  WebmPcmWorkerSnapshot stop_snapshot = {};
  LightLock_Lock(&lock_);
  should_log_stop = thread_ != NULL || running_ || failed_ || eof_ ||
                    produced_chunks_ > 0 || consumed_chunks_ > 0;
  if (should_log_stop) {
    stop_snapshot.running = running_;
    stop_snapshot.failed = failed_;
    stop_snapshot.eof = eof_;
    stop_snapshot.error = error_;
    stop_snapshot.queued_chunks = queued_chunks_;
    stop_snapshot.produced_chunks = produced_chunks_;
    stop_snapshot.consumed_chunks = consumed_chunks_;
    stop_snapshot.queue_full_count = queue_full_count_;
    stop_snapshot.last_decode_ticks = last_decode_ticks_;
  }
  thread_ = NULL;
  running_ = false;
  stop_requested_ = false;
  failed_ = false;
  eof_ = false;
  error_ = WebmRemuxError::None;
  clear_queue_locked();
  produced_chunks_ = 0;
  consumed_chunks_ = 0;
  queue_full_count_ = 0;
  last_decode_ticks_ = 0;
  last_queue_full_log_ms_ = 0;
  last_seek_runtime_start_byte_ = 0;
  last_seek_runtime_timecode_ms_ = -1;
  LightLock_Unlock(&lock_);

  if (should_log_stop) {
    append_worker_log("worker_stop", stop_snapshot);
  }
}

bool WebmPcmDecodeWorker::pop_decoded_pcm(int16_t *pcm_out,
                                          size_t pcm_capacity_samples,
                                          OpusDecodeResult *out_result) {
  if (!pcm_out || !out_result || pcm_capacity_samples == 0U) {
    return false;
  }

  LightLock_Lock(&lock_);
  if (queued_chunks_ <= 0) {
    LightLock_Unlock(&lock_);
    return false;
  }

  PcmChunk &chunk = queue_[read_index_];
  const OpusDecodeResult decoded = {true, chunk.samples_per_channel,
                                    chunk.sample_rate, chunk.channels, false};
  const size_t sample_count = decoded_sample_count(decoded);
  if (sample_count == 0U || sample_count > pcm_capacity_samples) {
    failed_ = true;
    error_ = WebmRemuxError::InvalidBlock;
    LightLock_Unlock(&lock_);
    return false;
  }
  memcpy(pcm_out, chunk.samples, sample_count * sizeof(int16_t));
  chunk.samples_per_channel = 0;
  read_index_ = (read_index_ + 1) % kWebmWorkerQueueCapacity;
  --queued_chunks_;
  ++consumed_chunks_;
  *out_result = decoded;
  LightLock_Unlock(&lock_);
  return true;
}

bool WebmPcmDecodeWorker::is_running() const { return snapshot().running; }

bool WebmPcmDecodeWorker::is_eof() const {
  LightLock_Lock(&lock_);
  const bool value = eof_ && queued_chunks_ == 0;
  LightLock_Unlock(&lock_);
  return value;
}

bool WebmPcmDecodeWorker::has_failed() const { return snapshot().failed; }

WebmRemuxError WebmPcmDecodeWorker::remux_error() const {
  return snapshot().error;
}

WebmPcmWorkerSnapshot WebmPcmDecodeWorker::snapshot() const {
  WebmPcmWorkerSnapshot value = {};
  LightLock_Lock(&lock_);
  value.running = running_;
  value.failed = failed_;
  value.eof = eof_;
  value.error = error_;
  value.queued_chunks = queued_chunks_;
  value.produced_chunks = produced_chunks_;
  value.consumed_chunks = consumed_chunks_;
  value.queue_full_count = queue_full_count_;
  value.last_decode_ticks = last_decode_ticks_;
  LightLock_Unlock(&lock_);
  return value;
}

bool WebmPcmDecodeWorker::get_last_seek_runtime_point(
    uint64_t *out_start_byte, int *out_timecode_ms) const {
  if (!out_start_byte || !out_timecode_ms) {
    return false;
  }
  LightLock_Lock(&lock_);
  const bool has_point =
      last_seek_runtime_start_byte_ > 0U && last_seek_runtime_timecode_ms_ >= 0;
  if (has_point) {
    *out_start_byte = last_seek_runtime_start_byte_;
    *out_timecode_ms = last_seek_runtime_timecode_ms_;
  }
  LightLock_Unlock(&lock_);
  return has_point;
}

void WebmPcmDecodeWorker::thread_entry(void *arg) {
  WebmPcmDecodeWorker *worker = static_cast<WebmPcmDecodeWorker *>(arg);
  if (worker) {
    worker->run();
  }
}

void WebmPcmDecodeWorker::run() {
  while (true) {
    LightLock_Lock(&lock_);
    const bool should_stop = stop_requested_;
    const bool queue_full = queued_chunks_ >= kWebmWorkerQueueCapacity;
    if (queue_full) {
      ++queue_full_count_;
    }
    const u64 now_ms = osGetTime();
    const bool should_log_queue_full =
        queue_full &&
        (last_queue_full_log_ms_ == 0 ||
         now_ms >= last_queue_full_log_ms_ + kWebmWorkerQueueFullLogIntervalMs);
    if (should_log_queue_full) {
      last_queue_full_log_ms_ = now_ms;
    }
    LightLock_Unlock(&lock_);

    if (should_stop) {
      break;
    }
    if (queue_full) {
      if (should_log_queue_full) {
        append_worker_log("worker_queue_full", snapshot());
      }
      svcSleepThread(2 * 1000 * 1000);
      continue;
    }

    const u64 decode_start_ticks = svcGetSystemTick();
    const u64 decode_start_ms = osGetTime();
    OpusDecodeResult decoded =
        decoder_.decode(decode_scratch_, kWebmPcmCapacitySamples);
    const u64 decode_end_ticks = svcGetSystemTick();
    const u64 decode_ticks = decode_end_ticks >= decode_start_ticks
                                 ? decode_end_ticks - decode_start_ticks
                                 : 0;
    LightLock_Lock(&lock_);
    last_decode_ticks_ = decode_ticks;
    LightLock_Unlock(&lock_);

    const u64 decode_duration_ms =
        osGetTime() >= decode_start_ms ? osGetTime() - decode_start_ms : 0;
    if (decode_duration_ms >= kWebmWorkerDecodeSlowMs) {
      append_worker_log("worker_decode_slow", snapshot());
    }

    if (decoded.ok) {
      capture_seek_runtime_point();
      LightLock_Lock(&lock_);
      const bool pushed = push_chunk_locked(decoded, decode_scratch_);
      LightLock_Unlock(&lock_);
      if (!pushed) {
        mark_failed(WebmRemuxError::InvalidBlock);
        break;
      }
      continue;
    }
    if (decoded.eof) {
      mark_eof();
      break;
    }
    if (decoder_.has_failed()) {
      mark_failed(decoder_.remux_error());
      break;
    }
    svcSleepThread(2 * 1000 * 1000);
  }

  LightLock_Lock(&lock_);
  running_ = false;
  LightLock_Unlock(&lock_);
}

bool WebmPcmDecodeWorker::push_chunk_locked(const OpusDecodeResult &decoded,
                                            const int16_t *samples) {
  if (!samples || queued_chunks_ >= kWebmWorkerQueueCapacity) {
    return false;
  }
  const size_t sample_count = decoded_sample_count(decoded);
  if (sample_count == 0U || sample_count > kWebmPcmCapacitySamples) {
    return false;
  }

  PcmChunk &chunk = queue_[write_index_];
  if (!chunk.samples) {
    return false;
  }
  memcpy(chunk.samples, samples, sample_count * sizeof(int16_t));
  chunk.samples_per_channel = decoded.samples_per_channel;
  chunk.sample_rate = decoded.sample_rate;
  chunk.channels = decoded.channels;
  write_index_ = (write_index_ + 1) % kWebmWorkerQueueCapacity;
  ++queued_chunks_;
  ++produced_chunks_;
  return true;
}

void WebmPcmDecodeWorker::clear_queue_locked() {
  read_index_ = 0;
  write_index_ = 0;
  queued_chunks_ = 0;
  for (int i = 0; i < kWebmWorkerQueueCapacity; ++i) {
    queue_[i].samples_per_channel = 0;
    queue_[i].sample_rate = 48000;
    queue_[i].channels = 2;
  }
}

void WebmPcmDecodeWorker::append_worker_log(
    const char *event, const WebmPcmWorkerSnapshot &state) const {
  if (!event) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  fprintf(
      f,
      "[webm-worker] +%llums %s queued=%d produced=%d consumed=%d "
      "full=%d running=%d failed=%d eof=%d error=%d decode_ticks=%llu\n",
      static_cast<unsigned long long>(elapsed_ms_since(config_.perf_start_ms)),
      event, state.queued_chunks, state.produced_chunks, state.consumed_chunks,
      state.queue_full_count, state.running ? 1 : 0, state.failed ? 1 : 0,
      state.eof ? 1 : 0, static_cast<int>(state.error),
      static_cast<unsigned long long>(state.last_decode_ticks));
  fclose(f);
}

void WebmPcmDecodeWorker::mark_failed(WebmRemuxError error) {
  LightLock_Lock(&lock_);
  failed_ = true;
  eof_ = false;
  running_ = false;
  error_ = error;
  LightLock_Unlock(&lock_);
  append_worker_log("worker_failed", snapshot());
}

void WebmPcmDecodeWorker::mark_eof() {
  LightLock_Lock(&lock_);
  eof_ = true;
  running_ = false;
  LightLock_Unlock(&lock_);
  append_worker_log("worker_eof", snapshot());
}

void WebmPcmDecodeWorker::capture_seek_runtime_point() {
  uint64_t start_byte = 0;
  int timecode_ms = -1;
  if (!decoder_.get_last_seek_runtime_point(&start_byte, &timecode_ms)) {
    return;
  }
  LightLock_Lock(&lock_);
  last_seek_runtime_start_byte_ = start_byte;
  last_seek_runtime_timecode_ms_ = timecode_ms;
  LightLock_Unlock(&lock_);
}
