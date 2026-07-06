#pragma once

#include "lvgl.h"
#include "fruit_game.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *root;
    fruit_game_t *game;

    lv_obj_t *title_label;
    lv_obj_t *score_label;
    lv_obj_t *time_label;
    lv_obj_t *input_label;
    lv_obj_t *state_label;

    lv_obj_t *start_btn;
    lv_obj_t *start_btn_label;

    /*
     * 扣血反馈。
     */
    lv_obj_t *miss_overlay;
    lv_obj_t *miss_label;
    uint32_t last_missed_count;
    uint32_t last_bomb_hit_count;
    uint32_t miss_flash_start_ms;
    bool miss_flash_is_bomb;

    /*
     * 完整水果对象。
     */
    lv_obj_t *fruit_objs[FRUIT_MAX_COUNT];

    /*
     * 炸弹标记。
     */
    lv_obj_t *bomb_mark_labels[FRUIT_MAX_COUNT];
    lv_obj_t *bomb_spark_objs[FRUIT_MAX_COUNT];

    /*
     * 切开后的两块水果。
     * 竖切时作为左右两块。
     * 横切时作为上下两块。
     */
    lv_obj_t *fruit_left_objs[FRUIT_MAX_COUNT];
    lv_obj_t *fruit_right_objs[FRUIT_MAX_COUNT];

    lv_obj_t *blade_line;
    lv_obj_t *blade_tip;

    lv_point_t blade_lv_points[BLADE_TRAIL_MAX_POINTS];
} fruit_ui_t;

void fruit_ui_create(fruit_ui_t *ui, lv_obj_t *parent, fruit_game_t *game);
void fruit_ui_update(fruit_ui_t *ui, const fruit_game_t *game, uint32_t now_ms);

#ifdef __cplusplus
}
#endif