#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// =======================
// Display / Camera Config
// =======================

#define LCD_WIDTH                       1024
#define LCD_HEIGHT                      600

#define CAM_WIDTH                       1280
#define CAM_HEIGHT                      960

// 第一版先不叠加摄像头预览，直接全屏游戏
#define FRUIT_GAME_SHOW_CAMERA_PREVIEW  0

#if FRUIT_GAME_SHOW_CAMERA_PREVIEW
#define GAME_AREA_X                     112
#define GAME_AREA_Y                     0
#define GAME_AREA_W                     800
#define GAME_AREA_H                     600
#else
#define GAME_AREA_X                     0
#define GAME_AREA_Y                     0
#define GAME_AREA_W                     LCD_WIDTH
#define GAME_AREA_H                     LCD_HEIGHT
#endif

// =======================
// Game Config
// =======================

#define GAME_DURATION_MS                60000
#define GAME_UPDATE_PERIOD_MS           33

#define FRUIT_MAX_COUNT                 5
#define FRUIT_RADIUS_MIN                28
#define FRUIT_RADIUS_MAX                44

#define FRUIT_GRAVITY_PER_FRAME         1

// =======================
// Blade Config
// =======================

#define BLADE_TRAIL_MAX_POINTS          16
#define BLADE_COLLISION_WIDTH           18
#define BLADE_MIN_CUT_DISTANCE          20

// =======================
// Input Config
// =======================

#define FRUIT_SLICE_INPUT_MODE_FAKE     0
#define FRUIT_SLICE_INPUT_MODE_AI       1
#define FRUIT_SLICE_INPUT_MODE_TOUCH    2

/*
 * 当前检查版默认使用触屏输入：
 * 手指按下/滑动 -> 刀光跟随触点
 * 手指松开     -> 刀光断开
 *
 * 后续接入 AI 时，只需要改成：
 * #define FRUIT_SLICE_INPUT_MODE FRUIT_SLICE_INPUT_MODE_AI
 */
#define FRUIT_SLICE_INPUT_MODE          FRUIT_SLICE_INPUT_MODE_TOUCH

#ifdef __cplusplus
}
#endif