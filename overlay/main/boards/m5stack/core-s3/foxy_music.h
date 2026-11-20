#ifndef FOXY_MUSIC_H
#define FOXY_MUSIC_H

#include <atomic>
#include <cstdint>
#include <utility>
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
    void PlayTone();
    void PlayRawTest();

    bool IsPlaying() const;
    bool IsPaused() const;
    int CurrentIndex() const;
    int ElapsedSeconds() const;
    std::string StatusText() const;

    // 封面：服务端已经把它裁成 320x240 并转成 RGB565，设备只需要拉下来贴屏
    bool LoadCover(int index);
    const uint8_t* CoverData() const;

    // 歌词：服务端返回同名 .lrc，按时间戳解析成 (毫秒, 文本)
    bool LoadLyrics(int index);
    std::string LyricAt(int elapsed_ms) const;

    int DurationMs() const;
    void SeekTo(int seconds);

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

    std::atomic<int> index_{-1};
    std::atomic<int> generation_{0};
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int> elapsed_seconds_{0};
    std::atomic<int> seek_seconds_{0};

    mutable std::mutex cover_mutex_;
    std::vector<uint8_t> cover_;
    int cover_index_ = -1;
    std::vector<std::pair<int, std::string>> lyrics_;

    mutable std::mutex status_mutex_;
    std::string status_;};

#endif  // FOXY_MUSIC_H



