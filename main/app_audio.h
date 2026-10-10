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

/** 事件回调（唤醒词命中 / VAD 判定说完），在音频任务上下文调用，必须非阻塞。 */
typedef void (*app_audio_event_cb_t)(void);

/** 初始化 I2S + ES8311(喇叭) + ES7210(麦克风) + Opus 编解码器。 */
esp_err_t app_audio_init(void);

/** 注册 Opus 发送回调（app_chat 里注册）。 */
void app_audio_register_tx(app_audio_tx_cb_t cb);

/** 注册唤醒词命中回调（离线 WakeNet 检出唤醒词时触发）。 */
void app_audio_register_wake(app_audio_event_cb_t cb);

/** 注册"说完话"回调（VAD 检测到从说话转为静音时触发一次）。 */
void app_audio_register_vad_stop(app_audio_event_cb_t cb);

/** 控制麦克风数据是否上行（说话时开，其它状态关，半双工防回声）。 */
void app_audio_set_send_enabled(bool enabled);

/** 收到一帧 TTS Opus，解码后排队播放（可在协议回调里直接调用）。 */
void app_audio_play_opus(const uint8_t *opus, size_t len);

/** 丢掉尚未播放的 TTS 音频（打断/结束时）。 */
void app_audio_flush_playback(void);

/** 设置喇叭音量 0-100（自动钳制并写入 NVS，重启后保持）。 */
void app_audio_set_volume(int volume);

/** 读取当前喇叭音量 0-100。 */
int app_audio_get_volume(void);

#ifdef __cplusplus
}
#endif
