#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool airtouch_cloud_uart_init(void);
bool airtouch_cloud_uart_is_ready(void);
bool airtouch_cloud_uart_send_json(uint32_t seq, const char *json, uint32_t timeout_ms);
bool airtouch_cloud_uart_start_test_task(void);


typedef struct {
    bool initialized;
    bool guard_on;
    uint32_t seq;
    uint16_t distance_mm;
    int64_t updated_ms;
    char reason[24];
} airtouch_guard_state_t;

/**
 * Get latest guard state received from ESP8266 ToF guard bridge.
 * Returns true if at least one GUARD event has been received.
 */
bool airtouch_cloud_guard_get_state(airtouch_guard_state_t *out);

#ifdef __cplusplus
}
#endif
