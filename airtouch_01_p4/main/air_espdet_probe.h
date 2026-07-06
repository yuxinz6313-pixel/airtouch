#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t air_espdet_probe_start(void);

esp_err_t air_espdet_probe_process_rgb565_once(uint8_t *camera_buf,
                                               uint32_t camera_w,
                                               uint32_t camera_h,
                                               size_t camera_buf_len);

/*
 * AI confirmation gate for AirTouch pointer.
 * raw_x/raw_y are display-oriented reduced mask coordinates from air_pointer_result_t.
 */
bool air_espdet_probe_ai_accept_recent_for_pointer(int16_t raw_x,
                                                   int16_t raw_y);

#ifdef __cplusplus
}
#endif
