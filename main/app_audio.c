// 音频：ES8311(喇叭) + ES7210(麦克风) + Opus 编解码
//
// 数据流：
//   麦克风 --I2S(16k/2ch)--> 降混单声道 --> Opus 编码 --(回调)--> 小智协议上行
//   小智协议下行 --> [队列] --> 播放任务内 Opus 解码 --> 复制为双声道 --> I2S 播放
//
// 注意：小智协议回调（websocket_task，栈只有 ~4KB）里绝不做解码等重活，
// 只做「拷到 PSRAM + 入队」；解码和写 I2S 全部在播放任务（大栈）里完成。

#include "app_audio.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_codec_dev.h"

#include "esp_audio_types.h"
#include "esp_audio_dec.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"

#include "bsp/esp-bsp.h"

#if CONFIG_XZ_WAKEWORD_ENABLE
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#endif

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_CHANNELS        2          // I2S 物理通道（codec 侧）
#define AUDIO_BITS            16
#define AUDIO_MCLK_MULTIPLE   256
#define OPUS_FRAME_MS         60
#define FRAME_SAMPLES         (AUDIO_SAMPLE_RATE / 1000 * OPUS_FRAME_MS)   // 960
#define MIC_READ_BYTES        (FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))  // 3840
#define OPUS_OUT_BUF_SIZE     1024
#define OPUS_MAX_PKT          1024       // 单包 Opus 上限
#define OPUS_QUEUE_LEN        512        // 服务端按句突发下发（一整句音频几百 ms 内到齐），
                                         // 队列需容纳 ~30 秒音频，否则会丢帧导致播放中途截断
#define PCM_MONO_BUF_SIZE     (FRAME_SAMPLES * sizeof(int16_t))            // 1920
#define PCM_STEREO_BUF_SIZE   (FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))  // 3840

static const char *TAG = "app_audio";

typedef struct {
    uint8_t *data;
    uint16_t len;
} opus_pkt_t;

static esp_codec_dev_handle_t s_spk = NULL;
static esp_codec_dev_handle_t s_mic = NULL;
static void *s_opus_enc = NULL;
static void *s_opus_dec = NULL;
static QueueHandle_t s_opus_q = NULL;

static volatile bool s_send_enabled = false;
static app_audio_tx_cb_t s_tx_cb = NULL;
static app_audio_event_cb_t s_wake_cb = NULL;
static app_audio_event_cb_t s_vad_stop_cb = NULL;

#define AFE_MAX_FEED_SAMPLES 1024

#if CONFIG_XZ_WAKEWORD_ENABLE
static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static int s_feed_samples = 0;
static volatile bool s_vad_speech_seen = false;
#endif

// ---------------------------------------------------------------- 下行（播放）

// 由小智协议回调（websocket_task）调用：只拷贝 + 入队，不能阻塞、不能占大栈
void app_audio_play_opus(const uint8_t *opus, size_t len)
{
    if (!s_opus_q || !opus || len == 0 || len > OPUS_MAX_PKT) {
        return;
    }
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        return;
    }
    memcpy(copy, opus, len);
    opus_pkt_t pkt = {.data = copy, .len = (uint16_t)len};
    if (xQueueSend(s_opus_q, &pkt, 0) != pdTRUE) {
        // 队列满：丢弃这一帧，保证协议任务永不阻塞（正常不应发生，队列已按整句容量设计）
        static int drop_cnt = 0;
        if ((drop_cnt++ % 20) == 0) {
            ESP_LOGW(TAG, "playback queue full, drop frame (%d dropped)", drop_cnt);
        }
        heap_caps_free(copy);
    }
}

