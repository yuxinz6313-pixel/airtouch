#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "parent_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *root;

    lv_obj_t *input_label;
    lv_obj_t *status_label;

    parent_control_t *parent_ctrl;

    char input_buf[PARENT_PASSWORD_LEN + 1];
    uint8_t input_len;

    void (*on_unlocked)(void *user_data);
    void *user_data;
} lock_screen_t;

void lock_screen_show(
    lock_screen_t *lock,
    lv_obj_t *parent,
    parent_control_t *parent_ctrl,
    void (*on_unlocked)(void *user_data),
    void *user_data
);

#ifdef __cplusplus
}
#endif