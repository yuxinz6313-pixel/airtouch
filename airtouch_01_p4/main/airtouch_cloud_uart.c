#include "airtouch_cloud_uart.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"

// 第一版临时引脚：后面如果实际接线不方便，只改这里
#define AIRTOUCH_CLOUD_UART_NUM       UART_NUM_1
#define AIRTOUCH_CLOUD_UART_TX_GPIO   GPIO_NUM_4
#define AIRTOUCH_CLOUD_UART_RX_GPIO   GPIO_NUM_5
#define AIRTOUCH_CLOUD_UART_BAUD      9600
#define AIRTOUCH_CLOUD_UART_BUF_SIZE  2048

static const char *TAG = "airtouch_cloud_uart";

// ================= AirTouch GUARD RX v2l =================
// ESP8266 ToF guard bridge sends:
//   GUARD,<seq>,<guard_on>,<distance_mm>,<reason>
// Example:
//   GUARD,3,1,249,TOO_CLOSE
//   GUARD,4,0,8191,SAFE_AGAIN

static airtouch_guard_state_t s_airtouch_guard_state_v2l = {0};

bool airtouch_cloud_guard_get_state(airtouch_guard_state_t *out)
{
    if (out == NULL) {
        return false;
    }

    if (!s_airtouch_guard_state_v2l.initialized) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    *out = s_airtouch_guard_state_v2l;
    return true;
}

static bool airtouch_cloud_parse_guard_line_v2l(const char *line)
{
    if (line == NULL) {
        return false;
    }

    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
        line++;
    }

    if (strncmp(line, "GUARD,", 6) != 0) {
        return false;
    }

    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *rn = strpbrk(buf, "\r\n");
    if (rn != NULL) {
        *rn = '\0';
    }

    char *save = NULL;
    char *tok_guard = strtok_r(buf, ",", &save);
    char *tok_seq = strtok_r(NULL, ",", &save);
    char *tok_on = strtok_r(NULL, ",", &save);
    char *tok_mm = strtok_r(NULL, ",", &save);
    char *tok_reason = strtok_r(NULL, ",", &save);

    if (tok_guard == NULL || tok_seq == NULL || tok_on == NULL || tok_mm == NULL) {
        ESP_LOGW(TAG, "AirTouch GUARD v2l malformed line: %s", line);
        return true;
    }

    uint32_t seq = (uint32_t)strtoul(tok_seq, NULL, 10);
    int guard_on_i = (int)strtol(tok_on, NULL, 10);
    uint16_t mm = (uint16_t)strtoul(tok_mm, NULL, 10);
    const char *reason = (tok_reason != NULL) ? tok_reason : "NA";

    s_airtouch_guard_state_v2l.initialized = true;
    s_airtouch_guard_state_v2l.guard_on = (guard_on_i != 0);
    s_airtouch_guard_state_v2l.seq = seq;
    s_airtouch_guard_state_v2l.distance_mm = mm;
    s_airtouch_guard_state_v2l.updated_ms = (int64_t)(esp_timer_get_time() / 1000);

    strncpy(s_airtouch_guard_state_v2l.reason, reason, sizeof(s_airtouch_guard_state_v2l.reason) - 1);
    s_airtouch_guard_state_v2l.reason[sizeof(s_airtouch_guard_state_v2l.reason) - 1] = '\0';

    ESP_LOGW(TAG,
             "AirTouch GUARD v2l RX: seq=%lu guard_on=%d distance=%u mm reason=%s",
             (unsigned long)s_airtouch_guard_state_v2l.seq,
             s_airtouch_guard_state_v2l.guard_on ? 1 : 0,
             (unsigned int)s_airtouch_guard_state_v2l.distance_mm,
             s_airtouch_guard_state_v2l.reason);

    return true;
}
// ================= AirTouch GUARD RX v2l END =================









/* Cloud-SD v2i forward declarations */
static void airtouch_cloud_uart_start_cfg_rx_task_v2gb(void);


static bool s_uart_ready = false;
static bool s_test_task_started = false;

/* Cloud-SD v2g-b CFG RX globals */
static bool s_cfg_rx_task_started_v2gb = false;
static volatile bool s_ack_waiting_v2gb = false;

