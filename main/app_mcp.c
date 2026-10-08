// 设备端 MCP 工具：让小智（LLM）通过语音控制开发板硬件
//
// 当前提供：self.gpio.set_output —— 设置 40PIN 排针上 GPIO 的输出电平。
// 服务端（server/app/session.py）会向设备请求 tools/list，并把工具描述交给
// DeepSeek 做 function calling，再通过 tools/call 下发到这里执行。

#include "app_mcp.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"

#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"

static const char *TAG = "app_mcp";

static esp_mcp_t *s_mcp = NULL;

// 40PIN 排针上引出的 GPIO（不含 I2C 7/8、I2S 9-13、LCD 26/27、SD 39-44、功放 53 等板载占用）
static const int s_allowed_pins[] = {
    2, 3, 4, 5, 21, 22, 24, 25, 28, 29, 30, 31, 32,
    34, 35, 37, 38, 46, 47, 48, 49, 50, 51, 52,
};

static bool pin_is_allowed(int pin)
{
    for (size_t i = 0; i < sizeof(s_allowed_pins) / sizeof(s_allowed_pins[0]); i++) {
        if (s_allowed_pins[i] == pin) {
            return true;
        }
    }
    return false;
}

static esp_mcp_value_t gpio_set_output_cb(const esp_mcp_property_list_t *properties)
{
    char msg[128];
    int pin = esp_mcp_property_list_get_property_int(properties, "pin");
    int level = esp_mcp_property_list_get_property_int(properties, "level");

    if (!pin_is_allowed(pin)) {
        snprintf(msg, sizeof(msg), "错误：GPIO%d 不可用。可用引脚：2,3,4,5,21,22,24,25,28,29,30,31,32,34,35,37,38,46,47,48,49,50,51,52", pin);
        ESP_LOGW(TAG, "%s", msg);
        return esp_mcp_value_create_string(msg);
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        snprintf(msg, sizeof(msg), "错误：配置 GPIO%d 失败(%s)", pin, esp_err_to_name(ret));
        return esp_mcp_value_create_string(msg);
    }

    gpio_set_level(pin, level ? 1 : 0);
    snprintf(msg, sizeof(msg), "已设置 GPIO%d 输出为%s。", pin, level ? "高电平" : "低电平");
    ESP_LOGI(TAG, "%s", msg);
    return esp_mcp_value_create_string(msg);
}

esp_err_t app_mcp_init(void)
{
    ESP_RETURN_ON_FALSE(s_mcp == NULL, ESP_OK, TAG, "already initialized");

    esp_err_t ret = esp_mcp_create(&s_mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_mcp_create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "self.gpio.set_output",
        "控制开发板 40PIN 排针上的 GPIO 输出电平，可用于点亮 LED、驱动继电器等。"
        "pin 是 GPIO 编号；level=1 输出高电平（打开），level=0 输出低电平（关闭）。"
        "无论引脚当前状态如何，用户每次要求打开/关闭/设置高（低）电平时都必须实际调用本工具执行，"
        "不要凭记忆或猜测回答“已经是/无需操作”。"
        "可用引脚：2,3,4,5,21,22,24,25,28,29,30,31,32,34,35,37,38,46,47,48,49,50,51,52。",
        gpio_set_output_cb);
    if (!tool) {
        ESP_LOGE(TAG, "create gpio tool failed");
        return ESP_FAIL;
    }

    esp_mcp_tool_add_property(tool, esp_mcp_property_create_with_int_and_range("pin", 2, 0, 54));
    esp_mcp_tool_add_property(tool, esp_mcp_property_create_with_int_and_range("level", 1, 0, 1));

    ret = esp_mcp_add_tool(s_mcp, tool);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add gpio tool failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "mcp ready: self.gpio.set_output");
    return ESP_OK;
}

esp_mcp_t *app_mcp_get(void)
{
    return s_mcp;
}
