#ifndef MUSIC_LIBRARY_H
#define MUSIC_LIBRARY_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct MusicTrack {
    uint32_t id = 0;  // stable hash of the absolute path
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string extension;
    uint32_t duration_ms = 0;
    uint32_t file_size = 0;
};

// Flat music library backed by the SD card (/sdcard/music). The first access
// scans the tree, reads ID3/Vorbis/FLAC tags and stores a small index cache so
// the next boot is fast. All public methods are thread-safe.
class MusicLibrary {
public:
    static constexpr const char* kCardRootPath = "/sdcard";
    static constexpr const char* kRootPath = "/sdcard/music";
    static constexpr const char* kCachePath = "/sdcard/.music_index";
    static constexpr const char* kRecentPath = "/sdcard/.music_recent";
    static constexpr int kMaxTracks = 1000;
    static constexpr int kMaxRecent = 20;

    static MusicLibrary& GetInstance();

    // Blocking load. Returns false when the SD card is not mounted or empty.
    bool EnsureLoaded(bool force = false);
    // Rebuild the index from the SD card, ignoring the cached index.
    bool Rescan();
    // Non-blocking: spawns one background scan if needed.
    void StartBackgroundScan();
    // True when the SD tree no longer matches the indexed signature (new or
    // removed files anywhere under /sdcard/music).
    bool TreeChanged() const;
    bool IsReady() const { return ready_.load(); }
    bool IsScanning() const { return scanning_.load(); }
    void MarkDirty();

    int Count() const;
    bool TrackAt(int index, MusicTrack* out) const;
    std::vector<MusicTrack> Snapshot() const;
    int IndexOfId(uint32_t id) const;

    std::vector<std::string> Artists() const;
    std::vector<std::string> Albums() const;
    std::vector<int> IndicesByArtist(const std::string& artist) const;
    std::vector<int> IndicesByAlbum(const std::string& album) const;
    std::vector<int> AllIndices() const;
    std::vector<int> RecentIndices() const;
    void NotePlayed(int index);

    // Searches title/artist/album. Empty query with artist/album filters
    // returns every track in the requested scope (ordered by title).
    std::vector<int> Search(const std::string& query, const std::string& artist,
                            const std::string& album, int max_results = 20) const;

    static std::string DisplayTitle(const MusicTrack& track);
    static std::string DisplayArtist(const MusicTrack& track);
    static std::string DisplayAlbum(const MusicTrack& track);
    static std::string FormatDuration(uint32_t duration_ms);

    // Human-readable summary used by the MCP status tool.
    std::string StatusJson() const;

private:
    MusicLibrary() = default;

    bool LoadCacheLocked(uint32_t signature);
    void SaveCacheLocked(uint32_t signature) const;
    void ScanLocked(std::vector<MusicTrack>& out);
    uint32_t ComputeTreeSignature(int* file_count = nullptr) const;
    void LoadRecentLocked();
    void SaveRecentLocked() const;

    mutable std::mutex mutex_;
    std::vector<MusicTrack> tracks_;
    std::vector<uint32_t> recent_ids_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<bool> scan_task_pending_{false};
    std::atomic<bool> dirty_{true};
    std::atomic<uint32_t> signature_{0};
};

#endif  // MUSIC_LIBRARY_H
