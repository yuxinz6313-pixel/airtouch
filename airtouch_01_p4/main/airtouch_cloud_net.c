#include "airtouch_cloud_net.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_hosted.h"

#define AIRTOUCH_WIFI_SSID "H"
#define AIRTOUCH_WIFI_PASS "12345678"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "airtouch_cloud";

static bool s_hosted_attempted = false;
static bool s_hosted_ready = false;

static bool s_wifi_task_started = false;
static bool s_wifi_init_done = false;
static bool s_wifi_ready = false;
static int s_wifi_retry = 0;

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;

esp_err_t airtouch_cloud_net_hosted_probe(void)
{
    if (s_hosted_attempted) {
        return s_hosted_ready ? ESP_OK : ESP_FAIL;
    }

    s_hosted_attempted = true;

    ESP_LOGI(TAG, "Cloud v1b: ESP-Hosted probe begin");
    ESP_LOGI(TAG, "Cloud v1b: calling esp_hosted_init()");

    esp_err_t ret = esp_hosted_init();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Cloud v1b: esp_hosted_init failed: %s",
                 esp_err_to_name(ret));
        s_hosted_ready = false;
        return ret;
    }

    s_hosted_ready = true;

    ESP_LOGI(TAG, "Cloud v1b: ESP-Hosted probe OK");

    return ESP_OK;
}

bool airtouch_cloud_net_hosted_ready(void)
{
    return s_hosted_ready;
}

bool airtouch_cloud_net_wifi_ready(void)
{
    return s_wifi_ready;
}

static void airtouch_wifi_event_handler(void *arg,
                                        esp_event_base_t event_base,
                                        int32_t event_id,
                                        void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Cloud v1c: WIFI_EVENT_STA_START, connecting...");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ready = false;

        if (s_wifi_retry < 5) {
            s_wifi_retry++;
            ESP_LOGW(TAG,
                     "Cloud v1c: WiFi disconnected, retry=%d",
                     s_wifi_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Cloud v1c: WiFi connect failed after retries");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_wifi_retry = 0;
        s_wifi_ready = true;

        ESP_LOGI(TAG,
                 "Cloud v1c: got ip: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        return;
    }
}

static esp_err_t airtouch_cloud_wifi_sta_init(void)
{
    if (s_wifi_init_done) {
        return ESP_OK;
    }

    if (!s_hosted_ready) {
        esp_err_t hosted_ret = airtouch_cloud_net_hosted_probe();
        if (hosted_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "Cloud v1c: hosted not ready: %s",
                     esp_err_to_name(hosted_ret));
            return hosted_ret;
        }
    }

    ESP_LOGI(TAG, "Cloud v1c: WiFi STA init begin");
    ESP_LOGI(TAG, "Cloud v1c: target SSID: %s", AIRTOUCH_WIFI_SSID);

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Cloud v1c: create WiFi event group failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Cloud v1c: esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG,
                 "Cloud v1c: esp_event_loop_create_default failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "Cloud v1c: create default WiFi STA netif failed");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Cloud v1c: esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &airtouch_wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Cloud v1c: register WIFI_EVENT handler ret=%s", esp_err_to_name(ret));
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &airtouch_wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Cloud v1c: register IP_EVENT handler ret=%s", esp_err_to_name(ret));
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));

    strncpy((char *)wifi_config.sta.ssid,
            AIRTOUCH_WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);

    strncpy((char *)wifi_config.sta.password,
            AIRTOUCH_WIFI_PASS,
            sizeof(wifi_config.sta.password) - 1);

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cloud v1c: esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cloud v1c: esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "Cloud v1c: esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_wifi_init_done = true;

    ESP_LOGI(TAG, "Cloud v1c: WiFi STA init done");

    return ESP_OK;
}

static void airtouch_cloud_wifi_task(void *arg)
{
    ESP_LOGI(TAG, "Cloud v1c: WiFi connect task started");

    esp_err_t ret = airtouch_cloud_wifi_sta_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Cloud v1c: WiFi STA init failed: %s",
                 esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Cloud v1c: WiFi connected");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Cloud v1c: WiFi failed");
    } else {
        ESP_LOGW(TAG, "Cloud v1c: WiFi connect timeout");
    }

    vTaskDelete(NULL);
}

esp_err_t airtouch_cloud_net_start_wifi_test(void)
{
    if (s_wifi_task_started) {
        ESP_LOGW(TAG, "Cloud v1c: WiFi task already started");
        return ESP_OK;
    }

    s_wifi_task_started = true;

    BaseType_t ok = xTaskCreate(airtouch_cloud_wifi_task,
                                "air_cloud_wifi",
                                8192,
                                NULL,
                                5,
                                NULL);

    if (ok != pdPASS) {
        s_wifi_task_started = false;
        ESP_LOGE(TAG, "Cloud v1c: create WiFi task failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Cloud v1c: WiFi test task created");

    return ESP_OK;
}
