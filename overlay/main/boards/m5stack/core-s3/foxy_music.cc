#include "board.h"
#include "cores3_audio_codec.h"
#include <vector>

#ifndef FOXY_MUSIC_USE_CODEC_OUTPUT
#define FOXY_MUSIC_USE_CODEC_OUTPUT 1
#endif
#include <cmath>
#include "foxy_music.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include <cJSON.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mp3_dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "audio/audio_codec.h"
#include "audio/audio_service.h"
#include "device_state.h"
#include "settings.h"

static const char* TAG = "FoxyMusic";

namespace {

constexpr int kOutputSampleRate = 24000;
constexpr int kChunkSamples = 2400;      // 20ms @24k
constexpr int kReadBufferSize = 8192;
constexpr int kTaskStack = 14336;

// 线性插值到 24k 单声道，攒够 20ms 就推一次喇叭


// 统一出口：24k 单声道块 -> 立体声交错 -> 直接写 I2S。
// 之前走 codec->OutputData()（esp_codec_dev 那一层），喂多快就丢多快；
// 改成阻塞写 i2s_channel_write 之后，队列满时它自己会等，天然就是实时节拍。
static void WriteMonoToCodec(AudioCodec* codec, const std::vector<int16_t>& mono) {
    if (codec == nullptr || mono.empty()) {
        return;
    }
    auto* c3 = static_cast<CoreS3AudioCodec*>(codec);  // 板上只有这一种 codec，不需要 RTTI
    if (c3 == nullptr) {
        return;
    }
    // 先走和 TTS 完全相同的通路（AudioCodec::OutputData -> esp_codec_dev_write）做对照，
    // 验证"直写 I2S"与"codec 抽象层"哪一条才出得了声。
#if FOXY_MUSIC_USE_CODEC_OUTPUT
    std::vector<int16_t> copy = mono;
    codec->OutputData(copy);
    return;
#endif
    // 本板 I2S TX 是 MONO（slot_mode=MONO、slot_mask=0x1），直接写单声道样本，
    // 千万别复制成 L/R 交错：那样每个采样会被当成两个采样播出去，声音变调成噪声。
    const std::vector<int16_t>& stereo = mono;
    const size_t kFramesPerWrite = 480;  // 20ms @24k，和 DMA 一格同量级
    size_t sent = 0;
    static int write_errors = 0;
    while (sent < mono.size()) {
        size_t frames = mono.size() - sent;
        if (frames > kFramesPerWrite) {
            frames = kFramesPerWrite;
        }
        // 每次写之前重新确认"输出已开 + 音量已应用"：esp_codec_dev 在静音/空闲时会关掉功放，
    // 只 EnableOutput 不一定把 AW88298 的 unmute 写回去，所以再打一次音量强制生效。
    c3->EnableOutput(true);
    c3->SetOutputVolume(c3->output_volume());
    int wrote = c3->WritePcm(stereo.data() + sent, frames, 200);
        if (wrote < 0) {
            if (write_errors++ < 3) {
                ESP_LOGE("FoxyMusic", "i2s write failed at %u/%u", (unsigned)sent, (unsigned)mono.size());
            }
            if (write_errors > 200) {
                ESP_LOGE("FoxyMusic", "i2s keeps failing, stop playback");
                return;
            }
        } else {
            write_errors = 0;
        }
        sent += frames;
    }
}

// 播放节拍器：I2S 的 DMA 队列很短，灌得比实时快就会丢帧，
// 听感就是"滋滋"的电流声。这里按采样数算时间戳，写完就睡到下一个点。
// 播放节拍器：I2S 的 DMA 队列很短，灌得比实时快就会丢帧，听感就是电流声；
// 灌得比实时慢则会断流，听感是咔哒。这里把"已写入的音频时长"和墙上时间对齐，
// 并刻意保持 ahead_us 的余量吸收调度抖动（本机 CONFIG_FREERTOS_HZ=100，tick 只有 10ms）。
static void PaceAfter(AudioCodec* codec, size_t samples, int64_t* deadline_us) {
    if (codec == nullptr) {
        return;
    }
    *deadline_us += static_cast<int64_t>(samples) * 1000000LL / kOutputSampleRate;
    const int64_t ahead_us = 40000;  // 队列里领先 40ms，够抗 10ms tick 的抖动
    while (true) {
        int64_t now = esp_timer_get_time();
        int64_t ahead = *deadline_us - now;
        if (ahead <= ahead_us) {
            break;
        }
        vTaskDelay(1);  // 1 tick = 10ms
    }
    if (esp_timer_get_time() - *deadline_us > 500000) {
        *deadline_us = esp_timer_get_time();  // 落后半秒以上就不再追
    }
}
class Resampler {
public:
    void Push(AudioCodec* codec, const float* src, size_t frames, int channels, int in_rate,
              std::vector<int16_t>& out, int* elapsed_ms) {
        if (in_rate != in_rate_ || channels != channels_) {
            in_rate_ = in_rate;
            channels_ = channels;
            pos_ = 0.0;
            acc_ = 0.0f;
            acc_n_ = 0;
            ESP_LOGI(TAG, "stream %d Hz %d ch", in_rate_, channels_);
        }
        // 降采样用"盒式平均"（把落在同一个输出点里的若干输入样本求平均）。
        // 之前是直接重复上一个样本，44.1k -> 24k 时会严重混叠，听感就是电流声。
        const double step = static_cast<double>(in_rate_) / kOutputSampleRate;
        for (size_t i = 0; i < frames; ++i) {
            const float* frame = src + i * channels_;
            const float l = frame[0];
            const float r = channels_ > 1 ? frame[1] : l;
            acc_ += (l + r) * 0.5f;
            acc_n_ += 1;
            pos_ += 1.0;
            while (pos_ >= step) {
                pos_ -= step;
                out.push_back(static_cast<int16_t>((acc_ / static_cast<float>(acc_n_)) * 32767.0f));
                acc_ = 0.0f;
                acc_n_ = 0;
                if (static_cast<int>(out.size()) >= kChunkSamples) {
                    WriteMonoToCodec(codec, out);
                    out.clear();
                    *elapsed_ms += kChunkSamples * 1000 / kOutputSampleRate;
                }
            }
        }
    }

private:
    int in_rate_ = 0;
    int channels_ = 0;
    double pos_ = 0.0;
    float acc_ = 0.0f;
    int acc_n_ = 0;
    int64_t deadline_us_ = 0;
};

}  // namespace

