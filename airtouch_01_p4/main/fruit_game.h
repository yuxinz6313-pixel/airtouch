#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "game_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRUIT_GAME_STATE_IDLE = 0,
    FRUIT_GAME_STATE_RUNNING,
    FRUIT_GAME_STATE_GAME_OVER,
} fruit_game_state_t;

typedef struct {
    bool active;
    bool sliced;

    /*
     * 是否是炸弹。
     * 普通水果：切中加分，漏掉扣生命。
     * 炸弹：切中扣生命，不切飞走不扣生命。
     */
    bool is_bomb;

    /*
     * 是否已经真正进入过游戏可视区域。
     * 只有普通水果进入屏幕后再从底部掉出，才算漏水果。
     */
    bool entered_play_area;

    int16_t x;
    int16_t y;

    int16_t vx;
    int16_t vy;

    uint16_t radius;
    uint16_t score_value;

    uint32_t color;

    uint32_t sliced_time_ms;
} fruit_t;

typedef struct {
    bool valid;
    int16_t x;
    int16_t y;
    uint32_t time_ms;
} blade_point_t;

typedef struct {
    fruit_game_state_t state;

    fruit_t fruits[FRUIT_MAX_COUNT];

    blade_point_t blade_points[BLADE_TRAIL_MAX_POINTS];
    uint8_t blade_count;

    uint32_t score;
    uint32_t start_time_ms;

    uint32_t duration_ms;

    uint32_t rng_state;

    uint8_t lives;
    uint8_t max_lives;

    uint16_t combo;
    uint16_t max_combo;

    uint32_t sliced_count;
    uint32_t missed_count;
    uint32_t bomb_hit_count;

    uint32_t level;
    uint32_t next_spawn_ms;
    uint32_t spawn_interval_ms;
    uint32_t last_slice_time_ms;
} fruit_game_t;

void fruit_game_init(fruit_game_t *game);
void fruit_game_start(fruit_game_t *game, uint32_t now_ms);
void fruit_game_reset(fruit_game_t *game);

void fruit_game_update(fruit_game_t *game, uint32_t now_ms, uint32_t dt_ms);

void fruit_game_add_blade_point(
    fruit_game_t *game,
    int16_t x,
    int16_t y,
    bool valid,
    uint32_t now_ms
);

fruit_game_state_t fruit_game_get_state(const fruit_game_t *game);
uint32_t fruit_game_get_score(const fruit_game_t *game);

uint32_t fruit_game_get_remaining_ms(const fruit_game_t *game, uint32_t now_ms);

uint8_t fruit_game_get_lives(const fruit_game_t *game);
uint8_t fruit_game_get_max_lives(const fruit_game_t *game);
uint16_t fruit_game_get_combo(const fruit_game_t *game);
uint16_t fruit_game_get_max_combo(const fruit_game_t *game);
uint32_t fruit_game_get_level(const fruit_game_t *game);
uint32_t fruit_game_get_sliced_count(const fruit_game_t *game);
uint32_t fruit_game_get_missed_count(const fruit_game_t *game);
uint32_t fruit_game_get_bomb_hit_count(const fruit_game_t *game);
uint32_t fruit_game_get_elapsed_ms(const fruit_game_t *game, uint32_t now_ms);

#ifdef __cplusplus
}
#endif