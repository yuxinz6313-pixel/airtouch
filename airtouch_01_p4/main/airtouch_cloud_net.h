#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t airtouch_cloud_net_hosted_probe(void);
bool airtouch_cloud_net_hosted_ready(void);

esp_err_t airtouch_cloud_net_start_wifi_test(void);
bool airtouch_cloud_net_wifi_ready(void);

#ifdef __cplusplus
}
#endif
