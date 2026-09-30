#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动对话任务：等待 Wi-Fi -> 获取服务端配置 -> 建立会话（自动重连）。 */
esp_err_t app_chat_start(void);

/** 触摸按钮点击（LVGL 回调里调用，非阻塞）。 */
void app_chat_handle_tap(void);

#ifdef __cplusplus
}
#endif