void app_audio_flush_playback(void)
{
    if (!s_opus_q) {
        return;
    }
    opus_pkt_t pkt;
    while (xQueueReceive(s_opus_q, &pkt, 0) == pdTRUE) {
        heap_caps_free(pkt.data);
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    uint8_t mono[PCM_MONO_BUF_SIZE];
    uint8_t stereo[PCM_STEREO_BUF_SIZE];
    opus_pkt_t pkt;

    while (1) {
        if (xQueueReceive(s_opus_q, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_opus_dec && pkt.data && pkt.len) {
            esp_audio_dec_in_raw_t raw = {
                .buffer = pkt.data,
                .len = pkt.len,
            };
            int guard = 0;
            while (raw.len > 0 && ++guard < 4) {
                esp_audio_dec_out_frame_t frame = {
                    .buffer = mono,
                    .len = sizeof(mono),
                };
                esp_audio_dec_info_t info = {0};
                esp_audio_err_t ret = esp_opus_dec_decode(s_opus_dec, &raw, &frame, &info);
                if (ret != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
                    if (ret != ESP_AUDIO_ERR_OK) {
                        ESP_LOGW(TAG, "opus decode failed: %d", ret);
                    }
                    break;
                }
                // 单声道 -> 双声道（I2S 配置为双声道）
                int16_t *src = (int16_t *)mono;
                int16_t *dst = (int16_t *)stereo;
                int samples = (int)frame.decoded_size / (int)sizeof(int16_t);
                for (int i = 0; i < samples; i++) {
                    dst[2 * i] = src[i];
                    dst[2 * i + 1] = src[i];
                }
                if (s_spk) {
                    esp_codec_dev_write(s_spk, stereo, samples * AUDIO_CHANNELS * (int)sizeof(int16_t));
                }
                if (raw.consumed == 0) {
                    break;
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
        }
        heap_caps_free(pkt.data);
    }
}

// ---------------------------------------------------------------- 上行（录音）

void app_audio_set_send_enabled(bool enabled)
{
#if CONFIG_XZ_WAKEWORD_ENABLE
    if (enabled && !s_send_enabled) {
        s_vad_speech_seen = false;   // 新一轮聆听：重置 VAD 状态，避免误触发自动结束
    }
#endif
    s_send_enabled = enabled;
}

void app_audio_register_tx(app_audio_tx_cb_t cb)
{
    s_tx_cb = cb;
}

void app_audio_register_wake(app_audio_event_cb_t cb)
{
    s_wake_cb = cb;
}

void app_audio_register_vad_stop(app_audio_event_cb_t cb)
{
    s_vad_stop_cb = cb;
}

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t raw[AFE_MAX_FEED_SAMPLES * AUDIO_CHANNELS];
    static int16_t mono[AFE_MAX_FEED_SAMPLES];
    static uint8_t opus_buf[OPUS_OUT_BUF_SIZE];
#if CONFIG_XZ_WAKEWORD_ENABLE
    static int16_t send_frame[FRAME_SAMPLES];
    int send_fill = 0;
#endif

    while (1) {
#if CONFIG_XZ_WAKEWORD_ENABLE
        if (s_afe_data) {
            // ---------- AFE 路径：唤醒词 + VAD + 降噪后的上行 ----------
            int nbytes = s_feed_samples * AUDIO_CHANNELS * (int)sizeof(int16_t);
            int ret = esp_codec_dev_read(s_mic, raw, nbytes);
            if (ret != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "mic read failed: %d", ret);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            // 双声道 -> 单声道，喂给 AFE
            for (int i = 0; i < s_feed_samples; i++) {
                int32_t mix = (int32_t)raw[2 * i] + (int32_t)raw[2 * i + 1];
                mono[i] = (int16_t)(mix / 2);
            }
            s_afe->feed(s_afe_data, mono);

            afe_fetch_result_t *res = s_afe->fetch(s_afe_data);   // 阻塞到下一帧（≤2s）
            if (!res || res->ret_value == ESP_FAIL) {
                continue;
            }

            // 唤醒词检测
            if (res->wakeup_state == WAKENET_DETECTED && s_wake_cb) {
                s_wake_cb();
            }

            // VAD：speech -> silence 的下降沿，回调一次（用于自动断句）
            if (res->vad_state == VAD_SPEECH) {
                s_vad_speech_seen = true;
            } else if (s_vad_speech_seen) {
                s_vad_speech_seen = false;
                if (s_vad_stop_cb) {
                    s_vad_stop_cb();
                }
            }

            // 上行：把 AFE 输出（单声道）攒满 60ms 再编码发送
            if (s_send_enabled && s_tx_cb && s_opus_enc && res->data && res->data_size > 0) {
                int samples = res->data_size / (int)sizeof(int16_t);
                const int16_t *src = res->data;
                while (samples > 0) {
                    int space = FRAME_SAMPLES - send_fill;
                    int n = samples < space ? samples : space;
                    memcpy(&send_frame[send_fill], src, n * sizeof(int16_t));
                    send_fill += n;
                    src += n;
                    samples -= n;
                    if (send_fill == FRAME_SAMPLES) {
                        esp_audio_enc_in_frame_t in_frame = {
                            .buffer = (uint8_t *)send_frame,
                            .len = sizeof(send_frame),
                        };
                        esp_audio_enc_out_frame_t out_frame = {
                            .buffer = opus_buf,
                            .len = sizeof(opus_buf),
                        };
                        esp_audio_err_t enc_ret = esp_opus_enc_process(s_opus_enc, &in_frame, &out_frame);
                        if (enc_ret == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0) {
                            s_tx_cb(opus_buf, out_frame.encoded_bytes);
                        } else if (enc_ret != ESP_AUDIO_ERR_OK) {
                            ESP_LOGW(TAG, "opus encode failed: %d", enc_ret);
                        }
                        send_fill = 0;
                    }
                }
            } else {
                send_fill = 0;
            }
            continue;
        }
#endif
        // ---------- 直通路径（未启用唤醒词，或 AFE 初始化失败时的降级） ----------
        int ret = esp_codec_dev_read(s_mic, raw, MIC_READ_BYTES);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_send_enabled || !s_tx_cb || !s_opus_enc) {
            continue;
        }

        // 双声道 -> 单声道
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            int32_t mix = (int32_t)raw[2 * i] + (int32_t)raw[2 * i + 1];
            mono[i] = (int16_t)(mix / 2);
        }

        esp_audio_enc_in_frame_t in_frame = {
            .buffer = (uint8_t *)mono,
            .len = (uint32_t)(FRAME_SAMPLES * sizeof(int16_t)),
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = opus_buf,
            .len = sizeof(opus_buf),
        };
        esp_audio_err_t enc_ret = esp_opus_enc_process(s_opus_enc, &in_frame, &out_frame);
        if (enc_ret == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0) {
            s_tx_cb(opus_buf, out_frame.encoded_bytes);
        } else if (enc_ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "opus encode failed: %d", enc_ret);
        }
    }
}

