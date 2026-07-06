#include "air_distance_guard_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "airtouch_cloud_uart.h"
#include "ui_fonts.h"

/*
 * AirTouch Distance Guard UI v2m.1
 *
 * Data source:
 *   ESP8266 VL53L0X guard event over UART:
 *     GUARD,<seq>,<guard_on>,<distance_mm>,<reason>
 *
 * UI style:
 *   Light storybook style, consistent with the AirTouch main menu.
 *   Full-screen overlay still blocks the screen during unsafe distance.
 */

#define AIR_DISTANCE_GUARD_TIMER_MS 120

static const char *TAG = "distance_guard";

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_card = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_info = NULL;
static lv_obj_t *s_distance_value = NULL;
static lv_obj_t *s_reason_value = NULL;
static lv_obj_t *s_hint = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_visible = false;
static uint32_t s_last_seq = 0;
static bool s_has_last_seq = false;

static const char *distance_guard_reason_text(const char *reason)
{
    if (reason == NULL) {
        return "NA";
    }

    if (strcmp(reason, "INIT_TOO_CLOSE") == 0) {
        return "初始距离过近";
    }
    if (strcmp(reason, "TOO_CLOSE") == 0) {
        return "距离过近";
    }
    if (strcmp(reason, "SAFE_AGAIN") == 0) {
        return "距离已恢复";
    }
    if (strcmp(reason, "SAFE_AGAIN_INVALID") == 0) {
        return "距离已恢复";
    }

    return reason;
}

static void distance_guard_create_deco_circle(lv_obj_t *parent,
                                              int x,
                                              int y,
                                              int size,
                                              uint32_t color_hex,
                                              lv_opa_t opa)
{
    if (parent == NULL) {
        return;
    }

    lv_obj_t *dot = lv_obj_create(parent);
    if (dot == NULL) {
        return;
    }

    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, opa, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *distance_guard_create_small_card(lv_obj_t *parent,
                                                  int x,
                                                  int y,
                                                  int w,
                                                  int h,
                                                  uint32_t bg_hex,
                                                  uint32_t border_hex)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        return NULL;
    }

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, 245, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_hex), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    return obj;
}

static void distance_guard_set_label(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(label, text);
}

static void distance_guard_update_info(const airtouch_guard_state_t *state)
{
    if (state == NULL || !state->initialized) {
        distance_guard_set_label(s_title, "距离保护");
        distance_guard_set_label(s_info, "正在等待距离保护数据");
        distance_guard_set_label(s_distance_value, "当前距离\n-- mm");
        distance_guard_set_label(s_reason_value, "保护状态\n等待中");
        distance_guard_set_label(s_hint, "请保持合适观看距离");
        return;
    }

    const char *reason_text = distance_guard_reason_text(state->reason);

    if (state->guard_on) {
        distance_guard_set_label(s_title, "请保持安全距离");
        distance_guard_set_label(s_info, "AirTouch 已启动护眼距离保护");

        if (s_distance_value != NULL) {
            lv_label_set_text_fmt(s_distance_value,
                                  "当前距离\n%u mm",
                                  (unsigned int)state->distance_mm);
        }

        if (s_reason_value != NULL) {
            lv_label_set_text_fmt(s_reason_value,
                                  "保护原因\n%s",
                                  reason_text);
        }

        distance_guard_set_label(s_hint,
                                 "请向后移动一点，恢复安全距离后系统会自动继续");
    } else {
        distance_guard_set_label(s_title, "距离已恢复");
        distance_guard_set_label(s_info, "正在恢复训练界面");

        if (s_distance_value != NULL) {
            lv_label_set_text_fmt(s_distance_value,
                                  "当前距离\n%u mm",
                                  (unsigned int)state->distance_mm);
        }

        if (s_reason_value != NULL) {
            lv_label_set_text_fmt(s_reason_value,
                                  "保护状态\n%s",
                                  reason_text);
        }

        distance_guard_set_label(s_hint, "可以继续训练");
    }
}

