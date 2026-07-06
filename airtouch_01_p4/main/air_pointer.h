#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;

    /*
     * Raw center in reduced mask space.
     * Mainly for debug.
     */
    int16_t raw_x;
    int16_t raw_y;

    /*
     * Final mapped screen coordinate.
     */
    int16_t screen_x;
    int16_t screen_y;

    /*
     * Largest connected-component area in mask space.
     */
    uint16_t area;

    /*
     * 0~100
     */
    uint8_t confidence;

    uint32_t timestamp_ms;
} air_pointer_result_t;

esp_err_t air_pointer_init(void);

void air_pointer_process_rgb565(const uint8_t *camera_buf,
                                uint32_t width,
                                uint32_t height,
                                size_t camera_buf_len);

/*
 * Raw result, without AI gate.
 * Used internally by AI side-channel inference.
 */
bool air_pointer_get_latest_raw(air_pointer_result_t *out_result);

/*
 * Public result, with AI confirmation gate.
 */
bool air_pointer_get_latest(air_pointer_result_t *out_result);

#ifdef __cplusplus
}
#endif