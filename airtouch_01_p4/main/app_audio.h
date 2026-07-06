#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 儿童互动游戏板音效模块。
 *
 * 当前音效全部由程序生成，不依赖 SD 卡文件。
 * 后续如果要换成 WAV，可以保持这些接口不变。
 */

esp_err_t app_audio_init(void);
bool app_audio_is_ready(void);

void app_audio_set_volume_percent(uint8_t percent);
uint8_t app_audio_get_volume_percent(void);

void app_audio_play_click(void);
void app_audio_play_slice(void);
void app_audio_play_success(void);
void app_audio_play_error(void);
void app_audio_play_win(void);
void app_audio_play_lock(void);
void app_audio_play_roller(void);
void app_audio_play_bomb(void);

void app_audio_play_test_sequence(void);

#ifdef __cplusplus
}
#endif