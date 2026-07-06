#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t id;
    int x;
    int y;
    int w;
    int h;
} air_input_target_t;

typedef struct {
    uint16_t id;
    int cx;
    int cy;
    int r;
} air_input_circle_target_t;

typedef struct {
    uint32_t dwell_ms;
    uint32_t cooldown_ms;
    int stable_radius_px;
    int target_expand_px;
} air_input_config_t;

typedef struct {
    bool pointer_valid;
    int x;
    int y;

    bool target_inside;
    bool hovering;
    bool click;
    bool cooldown;

    uint16_t target_id;

    /*
     * 0~1000
     * 0 means no hover progress.
     * 1000 means hover click is ready.
     */
    uint16_t hover_progress;

    uint32_t timestamp_ms;
} air_input_state_t;

void air_input_init(void);

void air_input_set_config(const air_input_config_t *config);
void air_input_get_config(air_input_config_t *config);

bool air_input_update_target(const air_input_target_t *target,
                             air_input_state_t *out_state);

bool air_input_update_circle_target(const air_input_circle_target_t *target,
                                    air_input_state_t *out_state);

bool air_input_update_circle_targets(const air_input_circle_target_t *targets,
                                     int target_count,
                                     air_input_state_t *out_state);

bool air_input_get_state(air_input_state_t *out_state);

#ifdef __cplusplus
}
#endif