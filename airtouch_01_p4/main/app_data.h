#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t boot_count;
    uint32_t total_usage_seconds;
    uint32_t current_boot_seconds;
    uint8_t volume_percent;
} app_system_usage_data_t;

/*
 * 本地数据管理模块。
 *
 * 第一阶段保存：
 * 1. 设备启动次数
 * 2. 累计使用时长
 * 3. 本次开机使用时长
 * 4. 音量百分比
 * 5. 数据初始化密码
 *
 * 数据保存位置：NVS
 */
esp_err_t app_data_init(void);
bool app_data_is_ready(void);

esp_err_t app_data_on_boot(uint32_t now_ms);
esp_err_t app_data_update_usage(uint32_t now_ms);
esp_err_t app_data_save_usage_now(void);

esp_err_t app_data_save_volume_percent(uint8_t percent);
uint8_t app_data_get_volume_percent(void);

void app_data_get_system_usage_data(app_system_usage_data_t *out, uint32_t now_ms);

bool app_data_verify_reset_password(const char *password);
esp_err_t app_data_set_reset_password(const char *password);
esp_err_t app_data_reset_all_to_default(void);

#ifdef __cplusplus
}
#endif