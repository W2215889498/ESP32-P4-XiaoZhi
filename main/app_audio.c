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

#define AUDIO_SAMPLE_RATE     16000
#define AUDIO_CHANNELS        2          // I2S 物理通道（codec 侧）
#define AUDIO_BITS            16
#define AUDIO_MCLK_MULTIPLE   256
#define OPUS_FRAME_MS         60
#define FRAME_SAMPLES         (AUDIO_SAMPLE_RATE / 1000 * OPUS_FRAME_MS)   // 960
#define MIC_READ_BYTES        (FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))  // 3840
#define OPUS_OUT_BUF_SIZE     1024
#define OPUS_MAX_PKT          1024       // 单包 Opus 上限
#define OPUS_QUEUE_LEN        16
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
        // 队列满：丢弃这一帧，保证协议任务永不阻塞
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
    s_send_enabled = enabled;
}

void app_audio_register_tx(app_audio_tx_cb_t cb)
{
    s_tx_cb = cb;
}

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t raw[MIC_READ_BYTES / sizeof(int16_t)];
    static int16_t mono[FRAME_SAMPLES];
    static uint8_t opus_buf[OPUS_OUT_BUF_SIZE];

    while (1) {
        int ret = esp_codec_dev_read(s_mic, raw, sizeof(raw));
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
            .len = sizeof(mono),
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
