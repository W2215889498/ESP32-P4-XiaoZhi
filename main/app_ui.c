// LVGL 简洁对话界面：顶部状态栏 + 用户/小智文字 + 底部触摸按钮

#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#define COLOR_BG        0x0F1216
#define COLOR_HEADER    0x161B22
#define COLOR_TEXT      0xE6EAF2
#define COLOR_TEXT_DIM  0x9DA7B3
#define COLOR_ACCENT    0x2F6BE4
#define COLOR_LISTEN    0xE24A4A
#define COLOR_SPEAK     0xE8912E
#define COLOR_DISABLED  0x3A3F46

static const char *TAG = "app_ui";

static lv_obj_t *s_status = NULL;
static lv_obj_t *s_user = NULL;
static lv_obj_t *s_assistant = NULL;
static lv_obj_t *s_btn = NULL;
static lv_obj_t *s_btn_label = NULL;

static app_ui_tap_cb_t s_tap_cb = NULL;
static app_ui_button_state_t s_btn_state = APP_UI_BTN_DISABLED;

static void btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_btn_state == APP_UI_BTN_DISABLED) {
        return;
    }
    if (s_tap_cb) {
        s_tap_cb();
    }
}

static void apply_button_locked(void)
{
    const char *text = "";
    lv_color_t color = lv_color_hex(COLOR_DISABLED);
    bool disabled = false;

    switch (s_btn_state) {
    case APP_UI_BTN_TALK:
        text = "点击说话";
        color = lv_color_hex(COLOR_ACCENT);
        break;
    case APP_UI_BTN_STOP_LISTEN:
        text = "点击结束";
        color = lv_color_hex(COLOR_LISTEN);
        break;
    case APP_UI_BTN_STOP_SPEAK:
        text = "停止播放";
        color = lv_color_hex(COLOR_SPEAK);
        break;
    case APP_UI_BTN_THINKING:
        text = "识别中…";
        color = lv_color_hex(COLOR_DISABLED);
        disabled = true;
        break;
    case APP_UI_BTN_DISABLED:
    default:
        text = "等待连接…";
        color = lv_color_hex(COLOR_DISABLED);
        disabled = true;
        break;
    }

    lv_label_set_text(s_btn_label, text);
    lv_obj_center(s_btn_label);
    lv_obj_set_style_bg_color(s_btn, color, 0);
    if (disabled) {
        lv_obj_add_state(s_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_btn, LV_STATE_DISABLED);
    }
}

static lv_obj_t *make_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_source_han_sans_sc_14_cjk, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_DIM), 0);
    return label;
}

esp_err_t app_ui_init(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        }};
    if (!bsp_display_start_with_config(&cfg)) {
        ESP_LOGE(TAG, "display start failed");
        return ESP_FAIL;
    }
    bsp_display_backlight_on();

    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---------------- 顶部状态栏 ----------------
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LV_PCT(100), 42);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(COLOR_HEADER), 0);
    lv_obj_set_style_pad_hor(header, 14, 0);
    lv_obj_set_style_pad_ver(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "小智 AI 助手");
    lv_obj_set_style_text_font(title, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    s_status = lv_label_create(header);
    lv_label_set_text(s_status, "启动中…");
    lv_obj_set_style_text_font(s_status, &lv_font_source_han_sans_sc_14_cjk, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(s_status, LV_ALIGN_RIGHT_MID, 0, 0);

    // ---------------- 中部对话文字 ----------------
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(100), 480 - 42 - 104);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 12, 0);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    make_caption(content, "你：");
    s_user = lv_label_create(content);
    lv_obj_set_width(s_user, LV_PCT(100));
    lv_obj_set_height(s_user, 78);
    lv_label_set_long_mode(s_user, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_user, "");
    lv_obj_set_style_text_font(s_user, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(s_user, lv_color_hex(COLOR_TEXT), 0);

    make_caption(content, "小智：");
    s_assistant = lv_label_create(content);
    lv_obj_set_width(s_assistant, LV_PCT(100));
    lv_obj_set_height(s_assistant, 150);
    lv_label_set_long_mode(s_assistant, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_assistant, "你好，我是小智。点下面的按钮就能和我说话。");
    lv_obj_set_style_text_font(s_assistant, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(s_assistant, lv_color_hex(COLOR_TEXT), 0);

    // ---------------- 底部按钮 ----------------
    s_btn = lv_button_create(scr);
    lv_obj_set_size(s_btn, 300, 84);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(s_btn, 42, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(COLOR_DISABLED), 0);
    lv_obj_add_event_cb(s_btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    s_btn_label = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btn_label, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(s_btn_label, lv_color_hex(0xFFFFFF), 0);
    apply_button_locked();

    bsp_display_unlock();

    ESP_LOGI(TAG, "ui ready");
    return ESP_OK;
}

void app_ui_register_tap(app_ui_tap_cb_t cb)
{
    s_tap_cb = cb;
}

void app_ui_set_status(const char *text)
{
    if (!s_status) {
        return;
    }
    bsp_display_lock(-1);
    lv_label_set_text(s_status, text ? text : "");
    lv_obj_align(s_status, LV_ALIGN_RIGHT_MID, 0, 0);
    bsp_display_unlock();
}

void app_ui_set_user_text(const char *text)
{
    if (!s_user) {
        return;
    }
    bsp_display_lock(-1);
    lv_label_set_text(s_user, text ? text : "");
    bsp_display_unlock();
}

void app_ui_set_assistant_text(const char *text)
{
    if (!s_assistant) {
        return;
    }
    bsp_display_lock(-1);
    lv_label_set_text(s_assistant, text ? text : "");
    bsp_display_unlock();
}

void app_ui_append_assistant_text(const char *text)
{
    if (!s_assistant || !text) {
        return;
    }
    bsp_display_lock(-1);
    const char *cur = lv_label_get_text(s_assistant);
    char buf[1024];
    if (cur && cur[0]) {
        snprintf(buf, sizeof(buf), "%s%s", cur, text);
    } else {
        snprintf(buf, sizeof(buf), "%s", text);
    }
    lv_label_set_text(s_assistant, buf);
    bsp_display_unlock();
}

void app_ui_clear_texts(void)
{
    app_ui_set_user_text("");
    app_ui_set_assistant_text("");
}

void app_ui_set_button(app_ui_button_state_t state)
{
    if (!s_btn) {
        return;
    }
    bsp_display_lock(-1);
    s_btn_state = state;
    apply_button_locked();
    bsp_display_unlock();
}