static void airtouch_cloud_uart_start_cfg_rx_task_v2gb(void);
static void airtouch_cloud_uart_handle_cfg_line_v2gb(const char *line);

bool airtouch_cloud_uart_is_ready(void)
{
    return s_uart_ready;
}

bool airtouch_cloud_uart_init(void)
{
    if (s_uart_ready) {
        return true;
    }

    uart_config_t cfg = {
        .baud_rate = AIRTOUCH_CLOUD_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(AIRTOUCH_CLOUD_UART_NUM, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = uart_set_pin(AIRTOUCH_CLOUD_UART_NUM,
                       AIRTOUCH_CLOUD_UART_TX_GPIO,
                       AIRTOUCH_CLOUD_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = uart_driver_install(AIRTOUCH_CLOUD_UART_NUM,
                              AIRTOUCH_CLOUD_UART_BUF_SIZE,
                              AIRTOUCH_CLOUD_UART_BUF_SIZE,
                              0,
                              NULL,
                              0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return false;
    }

    uart_flush_input(AIRTOUCH_CLOUD_UART_NUM);

    s_uart_ready = true;

    ESP_LOGI(TAG,
             "Cloud-B UART ready: UART%d TX=GPIO%d RX=GPIO%d baud=%d",
             AIRTOUCH_CLOUD_UART_NUM,
             AIRTOUCH_CLOUD_UART_TX_GPIO,
             AIRTOUCH_CLOUD_UART_RX_GPIO,
             AIRTOUCH_CLOUD_UART_BAUD);

    airtouch_cloud_uart_start_cfg_rx_task_v2gb();

    return true;
}

/* --------------------------------------------------------------------------
 * Cloud-SD v2i CFG RX + CONFIG.TXT writer + CFGACK
 *
 * ESP8266 sends:
 *   CFG,version=1,star_target=64,...
 *
 * P4 behavior:
 *   1. Receive CFG line.
 *   2. Parse key=value parameters.
 *   3. Compare cloud config version with local CONFIG.TXT.
 *   4. If cloud version is newer, write CONFIG.TXT.
 *   5. Send CFGACK,version=N,ok=1/0 back to ESP8266.
 * -------------------------------------------------------------------------- */

#define AIRTOUCH_CONFIG_TXT_PATH_V2H "/sdcard/airtouch/users/child_001/CONFIG.TXT"

typedef struct {
    int version;

    int star_target;
    int star_dwell;
    int star_duration;
    int star_difficulty;
    int star_adaptive;

    int color_target;
    int color_dwell;
    int color_bubble;
    int color_nogo;
    int color_duration;
    int color_difficulty;
    int color_adaptive;

    int global_distance_guard;
    int global_difficulty_floor;
    int global_difficulty_ceiling;
} airtouch_cloud_config_v2h_t;

static bool airtouch_cloud_uart_starts_with_v2h(const char *s, const char *prefix)
{
    if (!s || !prefix) {
        return false;
    }

    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int airtouch_clamp_int_v2h(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static bool airtouch_find_key_value_v2h(const char *line,
                                        const char *key,
                                        const char **value_out)
{
    if (!line || !key || !value_out) {
        return false;
    }

    char pattern[64];

    int n = snprintf(pattern, sizeof(pattern), ",%s=", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return false;
    }

    const char *p = strstr(line, pattern);

    if (p) {
        *value_out = p + strlen(pattern);
        return true;
    }

    n = snprintf(pattern, sizeof(pattern), "%s=", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return false;
    }

    if (strncmp(line, pattern, strlen(pattern)) == 0) {
        *value_out = line + strlen(pattern);
        return true;
    }

    return false;
}

static int airtouch_cfg_get_int_v2h(const char *line, const char *key, int fallback)
{
    const char *value = NULL;

    if (!airtouch_find_key_value_v2h(line, key, &value)) {
        return fallback;
    }

    return atoi(value);
}

static void airtouch_cfg_default_v2h(airtouch_cloud_config_v2h_t *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->version = 0;

    cfg->star_target = 56;
    cfg->star_dwell = 336;
    cfg->star_duration = 45;
    cfg->star_difficulty = 2;
    cfg->star_adaptive = 1;

    cfg->color_target = 54;
    cfg->color_dwell = 520;
    cfg->color_bubble = 4;
    cfg->color_nogo = 25;
    cfg->color_duration = 45;
    cfg->color_difficulty = 2;
    cfg->color_adaptive = 1;

    cfg->global_distance_guard = 1;
    cfg->global_difficulty_floor = 1;
    cfg->global_difficulty_ceiling = 5;
}

static bool airtouch_cfg_parse_line_v2h(const char *line,
                                        airtouch_cloud_config_v2h_t *cfg)
{
    if (!line || !cfg) {
        return false;
    }

    // AirTouch GUARD v2k parse hook
    airtouch_cloud_parse_guard_line_v2l(line);
    if (strncmp(line, "CFG,", 4) != 0) {
        return false;
    }

    airtouch_cfg_default_v2h(cfg);

    cfg->version = airtouch_cfg_get_int_v2h(line, "version", 0);

    cfg->star_target = airtouch_cfg_get_int_v2h(line, "star_target", cfg->star_target);
    cfg->star_dwell = airtouch_cfg_get_int_v2h(line, "star_dwell", cfg->star_dwell);
    cfg->star_duration = airtouch_cfg_get_int_v2h(line, "star_duration", cfg->star_duration);
    cfg->star_difficulty = airtouch_cfg_get_int_v2h(line, "star_difficulty", cfg->star_difficulty);
    cfg->star_adaptive = airtouch_cfg_get_int_v2h(line, "star_adaptive", cfg->star_adaptive);

    cfg->color_target = airtouch_cfg_get_int_v2h(line, "color_target", cfg->color_target);
    cfg->color_dwell = airtouch_cfg_get_int_v2h(line, "color_dwell", cfg->color_dwell);
    cfg->color_bubble = airtouch_cfg_get_int_v2h(line, "color_bubble", cfg->color_bubble);
    cfg->color_nogo = airtouch_cfg_get_int_v2h(line, "color_nogo", cfg->color_nogo);
    cfg->color_duration = airtouch_cfg_get_int_v2h(line, "color_duration", cfg->color_duration);
    cfg->color_difficulty = airtouch_cfg_get_int_v2h(line, "color_difficulty", cfg->color_difficulty);
    cfg->color_adaptive = airtouch_cfg_get_int_v2h(line, "color_adaptive", cfg->color_adaptive);

    cfg->version = airtouch_clamp_int_v2h(cfg->version, 0, 999999);

    cfg->star_target = airtouch_clamp_int_v2h(cfg->star_target, 36, 90);
    cfg->star_dwell = airtouch_clamp_int_v2h(cfg->star_dwell, 250, 1200);
    cfg->star_duration = airtouch_clamp_int_v2h(cfg->star_duration, 30, 90);
    cfg->star_difficulty = airtouch_clamp_int_v2h(cfg->star_difficulty, 1, 5);
    cfg->star_adaptive = cfg->star_adaptive ? 1 : 0;

    cfg->color_target = airtouch_clamp_int_v2h(cfg->color_target, 36, 90);
    cfg->color_dwell = airtouch_clamp_int_v2h(cfg->color_dwell, 300, 1200);
    cfg->color_bubble = airtouch_clamp_int_v2h(cfg->color_bubble, 3, 8);
    cfg->color_nogo = airtouch_clamp_int_v2h(cfg->color_nogo, 10, 50);
    cfg->color_duration = airtouch_clamp_int_v2h(cfg->color_duration, 30, 90);
    cfg->color_difficulty = airtouch_clamp_int_v2h(cfg->color_difficulty, 1, 5);
    cfg->color_adaptive = cfg->color_adaptive ? 1 : 0;

    cfg->global_distance_guard = 1;
    cfg->global_difficulty_floor = 1;
    cfg->global_difficulty_ceiling = 5;

    return cfg->version > 0;
}

static int airtouch_cfg_read_current_version_v2h(void)
{
    FILE *f = fopen(AIRTOUCH_CONFIG_TXT_PATH_V2H, "r");

    if (!f) {
        return 0;
    }

    char line[160];
    int version = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "config_version=", strlen("config_version=")) == 0) {
            version = atoi(line + strlen("config_version="));
            break;
        }
    }

    fclose(f);
    return version;
}

static bool airtouch_cfg_write_config_txt_v2h(const airtouch_cloud_config_v2h_t *cfg)
{
    if (!cfg || cfg->version <= 0) {
        return false;
    }

    FILE *f = fopen(AIRTOUCH_CONFIG_TXT_PATH_V2H, "w");

    if (!f) {
        ESP_LOGE(TAG, "Cloud-SD v2i: open CONFIG.TXT failed: %s", AIRTOUCH_CONFIG_TXT_PATH_V2H);
        return false;
    }

    fprintf(f, "config_file_version=1\n");
    fprintf(f, "config_version=%d\n", cfg->version);
    fprintf(f, "cloud_config_enabled=1\n");
    fprintf(f, "updated_by=cloud_uart_v2i\n");
    fprintf(f, "\n");

    fprintf(f, "star_target_radius=%d\n", cfg->star_target);
    fprintf(f, "star_dwell_ms=%d\n", cfg->star_dwell);
    fprintf(f, "star_duration_s=%d\n", cfg->star_duration);
    fprintf(f, "star_difficulty=%d\n", cfg->star_difficulty);
    fprintf(f, "star_adaptive_enabled=%d\n", cfg->star_adaptive);
    fprintf(f, "\n");

    fprintf(f, "color_target_radius=%d\n", cfg->color_target);
    fprintf(f, "color_dwell_ms=%d\n", cfg->color_dwell);
    fprintf(f, "color_bubble_count=%d\n", cfg->color_bubble);
    fprintf(f, "color_nogo_ratio=%d\n", cfg->color_nogo);
    fprintf(f, "color_duration_s=%d\n", cfg->color_duration);
    fprintf(f, "color_difficulty=%d\n", cfg->color_difficulty);
    fprintf(f, "color_adaptive_enabled=%d\n", cfg->color_adaptive);
    fprintf(f, "\n");

    fprintf(f, "global_distance_guard_enabled=%d\n", cfg->global_distance_guard);
    fprintf(f, "global_difficulty_floor=%d\n", cfg->global_difficulty_floor);
    fprintf(f, "global_difficulty_ceiling=%d\n", cfg->global_difficulty_ceiling);

    fclose(f);

    ESP_LOGI(TAG,
             "Cloud-SD v2i: CONFIG.TXT written version=%d star(radius=%d dwell=%d dur=%d diff=%d adapt=%d) color(radius=%d dwell=%d bubble=%d nogo=%d dur=%d diff=%d adapt=%d)",
             cfg->version,
             cfg->star_target,
             cfg->star_dwell,
             cfg->star_duration,
             cfg->star_difficulty,
             cfg->star_adaptive,
             cfg->color_target,
             cfg->color_dwell,
             cfg->color_bubble,
             cfg->color_nogo,
             cfg->color_duration,
             cfg->color_difficulty,
             cfg->color_adaptive);

    return true;
}

static bool airtouch_cfg_apply_line_v2h(const char *line)
{
    airtouch_cloud_config_v2h_t cfg;

    if (!airtouch_cfg_parse_line_v2h(line, &cfg)) {
        ESP_LOGW(TAG, "Cloud-SD v2i: CFG parse failed: %s", line ? line : "(null)");
        return false;
    }

    const int current_version = airtouch_cfg_read_current_version_v2h();

    ESP_LOGI(TAG,
             "Cloud-SD v2i: CFG version check cloud=%d local=%d",
             cfg.version,
             current_version);

    if (cfg.version <= current_version) {
        ESP_LOGI(TAG,
                 "Cloud-SD v2i: skip CONFIG.TXT write, cloud version already applied or older");
        return true;
    }

    return airtouch_cfg_write_config_txt_v2h(&cfg);
}

static void airtouch_cloud_uart_send_cfgack_v2i(int version, bool ok)
{
    if (!s_uart_ready || version <= 0) {
        ESP_LOGW(TAG,
                 "Cloud-SD v2i: skip CFGACK, uart_ready=%d version=%d",
                 s_uart_ready ? 1 : 0,
                 version);
        return;
    }

    char line[96];

    int n = snprintf(line,
                     sizeof(line),
                     "CFGACK,version=%d,ok=%d\n",
                     version,
                     ok ? 1 : 0);

    if (n <= 0 || n >= (int)sizeof(line)) {
        ESP_LOGW(TAG, "Cloud-SD v2i: CFGACK line build failed");
        return;
    }

    int written = uart_write_bytes(AIRTOUCH_CLOUD_UART_NUM, line, n);

    if (written != n) {
        ESP_LOGW(TAG, "Cloud-SD v2i: CFGACK uart_write partial: %d/%d", written, n);
    }

    ESP_LOGI(TAG, "Cloud-SD v2i CFGACK TX to ESP8266: %s", line);
}

static void airtouch_cloud_uart_handle_cfg_line_v2gb(const char *line)
{
    if (!line || line[0] == '\0') {
        return;
    }

    if (airtouch_cloud_uart_starts_with_v2h(line, "CFG,")) {
        ESP_LOGI(TAG, "Cloud-SD v2i CFG received from ESP8266: %s", line);

        const int cfg_version = airtouch_cfg_get_int_v2h(line, "version", 0);
        const bool ok = airtouch_cfg_apply_line_v2h(line);

        if (ok) {
            ESP_LOGI(TAG, "Cloud-SD v2i: CFG apply/check done, version=%d", cfg_version);
        } else {
            ESP_LOGW(TAG, "Cloud-SD v2i: CFG apply failed, version=%d", cfg_version);
        }

        airtouch_cloud_uart_send_cfgack_v2i(cfg_version, ok);
    } else {
        // AirTouch GUARD v2k.1 idle non-CFG hook
        if (!airtouch_cloud_parse_guard_line_v2l(line)) {
        // AirTouch GUARD v2l idle RX hook
        if (!airtouch_cloud_parse_guard_line_v2l(line)) {
        ESP_LOGW(TAG, "Cloud-SD v2i idle RX non-CFG line: %s", line);
        }
        }
    }
}

bool airtouch_cloud_uart_send_json(uint32_t seq, const char *json, uint32_t timeout_ms)
{
    if (!s_uart_ready || json == NULL) {
        return false;
    }

    char line[1024];
    int n = snprintf(line,
                     sizeof(line),
                     "ATQ,%lu,%s\n",
                     (unsigned long)seq,
                     json);

    if (n <= 0 || n >= (int)sizeof(line)) {
        ESP_LOGW(TAG, "JSON line too long, seq=%lu", (unsigned long)seq);
        return false;
    }

    s_ack_waiting_v2gb = true;

    uart_flush_input(AIRTOUCH_CLOUD_UART_NUM);

    int written = uart_write_bytes(AIRTOUCH_CLOUD_UART_NUM, line, n);
    if (written != n) {
        ESP_LOGW(TAG, "uart_write_bytes partial: %d/%d", written, n);
    }

    ESP_LOGI(TAG, "TX to ESP8266: %s", line);

    char rx_line[512];
    int rx_len = 0;
    uint32_t elapsed = 0;
    bool got_any_line = false;
    bool ok = false;

    char expected[32];
    snprintf(expected, sizeof(expected), "ACK:%lu", (unsigned long)seq);

    while (elapsed < timeout_ms && rx_len < (int)sizeof(rx_line) - 1) {
        uint8_t ch = 0;
        int r = uart_read_bytes(AIRTOUCH_CLOUD_UART_NUM, &ch, 1, pdMS_TO_TICKS(50));

        if (r > 0) {
            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                rx_line[rx_len] = '\0';

                if (rx_len > 0) {
                    got_any_line = true;
                    ESP_LOGI(TAG, "RX from ESP8266: %s", rx_line);

                    if (airtouch_cloud_uart_starts_with_v2h(rx_line, "CFG,")) {
                        airtouch_cloud_uart_handle_cfg_line_v2gb(rx_line);
                        rx_len = 0;
                        continue;
                    }

                    if (strcmp(rx_line, expected) == 0) {
                        ESP_LOGI(TAG, "Cloud UART ACK OK, seq=%lu", (unsigned long)seq);
                        ok = true;
                        break;
                    }

                    ESP_LOGW(TAG,
                             "Cloud UART ACK mismatch, expected=%s got=%s",
                             expected,
                             rx_line);
                    ok = false;
                    break;
                }

                rx_len = 0;
                continue;
            }

            rx_line[rx_len++] = (char)ch;

            if (rx_len >= (int)sizeof(rx_line) - 1) {
                rx_line[rx_len] = '\0';
                ESP_LOGW(TAG, "RX line too long while waiting ACK: %s", rx_line);
                ok = false;
                break;
            }
        }

        elapsed += 50;
    }

    if (!got_any_line && !ok) {
        ESP_LOGW(TAG, "No ACK from ESP8266, seq=%lu", (unsigned long)seq);
    }

    s_ack_waiting_v2gb = false;

    return ok;
}

static void cloud_uart_cfg_rx_task_v2gb(void *arg)
{
    (void)arg;

    char line[512];
    int len = 0;

    ESP_LOGI(TAG, "Cloud-SD v2i CFG idle RX task started");

    while (1) {
        if (!s_uart_ready || s_ack_waiting_v2gb) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint8_t ch = 0;
        int r = uart_read_bytes(AIRTOUCH_CLOUD_UART_NUM, &ch, 1, pdMS_TO_TICKS(30));

        if (r > 0) {
            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                line[len] = '\0';

                if (len > 0) {
                    airtouch_cloud_uart_handle_cfg_line_v2gb(line);
                }

                len = 0;
                continue;
            }

            if (len < (int)sizeof(line) - 1) {
                line[len++] = (char)ch;
            } else {
                line[len] = '\0';
                ESP_LOGW(TAG, "Cloud-SD v2i idle RX line too long, drop: %s", line);
                len = 0;
            }
        }
    }
}