FoxyMusic& FoxyMusic::Get() {
    static FoxyMusic instance;
    return instance;
}

void FoxyMusic::Init(AudioCodec* codec) {
    codec_ = codec;
    ServerBase(nullptr, 0);
    ESP_LOGI(TAG, "music ready, server=%s", base_url_.c_str());
}

void FoxyMusic::ServerBase(char* out, size_t size) const {
    std::string url = "http://192.168.3.10:8003";
    Settings settings("wifi", false);
    std::string ota = settings.GetString("ota_url");
    size_t scheme = ota.find("://");
    if (scheme != std::string::npos) {
        size_t host_start = scheme + 3;
        size_t path = ota.find('/', host_start);
        std::string host = (path == std::string::npos) ? ota : ota.substr(0, path);
        if (host.size() > 10) {
            url = host;
        }
    }
    const_cast<FoxyMusic*>(this)->base_url_ = url;
    if (out != nullptr && size > 0) {
        snprintf(out, size, "%s", url.c_str());
    }
}

void FoxyMusic::Refresh() {
    ServerBase(nullptr, 0);
    char url[192];
    snprintf(url, sizeof(url), "%s/debug/music/list", base_url_.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 6000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGW(TAG, "no client");
        return;
    }

    std::string body;
    esp_err_t open_ret = esp_http_client_open(client, 0);
    int status = 0;
    if (open_ret == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        char buf[512];
        int read = 0;
        while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
            body.append(buf, read);
            if (body.size() > 8192) {
                break;
            }
        }
    } else {
        ESP_LOGW(TAG, "open %s failed: %s", url, esp_err_to_name(open_ret));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "list http=%d status=%d body=%d bytes", (int)open_ret, status,
             (int)body.size());

    if (body.empty()) {
        ESP_LOGW(TAG, "playlist empty (server offline?)");
        return;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "playlist json invalid");
        return;
    }
    cJSON* tracks = cJSON_GetObjectItem(root, "tracks");
    std::vector<std::string> names;
    std::vector<int> durations;
    if (cJSON_IsArray(tracks)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, tracks) {
            cJSON* name = cJSON_GetObjectItem(item, "name");
            cJSON* duration = cJSON_GetObjectItem(item, "duration_ms");
            if (cJSON_IsString(name)) {
                names.emplace_back(name->valuestring);
                durations.push_back(cJSON_IsNumber(duration) ? duration->valueint : 0);
            }
        }
    }
    cJSON_Delete(root);

    std::lock_guard<std::mutex> lock(list_mutex_);
    names_ = std::move(names);
    durations_ms_ = std::move(durations);
    ESP_LOGI(TAG, "playlist: %d track(s)", static_cast<int>(names_.size()));
}

