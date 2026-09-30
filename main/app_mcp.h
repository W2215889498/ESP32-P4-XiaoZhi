#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建 MCP 引擎并注册设备端工具（GPIO 输出）。 */
esp_err_t app_mcp_init(void);

/** 获取 MCP 引擎（传给 esp_xiaozhi_chat_config_t.mcp_engine）。 */
esp_mcp_t *app_mcp_get(void);

#ifdef __cplusplus
}
#endif
