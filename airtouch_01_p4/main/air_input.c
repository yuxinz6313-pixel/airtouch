/*
 * AirTouch Input Layer
 *
 * This module converts low-level AirTouch pointer coordinates into
 * higher-level interaction states:
 *
 *     pointer valid / x / y
 *     hover progress
 *     hover click
 *
 * It supports both rectangular targets and circular targets.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"
#include "esp_log.h"

#include "air_pointer.h"
#include "air_input.h"

static const char *TAG = "air_input";

#define AIR_INPUT_STALE_MS 180

#define AIR_DEFAULT_DWELL_MS          800
#define AIR_DEFAULT_COOLDOWN_MS       600
#define AIR_DEFAULT_STABLE_RADIUS_PX  35
#define AIR_DEFAULT_TARGET_EXPAND_PX  10

static air_input_state_t s_state;

static air_input_config_t s_cfg;

static bool s_hover_active = false;
static uint16_t s_hover_target_id = 0;
static uint32_t s_hover_start_ms = 0;

static int s_anchor_x = -1;
static int s_anchor_y = -1;

static uint32_t s_last_click_ms = 0;

static inline uint32_t now_ms_air_input(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline int clamp_int_air_input(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static inline int sqr_int_air_input(int v)
{
    return v * v;
}

static void reset_hover(void)
{
    s_hover_active = false;
    s_hover_target_id = 0;
    s_hover_start_ms = 0;
    s_anchor_x = -1;
    s_anchor_y = -1;
}

static bool read_pointer(int *out_x, int *out_y)
{
    uint32_t now = now_ms_air_input();

    air_pointer_result_t pointer;
    memset(&pointer, 0, sizeof(pointer));

    bool has_pointer = air_pointer_get_latest(&pointer);

    if (!has_pointer || !pointer.valid) {
        return false;
    }

    uint32_t age = now - pointer.timestamp_ms;
    if (age > AIR_INPUT_STALE_MS) {
        return false;
    }

    if (out_x != NULL) {
        *out_x = clamp_int_air_input(pointer.screen_x, 0, 1023);
    }

    if (out_y != NULL) {
        *out_y = clamp_int_air_input(pointer.screen_y, 0, 599);
    }

    return true;
}

static void fill_pointer_only_state(bool pointer_valid,
                                    int px,
                                    int py,
                                    air_input_state_t *out_state)
{
    uint32_t now = now_ms_air_input();

    memset(&s_state, 0, sizeof(s_state));

    s_state.pointer_valid = pointer_valid;
    s_state.x = pointer_valid ? px : -1;
    s_state.y = pointer_valid ? py : -1;
    s_state.target_inside = false;
    s_state.hovering = false;
    s_state.click = false;
    s_state.cooldown = false;
    s_state.target_id = 0;
    s_state.hover_progress = 0;
    s_state.timestamp_ms = now;

    if (!pointer_valid) {
        reset_hover();
    }

    if (out_state != NULL) {
        *out_state = s_state;
    }
}

static bool point_in_target_expanded(int px,
                                     int py,
                                     const air_input_target_t *target)
{
    if (target == NULL) {
        return false;
    }

    int left = target->x - s_cfg.target_expand_px;
    int top = target->y - s_cfg.target_expand_px;
    int right = target->x + target->w + s_cfg.target_expand_px;
    int bottom = target->y + target->h + s_cfg.target_expand_px;

    return (px >= left && px <= right && py >= top && py <= bottom);
}

static bool point_in_circle_expanded(int px,
                                     int py,
                                     const air_input_circle_target_t *target)
{
    if (target == NULL) {
        return false;
    }

    int dx = px - target->cx;
    int dy = py - target->cy;
    int rr = target->r + s_cfg.target_expand_px;

    return (dx * dx + dy * dy) <= rr * rr;
}

static void update_hover_common(bool pointer_valid,
                                int px,
                                int py,
                                bool inside,
                                uint16_t target_id,
                                air_input_state_t *out_state)
{
    uint32_t now = now_ms_air_input();

    memset(&s_state, 0, sizeof(s_state));

    s_state.pointer_valid = pointer_valid;
    s_state.x = pointer_valid ? px : -1;
    s_state.y = pointer_valid ? py : -1;
    s_state.target_id = target_id;
    s_state.timestamp_ms = now;

    if (!pointer_valid) {
        reset_hover();

        s_state.target_inside = false;
        s_state.hovering = false;
        s_state.click = false;
        s_state.cooldown = false;
        s_state.hover_progress = 0;

        if (out_state != NULL) {
            *out_state = s_state;
        }

        return;
    }

    bool in_cooldown = false;
    if (s_last_click_ms != 0 && (now - s_last_click_ms) < s_cfg.cooldown_ms) {
        in_cooldown = true;
    }

    s_state.cooldown = in_cooldown;
    s_state.target_inside = inside;

    if (!inside || target_id == 0) {
        reset_hover();

        s_state.hovering = false;
        s_state.click = false;
        s_state.hover_progress = 0;

        if (out_state != NULL) {
            *out_state = s_state;
        }

        return;
    }

    if (in_cooldown) {
        reset_hover();

        s_state.hovering = false;
        s_state.click = false;
        s_state.hover_progress = 0;

        if (out_state != NULL) {
            *out_state = s_state;
        }

        return;
    }

    if (!s_hover_active || s_hover_target_id != target_id) {
        s_hover_active = true;
        s_hover_target_id = target_id;
        s_hover_start_ms = now;
        s_anchor_x = px;
        s_anchor_y = py;
    }

    int dx = px - s_anchor_x;
    int dy = py - s_anchor_y;
    int dist2 = sqr_int_air_input(dx) + sqr_int_air_input(dy);
    int radius2 = s_cfg.stable_radius_px * s_cfg.stable_radius_px;

    if (dist2 > radius2) {
        s_hover_start_ms = now;
        s_anchor_x = px;
        s_anchor_y = py;

        s_state.hovering = true;
        s_state.click = false;
        s_state.hover_progress = 0;

        if (out_state != NULL) {
            *out_state = s_state;
        }

        return;
    }

    uint32_t elapsed = now - s_hover_start_ms;

    int progress = (int)((elapsed * 1000U) / s_cfg.dwell_ms);
    progress = clamp_int_air_input(progress, 0, 1000);

    s_state.hovering = true;
    s_state.hover_progress = (uint16_t)progress;

    if (elapsed >= s_cfg.dwell_ms) {
        s_state.click = true;
        s_state.hover_progress = 1000;

        s_last_click_ms = now;

        ESP_LOGI(TAG, "AirInput CLICK target=%u x=%d y=%d",
                 target_id,
                 px,
                 py);

        reset_hover();
    } else {
        s_state.click = false;
    }

    if (out_state != NULL) {
        *out_state = s_state;
    }
}

void air_input_init(void)
{
    memset(&s_state, 0, sizeof(s_state));

    s_cfg.dwell_ms = AIR_DEFAULT_DWELL_MS;
    s_cfg.cooldown_ms = AIR_DEFAULT_COOLDOWN_MS;
    s_cfg.stable_radius_px = AIR_DEFAULT_STABLE_RADIUS_PX;
    s_cfg.target_expand_px = AIR_DEFAULT_TARGET_EXPAND_PX;

    s_state.pointer_valid = false;
    s_state.x = -1;
    s_state.y = -1;
    s_state.target_inside = false;
    s_state.hovering = false;
    s_state.click = false;
    s_state.cooldown = false;
    s_state.target_id = 0;
    s_state.hover_progress = 0;
    s_state.timestamp_ms = 0;

    reset_hover();
    s_last_click_ms = 0;

    ESP_LOGI(TAG,
             "AirInput init: dwell=%lu ms, cooldown=%lu ms, stable_radius=%d px, expand=%d px",
             (unsigned long)s_cfg.dwell_ms,
             (unsigned long)s_cfg.cooldown_ms,
             s_cfg.stable_radius_px,
             s_cfg.target_expand_px);
}

void air_input_set_config(const air_input_config_t *config)
{
    if (config == NULL) {
        return;
    }

    s_cfg.dwell_ms = config->dwell_ms;
    s_cfg.cooldown_ms = config->cooldown_ms;
    s_cfg.stable_radius_px = config->stable_radius_px;
    s_cfg.target_expand_px = config->target_expand_px;

    if (s_cfg.dwell_ms < 100) {
        s_cfg.dwell_ms = 100;
    }

    if (s_cfg.cooldown_ms < 50) {
        s_cfg.cooldown_ms = 50;
    }

    if (s_cfg.stable_radius_px < 5) {
        s_cfg.stable_radius_px = 5;
    }

    if (s_cfg.target_expand_px < 0) {
        s_cfg.target_expand_px = 0;
    }

    reset_hover();

    ESP_LOGI(TAG,
             "AirInput config updated: dwell=%lu ms, cooldown=%lu ms, stable_radius=%d px, expand=%d px",
             (unsigned long)s_cfg.dwell_ms,
             (unsigned long)s_cfg.cooldown_ms,
             s_cfg.stable_radius_px,
             s_cfg.target_expand_px);
}

void air_input_get_config(air_input_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = s_cfg;
}

bool air_input_update_target(const air_input_target_t *target,
                             air_input_state_t *out_state)
{
    int px = -1;
    int py = -1;

    bool pointer_valid = read_pointer(&px, &py);

    if (!pointer_valid) {
        fill_pointer_only_state(false, -1, -1, out_state);
        return false;
    }

    if (target == NULL) {
        fill_pointer_only_state(true, px, py, out_state);
        return true;
    }

    bool inside = point_in_target_expanded(px, py, target);

    update_hover_common(true,
                        px,
                        py,
                        inside,
                        inside ? target->id : 0,
                        out_state);

    return true;
}

bool air_input_update_circle_target(const air_input_circle_target_t *target,
                                    air_input_state_t *out_state)
{
    if (target == NULL) {
        return air_input_update_circle_targets(NULL, 0, out_state);
    }

    return air_input_update_circle_targets(target, 1, out_state);
}

bool air_input_update_circle_targets(const air_input_circle_target_t *targets,
                                     int target_count,
                                     air_input_state_t *out_state)
{
    int px = -1;
    int py = -1;

    bool pointer_valid = read_pointer(&px, &py);

    if (!pointer_valid) {
        fill_pointer_only_state(false, -1, -1, out_state);
        return false;
    }

    if (targets == NULL || target_count <= 0) {
        fill_pointer_only_state(true, px, py, out_state);
        return true;
    }

    int best_index = -1;
    int best_dist2 = 0x7FFFFFFF;

    for (int i = 0; i < target_count; i++) {
        const air_input_circle_target_t *t = &targets[i];

        if (t->id == 0 || t->r <= 0) {
            continue;
        }

        if (!point_in_circle_expanded(px, py, t)) {
            continue;
        }

        int dx = px - t->cx;
        int dy = py - t->cy;
        int dist2 = dx * dx + dy * dy;

        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            best_index = i;
        }
    }

    if (best_index < 0) {
        update_hover_common(true,
                            px,
                            py,
                            false,
                            0,
                            out_state);
        return true;
    }

    update_hover_common(true,
                        px,
                        py,
                        true,
                        targets[best_index].id,
                        out_state);

    return true;
}

bool air_input_get_state(air_input_state_t *out_state)
{
    if (out_state == NULL) {
        return false;
    }

    *out_state = s_state;
    return true;
}