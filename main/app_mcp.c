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

#include "app_audio.h"

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

// ---------------- 扬声器音量工具（语音控制："声音大一点/小一点"）----------------

static esp_mcp_value_t speaker_set_volume_cb(const esp_mcp_property_list_t *properties)
{
    char msg[96];
    int volume = esp_mcp_property_list_get_property_int(properties, "volume");
    app_audio_set_volume(volume);
    snprintf(msg, sizeof(msg), "已把扬声器音量设置为 %d（范围 0-100）。", app_audio_get_volume());
    ESP_LOGI(TAG, "%s", msg);
    return esp_mcp_value_create_string(msg);
}

static esp_mcp_value_t speaker_get_volume_cb(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    char msg[64];
    snprintf(msg, sizeof(msg), "当前扬声器音量是 %d（范围 0-100）。", app_audio_get_volume());
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

    // 扬声器音量：设置 + 查询
    esp_mcp_tool_t *vol_tool = esp_mcp_tool_create(
        "self.audio_speaker.set_volume",
        "设置开发板扬声器音量。volume 为 0-100 的整数，数字越大声音越大。"
        "用户说声音太小/太大、调大/调小、静音时调用；如果要按相对量调整，可先调用 self.audio_speaker.get_volume 查询当前值。",
        speaker_set_volume_cb);
    if (vol_tool) {
        esp_mcp_tool_add_property(vol_tool, esp_mcp_property_create_with_int_and_range("volume", 70, 0, 100));
        esp_mcp_add_tool(s_mcp, vol_tool);
    }

    esp_mcp_tool_t *vol_get = esp_mcp_tool_create(
        "self.audio_speaker.get_volume",
        "查询开发板扬声器当前音量（0-100）。",
        speaker_get_volume_cb);
    if (vol_get) {
        esp_mcp_add_tool(s_mcp, vol_get);
    }

    ESP_LOGI(TAG, "mcp ready: self.gpio.set_output, self.audio_speaker.set_volume, self.audio_speaker.get_volume");
    return ESP_OK;
}

esp_mcp_t *app_mcp_get(void)
{
    return s_mcp;
}
