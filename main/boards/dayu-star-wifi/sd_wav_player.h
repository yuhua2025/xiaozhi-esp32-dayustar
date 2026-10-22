#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class AudioCodec;

// SD card + MP3 player singleton for dayu-star-wifi board.
class SdWavPlayer {
public:
    static SdWavPlayer& GetInstance();

    // Mount SD card at /sdcard (SPI2_HOST)
    esp_err_t Mount();
    bool IsMounted() const { return mounted_; }

    // Play the next MP3 under dir (scans recursively on first call)
    bool PlayNext(const std::string& dir, AudioCodec* codec);

    // Build (once) and return the sorted playlist
    const std::vector<std::string>& BuildPlaylist(const std::string& dir);

    // Play playlist entry index (0-based)
    bool PlayIndex(size_t index, const std::string& dir, AudioCodec* codec);

    // ID3 title of playlist entry index (empty if unknown)
    std::string TrackTitle(size_t index);

    // Play an MP3 file. Returns true if playback started.
    bool PlayAudio(const std::string& path, AudioCodec* codec);

    // Stop playback (non-blocking)
    void Stop();

    int TrackCount() const { return (int)audio_files_.size(); }
    int CurrentTrack() const { return (int)audio_index_; }
    // Song title from ID3v2 TIT2 (empty if the file has no usable tag)
    const std::string& CurrentTitle() const { return current_title_; }
    bool IsPlaying() const { return playing_; }
    // Playing but temporarily silenced because a conversation is in progress
    bool IsPaused() const { return playing_ && paused_for_conversation_; }
    int DurationSeconds() const { return duration_sec_; }
    int ElapsedSeconds() const { return elapsed_sec_; }

private:
    SdWavPlayer() = default;
    ~SdWavPlayer() = default;
    SdWavPlayer(const SdWavPlayer&) = delete;
    SdWavPlayer& operator=(const SdWavPlayer&) = delete;

    static void PlaybackTaskTrampoline(void* arg);
    void PlaybackTask();
    static std::string ReadId3Title(const std::string& path);
    static int ProbeDuration(const std::string& path);

    bool mounted_ = false;
    std::string current_path_;
    AudioCodec* current_codec_ = nullptr;
    volatile bool want_stop_ = false;
    bool paused_for_conversation_ = false;
    volatile bool playing_ = false;
    TaskHandle_t task_handle_ = nullptr;
    std::vector<std::string> audio_files_;
    size_t audio_index_ = 0;
    std::string current_title_;
    volatile int duration_sec_ = 0;
    volatile int elapsed_sec_ = 0;
};
