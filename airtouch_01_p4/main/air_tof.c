#include "air_tof.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOF_I2C_PORT        I2C_NUM_0
#define TOF_I2C_SCL_IO      GPIO_NUM_1
#define TOF_I2C_SDA_IO      GPIO_NUM_2
#define TOF_I2C_FREQ_HZ     100000
#define TOF_I2C_TIMEOUT_MS  100

#define VL53L0X_ADDR        0x29

#define SYSRANGE_START                              0x00
#define SYSTEM_SEQUENCE_CONFIG                      0x01
#define SYSTEM_INTERRUPT_CONFIG_GPIO                0x0A
#define SYSTEM_INTERRUPT_CLEAR                      0x0B
#define RESULT_INTERRUPT_STATUS                     0x13
#define RESULT_RANGE_STATUS                         0x14
#define MSRC_CONFIG_CONTROL                         0x60
#define FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT 0x44
#define GLOBAL_CONFIG_REF_EN_START_SELECT           0xB6
#define DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD         0x4E
#define DYNAMIC_SPAD_REF_EN_START_OFFSET            0x4F
#define GLOBAL_CONFIG_SPAD_ENABLES_REF_0            0xB0
#define GPIO_HV_MUX_ACTIVE_HIGH                     0x84
#define VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV           0x89

static const char *TAG = "AIR_TOF";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_tof_dev = NULL;
static TaskHandle_t s_tof_task = NULL;

static uint8_t s_stop_variable = 0;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static air_tof_state_t s_state = {
    .status = AIR_TOF_STATUS_STOPPED,
    .initialized = false,
    .valid = false,
    .distance_mm = 0,
    .raw_status = 0,
    .range_status = 0,
    .ambient = 0,
    .signal = 0,
    .sample_count = 0,
    .valid_count = 0,
    .invalid_count = 0,
    .last_error = ESP_OK,
};

static void air_tof_update_state(const air_tof_state_t *new_state)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state = *new_state;
    portEXIT_CRITICAL(&s_state_lock);
}

bool air_tof_get_state(air_tof_state_t *out_state)
{
    if (out_state == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_state_lock);
    *out_state = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    return true;
}

static esp_err_t vl_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};
    return i2c_master_transmit(s_tof_dev, tx, sizeof(tx), TOF_I2C_TIMEOUT_MS);
}

static esp_err_t vl_write_reg16(uint8_t reg, uint16_t value)
{
    uint8_t tx[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };
    return i2c_master_transmit(s_tof_dev, tx, sizeof(tx), TOF_I2C_TIMEOUT_MS);
}

static esp_err_t vl_write_multi(uint8_t reg, const uint8_t *data, size_t len)
{
    if (len > 15) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t tx[16];
    tx[0] = reg;
    memcpy(&tx[1], data, len);

    return i2c_master_transmit(s_tof_dev, tx, len + 1, TOF_I2C_TIMEOUT_MS);
}

static esp_err_t vl_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_tof_dev, &reg, 1, value, 1, TOF_I2C_TIMEOUT_MS);
}

static esp_err_t vl_read_reg16(uint8_t reg, uint16_t *value)
{
    uint8_t buf[2] = {0};

    esp_err_t ret = i2c_master_transmit_receive(s_tof_dev, &reg, 1, buf, 2, TOF_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    *value = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

static esp_err_t vl_read_multi(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_tof_dev, &reg, 1, data, len, TOF_I2C_TIMEOUT_MS);
}

static esp_err_t vl_update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value = 0;
    esp_err_t ret = vl_read_reg(reg, &value);
    if (ret != ESP_OK) {
        return ret;
    }

    value &= (uint8_t)(~clear_mask);
    value |= set_mask;

    return vl_write_reg(reg, value);
}

static esp_err_t vl_set_signal_rate_limit(float limit_mcps)
{
    if (limit_mcps < 0.0f || limit_mcps > 511.99f) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t fixed = (uint16_t)(limit_mcps * 128.0f);
    return vl_write_reg16(FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, fixed);
}

