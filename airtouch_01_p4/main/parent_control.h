#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARENT_PASSWORD_LEN 4

typedef struct {
    bool enabled;
    bool locked;

    uint32_t start_ms;
    uint32_t limit_ms;

    char password[PARENT_PASSWORD_LEN + 1];
} parent_control_t;

void parent_control_init(parent_control_t *ctrl);

void parent_control_start(parent_control_t *ctrl, uint32_t limit_minutes, uint32_t now_ms);
void parent_control_start_seconds(parent_control_t *ctrl, uint32_t limit_seconds, uint32_t now_ms);
void parent_control_stop(parent_control_t *ctrl);

bool parent_control_is_enabled(const parent_control_t *ctrl);
bool parent_control_is_locked(const parent_control_t *ctrl);

bool parent_control_check_timeout(parent_control_t *ctrl, uint32_t now_ms);
void parent_control_lock(parent_control_t *ctrl);

bool parent_control_unlock(parent_control_t *ctrl, const char *password_input);
bool parent_control_set_password(parent_control_t *ctrl, const char *new_password);

uint32_t parent_control_get_limit_minutes(const parent_control_t *ctrl);
uint32_t parent_control_get_limit_seconds(const parent_control_t *ctrl);
uint32_t parent_control_get_remaining_ms(const parent_control_t *ctrl, uint32_t now_ms);

#ifdef __cplusplus
}
#endif