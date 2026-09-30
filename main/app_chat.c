// 对话业务：状态机 + esp_xiaozhi 协议接入
//
// 交互（触摸按钮点按）：
//   IDLE  --点击-->  LISTENING（发送 listen start/manual，麦克风开始上行）
//   LISTENING --点击--> THINKING（发送 listen stop，服务端 ASR+LLM+TTS）
//   SPEAKING --点击--> IDLE（发送 abort，打断播放）
//   TTS stop 事件 --> IDLE
//
// 半双工：仅 LISTENING 期间上行麦克风数据，播放 TTS 时不上行，避免自问自答。

#include "app_chat.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"

#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

#include "app_audio.h"
#include "app_mcp.h"
#include "app_ui.h"
#include "app_wifi.h"

#define THINKING_TIMEOUT_MS 12000

static const char *TAG = "app_chat";

typedef enum {
    ST_OFFLINE = 0,
    ST_IDLE,
    ST_LISTENING,
    ST_THINKING,
    ST_SPEAKING,
} chat_state_t;

static esp_xiaozhi_chat_handle_t s_chat = 0;
static EventGroupHandle_t s_eg = NULL;
static QueueHandle_t s_tap_q = NULL;
static volatile chat_state_t s_state = ST_OFFLINE;
static TickType_t s_thinking_since = 0;
static char s_assistant_buf[1024];

#define READY_STATUS "就绪，点击按钮和我说话"

// ------------------------------------------------------------------ 事件

static void chat_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch (id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to server");
        xEventGroupClearBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED);
        xEventGroupSetBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_CONNECTED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from server");
        xEventGroupClearBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_CONNECTED);
        xEventGroupSetBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "audio channel opened");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "audio channel closed");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        ESP_LOGW(TAG, "server goodbye");
        xEventGroupSetBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE);
        break;
    default:
        break;
    }
}

static void chat_event_cb(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    (void)ctx;
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *t = (esp_xiaozhi_chat_text_data_t *)event_data;
        if (!t || !t->text) {
            break;
        }
        if (t->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) {
            ESP_LOGI(TAG, "stt: %s", t->text);
            app_ui_set_user_text(t->text);
        } else {
            ESP_LOGI(TAG, "tts sentence: %s", t->text);
            size_t used = strlen(s_assistant_buf);
            if (used + strlen(t->text) + 1 < sizeof(s_assistant_buf)) {
                strcat(s_assistant_buf, t->text);
            }
            app_ui_set_assistant_text(s_assistant_buf);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *tts = (esp_xiaozhi_chat_tts_state_t *)event_data;
        if (!tts) {
            break;
        }
        if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            s_state = ST_SPEAKING;
            app_audio_set_send_enabled(false);
            app_ui_set_button(APP_UI_BTN_STOP_SPEAK);
            app_ui_set_status("小智正在回答…");
        } else if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            if (s_state == ST_SPEAKING || s_state == ST_THINKING) {
                s_state = ST_IDLE;
                app_ui_set_button(APP_UI_BTN_TALK);
                app_ui_set_status(READY_STATUS);
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        if (event_data && strcmp((const char *)event_data, "reboot") == 0) {
            ESP_LOGW(TAG, "system command: reboot");
            esp_restart();
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        esp_xiaozhi_chat_error_info_t *err = (esp_xiaozhi_chat_error_info_t *)event_data;
        ESP_LOGW(TAG, "chat error: %s (%s)", err ? esp_err_to_name(err->code) : "?",
                 (err && err->source) ? err->source : "?");
        break;
    }
    default:
        break;
    }
}

static void chat_audio_cb(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    app_audio_play_opus(data, (size_t)len);
}

static void audio_tx_cb(const uint8_t *opus, size_t len)
{
    if (s_chat) {
        esp_xiaozhi_chat_send_audio_data(s_chat, (const char *)opus, len);
    }
}

// ------------------------------------------------------------------ 状态机

void app_chat_handle_tap(void)
{
    uint8_t msg = 1;
    if (s_tap_q) {
        xQueueSend(s_tap_q, &msg, 0);
    }
}