int FoxyMusic::TrackCount() const {
    std::lock_guard<std::mutex> lock(list_mutex_);
    return static_cast<int>(names_.size());
}

std::string FoxyMusic::TrackName(int index) const {
    std::lock_guard<std::mutex> lock(list_mutex_);
    if (index < 0 || index >= static_cast<int>(names_.size())) {
        return std::string();
    }
    return names_[index];
}

int FoxyMusic::TrackDurationMs(int index) const {
    std::lock_guard<std::mutex> lock(list_mutex_);
    if (index < 0 || index >= static_cast<int>(durations_ms_.size())) {
        return 0;
    }
    return durations_ms_[index];
}

void FoxyMusic::Play(int index) {
    if (codec_ == nullptr) {
        return;
    }
    if (TrackCount() == 0) {
        Refresh();
    }
    if (TrackCount() == 0) {
        return;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= TrackCount()) {
        index = 0;
    }

    int new_generation = generation_.fetch_add(1) + 1;
    stop_ = true;
    vTaskDelay(pdMS_TO_TICKS(150));  // 给上一个任务收尾的时间
    stop_ = false;
    paused_ = false;
    index_ = index;
    elapsed_seconds_.store(seek_seconds_.load());
    playing_ = true;

    // 播放期间不要跟语音抢喇叭和麦克风；但**不动设备状态**，
    // 否则状态机会把已经建立的语音通道关掉，放歌就等于把会话踢下线。
    Application::GetInstance().GetAudioService().EnableVoiceProcessing(false);

    if (xTaskCreate(TaskEntry, "foxy_music", kTaskStack, this, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        playing_ = false;
        return;
    }
    ESP_LOGI(TAG, "play index=%d gen=%d name=%s", index, new_generation,
             TrackName(index).c_str());
}

void FoxyMusic::TogglePause() {
    if (!playing_) {
        return;
    }
    paused_ = !paused_;
    ESP_LOGI(TAG, "paused=%d", paused_.load() ? 1 : 0);
}

void FoxyMusic::Next() {
    if (TrackCount() == 0) {
        return;
    }
    Play((CurrentIndex() + 1) % TrackCount());
}

void FoxyMusic::Prev() {
    if (TrackCount() == 0) {
        return;
    }
    int index = CurrentIndex() - 1;
    if (index < 0) {
        index = TrackCount() - 1;
    }
    Play(index);
}

void FoxyMusic::Stop() {
    stop_ = true;
    playing_ = false;
    paused_ = false;
    elapsed_seconds_ = 0;
    if (codec_ != nullptr) {
        codec_->EnableOutput(true);
    }
    Application::GetInstance().GetAudioService().EnableVoiceProcessing(true);
    ESP_LOGI(TAG, "stop");
}

bool FoxyMusic::IsPlaying() const {
    return playing_;
}

bool FoxyMusic::IsPaused() const {
    return paused_;
}

int FoxyMusic::CurrentIndex() const {
    return index_;
}

int FoxyMusic::ElapsedSeconds() const {
    return elapsed_seconds_;
}

std::string FoxyMusic::StatusText() const {
    if (!playing_) {
        std::string extra;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            extra = status_;
        }
        return extra.empty() ? "未播放" : extra;
    }
    if (paused_) {
        return "已暂停";
    }
    return "播放中";
}


// 试音：2 秒 440Hz 正弦，直接写 codec。用来把"扬声器/功放坏"和"音乐链路有问题"分开。
static void ToneTask(void*) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    codec->EnableOutput(true);
    if (codec->output_volume() <= 0) {
        codec->SetOutputVolume(60);
    }
    std::vector<int16_t> blob(2400);
    const int total = 24000 * 2 / 2400;
    const double step = 2.0 * M_PI * 440.0 / 24000.0;
    int64_t deadline = esp_timer_get_time();
    ESP_LOGI("FoxyMusic", "tone start, volume=%d", codec->output_volume());
    for (int frame = 0; frame < total; frame++) {
        for (int i = 0; i < 480; i++) {
            blob[i] = static_cast<int16_t>(8000 * sin(step * (frame * 2400 + i)));
        }
        WriteMonoToCodec(codec, blob);
    }
    ESP_LOGI("FoxyMusic", "tone done");
    vTaskDelete(nullptr);
}

void FoxyMusic::PlayTone() {
    xTaskCreate(ToneTask, "tone", 4096, nullptr, 3, nullptr);
}

