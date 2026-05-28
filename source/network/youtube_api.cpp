#include "../../include/network/youtube_api.h"
#include <3ds.h>
#include <curl/curl.h>
#include <memory>
#include <sstream>
#include <stdio.h>

extern std::unique_ptr<std::vector<uint8_t>> g_stream_buffer_ptr;
extern LightLock stream_lock;
extern bool g_stream_download_complete;
bool YouTubeAPI::should_cancel = false;

#if STREAMU_ENABLE_OPUS_PERF_LOG
static bool g_opus_perf_active = false;
static bool g_opus_perf_first_byte_logged = false;
static bool g_opus_perf_start_bytes_logged = false;
static size_t g_opus_perf_bytes = 0;
static u64 g_opus_perf_start_ms = 0;

static void append_opus_perf_log(const char *event, size_t bytes) {
  if (!event || !g_opus_perf_active) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/opus_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (g_opus_perf_start_ms > 0 && now_ms >= g_opus_perf_start_ms)
          ? now_ms - g_opus_perf_start_ms
          : 0;
  fprintf(f, "[opus-perf] +%llums %s bytes=%lu\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          static_cast<unsigned long>(bytes));
  fclose(f);
}
#endif

static int progress_callback(void *p, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
  return YouTubeAPI::should_cancel ? 1 : 0;
}

static size_t StreamingWriteCallback(void *contents, size_t size, size_t nmemb,
                                     void *userp) {
  size_t total_size = size * nmemb;
#if STREAMU_ENABLE_OPUS_PERF_LOG
  if (g_opus_perf_active && total_size > 0) {
    g_opus_perf_bytes += total_size;
    if (!g_opus_perf_first_byte_logged) {
      g_opus_perf_first_byte_logged = true;
      append_opus_perf_log("first-byte", g_opus_perf_bytes);
    }
    if (!g_opus_perf_start_bytes_logged && g_opus_perf_bytes >= 16U * 1024U) {
      g_opus_perf_start_bytes_logged = true;
      append_opus_perf_log("received-16kb", g_opus_perf_bytes);
    }
  }
#endif
  if (!YouTubeAPI::should_cancel && g_stream_buffer_ptr) {
    uint8_t *data = (uint8_t *)contents;
    LightLock_Lock(&stream_lock);
    g_stream_buffer_ptr->insert(g_stream_buffer_ptr->end(), data,
                                data + total_size);
    LightLock_Unlock(&stream_lock);
  }
  return total_size;
}

static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                            void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

void YouTubeAPI::init() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  YouTubeAPI::should_cancel = false;
}
void YouTubeAPI::cleanup() { curl_global_cleanup(); }

bool YouTubeAPI::start_streaming(const std::string &url) {
  CURL *curl = curl_easy_init();
  bool success = false;
  if (!curl) {
    LightLock_Lock(&stream_lock);
    g_stream_download_complete = true;
    LightLock_Unlock(&stream_lock);
    return false;
  }
  LightLock_Lock(&stream_lock);
  g_stream_download_complete = false;
  LightLock_Unlock(&stream_lock);

  const bool is_opus_ogg = url.find("/stream_opus_ogg?") != std::string::npos;
#if STREAMU_ENABLE_OPUS_PERF_LOG
  g_opus_perf_active = is_opus_ogg;
  g_opus_perf_first_byte_logged = false;
  g_opus_perf_start_bytes_logged = false;
  g_opus_perf_bytes = 0;
  g_opus_perf_start_ms = is_opus_ogg ? osGetTime() : 0;
  if (is_opus_ogg) {
    append_opus_perf_log("stream-request-start", 0);
  }
#else
  (void)is_opus_ogg;
#endif
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamingWriteCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "3DS-YT");
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);  // TCP connect timeout
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // 1 byte/s threshold
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, is_opus_ogg ? 180L : 60L);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK && !should_cancel) {
    success = true;
  }
  LightLock_Lock(&stream_lock);
  g_stream_download_complete = true;
  LightLock_Unlock(&stream_lock);
#if STREAMU_ENABLE_OPUS_PERF_LOG
  if (is_opus_ogg) {
    append_opus_perf_log(success ? "stream-complete" : "stream-failed",
                         g_opus_perf_bytes);
  }
  g_opus_perf_active = false;
#endif
  curl_easy_cleanup(curl);
  return success;
}

std::string YouTubeAPI::http_get(const std::string &url, long timeout_sec) {
  CURL *curl = curl_easy_init();
  std::string readBuffer;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     timeout_sec);                        // Connection timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec); // Overall timeout
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      readBuffer = ""; // Return empty on error
    }
    curl_easy_cleanup(curl);
  }
  return readBuffer;
}

std::string YouTubeAPI::http_get_ms(const std::string &url, long timeout_ms) {
  CURL *curl = curl_easy_init();
  std::string readBuffer;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      readBuffer = "";
    }
    curl_easy_cleanup(curl);
  }
  return readBuffer;
}

bool YouTubeAPI::check_connection(long timeout_ms) {
  std::string url = get_base_url() + "/healthz";
  std::string res = http_get_ms(url, timeout_ms);
  return !res.empty();
}

void YouTubeAPI::search(const std::string &query, const std::string &lang,
                        SearchCallback callback) {
  std::string url = get_base_url() + "/search?q=" + query + "&lang=" + lang;
  std::string json = http_get(url, 15L); // 15s timeout for search
  if (!json.empty())
    callback(parse_search_results(json), true);
  else
    callback({}, false);
}

void YouTubeAPI::get_audio_stream_url(const std::string &video_id,
                                      int seek_seconds,
                                      StreamCallback callback) {
  // Delegate WebM->Ogg remuxing to the proxy; 3DS decodes Opus directly.
  std::string url = get_base_url();
  url += "/stream_opus_ogg?i=" + video_id;
  if (seek_seconds > 0)
    url += "&t=" + std::to_string(seek_seconds);
  callback(url, true);
}
std::vector<Track> YouTubeAPI::parse_search_results(const std::string &data) {
  std::vector<Track> results;
  std::stringstream ss(data);
  std::string line;

  while (std::getline(ss, line) && results.size() < 10) {
    std::stringstream linestream(line);
    std::string vid, title, duration, views, uploader, upload_date;

    std::getline(linestream, vid, '\t');
    std::getline(linestream, title, '\t');
    std::getline(linestream, duration, '\t');
    std::getline(linestream, views, '\t');
    std::getline(linestream, uploader, '\t');
    std::getline(linestream, upload_date, '\t');

    if (!vid.empty()) {
      std::string thumb = get_base_url() + "/thumbnail?id=" + vid;
      results.push_back(
          {title, vid, duration, views, uploader, upload_date, thumb});
    }
  }
  return results;
}
// Stub function implementations
bool YouTubeAPI::download_thumbnail(const std::string &video_id,
                                    std::vector<uint8_t> &data) {
  std::string url = get_base_url() + "/thumbnail?id=" + video_id;
  std::string raw = http_get(url, 8L);
  if (raw.empty())
    return false;
  if (raw.size() > 512 * 1024)
    return false; // Reject unexpectedly large responses (>512KB)
  data.assign(raw.begin(), raw.end());
  return true;
}
