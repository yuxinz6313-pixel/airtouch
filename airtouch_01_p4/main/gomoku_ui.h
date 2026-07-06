#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "lvgl.h"
#include "gomoku_game.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GOMOKU_STONE_MAX_COUNT
#define GOMOKU_STONE_MAX_COUNT (GOMOKU_BOARD_SIZE * GOMOKU_BOARD_SIZE)
#endif

typedef enum {
    GOMOKU_MODE_PVP = 0,
    GOMOKU_MODE_PVC_EASY,
    GOMOKU_MODE_PVC_NORMAL,
    GOMOKU_MODE_PVC_HARD,
} gomoku_mode_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *board_panel;

    lv_obj_t *title_label;
    lv_obj_t *turn_label;
    lv_obj_t *status_label;
    lv_obj_t *hint_label;

    lv_obj_t *mode_btn;
    lv_obj_t *mode_btn_label;

    lv_obj_t *reset_btn;
    lv_obj_t *back_btn;

    lv_obj_t *grid_lines[GOMOKU_BOARD_SIZE * 2];
    lv_point_t grid_points[GOMOKU_BOARD_SIZE * 2][2];

    lv_obj_t *stone_objs[GOMOKU_STONE_MAX_COUNT];

    gomoku_game_t *game;
    gomoku_mode_t mode;

    void (*on_back)(void *user_data);
    void *user_data;
} gomoku_ui_t;

void gomoku_ui_create(
    gomoku_ui_t *ui,
    lv_obj_t *parent,
    gomoku_game_t *game,
    void (*on_back)(void *user_data),
    void *user_data
);

void gomoku_ui_update(gomoku_ui_t *ui, const gomoku_game_t *game);
void gomoku_ui_set_mode(gomoku_ui_t *ui, gomoku_mode_t mode);

#ifdef __cplusplus
}
#endif