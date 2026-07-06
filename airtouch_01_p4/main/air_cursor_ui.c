#include "air_cursor_ui.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include "air_pointer.h"
#include "air_tof.h"
#include "air_attention_heatmap.h"

static const char *TAG = "air_cursor_ui";

#define AIR_CURSOR_SIZE 28
#define AIR_CURSOR_TIMER_MS 30
#define AIR_CURSOR_STALE_MS 500

static lv_obj_t *s_cursor = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_started = false;
static bool s_enabled = true;

static void air_cursor_push_heatmap_sample(bool pointer_valid, const air_pointer_result_t *r)
{
    air_tof_state_t tof_state = {0};
    bool tof_ok = air_tof_get_state(&tof_state);

    uint16_t distance_mm = 0;
    bool distance_valid = false;

    if (tof_ok && tof_state.initialized && tof_state.valid) {
        distance_mm = tof_state.distance_mm;
        distance_valid = true;
    }

    if (!pointer_valid || r == NULL) {
        air_attention_heatmap_push_sample(
            false,
            -1,
            -1,
            0,
            0,
            distance_mm,
            distance_valid
        );
        return;
    }

    air_attention_heatmap_push_sample(
        true,
        r->screen_x,
        r->screen_y,
        r->confidence,
        r->area,
        distance_mm,
        distance_valid
    );
}

static void air_cursor_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_started || !s_enabled || s_cursor == NULL) {
        return;
    }

    air_pointer_result_t r = {0};
    bool valid = air_pointer_get_latest(&r);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (!valid || !r.valid || (now_ms - r.timestamp_ms > AIR_CURSOR_STALE_MS)) {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        air_cursor_push_heatmap_sample(false, NULL);
        return;
    }

    int x = r.screen_x;
    int y = r.screen_y;

    if (x < 0) {
        x = 0;
    }

    if (x > 1023) {
        x = 1023;
    }

    if (y < 0) {
        y = 0;
    }

    if (y > 599) {
        y = 599;
    }

    /*
     * 用限幅后的屏幕坐标喂给热力图，避免越界。
     */
    r.screen_x = x;
    r.screen_y = y;

    air_cursor_push_heatmap_sample(true, &r);

    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(
        s_cursor,
        x - AIR_CURSOR_SIZE / 2,
        y - AIR_CURSOR_SIZE / 2
    );

    lv_obj_move_foreground(s_cursor);
}

esp_err_t air_cursor_ui_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    lv_obj_t *parent = lv_layer_top();

    s_cursor = lv_obj_create(parent);

    if (s_cursor == NULL) {
        ESP_LOGE(TAG, "Failed to create AirTouch cursor");
        return ESP_FAIL;
    }

    lv_obj_set_size(s_cursor, AIR_CURSOR_SIZE, AIR_CURSOR_SIZE);
    lv_obj_set_style_radius(s_cursor, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_cursor, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_80, LV_PART_MAIN);

    lv_obj_set_style_border_color(s_cursor, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cursor, 3, LV_PART_MAIN);

    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(air_cursor_timer_cb, AIR_CURSOR_TIMER_MS, NULL);

    if (s_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create AirTouch cursor timer");
        lv_obj_del(s_cursor);
        s_cursor = NULL;
        return ESP_FAIL;
    }

    s_started = true;
    s_enabled = true;

    ESP_LOGI(TAG, "AirTouch cursor UI started");

    return ESP_OK;
}

void air_cursor_ui_set_enabled(bool enabled)
{
    s_enabled = enabled;

    if (!enabled && s_cursor != NULL) {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    }
}