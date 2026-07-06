#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "fruit_game.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;

    int16_t screen_x;
    int16_t screen_y;

    float confidence;

    uint32_t timestamp_ms;
} fruit_input_result_t;

typedef struct {
    /*
     * 自动演示输入状态。
     */
    int16_t fake_x;
    int16_t fake_y;
    uint8_t fake_target_index;

    /*
     * 触屏输入状态。
     */
    int16_t touch_x;
    int16_t touch_y;
    bool touch_pressed;
    uint32_t touch_last_ms;

    /*
     * 通用计数。
     */
    uint32_t update_count;
} fruit_input_t;

void fruit_input_init(fruit_input_t *input);

/*
 * 由 fruit_ui.c 的触摸事件调用。
 * 这样不依赖 lv_indev_get_state()，兼容当前 LVGL 版本。
 */
void fruit_input_set_touch_state(
    bool pressed,
    int16_t x,
    int16_t y,
    uint32_t now_ms
);

void fruit_input_clear_touch(uint32_t now_ms);

void fruit_input_update(
    fruit_input_t *input,
    const fruit_game_t *game,
    uint32_t now_ms,
    fruit_input_result_t *out_result
);

#ifdef __cplusplus
}
#endif