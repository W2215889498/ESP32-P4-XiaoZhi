# ESP32-P4 Xiaozhi AI assistant (Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3)
cmake_minimum_required(VERSION 3.16)

# mcp-c-sdk 2.0.1 在 ESP-IDF 6.0 + GCC 15 下会命中 -Werror=stringop-truncation
# （组件内部 strncpy 截断告警，上游尚未修复）。这里全局关闭该告警，
# 待 mcp-c-sdk 发布修复版本后可删除本行。
add_compile_options(-Wno-stringop-truncation)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_p4_xiaozhi)
