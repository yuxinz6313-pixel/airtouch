#include "app_audio.h"
#include "wm8960_codec.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"

/*
 * 当前正式工程接线：
 *
 * WM8960 VCC    -> 3V3
 * WM8960 GND    -> GND
 * WM8960 SDA    -> IO46
 * WM8960 SCL    -> IO48
 *
 * WM8960 CLK    -> IO32
 * WM8960 WS     -> IO33
 * WM8960 RXSDA  -> IO36
 *
 * WM8960 TXMCLK -> 不接
 * WM8960 RXMCLK -> 不接
 *
 * 注意：
 * 1. ESP32-P4 的 DOUT 接 WM8960 的 RXSDA，不是 TXSDA。
 * 2. 不使用 IO8 做 MCLK。
 * 3. 不输出 MCLK。
 * 4. IO36 已验证触摸和音频同时正常。
 */

#define APP_AUDIO_I2S_BCLK_IO        GPIO_NUM_32
#define APP_AUDIO_I2S_WS_IO          GPIO_NUM_33
#define APP_AUDIO_I2S_DOUT_IO        GPIO_NUM_36

#define APP_AUDIO_SAMPLE_RATE        16000
#define APP_AUDIO_FRAME_CHUNK        256
#define APP_AUDIO_QUEUE_LEN          16

#define APP_AUDIO_DEFAULT_VOLUME     75

static const char *TAG = "APP_AUDIO";

typedef enum {
    AUDIO_CMD_CLICK = 1,
    AUDIO_CMD_SLICE,
    AUDIO_CMD_SUCCESS,
    AUDIO_CMD_ERROR,
    AUDIO_CMD_WIN,
    AUDIO_CMD_LOCK,
    AUDIO_CMD_ROLLER,
    AUDIO_CMD_BOMB,
    AUDIO_CMD_TEST_SEQUENCE,
} audio_cmd_t;

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static QueueHandle_t s_audio_queue = NULL;
static TaskHandle_t s_audio_task = NULL;
static bool s_audio_ready = false;

static volatile uint8_t s_volume_percent = APP_AUDIO_DEFAULT_VOLUME;

/*
 * 16 点正弦表，基准幅度 12000。
 * 实际输出还会乘以全局音量百分比。
 */
static const int16_t s_sine_table[16] = {
    0,
    4592,
    8485,
    11086,
    12000,
    11086,
    8485,
    4592,
    0,
    -4592,
    -8485,
    -11086,
    -12000,
    -11086,
    -8485,
    -4592,
};

void app_audio_set_volume_percent(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    s_volume_percent = percent;
    ESP_LOGI(TAG, "volume set to %u%%", (unsigned int)s_volume_percent);
}

uint8_t app_audio_get_volume_percent(void)
{
    return s_volume_percent;
}

static int16_t app_audio_apply_volume(int32_t sample)
{
    int32_t volume = (int32_t)s_volume_percent;
    int32_t out = (sample * volume) / 100;

    if (out > 32767) {
        out = 32767;
    } else if (out < -32768) {
        out = -32768;
    }

    return (int16_t)out;
}

static esp_err_t app_audio_i2s_init(void)
{
    if (s_i2s_tx_chan != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = APP_AUDIO_FRAME_CHUNK;
    chan_cfg.auto_clear_after_cb = true;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL),
        TAG,
        "i2s_new_channel failed"
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(APP_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_AUDIO_I2S_BCLK_IO,
            .ws = APP_AUDIO_I2S_WS_IO,
            .dout = APP_AUDIO_I2S_DOUT_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg),
        TAG,
        "i2s_channel_init_std_mode failed"
    );

    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(s_i2s_tx_chan),
        TAG,
        "i2s_channel_enable failed"
    );

    ESP_LOGI(TAG, "I2S init OK: MCLK=UNUSED, BCLK=IO%d, WS=IO%d, DOUT=IO%d -> WM8960 RXSDA",
             APP_AUDIO_I2S_BCLK_IO,
             APP_AUDIO_I2S_WS_IO,
             APP_AUDIO_I2S_DOUT_IO);

    return ESP_OK;
}

static void app_audio_i2s_write_frames(const int16_t *data, size_t frame_count)
{
    if (s_i2s_tx_chan == NULL || data == NULL || frame_count == 0) {
        return;
    }

    size_t bytes_to_write = frame_count * 2 * sizeof(int16_t);
    size_t bytes_written = 0;

    esp_err_t ret = i2s_channel_write(
        s_i2s_tx_chan,
        data,
        bytes_to_write,
        &bytes_written,
        pdMS_TO_TICKS(1000)
    );

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(ret));
    }
}

