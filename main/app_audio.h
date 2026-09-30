#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 音频发送回调：把一帧 Opus 数据交给小智协议层。 */
typedef void (*app_audio_tx_cb_t)(const uint8_t *opus, size_t len);

/** 初始化 I2S + ES8311(喇叭) + ES7210(麦克风) + Opus 编解码器。 */
esp_err_t app_audio_init(void);

/** 注册 Opus 发送回调（app_chat 里注册）。 */
void app_audio_register_tx(app_audio_tx_cb_t cb);

/** 控制麦克风数据是否上行（说话时开，其它状态关，半双工防回声）。 */
void app_audio_set_send_enabled(bool enabled);

/** 收到一帧 TTS Opus，解码后排队播放（可在协议回调里直接调用）。 */
void app_audio_play_opus(const uint8_t *opus, size_t len);

/** 丢掉尚未播放的 TTS 音频（打断/结束时）。 */
void app_audio_flush_playback(void);

/** 设置喇叭音量 0-100。 */
void app_audio_set_volume(int volume);

#ifdef __cplusplus
}
#endif
