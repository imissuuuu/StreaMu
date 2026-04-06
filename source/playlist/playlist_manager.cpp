#include "playlist_manager.h"
#define JSON_NOEXCEPTION // Disable exceptions for 3DS
#include "../../include/json.hpp" // JSON library
#include <fstream>
#include <sys/stat.h>
#include <random>

using json = nlohmann::json;

PlaylistManager::PlaylistManager() : m_file_path("sdmc:/3ds/StreaMu/playlists.json") {}
PlaylistManager::~PlaylistManager() {}

void PlaylistManager::ensure_directory_exists() {
    mkdir("sdmc:/3ds/StreaMu", 0777); // Create dir if missing
}

#include <3ds.h>
#include <time.h>

std::string PlaylistManager::generate_id() const {
    static u64 counter = 0;
    u64 now = osGetTime();
    return "play_" + std::to_string(now) + "_" + std::to_string(counter++); 
}

void PlaylistManager::init() {
    ensure_directory_exists();
    m_playlists.clear();

    std::ifstream file(m_file_path);
    if (!file.is_open()) return; // First launch or file missing

    json j = json::parse(file, nullptr, false); // No-throw parse
    if (j.is_discarded()) return; // Parse failed, keep empty

    if (j.is_array()) {
        for (const auto& item : j) {
            Playlist pl;
            pl.id = item.value("id", "");
            pl.name = item.value("name", "Untitled");
            if (item.contains("tracks") && item["tracks"].is_array()) {
                for (const auto& track_item : item["tracks"]) {
                    Track t;
                    t.id = track_item.value("id", "");
                    t.title = track_item.value("title", "");
                    t.duration = track_item.value("duration", "??:??");
                    t.views = track_item.value("views", "0");
                    t.uploader = track_item.value("uploader", "Unknown");
                    t.upload_date = track_item.value("upload_date", "Unknown");
                    pl.tracks.push_back(t);
                }
            }
            m_playlists.push_back(pl);
        }
    }
}

void PlaylistManager::save() {
    ensure_directory_exists();
    json j = json::array();
    
    for (const auto& pl : m_playlists) {
        json pl_json;
        pl_json["id"] = pl.id;
        pl_json["name"] = pl.name;
        json tracks_json = json::array();
        for (const auto& t : pl.tracks) {
            json t_json;
            t_json["id"] = t.id;
            t_json["title"] = t.title;
            t_json["duration"] = t.duration;
            t_json["views"] = t.views;
            t_json["uploader"] = t.uploader;
            t_json["upload_date"] = t.upload_date;
            tracks_json.push_back(t_json);
        }
        pl_json["tracks"] = tracks_json;
        j.push_back(pl_json);
    }

    std::ofstream file(m_file_path);
    if (file.is_open()) {
        file << j.dump(4);
    }
}

const std::vector<Playlist>& PlaylistManager::get_playlists() const {
    return m_playlists;
}

bool PlaylistManager::create_playlist(const std::string& name) {
    Playlist pl;
    pl.id = generate_id();
    pl.name = name;
    m_playlists.push_back(pl);
    save();
    return true;
}

bool PlaylistManager::delete_playlist(const std::string& playlist_id) {
    for (auto it = m_playlists.begin(); it != m_playlists.end(); ++it) {
        if (it->id == playlist_id) {
            m_playlists.erase(it);
            save();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::rename_playlist(const std::string& playlist_id, const std::string& new_name) {
    for (auto& pl : m_playlists) {
        if (pl.id == playlist_id) {
            pl.name = new_name;
            save();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::add_track(const std::string& playlist_id, const Track& track) {
    for (auto& pl : m_playlists) {
        if (pl.id == playlist_id) {
            // Duplicate check
            for (const auto& t : pl.tracks) {
                if (t.id == track.id) return false; // Already exists
            }
            pl.tracks.push_back(track);
            save();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::remove_track(const std::string& playlist_id, const std::string& track_id) {
    for (auto& pl : m_playlists) {
        if (pl.id == playlist_id) {
            for (auto it = pl.tracks.begin(); it != pl.tracks.end(); ++it) {
                if (it->id == track_id) {
                    pl.tracks.erase(it);
                    save();
                    return true;
                }
            }
        }
    }
    return false;
}

bool PlaylistManager::rename_track(const std::string& playlist_id, const std::string& track_id, const std::string& new_title) {
    for (auto& pl : m_playlists) {
        if (pl.id == playlist_id) {
            for (auto& t : pl.tracks) {
                if (t.id == track_id) {
                    t.title = new_title;
                    save();
                    return true;
                }
            }
        }
    }
    return false;
}

bool PlaylistManager::is_track_in_playlist(const std::string& playlist_id, const std::string& track_id) const {
    for (const auto& pl : m_playlists) {
        if (pl.id == playlist_id) {
            for (const auto& t : pl.tracks) {
                if (t.id == track_id) return true;
            }
        }
    }
    return false;
}
