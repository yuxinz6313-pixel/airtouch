#include "app_camera.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"

#include "bsp/esp-bsp.h"
#include "camera/app_video.h"

#include "air_pointer.h"
#include "air_espdet_probe.h"

static const char *TAG = "app_camera";

/*
 * OV5647 original frame size used by previous working camera demo.
 */
#define APP_CAMERA_H_RES 1280
#define APP_CAMERA_V_RES 960
#define APP_CAMERA_RGB565_BYTES_PER_PIXEL 2

#ifndef EXAMPLE_CAM_BUF_NUM
#define EXAMPLE_CAM_BUF_NUM 2
#endif

static bool s_camera_started = false;
static int s_video_fd = -1;

static uint8_t *s_cam_buffer[EXAMPLE_CAM_BUF_NUM] = {0};
static size_t s_cam_buffer_size[EXAMPLE_CAM_BUF_NUM] = {0};

static uint32_t s_frame_count = 0;

static void app_camera_frame_cb(uint8_t *camera_buf,
                                uint8_t camera_buf_index,
                                uint32_t camera_buf_hes,
                                uint32_t camera_buf_ves,
                                size_t camera_buf_len)
{
    s_frame_count++;

    /*
     * First AirTouch stage:
     * Do not touch LVGL here.
     * Do not display camera preview.
     * Only run blue-marker detection and print serial logs.
     */
    air_pointer_process_rgb565(
        camera_buf,
        camera_buf_hes,
        camera_buf_ves,
        camera_buf_len
    );
    if ((s_frame_count % 15) == 0) {
        esp_err_t espdet_ret = air_espdet_probe_process_rgb565_once(
            camera_buf,
            camera_buf_hes,
            camera_buf_ves,
            camera_buf_len
        );

        if (espdet_ret != ESP_OK && espdet_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "ESPDet probe infer failed: %s", esp_err_to_name(espdet_ret));
        }
    }


    if ((s_frame_count % 15) == 0) {
        ESP_LOGI(TAG,
                 "Camera frames=%lu, index=%d, size=%lux%lu, len=%u, free PSRAM=%u KB",
                 (unsigned long)s_frame_count,
                 camera_buf_index,
                 (unsigned long)camera_buf_hes,
                 (unsigned long)camera_buf_ves,
                 (unsigned int)camera_buf_len,
                 (unsigned int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

esp_err_t app_camera_start(void)
{
    if (s_camera_started) {
        ESP_LOGW(TAG, "Camera already started");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Start camera service for AirTouch");

    esp_err_t ret = air_pointer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "air_pointer_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "bsp_i2c_get_handle returned NULL");
        return ESP_ERR_INVALID_STATE;
    }
    ret = air_espdet_probe_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "air_espdet_probe_start failed: %s", esp_err_to_name(ret));
        return ret;
    }


    ESP_LOGI(TAG, "Init video subsystem");
    ret = app_video_main(i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_video_main failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Open camera device");
    s_video_fd = app_video_open((char *)EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT_RGB565);
    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "app_video_open failed");
        return ESP_FAIL;
    }

    size_t data_cache_line_size = 0;
    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_cache_get_alignment failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (data_cache_line_size == 0) {
        data_cache_line_size = 64;
    }

    const size_t frame_buf_size =
        APP_CAMERA_H_RES *
        APP_CAMERA_V_RES *
        APP_CAMERA_RGB565_BYTES_PER_PIXEL;

    ESP_LOGI(TAG,
             "Allocate camera frame buffers, count=%d, size=%u, align=%u",
             EXAMPLE_CAM_BUF_NUM,
             (unsigned int)frame_buf_size,
             (unsigned int)data_cache_line_size);

    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        s_cam_buffer[i] = (uint8_t *)heap_caps_aligned_alloc(
            data_cache_line_size,
            frame_buf_size,
            MALLOC_CAP_SPIRAM
        );

        if (s_cam_buffer[i] == NULL) {
            ESP_LOGE(TAG,
                     "Failed to allocate camera buffer %d, size=%u",
                     i,
                     (unsigned int)frame_buf_size);
            return ESP_ERR_NO_MEM;
        }

        s_cam_buffer_size[i] = frame_buf_size;
        memset(s_cam_buffer[i], 0, s_cam_buffer_size[i]);
    }

    ESP_LOGI(TAG, "Register camera frame callback");
    ret = app_video_register_frame_operation_cb(app_camera_frame_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "app_video_register_frame_operation_cb failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Set camera buffers");
    ret = app_video_set_bufs(
        s_video_fd,
        EXAMPLE_CAM_BUF_NUM,
        (const void **)s_cam_buffer
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_video_set_bufs failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Start camera stream task");
    ret = app_video_stream_task_start(s_video_fd, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_video_stream_task_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_camera_started = true;

    ESP_LOGI(TAG, "Camera service started");

    return ESP_OK;
}

uint32_t app_camera_get_frame_count(void)
{
    return s_frame_count;
}


