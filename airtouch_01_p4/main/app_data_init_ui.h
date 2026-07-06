#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_data_init_ui_show(
    lv_obj_t *parent,
    void (*on_back)(void *user_data),
    void *back_user_data,
    void (*on_reset_confirmed)(void *user_data),
    void *reset_user_data
);

#ifdef __cplusplus
}
#endif