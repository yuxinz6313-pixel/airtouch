#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "memory_game.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *root;
    lv_obj_t *panel;

    lv_obj_t *title_label;
    lv_obj_t *move_label;
    lv_obj_t *match_label;
    lv_obj_t *time_label;
    lv_obj_t *status_label;

    lv_obj_t *card_objs[MEMORY_CARD_COUNT];
    lv_obj_t *card_labels[MEMORY_CARD_COUNT];

    memory_game_t *game;

    void (*on_back)(void *user_data);
    void *user_data;
} memory_ui_t;

void memory_ui_create(
    memory_ui_t *ui,
    lv_obj_t *parent,
    memory_game_t *game,
    void (*on_back)(void *user_data),
    void *user_data
);

void memory_ui_update(memory_ui_t *ui, uint32_t now_ms);

#ifdef __cplusplus
}
#endif