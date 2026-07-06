#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(ui_font_cn_20);
LV_FONT_DECLARE(ui_font_cn_26);
LV_FONT_DECLARE(ui_font_cn_32);

#define UI_FONT_CN_SMALL    (&ui_font_cn_20)
#define UI_FONT_CN_MEDIUM   (&ui_font_cn_26)
#define UI_FONT_CN_TITLE    (&ui_font_cn_32)

#ifdef __cplusplus
}
#endif