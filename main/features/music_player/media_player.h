#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <esp_ae_rate_cvt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "music_library.h"

class AudioService;

// SD-card music player. Decodes MP3/AAC/M4A/FLAC/WAV/OGG with esp_audio_codec,
// downmixes to mono, resamples to the codec output rate and feeds the regular
// audio playback queue so TTS and music never write the codec concurrently.
//
// Waking the assistant pauses music (auto-resume when the session ends).
// MCP "play" requests made during a turn are queued as pending and start once
// the device returns to Idle.
class MediaPlayer {
public:
    enum class State { kIdle, kLoading, kPlaying, kPaused, kError };
    enum class PauseReason { kNone, kUser, kWakeWord, kTts, kNotify, kError };

    static MediaPlayer& GetInstance();

    // Stores the audio service pointer only; the task, decoder registration and
    // timer are created lazily on first playback so boot-time internal RAM stays
    // untouched (BLE/TLS handshakes need the contiguous internal headroom).
    void SetAudioService(AudioService* audio);

    // UI entry points (immediate).
    bool PlayTrack(int index);
    void TogglePlayPause();
    void Next();
    void Prev();
    void Pause(PauseReason reason);
    void Resume();
    void Stop();

    // MCP entry points. Every method returns a JSON document string.
    std::string PlayRequest(const std::string& query, const std::string& artist,
                            const std::string& album, int track_id, bool shuffle);
    std::string PauseRequest();
    std::string ResumeRequest();
    std::string StopRequest();
    std::string NextRequest();
    std::string PrevRequest();
    std::string StatusRequest();

    void SetShuffle(bool shuffle);
    bool shuffle() const { return shuffle_.load(); }

    State state() const { return state_.load(); }
    bool IsPlaying() const { return state_.load() == State::kPlaying; }
    bool IsPaused() const { return state_.load() == State::kPaused; }
    bool CurrentTrack(MusicTrack* out) const;
    int current_index() const { return current_index_.load(); }
    uint32_t position_ms() const { return position_ms_.load(); }
    uint32_t duration_ms() const { return duration_ms_.load(); }

    // Called from the main task via the Application state listener.
    void OnDeviceStateChanged(int old_state, int new_state);

    // Music mode command window. While active, playback is paused (including
    // tracks started during the window) so the microphone can hear commands;
    // ending the window resumes only playback that the window itself paused.
    void SetCommandWindowActive(bool active);

    static const char* StateName(State state);

private:
    MediaPlayer() = default;

    struct Command {
        enum class Type { kNone, kPlay, kStop, kNext, kPrev } type = Type::kNone;
        int index = -1;
    };

    void EnsureStarted();
    void TaskLoop();
    static void ResumeTimerCb(void* arg);
    void HandleCommand(const Command& command);
    void RunSession(int index);
    void StartPlayback(int index, std::vector<int> queue, bool shuffle);
    void StartPending();
    bool HasPendingCommand() const;
    int NextIndexInQueue(int direction, bool wrap) const;

    bool DecodeStep();
    void OnDecodedPcm(uint8_t* data, size_t size);
    bool PushSamples(const int16_t* samples, size_t count);
    bool PushCarry();
    bool PushCarryChunk(std::vector<int16_t>&& chunk);
    void CloseDecoder();
    bool SetupDecoder(const MusicTrack& track);
    bool WaitForPlaybackDrain(int timeout_ms);

    std::string BuildTrackJson(bool ok, const char* state,
                               const MusicTrack* track) const;
    std::string BuildErrorJson(const char* error) const;
    std::string BuildAmbiguousJson(const std::vector<int>& candidates) const;

    AudioService* audio_source_ = nullptr;
    AudioService* audio_ = nullptr;
    std::mutex init_mutex_;
    TaskHandle_t task_handle_ = nullptr;
    esp_timer_handle_t resume_timer_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Command command_;
    bool stop_task_ = false;
    std::atomic<bool> session_active_{false};

    std::atomic<State> state_{State::kIdle};
    std::atomic<PauseReason> pause_reason_{PauseReason::kNone};
    std::atomic<bool> paused_{false};
    std::atomic<bool> auto_resume_{false};
    std::atomic<bool> pending_{false};
    std::atomic<bool> shuffle_{true};
    std::atomic<bool> command_window_{false};
    std::atomic<uint32_t> generation_{1};
    mutable std::mutex track_mutex_;
    MusicTrack current_track_;
    std::atomic<int> current_index_{-1};
    std::atomic<uint32_t> position_ms_{0};
    std::atomic<uint32_t> duration_ms_{0};

    int pending_index_ = -1;
    std::vector<int> pending_queue_;
    bool pending_shuffle_ = false;

    std::vector<int> queue_;
    mutable int queue_pos_ = -1;

    // Decoder state, owned by the media task.
    FILE* file_ = nullptr;
    void* decoder_ = nullptr;              // esp_audio_simple_dec_handle_t
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    int source_rate_ = 0;
    int source_channels_ = 0;
    int output_rate_ = 24000;
    std::vector<uint8_t> input_buf_;
    std::vector<uint8_t> output_buf_;
    std::vector<int16_t> mono_buf_;
    std::vector<int16_t> resample_buf_;
    std::vector<int16_t> carry_;
    uint32_t played_samples_ = 0;
    bool decoder_error_ = false;
};

#endif  // MEDIA_PLAYER_H
