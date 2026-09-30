#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 Wi-Fi STA（板载 C6 通过 ESP-Hosted/SDIO 提供），非阻塞。 */
esp_err_t app_wifi_start(void);

/** 是否已获取 IP。 */
bool app_wifi_is_connected(void);

/** 读取当前 IP 字符串（未连接时为 "0.0.0.0"）。 */
void app_wifi_get_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
