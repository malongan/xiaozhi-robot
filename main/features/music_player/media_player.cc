#include "media_player.h"

#include <cJSON.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/idf_additions.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <thread>

#include "application.h"
#include "audio_service.h"
#include "board.h"

#define TAG "MediaPlayer"

namespace {

constexpr int kChunkMs = 60;
constexpr int kCarryLimit = 4096;

std::string LowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string TrimText(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        start++;
    }
    return value.substr(start);
}

esp_audio_simple_dec_type_t DecoderTypeForExtension(const std::string& extension) {
    if (extension == "mp3") {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (extension == "m4a") {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    if (extension == "aac") {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (extension == "flac") {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (extension == "wav") {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (extension == "ogg" || extension == "opus") {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

}  // namespace

MediaPlayer& MediaPlayer::GetInstance() {
    static MediaPlayer instance;
    return instance;
}

const char* MediaPlayer::StateName(State state) {
    switch (state) {
        case State::kIdle:
            return "idle";
        case State::kLoading:
            return "loading";
        case State::kPlaying:
            return "playing";
        case State::kPaused:
            return "paused";
        case State::kError:
            return "error";
    }
    return "idle";
}

void MediaPlayer::SetAudioService(AudioService* audio) {
    audio_source_ = audio;
}

void MediaPlayer::EnsureStarted() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (task_handle_ != nullptr || audio_source_ == nullptr) {
        return;
    }
    audio_ = audio_source_;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr && codec->output_sample_rate() > 0) {
        output_rate_ = codec->output_sample_rate();
    }

    esp_timer_create_args_t timer_args = {
        .callback = &MediaPlayer::ResumeTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "music_resume",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &resume_timer_);

    BaseType_t created = xTaskCreateWithCaps(
        [](void* arg) {
            static_cast<MediaPlayer*>(arg)->TaskLoop();
            vTaskDeleteWithCaps(nullptr);
        },
        "music_player", 10240, this, 4, &task_handle_, MALLOC_CAP_SPIRAM);
    if (created != pdPASS) {
        // Boards without PSRAM fall back to internal RAM.
        created = xTaskCreateWithCaps(
            [](void* arg) {
                static_cast<MediaPlayer*>(arg)->TaskLoop();
                vTaskDeleteWithCaps(nullptr);
            },
            "music_player", 10240, this, 4, &task_handle_,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        esp_timer_delete(resume_timer_);
        resume_timer_ = nullptr;
        audio_ = nullptr;
        ESP_LOGE(TAG, "Cannot create media task");
        return;
    }
    ESP_LOGI(TAG, "Media player started (output rate %d)", output_rate_);
}

void MediaPlayer::TaskLoop() {
    while (true) {
        Command command;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_task_ || command_.type != Command::Type::kNone;
            });
            if (stop_task_) {
                break;
            }
            command = command_;
            command_ = Command{};
        }
        HandleCommand(command);
    }
}

void MediaPlayer::HandleCommand(const Command& command) {
    switch (command.type) {
        case Command::Type::kPlay:
            RunSession(command.index);
            break;
        case Command::Type::kStop:
            CloseDecoder();
            session_active_.store(false);
            state_.store(State::kIdle);
            position_ms_.store(0);
            break;
        case Command::Type::kNext:
        case Command::Type::kPrev: {
            const int direction = command.type == Command::Type::kNext ? 1 : -1;
            int current = current_index_.load();
            int next = current < 0 ? -1 : NextIndexInQueue(direction, false);
            if (next < 0) {
                if (current < 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!queue_.empty()) {
                        next = queue_.front();
                        queue_pos_ = 0;
                    }
                }
                if (next < 0) {
                    ESP_LOGW(TAG, "No next track");
                    if (state_.load() == State::kPlaying) {
                        state_.store(State::kIdle);
                    }
                    return;
                }
            }
            RunSession(next);
            break;
        }
        case Command::Type::kNone:
            break;
    }
}

void MediaPlayer::RunSession(int index) {
    if (audio_ == nullptr) {
        state_.store(State::kError);
        return;
    }
    MusicLibrary& library = MusicLibrary::GetInstance();
    MusicTrack track;
    if (!library.TrackAt(index, &track)) {
        state_.store(State::kError);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(track_mutex_);
        current_track_ = track;
    }
    current_index_.store(index);
    duration_ms_.store(track.duration_ms);
    position_ms_.store(0);
    played_samples_ = 0;
    if (command_window_.load()) {
        // Started while the voice-command window is open: keep it paused so
        // the microphone only hears the command, then resume on window end.
        paused_.store(true);
        auto_resume_.store(true);
        pause_reason_.store(PauseReason::kWakeWord);
    } else {
        paused_.store(false);
        auto_resume_.store(false);
        pause_reason_.store(PauseReason::kNone);
    }

    uint32_t generation = ++generation_;
    audio_->SetMediaGeneration(generation);
    if (!SetupDecoder(track)) {
        ESP_LOGE(TAG, "Cannot open %s", track.path.c_str());
        state_.store(State::kError);
        CloseDecoder();
        return;
    }
    state_.store(State::kPlaying);
    session_active_.store(true);
    library.NotePlayed(index);
    ESP_LOGI(TAG, "Playing %s (%s)", track.title.c_str(), track.path.c_str());

    while (true) {
        if (stop_task_ || HasPendingCommand()) {
            break;
        }
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        const bool more = DecodeStep();
        if (decoder_error_) {
            ESP_LOGE(TAG, "Decode error, stopping");
            state_.store(State::kError);
            break;
        }
        if (more) {
            continue;
        }

        // End of file: wait for the last queued chunks to reach the speaker,
        // then continue with the next track (no repeat in v1).
        PushCarry();
        WaitForPlaybackDrain(3000);
        if (stop_task_ || HasPendingCommand()) {
            break;
        }
        if (paused_.load()) {
            continue;
        }
        const int next = NextIndexInQueue(1, false);
        if (next < 0) {
            state_.store(State::kIdle);
            position_ms_.store(duration_ms_.load());
            break;
        }
        MusicTrack next_track;
        if (!library.TrackAt(next, &next_track)) {
            state_.store(State::kIdle);
            break;
        }
        {
            std::lock_guard<std::mutex> lock(track_mutex_);
            current_track_ = next_track;
        }
        current_index_.store(next);
        duration_ms_.store(next_track.duration_ms);
        position_ms_.store(0);
        played_samples_ = 0;
        generation = ++generation_;
        audio_->SetMediaGeneration(generation);
        CloseDecoder();
        if (!SetupDecoder(next_track)) {
            state_.store(State::kError);
            break;
        }
        library.NotePlayed(next);
    }

    CloseDecoder();
    session_active_.store(false);
}

bool MediaPlayer::SetupDecoder(const MusicTrack& track) {
    decoder_error_ = false;
    file_ = fopen(track.path.c_str(), "rb");
    if (file_ == nullptr) {
        return false;
    }
    const esp_audio_simple_dec_type_t type = DecoderTypeForExtension(LowerAscii(track.extension));
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        return false;
    }
    esp_audio_simple_dec_cfg_t config = {};
    config.dec_type = type;
    config.use_frame_dec = false;
    esp_audio_simple_dec_handle_t handle = nullptr;
    auto ret = esp_audio_simple_dec_open(&config, &handle);
    if (ret != ESP_AUDIO_ERR_OK || handle == nullptr) {
        ESP_LOGE(TAG, "Simple decoder open failed: %d", ret);
        return false;
    }
    decoder_ = handle;
    input_buf_.resize(2048);
    output_buf_.resize(8192);
    carry_.clear();
    source_rate_ = 0;
    source_channels_ = 0;
    return true;
}

