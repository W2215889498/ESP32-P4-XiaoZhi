// 音频：ES8311(喇叭) + ES7210(麦克风) + Opus 编解码
//
// 数据流：
//   麦克风 --I2S(16k/2ch)--> 降混单声道 --> Opus 编码 --(回调)--> 小智协议上行
//   小智协议下行 --> Opus 解码(单声道) --> 复制为双声道 --> 环形缓冲 --> I2S 播放
//
// 说明：开发板 I2S 以 16kHz / 双声道 / 16bit 打开（与微雪官方 I2SCodec 例程一致），
// 应用层再做单声道转换；Opus 侧始终为 16kHz 单声道 60ms 帧（小智协议默认）。

#include "app_audio.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

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
#define PCM_MONO_BUF_SIZE     (FRAME_SAMPLES * sizeof(int16_t))            // 1920
#define PCM_STEREO_BUF_SIZE   (FRAME_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t))  // 3840
#define PLAY_RING_SIZE        (32 * 1024)

static const char *TAG = "app_audio";

static esp_codec_dev_handle_t s_spk = NULL;
static esp_codec_dev_handle_t s_mic = NULL;
static void *s_opus_enc = NULL;
static void *s_opus_dec = NULL;
static StreamBufferHandle_t s_play_ring = NULL;

static volatile bool s_send_enabled = false;
static app_audio_tx_cb_t s_tx_cb = NULL;

// ------------------------------------------------------------------ 播放

// 把小智协议下发的 Opus 帧解码为 PCM，写入播放环形缓冲（由协议回调调用）
void app_audio_play_opus(const uint8_t *opus, size_t len)
{
    if (!s_opus_dec || !s_play_ring || !opus || len == 0) {
        return;
    }

    uint8_t mono[PCM_MONO_BUF_SIZE];
    uint8_t stereo[PCM_STEREO_BUF_SIZE];
    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)opus,
        .len = (uint32_t)len,
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
        size_t bytes = (size_t)samples * AUDIO_CHANNELS * sizeof(int16_t);
        xStreamBufferSend(s_play_ring, stereo, bytes, 0);

        if (raw.consumed == 0) {
            break;
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
}

void app_audio_flush_playback(void)
{
    if (s_play_ring) {
        xStreamBufferReset(s_play_ring);
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    uint8_t chunk[1920];  // 30ms 双声道 PCM
    while (1) {
        size_t got = xStreamBufferReceive(s_play_ring, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (got == 0) {
            continue;
        }
        got &= ~(size_t)0x03;  // 保证整帧对齐
        if (got > 0 && s_spk) {
            esp_codec_dev_write(s_spk, chunk, (int)got);
        }
    }
}

// ------------------------------------------------------------------ 录音

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

// ------------------------------------------------------------------ 初始化

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

    s_play_ring = xStreamBufferCreate(PLAY_RING_SIZE, 1);
    if (!s_play_ring) {
        ESP_LOGE(TAG, "play ring create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "audio_mic", 40960, NULL, 8, NULL, 1);
    ok &= (xTaskCreatePinnedToCore(playback_task, "audio_play", 20480, NULL, 8, NULL, 1) == pdPASS);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio tasks create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "audio ready: %dHz %dch, opus %dms", AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, OPUS_FRAME_MS);
    return ESP_OK;
}
