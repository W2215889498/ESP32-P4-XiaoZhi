// Wi-Fi station：ESP32-P4 无自带 Wi-Fi，这里通过板载 ESP32-C6（ESP-Hosted/SDIO）
// 使用标准的 esp_wifi API（esp_wifi_remote 组件做了透明转发）。

#include "app_wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "app_wifi";

static esp_netif_t *s_netif = NULL;
static TimerHandle_t s_retry_timer = NULL;
static volatile bool s_connected = false;

static void retry_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "disconnected, retry in 3s");
        if (s_retry_timer) {
            xTimerStart(s_retry_timer, 0);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
    }
}

esp_err_t app_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_XZ_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_XZ_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    if (strlen(CONFIG_XZ_WIFI_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(3000), pdFALSE, NULL, retry_timer_cb);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 关闭省电，保证语音流低延迟
    esp_err_t ps_ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(ps_ret));
    }

    ESP_LOGI(TAG, "station started, ssid=%s", CONFIG_XZ_WIFI_SSID);
    return ESP_OK;
}

bool app_wifi_is_connected(void)
{
    return s_connected;
}

void app_wifi_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    esp_netif_ip_info_t ip_info = {0};
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, len, "0.0.0.0");
    }
}
