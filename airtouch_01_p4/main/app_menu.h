#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_fruit_slice)(void *user_data);
    void (*on_gomoku)(void *user_data);
    void (*on_memory_game)(void *user_data);
    void (*on_settings)(void *user_data);
    void (*on_parent_mode)(void *user_data);
    void (*on_usage_data)(void *user_data);
    void (*on_data_init)(void *user_data);
    void *user_data;
} app_menu_callbacks_t;

void app_menu_show_main(lv_obj_t *parent, const app_menu_callbacks_t *callbacks);

void app_menu_show_settings(
    lv_obj_t *parent,
    void (*on_back)(void *user_data),
    void *user_data
);

void app_menu_show_usage_data(
    lv_obj_t *parent,
    void (*on_back)(void *user_data),
    void *user_data
);

void app_menu_show_data_init(
    lv_obj_t *parent,
    void (*on_back)(void *user_data),
    void *back_user_data,
    void (*on_reset_confirmed)(void *user_data),
    void *reset_user_data
);

#ifdef __cplusplus
}
#endif