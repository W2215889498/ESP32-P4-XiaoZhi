idf_component_register(
    SRCS
        "app_main.c"
        "app_wifi.c"
        "app_audio.c"
        "app_ui.c"
        "app_mcp.c"
        "app_chat.c"
    INCLUDE_DIRS "."
)

# 全量中文二进制字库（lv_font_conv --format bin 生成，见 tools/gen_font.ps1），
# 链接进固件，运行时由 app_ui.c 通过 lv_binfont_create_from_buffer() 加载。
target_add_binary_data(${COMPONENT_LIB} "assets/font_tk_16.bin" TEXT)
