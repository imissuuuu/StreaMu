#pragma once
#include <vector>
#include <string>
#include "network/youtube_api.h" // Uses Track struct
#include <3ds.h>

struct Playlist {
    std::string id;
    std::string name;
    std::vector<Track> tracks;
};

class PlaylistManager {
public:
    PlaylistManager();
    ~PlaylistManager();

    // Init (load from SD card)
    void init();

    // Save in-memory playlists to file
    void save();

    // Get all playlists
    const std::vector<Playlist>& get_playlists() const;

    // Playlist operations
    bool create_playlist(const std::string& name);
    bool delete_playlist(const std::string& playlist_id);
    bool rename_playlist(const std::string& playlist_id, const std::string& new_name);

    // Track operations
    bool add_track(const std::string& playlist_id, const Track& track);
    bool remove_track(const std::string& playlist_id, const std::string& track_id);
    bool rename_track(const std::string& playlist_id, const std::string& track_id, const std::string& new_title);

    // Utility
    bool is_track_in_playlist(const std::string& playlist_id, const std::string& track_id) const;

private:
    std::vector<Playlist> m_playlists;
    std::string m_file_path;
    
    // Internal path and directory creation
    void ensure_directory_exists();
    std::string generate_id() const;
};
