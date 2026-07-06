#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t air_cursor_ui_start(void);

void air_cursor_ui_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif