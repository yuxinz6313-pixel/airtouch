#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "parent_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PARENT_UI_PAGE_MAIN = 0,
    PARENT_UI_PAGE_CUSTOM_TIME,
    PARENT_UI_PAGE_PASSWORD,
} parent_ui_page_t;

typedef enum {
    PARENT_UI_PWD_ACTION_START = 0,
    PARENT_UI_PWD_ACTION_CLOSE,
} parent_ui_pwd_action_t;

typedef struct {
    lv_obj_t *root;

    lv_obj_t *status_label;
    lv_obj_t *remain_label;

    lv_obj_t *hour_roller;
    lv_obj_t *min_roller;
    lv_obj_t *sec_roller;

    lv_obj_t *selected_time_label;
    lv_obj_t *pwd_input_label;
    lv_obj_t *pwd_status_label;

    char pwd_buf[PARENT_PASSWORD_LEN + 1];
    uint8_t pwd_len;

    uint32_t pending_seconds;

    parent_ui_page_t page;
    parent_ui_pwd_action_t pwd_action;

    parent_control_t *parent_ctrl;

    void (*on_back)(void *user_data);
    void *user_data;
} parent_ui_t;

void parent_ui_create(
    parent_ui_t *ui,
    lv_obj_t *parent,
    parent_control_t *parent_ctrl,
    void (*on_back)(void *user_data),
    void *user_data
);

void parent_ui_update(parent_ui_t *ui, uint32_t now_ms);

#ifdef __cplusplus
}
#endif