// 裸 PCM 直写：从服务器取 24k 单声道 16bit 正弦，不经过 mp3 解码、不重采样。
// 用来判断问题出在"解码/重采样"，还是"写到 I2S 这一步"。
static void RawTestTask(void*) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    codec->EnableOutput(true);
    ESP_LOGI("FoxyMusic", "raw test: volume=%d", codec->output_volume());
    const char* url = "http://192.168.3.10:8003/debug/music/raw?seconds=5";
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 4000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE("FoxyMusic", "raw test: open failed");
        esp_http_client_cleanup(client);
        vTaskDelete(nullptr);
        return;
    }
    esp_http_client_fetch_headers(client);
    std::vector<int16_t> chunk(2400);
    int total = 0;
    int64_t deadline = esp_timer_get_time();
    while (true) {
        int want = chunk.size() * 2;
        int got = esp_http_client_read(client, reinterpret_cast<char*>(chunk.data()), want);
        if (got <= 0) {
            break;
        }
        int samples = got / 2;
        chunk.resize(samples);
        WriteMonoToCodec(codec, chunk);
        chunk.resize(2400);
        total += got;
    }
    ESP_LOGI("FoxyMusic", "raw test done, %d bytes", total);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(nullptr);
}

void FoxyMusic::PlayRawTest() {
    xTaskCreate(RawTestTask, "rawtest", 6144, nullptr, 3, nullptr);
}

void FoxyMusic::TaskEntry(void* arg) {
    auto* self = static_cast<FoxyMusic*>(arg);
    int index = self->index_;
    int generation = self->generation_;
    self->TaskLoop(index, generation);
    if (self->generation_ == generation) {
        self->playing_ = false;
    }
    vTaskDelete(nullptr);
}

void FoxyMusic::TaskLoop(int index, int generation) {
    // 输出通道可能是关着的（静音、或别的流程关过）：播放前显式打开，
    // 否则 OutputData 一路丢样本，日志看着一切正常、耳朵里一点声都没有。
    codec_->EnableOutput(true);
    if (codec_->output_volume() <= 0) {
        codec_->SetOutputVolume(40);
    }
    ESP_LOGI(TAG, "codec output enabled=%d volume=%d", (int)codec_->output_enabled(),
             codec_->output_volume());

    char url[192];
    snprintf(url, sizeof(url), "%s/debug/music/file?index=%d", base_url_.c_str(), index);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed", url);
        esp_http_client_cleanup(client);
        // 连不上服务器时给屏幕上留句话，别让人对着黑屏猜
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_ = "连不上电脑";
        }
        playing_ = false;
        return;
    }
    esp_http_client_fetch_headers(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "streaming %s (len=%d)", url, content_length);

    esp_mp3_dec_register();
    void* decoder = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "mp3 decoder open failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    auto* frame_buf = static_cast<uint8_t*>(malloc(16384));
    std::vector<uint8_t> pending;
    pending.reserve(kReadBufferSize * 2);
    std::vector<float> pcm;
    std::vector<int16_t> out;
    out.reserve(kChunkSamples);
    Resampler resampler;
    int elapsed_ms = 0;
    bool eof = false;
    std::vector<uint8_t> read_buf(kReadBufferSize);

    while (!stop_ && generation_ == generation_) {
        if (paused_) {
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }
        int read = esp_http_client_read(client, reinterpret_cast<char*>(read_buf.data()),
                                       read_buf.size());
        if (read <= 0) {
            if (read < 0) {
                ESP_LOGW(TAG, "read error %d", read);
            }
            eof = true;
            break;
        }
        pending.insert(pending.end(), read_buf.begin(), read_buf.begin() + read);

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = pending.data();
        raw.len = pending.size();

        while (raw.len > 0 && !stop_ && generation_ == generation_) {
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = frame_buf;
            frame.len = 16384;
            esp_audio_dec_info_t info = {};
            esp_audio_err_t ret = esp_mp3_dec_decode(decoder, &raw, &frame, &info);
            if (ret != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
                break;  // 帧不完整，等更多数据
            }
            // 解码器只回填 raw.consumed，不会自己改 raw.len —— 必须手动推进输入指针。
            // 少了这一步就会把同一帧反复解码：听感是持续的电流声，进度条还会跑过歌曲时长。
            const uint32_t consumed = raw.consumed;
            if (consumed == 0) {
                break;
            }
            raw.buffer += consumed;
            raw.len -= consumed;
            int channels = info.channel > 0 ? info.channel : 2;
            int rate = info.sample_rate > 0 ? info.sample_rate : 44100;
            size_t frames = frame.decoded_size / (2 * channels);
            pcm.resize(frames * channels);
            for (size_t i = 0; i < frames * channels; ++i) {
                int16_t sample = static_cast<int16_t>(frame_buf[2 * i] | (frame_buf[2 * i + 1] << 8));
                pcm[i] = sample / 32768.0f;
            }
            // seek：还没到目标位置就只解码不输出（解码比实时快，跳过很快）
            AudioCodec* sink = (elapsed_ms < seek_seconds_ * 1000) ? nullptr : codec_;
            resampler.Push(sink, pcm.data(), frames, channels, rate, out, &elapsed_ms);
            if (sink == nullptr) {
                out.clear();
            }
            elapsed_seconds_ = elapsed_ms / 1000;
            static int progress_tick = 0;
            if (++progress_tick % 20 == 0) {
                ESP_LOGI(TAG, "progress %d ms, decoded %u frames", elapsed_ms, (unsigned)frames);
            }
            if (paused_) {
                break;
            }
        }

        // 已消费的部分丢掉
        size_t consumed = pending.size() - raw.len;
        if (consumed > 0) {
            pending.erase(pending.begin(), pending.begin() + consumed);
        }
        if (pending.size() > 256 * 1024) {
            pending.clear();  // 防御性：解码器认不出数据时不要无限吃内存
        }
    }

    if (!out.empty() && codec_ != nullptr) {
        codec_->OutputData(out);
    }
    free(frame_buf);
    esp_mp3_dec_close(decoder);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "task end index=%d eof=%d elapsed=%dms", index, eof ? 1 : 0, elapsed_ms);
}


