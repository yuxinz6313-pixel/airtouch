#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_camera_start(void);

uint32_t app_camera_get_frame_count(void);

#ifdef __cplusplus
}
#endif