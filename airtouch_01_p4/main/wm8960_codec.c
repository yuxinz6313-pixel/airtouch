#include "wm8960_codec.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"

#include "driver/i2c_master.h"

/*
 * 正式工程方案 A：
 * WM8960 使用独立 I2C 总线，避开 GT911 触摸 I2C。
 *
 * 当前接线：
 * WM8960 VCC    -> 3V3
 * WM8960 GND    -> GND
 * WM8960 SDA    -> IO46
 * WM8960 SCL    -> IO48
 *
 * WM8960 CLK    -> IO32
 * WM8960 WS     -> IO33
 * WM8960 RXSDA  -> IO36
 *
 * TXMCLK / RXMCLK 不接。
 * ESP32-P4 不输出 MCLK。
 */

#define WM8960_I2C_PORT          I2C_NUM_0
#define WM8960_I2C_SDA_IO        46
#define WM8960_I2C_SCL_IO        48
#define WM8960_I2C_FREQ_HZ       50000
#define WM8960_I2C_TIMEOUT_MS    300

#define WM8960_I2C_ADDR          0x1A
#define WM8960_PROBE_RETRY       10
#define WM8960_WRITE_RETRY       12

static const char *TAG = "WM8960_CODEC";

static bool s_i2c_inited = false;
static bool s_codec_inited = false;

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_wm8960_dev = NULL;

typedef struct {
    uint8_t reg;
    uint16_t val;
    uint32_t delay_ms;
    const char *name;
} wm8960_reg_cfg_t;

static esp_err_t wm8960_i2c_init(void)
{
    if (s_i2c_inited) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = WM8960_I2C_PORT,
        .sda_io_num = WM8960_I2C_SDA_IO,
        .scl_io_num = WM8960_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = WM8960_I2C_ADDR,
        .scl_speed_hz = WM8960_I2C_FREQ_HZ,
    };

    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_wm8960_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_i2c_inited = true;

    ESP_LOGI(TAG, "I2C master init OK: PORT=%d, SDA=IO%d, SCL=IO%d, FREQ=%dHz",
             WM8960_I2C_PORT,
             WM8960_I2C_SDA_IO,
             WM8960_I2C_SCL_IO,
             WM8960_I2C_FREQ_HZ);

    return ESP_OK;
}

static esp_err_t wm8960_probe_once(void)
{
    if (s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_probe(
        s_i2c_bus,
        WM8960_I2C_ADDR,
        WM8960_I2C_TIMEOUT_MS
    );
}

static esp_err_t wm8960_probe_retry(void)
{
    esp_err_t last_ret = ESP_FAIL;

    for (int i = 1; i <= WM8960_PROBE_RETRY; i++) {
        last_ret = wm8960_probe_once();

        if (last_ret == ESP_OK) {
            ESP_LOGI(TAG, "WM8960 found at 0x%02X, probe %d/%d",
                     WM8960_I2C_ADDR,
                     i,
                     WM8960_PROBE_RETRY);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "WM8960 probe failed %d/%d: %s",
                 i,
                 WM8960_PROBE_RETRY,
                 esp_err_to_name(last_ret));

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "WM8960 not found at 0x%02X", WM8960_I2C_ADDR);
    return last_ret;
}

static esp_err_t wm8960_write_reg_once(uint8_t reg, uint16_t val)
{
    if (s_wm8960_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * WM8960 写寄存器格式：
     * byte0 = 7 位寄存器地址左移 1 位 + 9 位数据最高位
     * byte1 = 9 位数据低 8 位
     */
    uint8_t data[2];
    data[0] = (uint8_t)((reg << 1) | ((val >> 8) & 0x01));
    data[1] = (uint8_t)(val & 0xFF);

    return i2c_master_transmit(
        s_wm8960_dev,
        data,
        sizeof(data),
        WM8960_I2C_TIMEOUT_MS
    );
}

static esp_err_t wm8960_write_reg_retry(uint8_t reg, uint16_t val, const char *name)
{
    esp_err_t last_ret = ESP_FAIL;

    for (int i = 1; i <= WM8960_WRITE_RETRY; i++) {
        last_ret = wm8960_write_reg_once(reg, val);

        if (last_ret == ESP_OK) {
            ESP_LOGI(TAG, "write OK: reg=0x%02X val=0x%03X %s",
                     reg,
                     val,
                     name ? name : "");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "write failed %d/%d: reg=0x%02X val=0x%03X %s err=%s",
                 i,
                 WM8960_WRITE_RETRY,
                 reg,
                 val,
                 name ? name : "",
                 esp_err_to_name(last_ret));

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "write finally failed: reg=0x%02X val=0x%03X %s err=%s",
             reg,
             val,
             name ? name : "",
             esp_err_to_name(last_ret));

    return last_ret;
}

static esp_err_t wm8960_write_sequence(const wm8960_reg_cfg_t *seq, size_t count)
{
    if (seq == NULL || count == 0) {
        return ESP_OK;
    }

    for (size_t i = 0; i < count; i++) {
        esp_err_t ret = wm8960_write_reg_retry(seq[i].reg, seq[i].val, seq[i].name);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "init stopped at step %u: %s",
                     (unsigned)i,
                     seq[i].name ? seq[i].name : "unknown");
            return ret;
        }

        if (seq[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(seq[i].delay_ms));
        }
    }

    return ESP_OK;
}


