#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIR_TOF_STATUS_STOPPED = 0,
    AIR_TOF_STATUS_INITING,
    AIR_TOF_STATUS_OK,
    AIR_TOF_STATUS_INVALID,
    AIR_TOF_STATUS_ERROR,
} air_tof_status_t;

typedef struct {
    air_tof_status_t status;
    bool initialized;
    bool valid;
    uint16_t distance_mm;
    uint8_t raw_status;
    uint8_t range_status;
    uint16_t ambient;
    uint16_t signal;
    uint32_t sample_count;
    uint32_t valid_count;
    uint32_t invalid_count;
    esp_err_t last_error;
} air_tof_state_t;

esp_err_t air_tof_start(void);
bool air_tof_get_state(air_tof_state_t *out_state);

#ifdef __cplusplus
}
#endif