// ---------------------------------------------------------------- 初始化

void app_audio_set_volume(int volume)
{
    if (s_spk) {
        esp_codec_dev_set_out_vol(s_spk, volume);
    }
}

esp_err_t app_audio_init(void)
{
    ESP_ERROR_CHECK(bsp_audio_init(NULL));

    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "microphone codec init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
        .channel_mask = 0x03,
        .mclk_multiple = AUDIO_MCLK_MULTIPLE,
    };
    int ret = esp_codec_dev_open(s_spk, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed: %d", ret);
        return ESP_FAIL;
    }
    ret = esp_codec_dev_open(s_mic, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone open failed: %d", ret);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, CONFIG_XZ_SPEAKER_VOLUME);
    esp_codec_dev_set_in_gain(s_mic, (float)CONFIG_XZ_MIC_GAIN_DB);

    esp_opus_enc_config_t enc_cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = 24000,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = false,
    };
    esp_audio_err_t aerr = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_opus_enc);
    if (aerr != ESP_AUDIO_ERR_OK || !s_opus_enc) {
        ESP_LOGE(TAG, "opus encoder open failed: %d", aerr);
        return ESP_FAIL;
    }

    esp_opus_dec_cfg_t dec_cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    aerr = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &s_opus_dec);
    if (aerr != ESP_AUDIO_ERR_OK || !s_opus_dec) {
        ESP_LOGE(TAG, "opus decoder open failed: %d", aerr);
        return ESP_FAIL;
    }

#if CONFIG_XZ_WAKEWORD_ENABLE
    // ---------------- ESP-SR：离线唤醒词（小特小特）+ VAD 自动断句 ----------------
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "srmodel init failed (model partition 没烧录？)");
    }
    char *wn_name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, NULL) : NULL;
    ESP_LOGI(TAG, "wake word model: %s", wn_name ? wn_name : "(none)");

    afe_config_t *afe_cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg) {
        afe_cfg->aec_init = false;   // 暂不启用回声消除（无回采参考）
        afe_cfg->wakenet_init = true;
        if (wn_name) {
            afe_cfg->wakenet_model_name = wn_name;
        }
        afe_cfg->vad_init = true;    // WebRTC VAD（自动断句用）
        afe_cfg->vad_min_noise_ms = CONFIG_XZ_WAKEWORD_AUTO_STOP_MS;
        afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
        afe_cfg->afe_perferred_core = 1;
        afe_cfg->afe_perferred_priority = 5;
        const esp_afe_sr_iface_t *afe = esp_afe_handle_from_config(afe_cfg);
        if (afe) {
            s_afe = afe;
            s_afe_data = afe->create_from_config(afe_cfg);
            if (s_afe_data) {
                s_feed_samples = afe->get_feed_chunksize(s_afe_data);
                ESP_LOGI(TAG, "AFE ready: feed %d samples/ch", s_feed_samples);
                if (s_feed_samples <= 0 || s_feed_samples > AFE_MAX_FEED_SAMPLES) {
                    ESP_LOGE(TAG, "unexpected feed chunk %d, disable AFE", s_feed_samples);
                    s_afe_data = NULL;
                }
            }
        }
        afe_config_free(afe_cfg);
    }
    if (!s_afe_data) {
        ESP_LOGW(TAG, "AFE unavailable: wake word disabled, fallback to direct mic path");
    }
#endif

    s_opus_q = xQueueCreate(OPUS_QUEUE_LEN, sizeof(opus_pkt_t));
    if (!s_opus_q) {
        ESP_LOGE(TAG, "opus queue create failed");
        return ESP_ERR_NO_MEM;
    }

    // 播放任务：解码 + I2S 写入都在这里（栈给足，含 ~6KB 局部缓冲 + opus 解码开销）
    BaseType_t ok = xTaskCreatePinnedToCore(playback_task, "audio_play", 32768, NULL, 8, NULL, 1);
    ok &= (xTaskCreatePinnedToCore(mic_task, "audio_mic", 40960, NULL, 8, NULL, 1) == pdPASS);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio tasks create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "audio ready: %dHz %dch, opus %dms", AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, OPUS_FRAME_MS);
    return ESP_OK;
}