static void handle_tap(void)
{
    if (!s_chat) {
        return;
    }
    switch (s_state) {
    case ST_IDLE:
        memset(s_assistant_buf, 0, sizeof(s_assistant_buf));
        app_ui_clear_texts();
        if (esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL) != ESP_OK) {
            ESP_LOGW(TAG, "send listen start failed");
            break;
        }
        s_state = ST_LISTENING;
        app_audio_set_send_enabled(true);
        app_ui_set_button(APP_UI_BTN_STOP_LISTEN);
        app_ui_set_status("正在聆听，说完点一下按钮");
        break;

    case ST_LISTENING:
        esp_xiaozhi_chat_send_stop_listening(s_chat);
        s_state = ST_THINKING;
        s_thinking_since = xTaskGetTickCount();
        app_audio_set_send_enabled(false);
        app_ui_set_button(APP_UI_BTN_THINKING);
        app_ui_set_status("小智正在思考…");
        break;

    case ST_SPEAKING:
        esp_xiaozhi_chat_send_abort_speaking(s_chat, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_STOP_LISTENING);
        app_audio_flush_playback();
        s_state = ST_IDLE;
        app_ui_set_button(APP_UI_BTN_TALK);
        app_ui_set_status(READY_STATUS);
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------------ 任务

static void cleanup_session(void)
{
    s_state = ST_OFFLINE;
    app_audio_set_send_enabled(false);
    app_audio_flush_playback();
    app_ui_set_button(APP_UI_BTN_DISABLED);
    if (s_chat) {
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        esp_xiaozhi_chat_stop(s_chat);
        esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
    }
}

static void chat_task(void *arg)
{
    (void)arg;
    esp_xiaozhi_chat_audio_t audio_params = {
        .format = "opus",
        .sample_rate = 16000,
        .channels = 1,
        .frame_duration = 60,
    };

    for (;;) {
        // 1) 等 Wi-Fi
        app_ui_set_button(APP_UI_BTN_DISABLED);
        app_ui_set_status("等待 Wi-Fi 连接…");
        while (!app_wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        char ip[24];
        char status[64];
        app_wifi_get_ip(ip, sizeof(ip));
        snprintf(status, sizeof(status), "Wi-Fi %s", ip);
        app_ui_set_status(status);
        ESP_LOGI(TAG, "wifi ready, ip=%s", ip);

        // 2) 从服务端拿配置（保存 WebSocket 地址到 NVS）
        app_ui_set_status("获取服务端配置…");
        esp_xiaozhi_chat_info_t info = {0};
        esp_err_t ret = esp_xiaozhi_chat_get_info(&info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "get_info failed: %s", esp_err_to_name(ret));
            app_ui_set_status("无法连接服务器，3 秒后重试…");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ESP_LOGI(TAG, "server config: ws=%d mqtt=%d", info.has_websocket_config, info.has_mqtt_config);

        // 3) 初始化并启动会话
        esp_xiaozhi_chat_config_t cfg = {0};
        cfg.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
        cfg.audio_callback = chat_audio_cb;
        cfg.event_callback = chat_event_cb;
        cfg.mcp_engine = app_mcp_get();
        cfg.owns_mcp_engine = false;
        cfg.has_mqtt_config = info.has_mqtt_config;
        cfg.has_websocket_config = info.has_websocket_config;
        esp_xiaozhi_chat_free_info(&info);

        app_ui_set_status("连接服务器中…");
        ret = esp_xiaozhi_chat_init(&cfg, &s_chat);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "chat init failed: %s", esp_err_to_name(ret));
            s_chat = 0;
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        ret = esp_xiaozhi_chat_start(s_chat);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "chat start failed: %s", esp_err_to_name(ret));
            cleanup_session();
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // 4) 等连接成功
        xEventGroupClearBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_CONNECTED |
                                      ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED |
                                      ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE);
        EventBits_t bits = xEventGroupWaitBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_CONNECTED, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(15000));
        if (!(bits & ESP_XIAOZHI_CHAT_EVENT_CONNECTED)) {
            ESP_LOGW(TAG, "connect timeout");
            cleanup_session();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // 5) 打开音频通道（hello 握手），最多试 3 次
        bool opened = false;
        for (int i = 0; i < 3 && !opened; i++) {
            ret = esp_xiaozhi_chat_open_audio_channel(s_chat, &audio_params, NULL, 0);
            if (ret == ESP_OK) {
                opened = true;
            } else {
                ESP_LOGW(TAG, "open audio channel failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (!opened) {
            cleanup_session();
            continue;
        }

        // 清理旧事件，准备进入会话循环
        xEventGroupClearBits(s_eg, ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED |
                                      ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE);
        s_state = ST_IDLE;
        app_ui_set_button(APP_UI_BTN_TALK);
        app_ui_set_status(READY_STATUS);

        // 6) 会话循环：处理按钮点击，直到断开
        for (;;) {
            EventBits_t cur = xEventGroupGetBits(s_eg);
            if (cur & (ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED | ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE)) {
                break;
            }

            uint8_t tap;
            if (xQueueReceive(s_tap_q, &tap, pdMS_TO_TICKS(200)) == pdTRUE) {
                if (tap) {
                    handle_tap();
                }
            }

            // 思考超时保护（服务端 ASR 空结果等异常时恢复）
            if (s_state == ST_THINKING &&
                xTaskGetTickCount() - s_thinking_since > pdMS_TO_TICKS(THINKING_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "thinking timeout, back to idle");
                s_state = ST_IDLE;
                app_ui_set_button(APP_UI_BTN_TALK);
                app_ui_set_status("没听清，请再试一次");
            }
        }

        // 7) 断线清理与重连
        ESP_LOGW(TAG, "session ended, reconnecting");
        cleanup_session();
        app_ui_set_status("连接已断开，重连中…");
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

esp_err_t app_chat_start(void)
{
    s_eg = xEventGroupCreate();
    s_tap_q = xQueueCreate(4, sizeof(uint8_t));
    if (!s_eg || !s_tap_q) {
        return ESP_ERR_NO_MEM;
    }

    app_audio_register_tx(audio_tx_cb);

    esp_err_t ret = esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, chat_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register event handler failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (xTaskCreate(chat_task, "app_chat", 12288, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
