#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WM8960 codec 初始化。
 *
 * 当前 AirTouch 正式接线：
 * VCC    -> 3V3
 * GND    -> GND
 * SDA    -> IO46
 * SCL    -> IO48
 *
 * I2S 播放数据由 app_audio.c 负责：
 * CLK    -> IO32
 * WS     -> IO33
 * RXSDA  -> IO36
 *
 * TXMCLK / RXMCLK 不接。
 * ESP32-P4 不输出 MCLK。
 */
esp_err_t wm8960_codec_init(void);

#ifdef __cplusplus
}
#endif
