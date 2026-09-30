// ESP32-P4-XiaoZhi 启动入口
//
// 启动顺序：
//   NVS -> 显示屏/UI -> 音频(codec+opus) -> MCP 工具 -> Wi-Fi(C6) -> 对话任务

#include "esp_log.h"
#include "nvs_flash.h"

#include "app_audio.h"
#include "app_chat.h"
#include "app_mcp.h"
#include "app_ui.h"
#include "app_wifi.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(app_ui_init());
    ESP_ERROR_CHECK(app_audio_init());
    ESP_ERROR_CHECK(app_mcp_init());

    app_ui_register_tap(app_chat_handle_tap);

    ESP_ERROR_CHECK(app_wifi_start());
    ESP_ERROR_CHECK(app_chat_start());

    ESP_LOGI(TAG, "boot done");
}