static esp_err_t vl_get_spad_info(uint8_t *count, bool *type_is_aperture)
{
    uint8_t tmp = 0;

    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x01), TAG, "spad 0x80 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "spad 0xFF failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x00), TAG, "spad 0x00 failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x06), TAG, "spad 0xFF 06 failed");
    ESP_RETURN_ON_ERROR(vl_read_reg(0x83, &tmp), TAG, "spad read 0x83 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x83, tmp | 0x04), TAG, "spad write 0x83 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x07), TAG, "spad 0xFF 07 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x81, 0x01), TAG, "spad 0x81 failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x01), TAG, "spad 0x80 2 failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(0x94, 0x6B), TAG, "spad 0x94 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x83, 0x00), TAG, "spad 0x83 zero failed");

    TickType_t start = xTaskGetTickCount();

    do {
        ESP_RETURN_ON_ERROR(vl_read_reg(0x83, &tmp), TAG, "spad wait read failed");

        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(500)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    } while (tmp == 0x00);

    ESP_RETURN_ON_ERROR(vl_write_reg(0x83, 0x01), TAG, "spad 0x83 one failed");

    ESP_RETURN_ON_ERROR(vl_read_reg(0x92, &tmp), TAG, "spad read 0x92 failed");

    *count = tmp & 0x7F;
    *type_is_aperture = ((tmp >> 7) & 0x01) != 0;

    ESP_RETURN_ON_ERROR(vl_write_reg(0x81, 0x00), TAG, "spad restore 0x81 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x06), TAG, "spad restore 0xFF 06 failed");
    ESP_RETURN_ON_ERROR(vl_read_reg(0x83, &tmp), TAG, "spad restore read 0x83 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x83, tmp & (uint8_t)(~0x04)), TAG, "spad restore write 0x83 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "spad restore 0xFF 01 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x01), TAG, "spad restore 0x00 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "spad restore 0xFF 00 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x00), TAG, "spad restore 0x80 failed");

    return ESP_OK;
}

static esp_err_t vl_perform_single_ref_calibration(uint8_t vhv_init_byte)
{
    ESP_RETURN_ON_ERROR(vl_write_reg(SYSRANGE_START, 0x01 | vhv_init_byte), TAG, "calibration start failed");

    TickType_t start = xTaskGetTickCount();
    uint8_t status = 0;

    do {
        ESP_RETURN_ON_ERROR(vl_read_reg(RESULT_INTERRUPT_STATUS, &status), TAG, "calibration status read failed");

        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(1000)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((status & 0x07) == 0);

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x01), TAG, "calibration clear failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(SYSRANGE_START, 0x00), TAG, "calibration stop failed");

    return ESP_OK;
}

static esp_err_t vl_static_init_sequence(void)
{
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x09, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x10, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x11, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x24, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x25, 0xFF), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x75, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x4E, 0x2C), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x48, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x30, 0x20), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x30, 0x09), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x54, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x31, 0x04), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x32, 0x03), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x40, 0x83), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x46, 0x25), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x60, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x27, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x50, 0x06), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x51, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x52, 0x96), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x56, 0x08), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x57, 0x30), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x61, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x62, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x64, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x65, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x66, 0xA0), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x22, 0x32), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x47, 0x14), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x49, 0xFF), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x4A, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x7A, 0x0A), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x7B, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x78, 0x21), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x23, 0x34), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x42, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x44, 0xFF), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x45, 0x26), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x46, 0x05), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x40, 0x40), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x0E, 0x06), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x20, 0x1A), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x43, 0x40), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x34, 0x03), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x35, 0x44), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x31, 0x04), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x4B, 0x09), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x4C, 0x05), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x4D, 0x04), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x44, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x45, 0x20), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x47, 0x08), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x48, 0x28), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x67, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x70, 0x04), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x71, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x72, 0xFE), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x76, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x77, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x0D, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x01, 0xF8), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x8E, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x01), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x00), TAG, "init");

    return ESP_OK;
}