static void app_audio_play_silence_ms(uint32_t duration_ms)
{
    int total_frames = (APP_AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    int16_t audio_buf[APP_AUDIO_FRAME_CHUNK * 2];

    memset(audio_buf, 0, sizeof(audio_buf));

    while (total_frames > 0) {
        int frames = total_frames;
        if (frames > APP_AUDIO_FRAME_CHUNK) {
            frames = APP_AUDIO_FRAME_CHUNK;
        }

        app_audio_i2s_write_frames(audio_buf, frames);
        total_frames -= frames;
    }
}

static void app_audio_play_tone_ms(uint32_t freq_hz, uint32_t duration_ms, int32_t amp)
{
    if (freq_hz == 0 || duration_ms == 0 || amp <= 0) {
        app_audio_play_silence_ms(duration_ms);
        return;
    }

    int total_frames = (APP_AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    int16_t audio_buf[APP_AUDIO_FRAME_CHUNK * 2];

    uint32_t phase = 0;
    uint32_t phase_step = freq_hz * 16;
    uint32_t phase_mod = APP_AUDIO_SAMPLE_RATE * 16;

    while (total_frames > 0) {
        int frames = total_frames;
        if (frames > APP_AUDIO_FRAME_CHUNK) {
            frames = APP_AUDIO_FRAME_CHUNK;
        }

        for (int i = 0; i < frames; i++) {
            uint32_t idx = phase / APP_AUDIO_SAMPLE_RATE;
            if (idx > 15) {
                idx = 15;
            }

            int32_t raw_sample = ((int32_t)s_sine_table[idx] * amp) / 12000;
            int16_t sample = app_audio_apply_volume(raw_sample);

            audio_buf[i * 2 + 0] = sample;
            audio_buf[i * 2 + 1] = sample;

            phase += phase_step;
            while (phase >= phase_mod) {
                phase -= phase_mod;
            }
        }

        app_audio_i2s_write_frames(audio_buf, frames);
        total_frames -= frames;
    }
}

static void app_audio_play_chirp_ms(uint32_t freq_start_hz, uint32_t freq_end_hz, uint32_t duration_ms, int32_t amp)
{
    if (duration_ms == 0 || amp <= 0) {
        return;
    }

    int total_frames = (APP_AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    int frames_left = total_frames;
    int frames_done = 0;
    int16_t audio_buf[APP_AUDIO_FRAME_CHUNK * 2];

    uint32_t phase = 0;
    uint32_t phase_mod = APP_AUDIO_SAMPLE_RATE * 16;

    while (frames_left > 0) {
        int frames = frames_left;
        if (frames > APP_AUDIO_FRAME_CHUNK) {
            frames = APP_AUDIO_FRAME_CHUNK;
        }

        for (int i = 0; i < frames; i++) {
            int current_frame = frames_done + i;
            uint32_t freq_hz = freq_start_hz;

            if (total_frames > 1) {
                int32_t diff = (int32_t)freq_end_hz - (int32_t)freq_start_hz;
                freq_hz = (uint32_t)((int32_t)freq_start_hz + (diff * current_frame) / total_frames);
            }

            uint32_t idx = phase / APP_AUDIO_SAMPLE_RATE;
            if (idx > 15) {
                idx = 15;
            }

            int32_t raw_sample = ((int32_t)s_sine_table[idx] * amp) / 12000;
            int16_t sample = app_audio_apply_volume(raw_sample);

            audio_buf[i * 2 + 0] = sample;
            audio_buf[i * 2 + 1] = sample;

            phase += freq_hz * 16;
            while (phase >= phase_mod) {
                phase -= phase_mod;
            }
        }

        app_audio_i2s_write_frames(audio_buf, frames);

        frames_done += frames;
        frames_left -= frames;
    }
}

static void app_audio_effect_click(void)
{
    app_audio_play_tone_ms(1600, 45, 10000);
    app_audio_play_silence_ms(20);
}

static void app_audio_effect_slice(void)
{
    app_audio_play_chirp_ms(700, 2400, 120, 11000);
    app_audio_play_silence_ms(30);
}

static void app_audio_effect_success(void)
{
    app_audio_play_tone_ms(1200, 90, 10000);
    app_audio_play_silence_ms(45);
    app_audio_play_tone_ms(1800, 130, 11000);
    app_audio_play_silence_ms(40);
}

static void app_audio_effect_error(void)
{
    app_audio_play_tone_ms(360, 180, 11000);
    app_audio_play_silence_ms(50);
}

static void app_audio_effect_bomb(void)
{
    /*
     * 炸弹命中特效：低频、下坠、危险感。
     * 和普通切水果的上扬 chirp 区分开。
     */
    app_audio_play_tone_ms(220, 90, 12500);
    app_audio_play_silence_ms(25);
    app_audio_play_tone_ms(150, 130, 12000);
    app_audio_play_silence_ms(25);
    app_audio_play_chirp_ms(850, 160, 170, 11000);
    app_audio_play_silence_ms(50);
}

static void app_audio_effect_win(void)
{
    app_audio_play_tone_ms(880, 110, 10000);
    app_audio_play_silence_ms(40);
    app_audio_play_tone_ms(1175, 110, 10000);
    app_audio_play_silence_ms(40);
    app_audio_play_tone_ms(1568, 220, 11500);
    app_audio_play_silence_ms(80);
}

static void app_audio_effect_lock(void)
{
    app_audio_play_tone_ms(420, 140, 10000);
    app_audio_play_silence_ms(70);
    app_audio_play_tone_ms(260, 260, 10500);
    app_audio_play_silence_ms(80);
}

static void app_audio_effect_roller(void)
{
    /*
     * 时间转盘齿轮感：短促、轻微、偏机械的双脉冲。
     * 不能太长，否则快速滚动时会堆积。
     */
    app_audio_play_tone_ms(950, 18, 6500);
    app_audio_play_silence_ms(8);
    app_audio_play_tone_ms(1350, 14, 5200);
    app_audio_play_silence_ms(10);
}

static void app_audio_effect_test_sequence(void)
{
    ESP_LOGI(TAG, "SFX test: click");
    app_audio_effect_click();
    app_audio_play_silence_ms(500);

    ESP_LOGI(TAG, "SFX test: slice");
    app_audio_effect_slice();
    app_audio_play_silence_ms(500);

    ESP_LOGI(TAG, "SFX test: success");
    app_audio_effect_success();
    app_audio_play_silence_ms(500);

    ESP_LOGI(TAG, "SFX test: error");
    app_audio_effect_error();
    app_audio_play_silence_ms(500);

    ESP_LOGI(TAG, "SFX test: bomb");
    app_audio_effect_bomb();
    app_audio_play_silence_ms(500);

    ESP_LOGI(TAG, "SFX test: win");
    app_audio_effect_win();
    app_audio_play_silence_ms(500);

    ESP_LOGI(TAG, "SFX test: lock");
    app_audio_effect_lock();
    app_audio_play_silence_ms(500);

    ESP_LOGI(TAG, "SFX test: roller");
    app_audio_effect_roller();
    app_audio_play_silence_ms(500);
}

static void app_audio_task(void *arg)
{
    (void)arg;

    audio_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_audio_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd) {
            case AUDIO_CMD_CLICK:
                app_audio_effect_click();
                break;

            case AUDIO_CMD_SLICE:
                app_audio_effect_slice();
                break;

            case AUDIO_CMD_SUCCESS:
                app_audio_effect_success();
                break;

            case AUDIO_CMD_ERROR:
                app_audio_effect_error();
                break;

            case AUDIO_CMD_WIN:
                app_audio_effect_win();
                break;

            case AUDIO_CMD_LOCK:
                app_audio_effect_lock();
                break;

            case AUDIO_CMD_ROLLER:
                app_audio_effect_roller();
                break;

            case AUDIO_CMD_BOMB:
                app_audio_effect_bomb();
                break;

            case AUDIO_CMD_TEST_SEQUENCE:
                app_audio_effect_test_sequence();
                break;

            default:
                break;
            }
        }
    }
}

