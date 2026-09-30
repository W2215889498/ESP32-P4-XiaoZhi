#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_UI_BTN_DISABLED = 0,   // 等待连接，不可点
    APP_UI_BTN_TALK,           // 点击说话
    APP_UI_BTN_STOP_LISTEN,    // 正在听，点击结束
    APP_UI_BTN_STOP_SPEAK,     // 正在播放，点击打断
    APP_UI_BTN_THINKING,       // 已说完，等待识别/回答（不可点）
} app_ui_button_state_t;

typedef void (*app_ui_tap_cb_t)(void);

/** 初始化显示屏 + LVGL 界面（调用后即可用下面的接口）。 */
esp_err_t app_ui_init(void);

/** 注册触摸按钮回调（在 LVGL 上下文中被调用，回调里不要阻塞）。 */
void app_ui_register_tap(app_ui_tap_cb_t cb);

/** 右上角状态文字，如 "Wi-Fi 已连接 192.168.1.5" / "连接服务器中…"。 */
void app_ui_set_status(const char *text);

/** 用户识别文字（STT）。 */
void app_ui_set_user_text(const char *text);

/** 小智回复文字（整段设置，句子追加请用 append）。 */
void app_ui_set_assistant_text(const char *text);

/** 追加一句小智回复。 */
void app_ui_append_assistant_text(const char *text);

/** 清空对话文字。 */
void app_ui_clear_texts(void);

/** 切换底部大按钮的状态/文案。 */
void app_ui_set_button(app_ui_button_state_t state);

#ifdef __cplusplus
}
#endif
