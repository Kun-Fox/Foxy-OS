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
constexpr int kChunkSamples = 480;      // 20ms @24k
constexpr int kReadBufferSize = 8192;
constexpr int kTaskStack = 14336;

// 线性插值到 24k 单声道，攒够 20ms 就推一次喇叭
class Resampler {
public:
    void Push(AudioCodec* codec, const float* src, size_t frames, int channels, int in_rate,
              std::vector<int16_t>& out, int* elapsed_ms) {
        if (in_rate != in_rate_ || channels != channels_) {
            in_rate_ = in_rate;
            channels_ = channels;
            pos_ = 0.0;
            ESP_LOGI(TAG, "stream %d Hz %d ch", in_rate_, channels_);
        }
        double step = static_cast<double>(in_rate_) / kOutputSampleRate;
        for (size_t i = 0; i < frames; ++i) {
            const float* frame = src + i * channels_;
            float l = frame[0];
            float r = channels_ > 1 ? frame[1] : l;
            float mono = (l + r) * 0.5f;
            last_ = mono;
            pos_ += step;
            while (pos_ >= 1.0) {
                pos_ -= 1.0;
                out.push_back(static_cast<int16_t>(last_ * 32767.0f));
                if (static_cast<int>(out.size()) >= kChunkSamples) {
                    if (codec != nullptr) {
                        codec->OutputData(out);
                    }
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
    float last_ = 0.0f;
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
    elapsed_seconds_ = 0;
    playing_ = true;

    // 播放期间不要跟语音抢喇叭和麦克风
    auto& app = Application::GetInstance();
    app.SetDeviceState(kDeviceStateIdle);
    app.GetAudioService().EnableVoiceProcessing(false);

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
        return "未播放";
    }
    if (paused_) {
        return "已暂停";
    }
    return "播放中";
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
            int channels = info.channel > 0 ? info.channel : 2;
            int rate = info.sample_rate > 0 ? info.sample_rate : 44100;
            size_t frames = frame.decoded_size / (2 * channels);
            pcm.resize(frames * channels);
            for (size_t i = 0; i < frames * channels; ++i) {
                int16_t sample = static_cast<int16_t>(frame_buf[2 * i] | (frame_buf[2 * i + 1] << 8));
                pcm[i] = sample / 32768.0f;
            }
            resampler.Push(codec_, pcm.data(), frames, channels, rate, out, &elapsed_ms);
            elapsed_seconds_ = elapsed_ms / 1000;
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