void MediaPlayer::CloseDecoder() {
    if (decoder_ != nullptr) {
        esp_audio_simple_dec_close(static_cast<esp_audio_simple_dec_handle_t>(decoder_));
        decoder_ = nullptr;
    }
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
    if (file_ != nullptr) {
        fclose(file_);
        file_ = nullptr;
    }
    source_rate_ = 0;
    source_channels_ = 0;
}

bool MediaPlayer::DecodeStep() {
    if (file_ == nullptr || decoder_ == nullptr) {
        return false;
    }
    auto* handle = static_cast<esp_audio_simple_dec_handle_t>(decoder_);
    const size_t read = fread(input_buf_.data(), 1, input_buf_.size(), file_);
    esp_audio_simple_dec_raw_t raw = {
        .buffer = input_buf_.data(),
        .len = static_cast<uint32_t>(read),
        .eos = read == 0,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };

    int guard = 0;
    while (raw.len > 0 && guard++ < 64) {
        esp_audio_simple_dec_out_t out = {
            .buffer = output_buf_.data(),
            .len = static_cast<uint32_t>(output_buf_.size()),
            .needed_size = 0,
            .decoded_size = 0,
        };
        auto ret = esp_audio_simple_dec_process(handle, &raw, &out);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            output_buf_.resize(out.needed_size + 1024);
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Decode failed: %d", ret);
            decoder_error_ = true;
            return false;
        }
        if (out.decoded_size > 0) {
            OnDecodedPcm(out.buffer, out.decoded_size);
            if (decoder_error_) {
                return false;
            }
        }
        if (raw.consumed == 0) {
            break;
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }

    if (read == 0) {
        // Flush the decoder (FLAC and friends buffer data until EOS).
        for (int i = 0; i < 8; ++i) {
            esp_audio_simple_dec_out_t out = {
                .buffer = output_buf_.data(),
                .len = static_cast<uint32_t>(output_buf_.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_simple_dec_raw_t flush = {
                .buffer = input_buf_.data(),
                .len = 0,
                .eos = true,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            auto ret = esp_audio_simple_dec_process(handle, &flush, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                output_buf_.resize(out.needed_size + 1024);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK || out.decoded_size == 0) {
                break;
            }
            OnDecodedPcm(out.buffer, out.decoded_size);
            if (decoder_error_) {
                break;
            }
        }
        return false;
    }
    return true;
}

void MediaPlayer::OnDecodedPcm(uint8_t* data, size_t size) {
    if (decoder_ == nullptr) {
        return;
    }
    esp_audio_simple_dec_info_t info = {};
    if (esp_audio_simple_dec_get_info(static_cast<esp_audio_simple_dec_handle_t>(decoder_), &info) !=
        ESP_AUDIO_ERR_OK) {
        return;
    }
    if (info.bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported bits per sample: %d", info.bits_per_sample);
        decoder_error_ = true;
        return;
    }
    const int channels = info.channel == 0 ? 1 : info.channel;
    if (channels <= 0 || size < sizeof(int16_t) * channels) {
        return;
    }
    const size_t frames = size / (sizeof(int16_t) * channels);
    if (frames == 0) {
        return;
    }
    const int16_t* source = reinterpret_cast<const int16_t*>(data);
    mono_buf_.resize(frames);
    if (channels == 1) {
        memcpy(mono_buf_.data(), source, frames * sizeof(int16_t));
    } else {
        for (size_t i = 0; i < frames; ++i) {
            int32_t sum = 0;
            for (int c = 0; c < channels; ++c) {
                sum += source[i * channels + c];
            }
            mono_buf_[i] = static_cast<int16_t>(sum / channels);
        }
    }

    if (source_rate_ != static_cast<int>(info.sample_rate) ||
        source_channels_ != channels) {
        source_rate_ = static_cast<int>(info.sample_rate);
        source_channels_ = channels;
        if (resampler_ != nullptr) {
            esp_ae_rate_cvt_close(resampler_);
            resampler_ = nullptr;
        }
        if (source_rate_ != output_rate_) {
            esp_ae_rate_cvt_cfg_t config = {};
            config.src_rate = static_cast<uint32_t>(source_rate_);
            config.dest_rate = static_cast<uint32_t>(output_rate_);
            config.channel = 1;
            config.bits_per_sample = ESP_AUDIO_BIT16;
            config.complexity = 2;
            config.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
            if (esp_ae_rate_cvt_open(&config, &resampler_) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "Resampler open failed (%d -> %d)", source_rate_, output_rate_);
                decoder_error_ = true;
                return;
            }
        }
        ESP_LOGI(TAG, "Stream %d Hz %d ch -> %d Hz mono", source_rate_, channels, output_rate_);
    }

    if (source_rate_ == output_rate_) {
        PushSamples(mono_buf_.data(), mono_buf_.size());
        return;
    }
    if (resampler_ == nullptr) {
        return;
    }
    uint32_t output_samples = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(resampler_, mono_buf_.size(), &output_samples);
    if (output_samples == 0) {
        return;
    }
    resample_buf_.resize(output_samples);
    uint32_t actual = output_samples;
    esp_ae_rate_cvt_process(resampler_, static_cast<esp_ae_sample_t>(mono_buf_.data()),
                            mono_buf_.size(), static_cast<esp_ae_sample_t>(resample_buf_.data()),
                            &actual);
    resample_buf_.resize(actual);
    PushSamples(resample_buf_.data(), resample_buf_.size());
}

bool MediaPlayer::PushSamples(const int16_t* samples, size_t count) {
    if (count == 0) {
        return true;
    }
    carry_.insert(carry_.end(), samples, samples + count);
    const size_t chunk_samples =
        static_cast<size_t>(output_rate_) * kChunkMs / 1000;
    while (carry_.size() >= chunk_samples) {
        std::vector<int16_t> chunk(carry_.begin(), carry_.begin() + chunk_samples);
        carry_.erase(carry_.begin(), carry_.begin() + chunk_samples);
        if (!PushCarryChunk(std::move(chunk))) {
            return false;
        }
    }
    if (carry_.size() > kCarryLimit) {
        carry_.clear();
    }
    return true;
}

bool MediaPlayer::PushCarry() {
    if (carry_.empty()) {
        return true;
    }
    std::vector<int16_t> chunk = std::move(carry_);
    carry_.clear();
    return PushCarryChunk(std::move(chunk));
}

bool MediaPlayer::PushCarryChunk(std::vector<int16_t>&& chunk) {
    if (audio_ == nullptr || chunk.empty()) {
        return true;
    }
    const size_t samples = chunk.size();
    const uint32_t generation = generation_.load();
    if (!audio_->PushPlaybackPcm(std::move(chunk), generation, 200)) {
        return false;
    }
    played_samples_ += samples;
    uint32_t position =
        static_cast<uint32_t>(static_cast<uint64_t>(played_samples_) * 1000 / output_rate_);
    const uint32_t duration = duration_ms_.load();
    if (duration > 0 && position > duration) {
        position = duration;
    }
    position_ms_.store(position);
    return true;
}

bool MediaPlayer::WaitForPlaybackDrain(int timeout_ms) {
    const int step_ms = 20;
    for (int waited = 0; waited < timeout_ms; waited += step_ms) {
        if (stop_task_ || HasPendingCommand()) {
            return false;
        }
        if (audio_ != nullptr && audio_->IsPlaybackIdle()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return false;
}

bool MediaPlayer::HasPendingCommand() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return command_.type != Command::Type::kNone;
}

int MediaPlayer::NextIndexInQueue(int direction, bool wrap) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return -1;
    }
    int pos = queue_pos_;
    if (pos < 0 || pos >= static_cast<int>(queue_.size()) ||
        queue_[pos] != current_index_.load()) {
        const int current = current_index_.load();
        const auto it = std::find(queue_.begin(), queue_.end(), current);
        if (it == queue_.end()) {
            return -1;
        }
        pos = static_cast<int>(it - queue_.begin());
    }
    int next = pos + direction;
    if (next < 0 || next >= static_cast<int>(queue_.size())) {
        if (!wrap) {
            return -1;
        }
        next = (next + static_cast<int>(queue_.size())) % static_cast<int>(queue_.size());
    }
    queue_pos_ = next;
    return queue_[next];
}

void MediaPlayer::StartPlayback(int index, std::vector<int> queue, bool shuffle) {
    if (index < 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_ = std::move(queue);
        if (shuffle) {
            const auto it = std::find(queue_.begin(), queue_.end(), index);
            if (it != queue_.end()) {
                queue_.erase(it);
            }
            std::shuffle(queue_.begin(), queue_.end(), std::mt19937(esp_random()));
            queue_.insert(queue_.begin(), index);
        } else {
            const auto it = std::find(queue_.begin(), queue_.end(), index);
            if (it == queue_.end()) {
                queue_.insert(queue_.begin(), index);
            }
        }
        queue_pos_ = 0;
        shuffle_.store(shuffle);
        pending_.store(false);
        command_ = {Command::Type::kPlay, index};
    }
    paused_.store(false);
    auto_resume_.store(false);
    pause_reason_.store(PauseReason::kNone);
    state_.store(State::kPlaying);
    uint32_t generation = ++generation_;
    if (audio_ != nullptr) {
        audio_->SetMediaGeneration(generation);
    }
    cv_.notify_all();
}

bool MediaPlayer::PlayTrack(int index) {
    EnsureStarted();
    if (audio_ == nullptr) {
        return false;
    }
    if (!MusicLibrary::GetInstance().EnsureLoaded(false)) {
        return false;
    }
    MusicTrack track;
    if (!MusicLibrary::GetInstance().TrackAt(index, &track)) {
        return false;
    }
    StartPlayback(index, MusicLibrary::GetInstance().AllIndices(), false);
    return true;
}

void MediaPlayer::Pause(PauseReason reason) {
    if (state_.load() != State::kPlaying) {
        return;
    }
    paused_.store(true);
    pause_reason_.store(reason);
    auto_resume_.store(reason != PauseReason::kUser && reason != PauseReason::kError);
    state_.store(State::kPaused);
    uint32_t generation = ++generation_;
    if (audio_ != nullptr) {
        audio_->SetMediaGeneration(generation);
    }
    ESP_LOGI(TAG, "Paused (reason %d)", static_cast<int>(reason));
}

void MediaPlayer::Resume() {
    if (state_.load() != State::kPaused) {
        return;
    }
    paused_.store(false);
    auto_resume_.store(false);
    pause_reason_.store(PauseReason::kNone);
    state_.store(State::kPlaying);
    uint32_t generation = ++generation_;
    if (audio_ != nullptr) {
        audio_->SetMediaGeneration(generation);
    }
    if (!session_active_.load() && current_index_.load() >= 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        command_ = {Command::Type::kPlay, current_index_.load()};
    }
    cv_.notify_all();
}

void MediaPlayer::TogglePlayPause() {
    EnsureStarted();
    const State state = state_.load();
    if (state == State::kPlaying) {
        Pause(PauseReason::kUser);
    } else if (state == State::kPaused) {
        Resume();
    } else if (state == State::kIdle) {
        if (current_index_.load() >= 0) {
            PlayTrack(current_index_.load());
        } else {
            PlayTrack(0);
        }
    }
}

void MediaPlayer::Next() {
    EnsureStarted();
    if (current_index_.load() < 0) {
        PlayTrack(0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_ = {Command::Type::kNext, -1};
    }
    paused_.store(false);
    state_.store(State::kPlaying);
    cv_.notify_all();
}

void MediaPlayer::Prev() {
    EnsureStarted();
    if (current_index_.load() < 0) {
        PlayTrack(0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_ = {Command::Type::kPrev, -1};
    }
    paused_.store(false);
    state_.store(State::kPlaying);
    cv_.notify_all();
}

void MediaPlayer::Stop() {
    paused_.store(false);
    auto_resume_.store(false);
    pending_.store(false);
    pause_reason_.store(PauseReason::kNone);
    state_.store(State::kIdle);
    position_ms_.store(0);
    uint32_t generation = ++generation_;
    if (audio_ != nullptr) {
        audio_->SetMediaGeneration(generation);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        command_ = {Command::Type::kStop, -1};
    }
    cv_.notify_all();
    ESP_LOGI(TAG, "Stopped");
}

void MediaPlayer::SetShuffle(bool shuffle) {
    shuffle_.store(shuffle);
}

bool MediaPlayer::CurrentTrack(MusicTrack* out) const {
    if (current_index_.load() < 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(track_mutex_);
    if (current_track_.path.empty()) {
        return false;
    }
    if (out != nullptr) {
        *out = current_track_;
    }
    return true;
}

void MediaPlayer::SetCommandWindowActive(bool active) {
    if (active) {
        command_window_.store(true);
        if (state_.load() == State::kPlaying) {
            Pause(PauseReason::kWakeWord);
        }
        return;
    }
    command_window_.store(false);
    // Resume only what the window (or the wake word) paused, and only when the
    // device is idle: during a reply the state listener pauses again anyway,
    // and resuming early would play over the assistant.
    if (state_.load() == State::kPaused && auto_resume_.load() &&
        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        Resume();
    }
}

void MediaPlayer::OnDeviceStateChanged(int old_state, int new_state) {
    if (audio_ == nullptr) {
        return;
    }
    if (new_state != kDeviceStateIdle) {
        if (resume_timer_ != nullptr) {
            esp_timer_stop(resume_timer_);
        }
        if (state_.load() == State::kPlaying) {
            PauseReason reason = PauseReason::kWakeWord;
            if (new_state == kDeviceStateSpeaking) {
                reason = PauseReason::kTts;
            } else if (new_state == kDeviceStateNotifying) {
                reason = PauseReason::kNotify;
            }
            Pause(reason);
        }
        return;
    }

    if (pending_.load()) {
        StartPending();
        return;
    }
    if (state_.load() == State::kPaused && auto_resume_.load() && resume_timer_ != nullptr) {
        esp_timer_stop(resume_timer_);
        esp_timer_start_once(resume_timer_, 600 * 1000);
        ESP_LOGI(TAG, "Resuming music in 600 ms");
    }
}

void MediaPlayer::ResumeTimerCb(void* arg) {
    auto* self = static_cast<MediaPlayer*>(arg);
    if (self == nullptr) {
        return;
    }
    if (self->state_.load() != State::kPaused || !self->auto_resume_.load()) {
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    self->Resume();
}

void MediaPlayer::StartPending() {
    if (!pending_.exchange(false)) {
        return;
    }
    int index = -1;
    std::vector<int> queue;
    bool shuffle = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = pending_index_;
        queue = std::move(pending_queue_);
        shuffle = pending_shuffle_;
        pending_queue_.clear();
    }
    if (index >= 0) {
        StartPlayback(index, std::move(queue), shuffle);
    }
}

std::string MediaPlayer::PlayRequest(const std::string& query, const std::string& artist,
                                     const std::string& album, int track_id, bool shuffle) {
    EnsureStarted();
    if (audio_ == nullptr) {
        return BuildErrorJson("unavailable");
    }
    MusicLibrary& library = MusicLibrary::GetInstance();
    if (!library.EnsureLoaded(false)) {
        return BuildErrorJson("empty");
    }

    int chosen = -1;
    std::vector<int> queue;
    bool use_shuffle = shuffle;
    if (track_id > 0) {
        chosen = library.IndexOfId(static_cast<uint32_t>(track_id));
        if (chosen < 0) {
            return BuildErrorJson("not_found");
        }
        queue = library.AllIndices();
        use_shuffle = false;
    } else {
        const int limit = TrimText(query).empty() ? MusicLibrary::kMaxTracks : 50;
        const std::vector<int> matches = library.Search(query, artist, album, limit);
        if (matches.empty()) {
            return BuildErrorJson("not_found");
        }
        if (TrimText(query).empty()) {
            chosen = matches.front();
            queue = matches;
        } else {
            const std::string wanted = LowerAscii(TrimText(query));
            for (int index : matches) {
                MusicTrack track;
                if (library.TrackAt(index, &track) &&
                    LowerAscii(MusicLibrary::DisplayTitle(track)) == wanted) {
                    chosen = index;
                    break;
                }
            }
            if (chosen < 0) {
                if (!artist.empty() || !album.empty() || matches.size() == 1) {
                    chosen = matches.front();
                } else {
                    const int candidate_count =
                        std::min<int>(5, static_cast<int>(matches.size()));
                    return BuildAmbiguousJson(std::vector<int>(
                        matches.begin(), matches.begin() + candidate_count));
                }
            }
            queue = library.AllIndices();
            use_shuffle = false;
        }
    }

    MusicTrack track;
    if (!library.TrackAt(chosen, &track)) {
        return BuildErrorJson("not_found");
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_index_ = chosen;
            pending_queue_ = queue;
            pending_shuffle_ = use_shuffle;
        }
        pending_.store(true);
        ESP_LOGI(TAG, "Queued %s until the conversation ends", track.title.c_str());
        return BuildTrackJson(true, "pending", &track);
    }
    StartPlayback(chosen, std::move(queue), use_shuffle);
    return BuildTrackJson(true, "playing", &track);
}

std::string MediaPlayer::PauseRequest() {
    if (state_.load() != State::kPlaying) {
        return BuildErrorJson("not_playing");
    }
    Pause(PauseReason::kUser);
    return BuildTrackJson(true, "paused", nullptr);
}

std::string MediaPlayer::ResumeRequest() {
    if (state_.load() != State::kPaused) {
        return BuildErrorJson("not_paused");
    }
    Resume();
    return BuildTrackJson(true, "playing", nullptr);
}

std::string MediaPlayer::StopRequest() {
    Stop();
    return BuildTrackJson(true, "idle", nullptr);
}

std::string MediaPlayer::NextRequest() {
    if (state_.load() != State::kPlaying && state_.load() != State::kPaused) {
        return BuildErrorJson("not_playing");
    }
    Next();
    return BuildTrackJson(true, "playing", nullptr);
}

std::string MediaPlayer::PrevRequest() {
    if (state_.load() != State::kPlaying && state_.load() != State::kPaused) {
        return BuildErrorJson("not_playing");
    }
    Prev();
    return BuildTrackJson(true, "playing", nullptr);
}

std::string MediaPlayer::StatusRequest() {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "state", StateName(state_.load()));
    cJSON_AddBoolToObject(root, "shuffle", shuffle_.load());
    cJSON_AddNumberToObject(root, "position_ms", position_ms_.load());
    cJSON_AddNumberToObject(root, "duration_ms", duration_ms_.load());
    MusicTrack track;
    if (CurrentTrack(&track)) {
        cJSON* json = cJSON_AddObjectToObject(root, "track");
        cJSON_AddNumberToObject(json, "id", track.id);
        cJSON_AddStringToObject(json, "title", MusicLibrary::DisplayTitle(track).c_str());
        cJSON_AddStringToObject(json, "artist", MusicLibrary::DisplayArtist(track).c_str());
        cJSON_AddStringToObject(json, "album", MusicLibrary::DisplayAlbum(track).c_str());
        cJSON_AddNumberToObject(json, "duration_ms", track.duration_ms);
    } else {
        cJSON_AddNullToObject(root, "track");
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed != nullptr ? printed : "{}";
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return result;
}

std::string MediaPlayer::BuildTrackJson(bool ok, const char* state,
                                        const MusicTrack* track) const {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    if (state != nullptr) {
        cJSON_AddStringToObject(root, "state", state);
    }
    if (track != nullptr) {
        cJSON* json = cJSON_AddObjectToObject(root, "track");
        cJSON_AddNumberToObject(json, "id", track->id);
        cJSON_AddStringToObject(json, "title", MusicLibrary::DisplayTitle(*track).c_str());
        cJSON_AddStringToObject(json, "artist", MusicLibrary::DisplayArtist(*track).c_str());
        cJSON_AddStringToObject(json, "album", MusicLibrary::DisplayAlbum(*track).c_str());
        cJSON_AddNumberToObject(json, "duration_ms", track->duration_ms);
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed != nullptr ? printed : "{}";
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return result;
}

std::string MediaPlayer::BuildErrorJson(const char* error) const {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error);
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed != nullptr ? printed : "{}";
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return result;
}

std::string MediaPlayer::BuildAmbiguousJson(const std::vector<int>& candidates) const {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", "ambiguous");
    cJSON* array = cJSON_AddArrayToObject(root, "candidates");
    MusicLibrary& library = MusicLibrary::GetInstance();
    for (int index : candidates) {
        MusicTrack track;
        if (!library.TrackAt(index, &track)) {
            continue;
        }
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", track.id);
        cJSON_AddStringToObject(item, "title", MusicLibrary::DisplayTitle(track).c_str());
        cJSON_AddStringToObject(item, "artist", MusicLibrary::DisplayArtist(track).c_str());
        cJSON_AddStringToObject(item, "album", MusicLibrary::DisplayAlbum(track).c_str());
        cJSON_AddItemToArray(array, item);
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed != nullptr ? printed : "{}";
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return result;
}