static void distance_guard_set_visible(bool visible, const airtouch_guard_state_t *state)
{
    if (s_overlay == NULL) {
        return;
    }

    if (s_visible == visible) {
        if (visible) {
            distance_guard_update_info(state);
            lv_obj_move_foreground(s_overlay);
        }
        return;
    }

    s_visible = visible;
    distance_guard_update_info(state);

    if (visible) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);

        ESP_LOGW(TAG,
                 "Distance guard ON from ESP8266: seq=%lu distance=%u reason=%s",
                 state ? (unsigned long)state->seq : 0UL,
                 state ? (unsigned int)state->distance_mm : 0U,
                 state ? state->reason : "NA");
    } else {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

        ESP_LOGI(TAG,
                 "Distance guard OFF from ESP8266: seq=%lu distance=%u reason=%s",
                 state ? (unsigned long)state->seq : 0UL,
                 state ? (unsigned int)state->distance_mm : 0U,
                 state ? state->reason : "NA");
    }
}

static void distance_guard_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    airtouch_guard_state_t state = {0};

    if (!airtouch_cloud_guard_get_state(&state)) {
        return;
    }

    if (s_has_last_seq && state.seq == s_last_seq) {
        if (s_visible) {
            distance_guard_update_info(&state);
            lv_obj_move_foreground(s_overlay);
        }
        return;
    }

    s_has_last_seq = true;
    s_last_seq = state.seq;

    distance_guard_set_visible(state.guard_on, &state);
}