static void wm8960_i2c_release_after_init_v1c1(void)
{
    /*
     * AirTouch SFX v1c.1:
     * WM8960 only needs I2C during register configuration.
     * After codec init, audio samples are sent through I2S.
     * Release I2C0 so ToF can initialize on I2C0 later.
     */
    if (s_wm8960_dev != NULL) {
        esp_err_t ret = i2c_master_bus_rm_device(s_wm8960_dev);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "remove WM8960 I2C device failed: %s", esp_err_to_name(ret));
        }
        s_wm8960_dev = NULL;
    }

    if (s_i2c_bus != NULL) {
        esp_err_t ret = i2c_del_master_bus(s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "delete WM8960 I2C bus failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "WM8960 I2C bus released after codec init");
        }
        s_i2c_bus = NULL;
    }

    s_i2c_inited = false;
}

esp_err_t wm8960_codec_init(void)
{
    if (s_codec_inited) {
        return ESP_OK;
    }

    esp_err_t ret = wm8960_i2c_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = wm8960_probe_retry();
    if (ret != ESP_OK) {
        wm8960_i2c_release_after_init_v1c1();
        return ret;
    }

    static const wm8960_reg_cfg_t init_seq[] = {
        {0x0F, 0x000, 300, "Reset"},

        {0x19, 0x0C0, 300, "Power Management 1: VMID/VREF"},
        {0x1A, 0x1F8, 150, "Power Management 2: DAC + HP + SPK"},
        {0x2F, 0x00C, 150, "Power Management 3: output mixer"},

        {0x07, 0x002, 80, "Audio Interface: I2S, 16-bit, slave"},
        {0x04, 0x000, 80, "Clocking 1"},

        {0x05, 0x000, 80, "DAC Control 1: unmute"},
        {0x06, 0x000, 80, "DAC Control 2"},

        {0x0A, 0x1E8, 80, "Left DAC volume"},
        {0x0B, 0x1E8, 80, "Right DAC volume"},

        {0x22, 0x100, 80, "Left DAC to left mixer"},
        {0x25, 0x100, 80, "Right DAC to right mixer"},

        {0x02, 0x178, 80, "LOUT1 headphone volume"},
        {0x03, 0x178, 80, "ROUT1 headphone volume"},

        {0x31, 0x0F7, 150, "Class-D speaker enable"},
        {0x33, 0x11B, 150, "Speaker DC/AC gain"},

        {0x28, 0x178, 80, "Left speaker volume"},
        {0x29, 0x178, 80, "Right speaker volume"},
    };

    ret = wm8960_write_sequence(init_seq, sizeof(init_seq) / sizeof(init_seq[0]));
    if (ret != ESP_OK) {
        return ret;
    }

    s_codec_inited = true;

    ESP_LOGI(TAG, "WM8960 codec init OK");
    wm8960_i2c_release_after_init_v1c1();
    return ESP_OK;
}