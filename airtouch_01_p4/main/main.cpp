/*
 * Fruit Slice main with AirTouch Aim Trainer test.
 *
 * Debug isolate version:
 *   1. Start original LVGL display.
 *   2. Do NOT start fruit_slice_app_start().
 *   3. Use dark screen background.
 *   4. Start AirTouch 3-target aim trainer UI.
 *   5. Start AirTouch cursor overlay.
 *   6. Start camera service in background.
 *
 * Purpose:
 *   Verify AirTouch hover-click game interaction before connecting final games.
 */

#include <stdio.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "app_audio.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "app_camera.h"
#include "air_cursor_ui.h"
#include "air_input.h"
#include "air_aim_trainer_ui.h"
#include "airtouch_storage.h"
#include "airtouch_cloud_net.h"
#include "airtouch_cloud_uart.h"
#include "air_tof.h"
#include "air_distance_guard_ui.h"
#include "air_attention_heatmap.h"

#define LVGL_PORT_INIT_CONFIG()   \
    {                             \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,  \
        .task_affinity = -1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

static const char *TAG = "fruit_slice_main";


static void airtouch_audio_init_task_v1a(void *arg)
{
    (void)arg;

    /*
     * SFX v1a:
     * 延迟启动音频，避免和 LCD / SD / Camera / UART 启动抢资源。
     */
    vTaskDelay(pdMS_TO_TICKS(2500));

    esp_err_t ret = app_audio_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AirTouch SFX v1a audio init OK");
        app_audio_play_success();
    } else {
        ESP_LOGW(TAG, "AirTouch SFX v1a audio init failed: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL);
}

static void airtouch_audio_start_init_task_v1a(void)
{
    BaseType_t ret = xTaskCreate(
        airtouch_audio_init_task_v1a,
        "audio_init_v1a",
        4096,
        NULL,
        3,
        NULL
    );

    if (ret != pdPASS) {
        ESP_LOGW(TAG, "AirTouch SFX v1a create audio init task failed");
    }
}
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Start Fruit Slice AirTouch aim trainer black-screen test");

    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg =
            {
                .hdmi_resolution = BSP_HDMI_RES_NONE,
                .dsi_bus =
                    {
                        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
                    },
            },
        .flags =
            {
                .buff_dma = false,
                .buff_spiram = true,
                .sw_rotate = false,
            },
    };

    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    assert(disp != NULL);

    bsp_display_backlight_on();

    ESP_LOGI(TAG, "LCD / LVGL init done");


    if (bsp_display_lock(0)) {
        lv_obj_t *scr = lv_scr_act();

        lv_obj_clean(scr);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        air_input_init();
	air_attention_heatmap_init();
        /*
         * Aim trainer UI first.
         */
        bool sd_ok = airtouch_storage_init();
        if (sd_ok) {
            ESP_LOGI("fruit_slice_main", "AirTouch SD storage initialized");
        ESP_LOGI("fruit_slice_main", "Cloud-B v2c: SD ready, init ESP8266 UART for real record upload");

        if (airtouch_cloud_uart_init()) {
            ESP_LOGI(TAG, "Cloud-B v2c: ESP8266 UART ready, real records will upload after training");
        airtouch_storage_cloud_start_replay_task();
        } else {
            ESP_LOGW(TAG, "Cloud-B v2c: ESP8266 UART init failed");
        }
        } else {
            ESP_LOGW("fruit_slice_main", "AirTouch SD storage unavailable, keep cloud upload disabled");
        }

        air_aim_trainer_ui_start();

        esp_err_t guard_ret = air_distance_guard_ui_start();

        if (guard_ret != ESP_OK) {
            ESP_LOGE(TAG, "Distance guard UI start failed: %s", esp_err_to_name(guard_ret));
        } else {
            ESP_LOGI(TAG, "Distance guard UI started");
        }

        /*
         * Cursor overlay last, so the red cursor stays above target objects.
         */
        esp_err_t cursor_ret = air_cursor_ui_start();

        if (cursor_ret != ESP_OK) {
            ESP_LOGE(TAG, "AirTouch cursor UI start failed: %s", esp_err_to_name(cursor_ret));
        } else {
            ESP_LOGI(TAG, "AirTouch cursor UI started");
    /* AirTouch SFX v1c.1: audio init moved to before ToF start. */
        }

        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock display");
        return;
    }

    esp_err_t cam_ret = app_camera_start();

    if (cam_ret != ESP_OK) {
        ESP_LOGE(TAG, "AirTouch camera start failed: %s", esp_err_to_name(cam_ret));
    } else {
        ESP_LOGI(TAG, "AirTouch camera started");
    }

    /*
     * AirTouch SFX v1c.1:
     * Initialize WM8960 before ToF.
     * WM8960 writes codec registers through I2C0 once, releases I2C0,
     * then ToF can safely start on I2C0.
     */
    ESP_LOGI(TAG, "AirTouch SFX v1c.1: init audio before ToF");
    esp_err_t audio_ret = app_audio_init();
    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "AirTouch SFX v1c.1 audio init OK, I2C0 released for ToF");
        app_audio_play_success();
    } else {
        ESP_LOGW(TAG, "AirTouch SFX v1c.1 audio init failed: %s", esp_err_to_name(audio_ret));
    }

    esp_err_t tof_ret = air_tof_start();

    if (tof_ret != ESP_OK) {
        ESP_LOGE(TAG, "Air ToF service start failed: %s", esp_err_to_name(tof_ret));
    } else {
        ESP_LOGI(TAG, "Air ToF service started");
    }

    while (1) {
        ESP_LOGI(TAG,
                 "Running AirTouch aim trainer test, camera_frames=%lu, free PSRAM=%u KB",
                 (unsigned long)app_camera_get_frame_count(),
                 (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}