static esp_err_t vl53l0x_init(void)
{
    uint8_t id_c0 = 0;
    uint8_t id_c1 = 0;
    uint8_t id_c2 = 0;

    ESP_RETURN_ON_ERROR(vl_read_reg(0xC0, &id_c0), TAG, "read ID C0 failed");
    ESP_RETURN_ON_ERROR(vl_read_reg(0xC1, &id_c1), TAG, "read ID C1 failed");
    ESP_RETURN_ON_ERROR(vl_read_reg(0xC2, &id_c2), TAG, "read ID C2 failed");

    ESP_LOGI(TAG, "VL53L0X ID: 0xC0=0x%02X, 0xC1=0x%02X, 0xC2=0x%02X", id_c0, id_c1, id_c2);

    if (id_c0 != 0xEE || id_c1 != 0xAA || id_c2 != 0x10) {
        ESP_LOGW(TAG, "Unexpected VL53L0X ID, continue anyway");
    }

    ESP_RETURN_ON_ERROR(vl_update_reg(VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV, 0x00, 0x01), TAG, "set 2V8 mode failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(0x88, 0x00), TAG, "init 0x88 failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x01), TAG, "init stop 0x80 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "init stop 0xFF failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x00), TAG, "init stop 0x00 failed");
    ESP_RETURN_ON_ERROR(vl_read_reg(0x91, &s_stop_variable), TAG, "read stop variable failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x01), TAG, "init stop restore 0x00 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "init stop restore 0xFF failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x00), TAG, "init stop restore 0x80 failed");

    ESP_LOGI(TAG, "stop_variable=0x%02X", s_stop_variable);

    ESP_RETURN_ON_ERROR(vl_update_reg(MSRC_CONFIG_CONTROL, 0x00, 0x12), TAG, "MSRC config failed");
    ESP_RETURN_ON_ERROR(vl_set_signal_rate_limit(0.25f), TAG, "signal rate limit failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_SEQUENCE_CONFIG, 0xFF), TAG, "sequence config failed");

    uint8_t spad_count = 0;
    bool spad_type_is_aperture = false;

    ESP_RETURN_ON_ERROR(vl_get_spad_info(&spad_count, &spad_type_is_aperture), TAG, "get SPAD info failed");

    ESP_LOGI(TAG, "SPAD info: count=%u, aperture=%s",
             (unsigned int)spad_count,
             spad_type_is_aperture ? "true" : "false");

    uint8_t ref_spad_map[6] = {0};
    ESP_RETURN_ON_ERROR(vl_read_multi(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6), TAG, "read SPAD map failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "SPAD cfg failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00), TAG, "SPAD cfg failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C), TAG, "SPAD cfg failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "SPAD cfg failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4), TAG, "SPAD cfg failed");

    uint8_t first_spad_to_enable = spad_type_is_aperture ? 12 : 0;
    uint8_t spads_enabled = 0;

    for (uint8_t i = 0; i < 48; i++) {
        if (i < first_spad_to_enable || spads_enabled == spad_count) {
            ref_spad_map[i / 8] &= (uint8_t)(~(1 << (i % 8)));
        } else if ((ref_spad_map[i / 8] >> (i % 8)) & 0x01) {
            spads_enabled++;
        }
    }

    ESP_RETURN_ON_ERROR(vl_write_multi(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6), TAG, "write SPAD map failed");

    ESP_RETURN_ON_ERROR(vl_static_init_sequence(), TAG, "static init sequence failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04), TAG, "interrupt config failed");
    ESP_RETURN_ON_ERROR(vl_update_reg(GPIO_HV_MUX_ACTIVE_HIGH, 0x10, 0x00), TAG, "GPIO polarity failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x01), TAG, "interrupt clear failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_SEQUENCE_CONFIG, 0xE8), TAG, "sequence config E8 failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_SEQUENCE_CONFIG, 0x01), TAG, "sequence config VHV failed");
    ESP_RETURN_ON_ERROR(vl_perform_single_ref_calibration(0x40), TAG, "VHV calibration failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_SEQUENCE_CONFIG, 0x02), TAG, "sequence config phase failed");
    ESP_RETURN_ON_ERROR(vl_perform_single_ref_calibration(0x00), TAG, "phase calibration failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_SEQUENCE_CONFIG, 0xE8), TAG, "sequence config restore failed");

    return ESP_OK;
}

static esp_err_t vl53l0x_read_single(uint16_t *distance_mm,
                                     uint8_t *raw_status,
                                     uint8_t *range_status,
                                     uint16_t *ambient,
                                     uint16_t *signal)
{
    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x01), TAG, "single 0x80 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x01), TAG, "single 0xFF failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x00), TAG, "single 0x00 failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x91, s_stop_variable), TAG, "single stop var failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x00, 0x01), TAG, "single 0x00 restore failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0xFF, 0x00), TAG, "single 0xFF restore failed");
    ESP_RETURN_ON_ERROR(vl_write_reg(0x80, 0x00), TAG, "single 0x80 restore failed");

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSRANGE_START, 0x01), TAG, "single start failed");

    TickType_t start = xTaskGetTickCount();
    uint8_t sysrange = 0;

    do {
        ESP_RETURN_ON_ERROR(vl_read_reg(SYSRANGE_START, &sysrange), TAG, "single wait sysrange failed");

        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(500)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    } while (sysrange & 0x01);

    start = xTaskGetTickCount();

    uint8_t interrupt_status = 0;

    do {
        ESP_RETURN_ON_ERROR(vl_read_reg(RESULT_INTERRUPT_STATUS, &interrupt_status), TAG, "single interrupt status failed");

        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(500)) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((interrupt_status & 0x07) == 0);

    uint8_t buf[12] = {0};
    ESP_RETURN_ON_ERROR(vl_read_multi(RESULT_RANGE_STATUS, buf, sizeof(buf)), TAG, "read result block failed");

    *raw_status = buf[0];
    *range_status = (buf[0] & 0x78) >> 3;
    *ambient = ((uint16_t)buf[6] << 8) | buf[7];
    *signal = ((uint16_t)buf[8] << 8) | buf[9];
    *distance_mm = ((uint16_t)buf[10] << 8) | buf[11];

    ESP_RETURN_ON_ERROR(vl_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x01), TAG, "single clear failed");

    return ESP_OK;
}

