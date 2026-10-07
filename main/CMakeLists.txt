idf_component_register(
    SRCS
        "app_main.c"
        "app_wifi.c"
        "app_audio.c"
        "app_ui.c"
        "app_mcp.c"
        "app_chat.c"
        "assets/font_tk_16.c"
    INCLUDE_DIRS "."
)

# lv_font_conv 生成的字体文件默认 #include "lvgl/lvgl.h"，
# 定义该宏让它改用 "lvgl.h"（与本工程其它文件一致）。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