static void app_audio_submit(audio_cmd_t cmd)
{
    if (!s_audio_ready || s_audio_queue == NULL) {
        return;
    }

    /*
     * 快速交互时容易产生大量音效事件。
     * 队列满就直接丢弃，不阻塞 UI。
     */
    (void)xQueueSend(s_audio_queue, &cmd, 0);
}

esp_err_t app_audio_init(void)
{
    if (s_audio_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "audio init start");

    esp_err_t ret = wm8960_codec_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wm8960_codec_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = app_audio_i2s_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "app_audio_i2s_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_audio_queue = xQueueCreate(APP_AUDIO_QUEUE_LEN, sizeof(audio_cmd_t));
    if (s_audio_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ret = xTaskCreate(
        app_audio_task,
        "app_audio_task",
        4096,
        NULL,
        5,
        &s_audio_task
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    app_audio_play_silence_ms(300);

    s_audio_ready = true;

    ESP_LOGI(TAG, "audio init OK, volume=%u%%", (unsigned int)s_volume_percent);
    return ESP_OK;
}

bool app_audio_is_ready(void)
{
    return s_audio_ready;
}

void app_audio_play_click(void)
{
    app_audio_submit(AUDIO_CMD_CLICK);
}

void app_audio_play_slice(void)
{
    app_audio_submit(AUDIO_CMD_SLICE);
}

void app_audio_play_success(void)
{
    app_audio_submit(AUDIO_CMD_SUCCESS);
}

void app_audio_play_error(void)
{
    app_audio_submit(AUDIO_CMD_ERROR);
}

void app_audio_play_win(void)
{
    app_audio_submit(AUDIO_CMD_WIN);
}

void app_audio_play_lock(void)
{
    app_audio_submit(AUDIO_CMD_LOCK);
}

void app_audio_play_roller(void)
{
    app_audio_submit(AUDIO_CMD_ROLLER);
}

void app_audio_play_bomb(void)
{
    app_audio_submit(AUDIO_CMD_BOMB);
}

void app_audio_play_test_sequence(void)
{
    app_audio_submit(AUDIO_CMD_TEST_SEQUENCE);
}