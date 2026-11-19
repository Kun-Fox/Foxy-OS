#ifndef FOXY_MUSIC_H
#define FOXY_MUSIC_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class AudioCodec;

// 音乐播放器：曲目列表放在电脑侧的 cores3-server\music 里，
// 设备通过 /debug/music/* 拉列表与音频流，MP3 在本地解码后直接推给喇叭。
// 与语音链路互斥：播放期间关掉语音处理（麦克风/唤醒词），停止后恢复。
class FoxyMusic {
public:
    static FoxyMusic& Get();

    void Init(AudioCodec* codec);
    void Refresh();  // 从服务器拉一次曲目列表（网络失败时保留旧列表）

    int TrackCount() const;
    std::string TrackName(int index) const;
    int TrackDurationMs(int index) const;

    void Play(int index);
    void TogglePause();
    void Next();
    void Prev();
    void Stop();

    bool IsPlaying() const;
    bool IsPaused() const;
    int CurrentIndex() const;
    int ElapsedSeconds() const;
    std::string StatusText() const;

private:
    FoxyMusic() = default;
    FoxyMusic(const FoxyMusic&) = delete;
    FoxyMusic& operator=(const FoxyMusic&) = delete;

    static void TaskEntry(void* arg);
    void TaskLoop(int index, int generation);
    void ServerBase(char* out, size_t size) const;

    AudioCodec* codec_ = nullptr;
    std::string base_url_;

    mutable std::mutex list_mutex_;
    std::vector<std::string> names_;
    std::vector<int> durations_ms_;

    std::atomic<int> index_{ -1 };
    std::atomic<int> generation_{ 0 };
    std::atomic<bool> playing_{ false };
    std::atomic<bool> paused_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<int> elapsed_seconds_{ 0 };
};

#endif  // FOXY_MUSIC_H