esp_err_t air_distance_guard_ui_start(void)
{
    if (s_timer != NULL) {
        return ESP_OK;
    }

    lv_obj_t *scr = lv_scr_act();
    if (scr == NULL) {
        return ESP_FAIL;
    }

    s_overlay = lv_obj_create(scr);
    if (s_overlay == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0xDFF3FF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_overlay, lv_color_hex(0xFFF5DF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_overlay, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_overlay, 0, LV_PART_MAIN);

    distance_guard_create_deco_circle(s_overlay, 78, 58, 34, 0xFFE58A, 210);
    distance_guard_create_deco_circle(s_overlay, 178, 128, 18, 0x9FD6FF, 210);
    distance_guard_create_deco_circle(s_overlay, 820, 70, 42, 0xD8C2FF, 180);
    distance_guard_create_deco_circle(s_overlay, 900, 418, 28, 0xB8F2C8, 195);
    distance_guard_create_deco_circle(s_overlay, 104, 462, 24, 0xFFB6D5, 190);
    distance_guard_create_deco_circle(s_overlay, 736, 510, 18, 0xFFE58A, 210);

    s_card = lv_obj_create(s_overlay);
    if (s_card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 790, 410);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_card, 34, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_card, 246, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0xB8DDF4), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_card, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_card, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_card, 45, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s_card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *badge = lv_obj_create(s_card);
    if (badge != NULL) {
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 122, 122);
        lv_obj_set_pos(badge, 58, 48);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0xFFE58A), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(badge, lv_color_hex(0xFFB86B), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(badge, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);

        /*
         * v2m.2: cartoon exclamation icon.
         * Use LVGL shapes instead of a text glyph, so the icon is visually centered
         * and looks softer / more child-friendly on the real LCD.
         */
        lv_obj_t *mark_bar = lv_obj_create(badge);
        if (mark_bar != NULL) {
            lv_obj_remove_style_all(mark_bar);
            lv_obj_set_size(mark_bar, 24, 58);
            lv_obj_set_pos(mark_bar, 49, 18);
            lv_obj_set_style_radius(mark_bar, 14, LV_PART_MAIN);
            lv_obj_set_style_bg_color(mark_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(mark_bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(mark_bar, 8, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(mark_bar, 42, LV_PART_MAIN);
            lv_obj_set_style_shadow_ofs_y(mark_bar, 3, LV_PART_MAIN);
            lv_obj_clear_flag(mark_bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(mark_bar, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *mark_dot = lv_obj_create(badge);
        if (mark_dot != NULL) {
            lv_obj_remove_style_all(mark_dot);
            lv_obj_set_size(mark_dot, 26, 26);
            lv_obj_set_pos(mark_dot, 48, 84);
            lv_obj_set_style_radius(mark_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(mark_dot, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(mark_dot, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(mark_dot, 8, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(mark_dot, 42, LV_PART_MAIN);
            lv_obj_set_style_shadow_ofs_y(mark_dot, 3, LV_PART_MAIN);
            lv_obj_clear_flag(mark_dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(mark_dot, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    s_title = lv_label_create(s_card);
    if (s_title != NULL) {
        lv_obj_set_width(s_title, 520);
        lv_obj_set_pos(s_title, 214, 54);
        lv_obj_set_style_text_color(s_title, lv_color_hex(0x17324D), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_title, &ui_font_cn_32, LV_PART_MAIN);
        lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
        lv_label_set_text(s_title, "请保持安全距离");
    }

    s_info = lv_label_create(s_card);
    if (s_info != NULL) {
        lv_obj_set_width(s_info, 520);
        lv_obj_set_pos(s_info, 216, 116);
        lv_obj_set_style_text_color(s_info, lv_color_hex(0x45657F), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_info, &ui_font_cn_26, LV_PART_MAIN);
        lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
        lv_label_set_text(s_info, "AirTouch 已启动护眼距离保护");
    }

    lv_obj_t *dist_card = distance_guard_create_small_card(s_card,
                                                           72,
                                                           206,
                                                           300,
                                                           106,
                                                           0xEAF7FF,
                                                           0x9FD6FF);
    if (dist_card != NULL) {
        s_distance_value = lv_label_create(dist_card);
        if (s_distance_value != NULL) {
            lv_obj_set_width(s_distance_value, 260);
            lv_obj_set_pos(s_distance_value, 22, 18);
            lv_obj_set_style_text_color(s_distance_value, lv_color_hex(0x17324D), LV_PART_MAIN);
            lv_obj_set_style_text_font(s_distance_value, &ui_font_cn_26, LV_PART_MAIN);
            lv_obj_set_style_text_align(s_distance_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_label_set_text(s_distance_value, "当前距离\n-- mm");
        }
    }

    lv_obj_t *reason_card = distance_guard_create_small_card(s_card,
                                                             418,
                                                             206,
                                                             300,
                                                             106,
                                                             0xFFF4DD,
                                                             0xFFD28A);
    if (reason_card != NULL) {
        s_reason_value = lv_label_create(reason_card);
        if (s_reason_value != NULL) {
            lv_obj_set_width(s_reason_value, 260);
            lv_obj_set_pos(s_reason_value, 22, 18);
            lv_obj_set_style_text_color(s_reason_value, lv_color_hex(0x6A4218), LV_PART_MAIN);
            lv_obj_set_style_text_font(s_reason_value, &ui_font_cn_26, LV_PART_MAIN);
            lv_obj_set_style_text_align(s_reason_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_label_set_text(s_reason_value, "保护状态\n等待中");
        }
    }

    s_hint = lv_label_create(s_card);
    if (s_hint != NULL) {
        lv_obj_set_width(s_hint, 690);
        lv_obj_set_pos(s_hint, 50, 344);
        lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5B7890), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_hint, &ui_font_cn_26, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
        lv_label_set_text(s_hint, "请向后移动一点，恢复安全距离后系统会自动继续");
    }

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(distance_guard_timer_cb, AIR_DISTANCE_GUARD_TIMER_MS, NULL);
    if (s_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "Distance guard UI v2m.2 started, source=ESP8266 GUARD UART, style=storybook cartoon-icon, timer=%d ms",
             AIR_DISTANCE_GUARD_TIMER_MS);

    return ESP_OK;
}
