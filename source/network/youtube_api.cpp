#include "../../include/network/youtube_api.h"
#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <sstream>
#include <memory>

extern std::unique_ptr<std::vector<uint8_t>> g_stream_buffer_ptr;
extern LightLock stream_lock;
bool YouTubeAPI::should_cancel = false;

static int progress_callback(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    return YouTubeAPI::should_cancel ? 1 : 0;
}

static size_t StreamingWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    if (!YouTubeAPI::should_cancel && g_stream_buffer_ptr) {
        uint8_t* data = (uint8_t*)contents;
        LightLock_Lock(&stream_lock);
        g_stream_buffer_ptr->insert(g_stream_buffer_ptr->end(), data, data + total_size);
        LightLock_Unlock(&stream_lock);
    }
    return total_size;
}

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

void YouTubeAPI::init() { curl_global_init(CURL_GLOBAL_DEFAULT); YouTubeAPI::should_cancel = false; }
void YouTubeAPI::cleanup() { curl_global_cleanup(); }

bool YouTubeAPI::start_streaming(const std::string& url) {
    CURL *curl = curl_easy_init();
    bool success = false;
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamingWriteCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); 
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "3DS-YT");
        
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK && !should_cancel) {
            success = true;
        }
        curl_easy_cleanup(curl);
    }
    return success;
}

std::string YouTubeAPI::http_get(const std::string& url, long timeout_sec) {
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); 
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_sec); // Connection timeout
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);        // Overall timeout
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            readBuffer = ""; // Return empty on error
        }
        curl_easy_cleanup(curl);
    }
    return readBuffer;
}

bool YouTubeAPI::check_connection() {
    std::string url = get_base_url() + "/api/logs";
    std::string res = http_get(url, 1L); // 1s timeout to avoid startup delay
    return !res.empty();
}

void YouTubeAPI::search(const std::string& query, const std::string& lang, SearchCallback callback) {
    std::string url = get_base_url() + "/search?q=" + query + "&lang=" + lang;
    std::string json = http_get(url, 15L); // 15s timeout for search
    if (!json.empty()) callback(parse_search_results(json), true);
    else callback({}, false);
}

void YouTubeAPI::get_audio_stream_url(const std::string& video_id, std::function<void(const std::string&, bool)> callback) {
    // Delegate heavy work to PC; 3DS just receives the MP3 stream
    std::string url = get_base_url() + "/stream?i=" + video_id;
    callback(url, true);
}
std::vector<Track> YouTubeAPI::parse_search_results(const std::string& data) {
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
            results.push_back({title, vid, duration, views, uploader, upload_date});
        }
    }
    return results;
}
// Stub function implementations
bool YouTubeAPI::download_thumbnail(const std::string&, std::vector<uint8_t>&) { return false; }
std::string YouTubeAPI::http_post(const std::string&, const std::string&, const std::string&) { return ""; }
std::string YouTubeAPI::parse_stream_url(const std::string&) { return ""; }