int FoxyMusic::DurationMs() const {
    return TrackDurationMs(CurrentIndex());
}

void FoxyMusic::SeekTo(int seconds) {
    if (TrackCount() == 0) {
        return;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    seek_seconds_ = seconds;
    Play(CurrentIndex());
}

bool FoxyMusic::LoadCover(int index) {
    {
        std::lock_guard<std::mutex> lock(cover_mutex_);
        if (cover_index_ == index && cover_.size() == 320 * 240 * 2) {
            return true;  // 同一首已经拉过封面，直接用，别再去占网络
        }
    }
    char url[192];
    snprintf(url, sizeof(url), "%s/debug/music/cover?index=%d&fmt=rgb565&w=320&h=240",
             base_url_.c_str(), index);
    std::vector<uint8_t> buffer(320 * 240 * 2);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 8000;   // 153KB 的封面在 2.4G 上偶尔要两三秒，超时给足
    cfg.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    int total = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int stalls = 0;
        while (total < static_cast<int>(buffer.size())) {
            int read = esp_http_client_read(client, reinterpret_cast<char*>(buffer.data() + total),
                                            buffer.size() - total);
            if (read <= 0) {
                // 读空了但还没收满：多半是这一下没等到数据，别急着放弃，退让一下再读
                if (++stalls > 8 || total == 0) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            stalls = 0;
            total += read;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total != static_cast<int>(buffer.size())) {
        ESP_LOGW(TAG, "cover index=%d got %d bytes (want %d)", index, total,
                 static_cast<int>(buffer.size()));
        return false;
    }
    std::lock_guard<std::mutex> lock(cover_mutex_);
    cover_ = std::move(buffer);
    cover_index_ = index;
    ESP_LOGI(TAG, "cover index=%d ready", index);
    return true;
}

const uint8_t* FoxyMusic::CoverData() const {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    if (cover_.size() != 320u * 240u * 2u) {
        return nullptr;
    }
    return cover_.data();
}

bool FoxyMusic::LoadLyrics(int index) {
    char url[192];
    snprintf(url, sizeof(url), "%s/debug/music/lyrics?index=%d", base_url_.c_str(), index);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 1500;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    std::string body;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        char buf[512];
        int read = 0;
        while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
            body.append(buf, read);
            if (body.size() > 16384) {
                break;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (body.empty()) {
        return false;
    }

    std::vector<std::pair<int, std::string>> parsed;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        int minutes = 0;
        double seconds = 0.0;
        if (sscanf(line.c_str(), "[%d:%lf]", &minutes, &seconds) == 2) {
            size_t close = line.find(']');
            std::string text = (close == std::string::npos) ? std::string() : line.substr(close + 1);
            parsed.emplace_back(static_cast<int>(minutes * 60000 + seconds * 1000), text);
        }
    }
    std::lock_guard<std::mutex> lock(cover_mutex_);
    lyrics_ = std::move(parsed);
    ESP_LOGI(TAG, "lyrics index=%d lines=%d", index, static_cast<int>(lyrics_.size()));
    return !lyrics_.empty();
}

std::string FoxyMusic::LyricAt(int elapsed_ms) const {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    std::string current;
    for (const auto& entry : lyrics_) {
        if (entry.first > elapsed_ms) {
            break;
        }
        current = entry.second;
    }
    return current;
}