static esp_err_t air_tof_i2c_init(void)
{
    if (s_i2c_bus != NULL && s_tof_dev != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOF_I2C_PORT,
        .sda_io_num = TOF_I2C_SDA_IO,
        .scl_io_num = TOF_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = VL53L0X_ADDR,
        .scl_speed_hz = TOF_I2C_FREQ_HZ,
    };

    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_tof_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C init OK: SDA=IO%d, SCL=IO%d, FREQ=%dHz",
             TOF_I2C_SDA_IO,
             TOF_I2C_SCL_IO,
             TOF_I2C_FREQ_HZ);

    return ESP_OK;
}

static void air_tof_task(void *arg)
{
    (void)arg;

    air_tof_state_t local = {0};
    local.status = AIR_TOF_STATUS_INITING;
    local.last_error = ESP_OK;
    air_tof_update_state(&local);

    ESP_LOGI(TAG, "Air ToF service start");
    ESP_LOGI(TAG, "Wiring: VIN=3.3V, GND=GND, SCL=IO1, SDA=IO2, XSHUT=3.3V or floating");

    esp_err_t ret = air_tof_i2c_init();
    if (ret != ESP_OK) {
        local.status = AIR_TOF_STATUS_ERROR;
        local.last_error = ret;
        air_tof_update_state(&local);
        ESP_LOGE(TAG, "Air ToF I2C init failed: %s", esp_err_to_name(ret));
        s_tof_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ret = vl53l0x_init();
    if (ret != ESP_OK) {
        local.status = AIR_TOF_STATUS_ERROR;
        local.last_error = ret;
        air_tof_update_state(&local);
        ESP_LOGE(TAG, "VL53L0X init failed: %s", esp_err_to_name(ret));
        s_tof_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    local.initialized = true;
    local.status = AIR_TOF_STATUS_OK;
    local.last_error = ESP_OK;
    air_tof_update_state(&local);

    ESP_LOGI(TAG, "VL53L0X init OK");

    while (1) {
        uint16_t distance = 0;
        uint8_t raw_status = 0;
        uint8_t range_status = 0;
        uint16_t ambient = 0;
        uint16_t signal = 0;

        ret = vl53l0x_read_single(&distance, &raw_status, &range_status, &ambient, &signal);

        local.sample_count++;
        local.last_error = ret;
        local.raw_status = raw_status;
        local.range_status = range_status;
        local.ambient = ambient;
        local.signal = signal;
        local.distance_mm = distance;

        bool valid = false;

        if (ret == ESP_OK) {
            if (distance > 20 && distance < 2000 && distance != 8190 && distance != 8191) {
                valid = true;
            }
        }

        local.valid = valid;

        if (valid) {
            local.status = AIR_TOF_STATUS_OK;
            local.valid_count++;
        } else {
            local.status = AIR_TOF_STATUS_INVALID;
            local.invalid_count++;
        }

        air_tof_update_state(&local);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "distance=%u mm, valid=%d, raw_status=0x%02X, status=%u, ambient=%u, signal=%u, samples=%lu, valid=%lu, invalid=%lu",
                     (unsigned int)distance,
                     valid ? 1 : 0,
                     (unsigned int)raw_status,
                     (unsigned int)range_status,
                     (unsigned int)ambient,
                     (unsigned int)signal,
                     (unsigned long)local.sample_count,
                     (unsigned long)local.valid_count,
                     (unsigned long)local.invalid_count);
        } else {
            ESP_LOGW(TAG,
                     "read failed: %s, samples=%lu",
                     esp_err_to_name(ret),
                     (unsigned long)local.sample_count);
        }

        vTaskDelay(pdMS_TO_TICKS(800));
    }
}

esp_err_t air_tof_start(void)
{
    if (s_tof_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        air_tof_task,
        "air_tof",
        6 * 1024,
        NULL,
        5,
        &s_tof_task,
        0
    );

    if (ok != pdPASS) {
        s_tof_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