static void airtouch_cloud_uart_start_cfg_rx_task_v2gb(void)
{
    if (s_cfg_rx_task_started_v2gb) {
        return;
    }

    BaseType_t ok = xTaskCreate(cloud_uart_cfg_rx_task_v2gb,
                                "cloud_cfg_rx",
                                4096,
                                NULL,
                                4,
                                NULL);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Cloud-SD v2i CFG RX task");
        return;
    }

    s_cfg_rx_task_started_v2gb = true;
}
static void cloud_uart_test_task(void *arg)
{
    (void)arg;

    uint32_t seq = 1;

    vTaskDelay(pdMS_TO_TICKS(3000));

    while (1) {
        char json[512];

        snprintf(json, sizeof(json),
                 "{\"device_id\":\"airtouch_001\","
                 "\"user_id\":\"child_001\","
                 "\"source\":\"p4_cloud_b_v2b\","
                 "\"seq\":%lu,"
                 "\"type\":\"test\","
                 "\"rid\":%lu,"
                 "\"msg\":\"hello_esp8266\","
                 "\"fw\":\"p4_cloud_b_v2b\"}",
                 (unsigned long)seq,
                 (unsigned long)seq);

        bool ok = airtouch_cloud_uart_send_json(seq, json, 6000);

        ESP_LOGI(TAG, "Cloud-B v2b test send result: seq=%lu ok=%d",
                 (unsigned long)seq,
                 ok ? 1 : 0);

        seq++;

        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}

bool airtouch_cloud_uart_start_test_task(void)
{
    if (!s_uart_ready) {
        ESP_LOGW(TAG, "UART not ready, cannot start test task");
        return false;
    }

    if (s_test_task_started) {
        return true;
    }

    BaseType_t ok = xTaskCreate(cloud_uart_test_task,
                                "cloud_uart_test",
                                4096,
                                NULL,
                                5,
                                NULL);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cloud_uart_test task");
        return false;
    }

    s_test_task_started = true;
    ESP_LOGI(TAG, "Cloud-B v2b UART test task started");
    return true;
}






