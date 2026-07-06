/*
 * AirTouch Aim Trainer UI
 *
 * Heatmap integrated version:
 *   - 3 random circular targets on screen.
 *   - Hover on any target for about 0.35s to hit it.
 *   - Record target position + reaction time into 3x3 heatmap.
 *   - 45s per round.
 *   - Result heatmap page is shown for 5s, then next round starts automatically.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "lvgl.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"

#include "air_input.h"
#include "app_audio.h"
#include "air_aim_trainer_ui.h"
#include "air_heatmap.h"
#include "air_attention_heatmap.h"


#include "airtouch_storage.h"
#include "ui_fonts.h"
static const char *TAG = "air_aim_trainer";




// ================= AirTouch Chinese Label Font Wrapper v2n.2 =================
// Chinese UI text must use generated ui_font_cn_* fonts. Otherwise LVGL default
// Montserrat shows square boxes. These wrappers keep ASCII labels unchanged and
// automatically apply Chinese fonts only when the text contains UTF-8 bytes.
static bool aim_text_has_cn_bytes_v2n2(const char *s)
{
    if (s == NULL) {
        return false;
    }

    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0U) {
        if (*p >= 0x80U) {
            return true;
        }
        p++;
    }

    return false;
}

static int aim_utf8_char_count_v2n2(const char *s)
{
    if (s == NULL) {
        return 0;
    }

    int count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p != 0U) {
        if ((*p & 0xC0U) != 0x80U) {
            count++;
        }
        p++;
    }

    return count;
}

static bool aim_text_has_newline_v2n2(const char *s)
{
    if (s == NULL) {
        return false;
    }

    while (*s != '\0') {
        if (*s == '\n') {
            return true;
        }
        s++;
    }

    return false;
}

static const lv_font_t *aim_pick_cn_font_v2n2(lv_obj_t *label, const char *s)
{
    int chars = aim_utf8_char_count_v2n2(s);
    bool multi_line = aim_text_has_newline_v2n2(s);
    int32_t w = 0;

    if (label != NULL) {
        w = lv_obj_get_width(label);
    }

    if (!multi_line && chars > 0 && chars <= 7 && w >= 260) {
        return UI_FONT_CN_TITLE;
    }

    if (!multi_line && chars > 0 && chars <= 10) {
        return UI_FONT_CN_MEDIUM;
    }

    return UI_FONT_CN_SMALL;
}

static void aim_apply_cn_font_for_text_v2n2(lv_obj_t *label, const char *s)
{
    if (label == NULL || s == NULL) {
        return;
    }

    if (aim_text_has_cn_bytes_v2n2(s)) {
        lv_obj_set_style_text_font(label, aim_pick_cn_font_v2n2(label, s), LV_PART_MAIN);
    }
}

static void aim_label_set_text_cn_v2n2(lv_obj_t *label, const char *txt)
{
    if (label == NULL) {
        return;
    }

    if (txt == NULL) {
        txt = "";
    }

    lv_label_set_text(label, txt);
    aim_apply_cn_font_for_text_v2n2(label, txt);
}

static void aim_label_set_text_fmt_cn_v2n2(lv_obj_t *label, const char *fmt, ...)
{
    if (label == NULL || fmt == NULL) {
        return;
    }

    char buf[768];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    buf[sizeof(buf) - 1] = '\0';

    lv_label_set_text(label, buf);
    aim_apply_cn_font_for_text_v2n2(label, buf);
}

/* --------------------------------------------------------------------------
 * AirTouch SFX v1d: game-event sound hooks
 * -------------------------------------------------------------------------- */
static void airtouch_sfx_click_v1d(void)
{
    if (app_audio_is_ready()) {
        app_audio_play_click();
    }
}

static void airtouch_sfx_start_v1d(void)
{
    if (app_audio_is_ready()) {
        app_audio_play_success();
    }
}

static void airtouch_sfx_hit_v1d(void)
{
    if (app_audio_is_ready()) {
        app_audio_play_slice();
    }
}

static void airtouch_sfx_error_v1d(void)
{
    if (app_audio_is_ready()) {
        app_audio_play_error();
    }
}

static void airtouch_sfx_result_v1d(void)
{
    if (app_audio_is_ready()) {
        app_audio_play_win();
    }
}

#define AIM_SCREEN_W 1024
#define AIM_SCREEN_H 600

#define AIM_TARGET_COUNT 1
#define AIM_TARGET_R_BASE 58
#define AIM_TARGET_MIN_DIST_BASE 170
#define AIM_TIMER_MS 30
#define AIM_ROUND_MS 45000
#define AIM_RESULT_SHOW_MS 5000

#define AIM_TOP_RESERVED_BASE 90
#define AIM_BOTTOM_RESERVED_BASE 35
#define AIM_SIDE_MARGIN_BASE 45
#define AIM_DWELL_MS_BASE 350
#define AIM_COOLDOWN_MS_BASE 180
#define AIM_STABLE_RADIUS_PX_BASE 48
#define AIM_TARGET_EXPAND_PX_BASE 14
#define AIM_DIFFICULTY_LEVEL_BASE 1
#ifndef LVGL_VERSION_MAJOR
#define LVGL_VERSION_MAJOR 8
#endif

#if LVGL_VERSION_MAJOR >= 9
#define AIM_OBJ_DELETE(obj) lv_obj_delete(obj)
#else
#define AIM_OBJ_DELETE(obj) lv_obj_del(obj)
#endif

typedef enum {
    AIM_STATE_IDLE = 0,
    AIM_STATE_RUNNING,
    AIM_STATE_RESULT,
} aim_state_t;

typedef enum {
    AIM_APP_MODE_BOOT = 0,
    AIM_APP_MODE_MENU,
    AIM_APP_MODE_STAR_CATCHER,
    AIM_APP_MODE_COLOR_GO,
    AIM_APP_MODE_RECORD,
} aim_app_mode_t;

typedef enum {
    COLOR_GO_KIND_GO = 0,
    COLOR_GO_KIND_DISTRACTOR,
    COLOR_GO_KIND_NOGO,
} color_go_bubble_kind_t;

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *label;
    uint32_t id;
    int cx;
    int cy;
    int r;
    color_go_bubble_kind_t kind;
    uint32_t spawn_ms;
} color_go_bubble_t;



typedef enum {
    AIM_RESULT_VIEW_SUMMARY = 0,
    AIM_RESULT_VIEW_TRANSFER,
    AIM_RESULT_VIEW_HEATMAP,
    AIM_RESULT_VIEW_ADAPTIVE,
    AIM_RESULT_VIEW_COUNT,
} aim_result_view_t;

typedef struct {
    uint16_t id;
    int cx;
    int cy;
    int r;

    lv_obj_t *obj;
    lv_obj_t *label;
    lv_obj_t *star_line;

    uint32_t spawn_ms;
} aim_target_t;

static aim_target_t s_targets[AIM_TARGET_COUNT];

/*
 * Star visual size follows the real adaptive target radius:
 * visual diameter = 2 * s_adaptive_target_r.
 *
 * Current adaptive range:
 * low load  r=68 -> visual 136 px
 * high load r=42 -> visual 84 px
 */

/*
 * Full-screen safe spawn region.
 * Targets can cover most of the screen for heatmap consistency,
 * but avoid top HUD text and bottom hint text.
 */
#define AIM_STAR_FIELD_X1 70
#define AIM_STAR_FIELD_Y1 108
#define AIM_STAR_FIELD_X2 954
#define AIM_STAR_FIELD_Y2 514

static lv_point_t s_star_line_points[AIM_TARGET_COUNT][6];

static lv_obj_t *s_title_label = NULL;
static lv_obj_t *s_info_label = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_bar = NULL;

static lv_obj_t *s_star_page = NULL;
static lv_obj_t *s_star_feedback_label = NULL;
static uint32_t s_star_feedback_until_ms = 0;

static lv_obj_t *s_result_page = NULL;
static lv_obj_t *s_result_layer = NULL;
static lv_obj_t *s_result_analysis_btn = NULL;
static lv_obj_t *s_result_next_btn = NULL;
static lv_obj_t *s_result_home_btn = NULL;
static lv_obj_t *s_control_button = NULL;
static lv_obj_t *s_control_button_label = NULL;
static aim_result_view_t s_result_view = AIM_RESULT_VIEW_SUMMARY;

/*
 * Result-page AirTouch anti-repeat latch.
 *
 * AirInput is intentionally sensitive during training, but result-page
 * controls should behave like deliberate menu choices. After ANALYSIS or
 * NEXT ROUND is selected once, the same virtual target will not trigger again
 * until the pointer leaves the target area and re-enters.
 */
static bool s_result_ctrl_wait_release = false;
static uint32_t s_result_ctrl_last_click_ms = 0;


static lv_timer_t *s_timer = NULL;

static aim_state_t s_game_state = AIM_STATE_RUNNING;


static aim_app_mode_t s_app_mode = AIM_APP_MODE_BOOT;

static lv_obj_t *s_menu_page = NULL;
static lv_obj_t *s_menu_star_btn = NULL;
static lv_obj_t *s_menu_color_btn = NULL;
static lv_obj_t *s_menu_record_btn = NULL;
static bool s_menu_ctrl_wait_release = false;

static lv_obj_t *s_boot_page = NULL;
static lv_timer_t *s_boot_timer = NULL;
static uint32_t s_boot_frame = 0;
static lv_obj_t *s_boot_loading_label = NULL;
static lv_obj_t *s_boot_dots[4] = {NULL, NULL, NULL, NULL};


/*
 * Color-Go MVP:
 * blue bubble = Go target
 * red bubble  = No-Go target
 * yellow/green bubbles = distractors
 */
#define COLOR_GO_BUBBLE_MAX 8
#define COLOR_GO_BUBBLE_COUNT 4
#define COLOR_GO_ROUND_MS 45000
#define COLOR_GO_BUBBLE_R 62
#define COLOR_GO_BUBBLE_ID_BASE 8201
#define COLOR_GO_NEXT_ID 8301
#define COLOR_GO_HOME_ID 8302

#define RECORD_BTN_STAR_ID 8401
#define RECORD_BTN_COLOR_ID 8402
#define RECORD_BTN_HOME_ID 8403

#define RECORD_BTN_STAR_X 248
#define RECORD_BTN_COLOR_X 512
#define RECORD_BTN_HOME_X 776
#define RECORD_BTN_Y 530
#define RECORD_BTN_R 58

#define COLOR_GO_HISTORY_MAX 12
#define STAR_HISTORY_MAX 12
#define RECORD_PAGE_COUNT 4

#define RECORD_BTN_UP_ID 8404
#define RECORD_BTN_DOWN_ID 8405
#define RECORD_BTN_UP_X 956
#define RECORD_BTN_UP_Y 126
#define RECORD_BTN_DOWN_X 956
#define RECORD_BTN_DOWN_Y 414
#define RECORD_BTN_SCROLL_R 46


#define RECORD_TREND_PANEL_X 496
#define RECORD_TREND_PANEL_Y 128
#define RECORD_TREND_PANEL_W 406
#define RECORD_TREND_PANEL_H 328

#define RECORD_TREND_CHART_X 32
#define RECORD_TREND_CHART_Y 76
#define RECORD_TREND_CHART_W 330
#define RECORD_TREND_CHART_H 164



static lv_obj_t *s_color_go_page = NULL;
static lv_obj_t *s_color_go_result_page = NULL;
static lv_obj_t *s_color_go_next_btn = NULL;
static lv_obj_t *s_color_go_home_btn = NULL;
static lv_obj_t *s_color_go_rule_label = NULL;
static lv_obj_t *s_color_go_score_label = NULL;

static color_go_bubble_t s_color_go_bubbles[COLOR_GO_BUBBLE_MAX];
static bool s_color_go_running = false;
static bool s_color_go_result_active = false;
static bool s_color_go_wait_release = false;

static uint32_t s_color_go_round_start_ms = 0;
static uint32_t s_color_go_correct_hits = 0;
static uint32_t s_color_go_wrong_hits = 0;
static uint32_t s_color_go_false_alarm_count = 0;
static uint32_t s_color_go_miss_count = 0;
static uint32_t s_color_go_total_reaction_ms = 0;
static uint32_t s_color_go_fastest_reaction_ms = UINT32_MAX;

static bool s_color_go_latest_valid = false;
static uint32_t s_color_go_latest_correct = 0;
static uint32_t s_color_go_latest_wrong = 0;
static uint32_t s_color_go_latest_false_alarm = 0;
static uint32_t s_color_go_latest_miss = 0;
static uint32_t s_color_go_latest_accuracy = 0;
static uint32_t s_color_go_latest_avg_ms = 0;
static uint32_t s_color_go_latest_fastest_ms = 0;
static uint32_t s_color_go_latest_inhibition = 0;

static uint32_t s_color_go_history_count = 0;
static uint32_t s_color_go_history_accuracy[COLOR_GO_HISTORY_MAX];
static uint32_t s_color_go_history_avg_ms[COLOR_GO_HISTORY_MAX];
static uint32_t s_color_go_history_inhibition[COLOR_GO_HISTORY_MAX];
static uint32_t s_color_go_history_correct[COLOR_GO_HISTORY_MAX];

static lv_point_t s_record_accuracy_points[COLOR_GO_HISTORY_MAX];
static lv_point_t s_record_inhibition_points[COLOR_GO_HISTORY_MAX];
static lv_point_t s_record_speed_points[COLOR_GO_HISTORY_MAX];

static bool s_star_latest_valid = false;
static uint32_t s_star_latest_hits = 0;
static uint32_t s_star_latest_hit_score = 0;
static uint32_t s_star_latest_avg_ms = 0;
static uint32_t s_star_latest_fastest_ms = 0;
static uint32_t s_star_latest_speed_score = 0;

static uint32_t s_star_history_count = 0;
static uint32_t s_star_history_hits[STAR_HISTORY_MAX];
static uint32_t s_star_history_hit_score[STAR_HISTORY_MAX];
static uint32_t s_star_history_avg_ms[STAR_HISTORY_MAX];
static uint32_t s_star_history_speed_score[STAR_HISTORY_MAX];

static lv_point_t s_record_star_hit_points[STAR_HISTORY_MAX];
static lv_point_t s_record_star_speed_points[STAR_HISTORY_MAX];



static lv_obj_t *s_record_page = NULL;
static lv_obj_t *s_record_star_btn = NULL;
static lv_obj_t *s_record_color_btn = NULL;
static lv_obj_t *s_record_home_btn = NULL;
static lv_obj_t *s_record_up_btn = NULL;
static lv_obj_t *s_record_down_btn = NULL;
static uint8_t s_record_page_index = 0;

static bool s_record_wait_release = false;



static uint32_t s_round_start_ms = 0;
static uint32_t s_result_start_ms = 0;

static uint32_t s_hits = 0;
static uint32_t s_total_reaction_ms = 0;
static uint32_t s_fastest_reaction_ms = UINT32_MAX;


#define AIM_ADAPTIVE_MIN_LEVEL 0
#define AIM_ADAPTIVE_MAX_LEVEL 2

static int s_adaptive_level = AIM_DIFFICULTY_LEVEL_BASE;
/*
 * Continuous adaptive training load.
 *
 * This is the real adaptive control variable:
 * 0   = low load, larger target, longer dwell, higher tolerance
 * 100 = high load, smaller target, shorter dwell, stricter stability
 *
 * s_adaptive_level is kept only as a coarse compatibility label.
 */
static int s_adaptive_load_score = 50;
static char s_adaptive_profile_summary[192] =
    "负荷=50 | 半径=58 | 悬停=350ms | 稳定=48 | 冷却=180ms | 间距=170";
static char s_adaptive_profile_delta_summary[192] =
    "负荷 50->50 | 半径 58->58 | 悬停 350->350 | 稳定 48->48 | 间距 170->170";
static int s_adaptive_target_r = AIM_TARGET_R_BASE;
static int s_adaptive_min_dist = AIM_TARGET_MIN_DIST_BASE;
static int s_adaptive_top_reserved = AIM_TOP_RESERVED_BASE;
static int s_adaptive_bottom_reserved = AIM_BOTTOM_RESERVED_BASE;
static int s_adaptive_side_margin = AIM_SIDE_MARGIN_BASE;
static uint32_t s_adaptive_dwell_ms = AIM_DWELL_MS_BASE;
static uint32_t s_adaptive_cooldown_ms = AIM_COOLDOWN_MS_BASE;
static int s_adaptive_stable_radius_px = AIM_STABLE_RADIUS_PX_BASE;
static int s_adaptive_target_expand_px = AIM_TARGET_EXPAND_PX_BASE;

/* --------------------------------------------------------------------------
 * Cloud-SD v2j runtime CONFIG.TXT loader
 *
 * This connects cloud-downloaded CONFIG.TXT to real Star / Color-Go gameplay.
 * CONFIG.TXT path:
 *   /sdcard/airtouch/users/child_001/CONFIG.TXT
 * -------------------------------------------------------------------------- */

#define AIRTOUCH_RUNTIME_CONFIG_PATH_V2J "/sdcard/airtouch/users/child_001/CONFIG.TXT"

static int s_cloud_config_version_v2j = 0;
static bool s_cloud_config_valid_v2j = false;

static int s_cloud_star_target_r_v2j = AIM_TARGET_R_BASE;
static uint32_t s_cloud_star_dwell_ms_v2j = AIM_DWELL_MS_BASE;
static uint32_t s_star_round_ms_v2j = AIM_ROUND_MS;
static int s_cloud_star_difficulty_v2j = AIM_DIFFICULTY_LEVEL_BASE;
static int s_cloud_star_adaptive_v2j = 1;

static int s_color_go_target_r_v2j = COLOR_GO_BUBBLE_R;
static uint32_t s_color_go_dwell_ms_v2j = 360U;
static int s_color_go_bubble_count_v2j = COLOR_GO_BUBBLE_COUNT;
static int s_color_go_nogo_ratio_v2j = 25;
static uint32_t s_color_go_round_ms_v2j = COLOR_GO_ROUND_MS;
static int s_color_go_difficulty_v2j = 1;
static int s_color_go_adaptive_v2j = 1;

static int aim_cloud_clamp_int_v2j(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static int aim_cloud_read_int_kv_v2j(const char *line, const char *key, int current)
{
    if (!line || !key) {
        return current;
    }

    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "%s=", key);

    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return current;
    }

    const size_t pattern_len = strlen(pattern);

    if (strncmp(line, pattern, pattern_len) == 0) {
        return atoi(line + pattern_len);
    }

    return current;
}

static void aim_cloud_runtime_defaults_v2j(void)
{
    s_cloud_config_version_v2j = 0;
    s_cloud_config_valid_v2j = false;

    s_cloud_star_target_r_v2j = AIM_TARGET_R_BASE;
    s_cloud_star_dwell_ms_v2j = AIM_DWELL_MS_BASE;
    s_star_round_ms_v2j = AIM_ROUND_MS;
    s_cloud_star_difficulty_v2j = AIM_DIFFICULTY_LEVEL_BASE;
    s_cloud_star_adaptive_v2j = 1;

    s_color_go_target_r_v2j = COLOR_GO_BUBBLE_R;
    s_color_go_dwell_ms_v2j = 360U;
    s_color_go_bubble_count_v2j = COLOR_GO_BUBBLE_COUNT;
    s_color_go_nogo_ratio_v2j = 25;
    s_color_go_round_ms_v2j = COLOR_GO_ROUND_MS;
    s_color_go_difficulty_v2j = 1;
    s_color_go_adaptive_v2j = 1;
}

static bool aim_cloud_runtime_config_load_v2j(void)
{
    aim_cloud_runtime_defaults_v2j();

    FILE *fp = fopen(AIRTOUCH_RUNTIME_CONFIG_PATH_V2J, "r");

    if (!fp) {
        ESP_LOGW(TAG,
                 "Cloud-SD v2j: CONFIG.TXT not found, use local defaults: %s",
                 AIRTOUCH_RUNTIME_CONFIG_PATH_V2J);
        return false;
    }

    char line[192];

    while (fgets(line, sizeof(line), fp)) {
        s_cloud_config_version_v2j = aim_cloud_read_int_kv_v2j(line, "config_version", s_cloud_config_version_v2j);

        s_cloud_star_target_r_v2j = aim_cloud_read_int_kv_v2j(line, "star_target_radius", s_cloud_star_target_r_v2j);
        s_cloud_star_dwell_ms_v2j = (uint32_t)aim_cloud_read_int_kv_v2j(line, "star_dwell_ms", (int)s_cloud_star_dwell_ms_v2j);
        s_star_round_ms_v2j = (uint32_t)aim_cloud_read_int_kv_v2j(line, "star_duration_s", (int)(s_star_round_ms_v2j / 1000U)) * 1000U;
        s_cloud_star_difficulty_v2j = aim_cloud_read_int_kv_v2j(line, "star_difficulty", s_cloud_star_difficulty_v2j);
        s_cloud_star_adaptive_v2j = aim_cloud_read_int_kv_v2j(line, "star_adaptive_enabled", s_cloud_star_adaptive_v2j);

        s_color_go_target_r_v2j = aim_cloud_read_int_kv_v2j(line, "color_target_radius", s_color_go_target_r_v2j);
        s_color_go_dwell_ms_v2j = (uint32_t)aim_cloud_read_int_kv_v2j(line, "color_dwell_ms", (int)s_color_go_dwell_ms_v2j);
        s_color_go_bubble_count_v2j = aim_cloud_read_int_kv_v2j(line, "color_bubble_count", s_color_go_bubble_count_v2j);
        s_color_go_nogo_ratio_v2j = aim_cloud_read_int_kv_v2j(line, "color_nogo_ratio", s_color_go_nogo_ratio_v2j);
        s_color_go_round_ms_v2j = (uint32_t)aim_cloud_read_int_kv_v2j(line, "color_duration_s", (int)(s_color_go_round_ms_v2j / 1000U)) * 1000U;
        s_color_go_difficulty_v2j = aim_cloud_read_int_kv_v2j(line, "color_difficulty", s_color_go_difficulty_v2j);
        s_color_go_adaptive_v2j = aim_cloud_read_int_kv_v2j(line, "color_adaptive_enabled", s_color_go_adaptive_v2j);
    }

    fclose(fp);

    s_cloud_star_target_r_v2j = aim_cloud_clamp_int_v2j(s_cloud_star_target_r_v2j, 36, 90);
    s_cloud_star_dwell_ms_v2j = (uint32_t)aim_cloud_clamp_int_v2j((int)s_cloud_star_dwell_ms_v2j, 250, 1200);
    s_star_round_ms_v2j = (uint32_t)aim_cloud_clamp_int_v2j((int)s_star_round_ms_v2j, 30000, 90000);
    s_cloud_star_difficulty_v2j = aim_cloud_clamp_int_v2j(s_cloud_star_difficulty_v2j, 1, 5);
    s_cloud_star_adaptive_v2j = s_cloud_star_adaptive_v2j ? 1 : 0;

    s_color_go_target_r_v2j = aim_cloud_clamp_int_v2j(s_color_go_target_r_v2j, 36, 90);
    s_color_go_dwell_ms_v2j = (uint32_t)aim_cloud_clamp_int_v2j((int)s_color_go_dwell_ms_v2j, 300, 1200);
    s_color_go_bubble_count_v2j = aim_cloud_clamp_int_v2j(s_color_go_bubble_count_v2j, 3, COLOR_GO_BUBBLE_MAX);
    s_color_go_nogo_ratio_v2j = aim_cloud_clamp_int_v2j(s_color_go_nogo_ratio_v2j, 10, 50);
    s_color_go_round_ms_v2j = (uint32_t)aim_cloud_clamp_int_v2j((int)s_color_go_round_ms_v2j, 30000, 90000);
    s_color_go_difficulty_v2j = aim_cloud_clamp_int_v2j(s_color_go_difficulty_v2j, 1, 5);
    s_color_go_adaptive_v2j = s_color_go_adaptive_v2j ? 1 : 0;

    s_cloud_config_valid_v2j = s_cloud_config_version_v2j > 0;

    ESP_LOGI(TAG,
             "Cloud-SD v2j CONFIG loaded: version=%d star(r=%d dwell=%lu dur=%lu diff=%d adapt=%d) color(r=%d dwell=%lu bubbles=%d nogo=%d dur=%lu diff=%d adapt=%d)",
             s_cloud_config_version_v2j,
             s_cloud_star_target_r_v2j,
             (unsigned long)s_cloud_star_dwell_ms_v2j,
             (unsigned long)(s_star_round_ms_v2j / 1000U),
             s_cloud_star_difficulty_v2j,
             s_cloud_star_adaptive_v2j,
             s_color_go_target_r_v2j,
             (unsigned long)s_color_go_dwell_ms_v2j,
             s_color_go_bubble_count_v2j,
             s_color_go_nogo_ratio_v2j,
             (unsigned long)(s_color_go_round_ms_v2j / 1000U),
             s_color_go_difficulty_v2j,
             s_color_go_adaptive_v2j);

    return s_cloud_config_valid_v2j;
}

static void aim_star_sync_header_duration_v2j(void)
{
    if (s_info_label == NULL) {
        return;
    }

    uint32_t avg_ms = 0;

    if (s_hits > 0) {
        avg_ms = s_total_reaction_ms / s_hits;
    }

    aim_label_set_text_fmt_cn_v2n2(s_info_label,
                          "时间 %lus    星星 %lu    平均 %lums",
                          (unsigned long)(s_star_round_ms_v2j / 1000U),
                          (unsigned long)s_hits,
                          (unsigned long)avg_ms);
}
static void aim_cloud_apply_star_runtime_v2j(void)
{
    static int s_star_cloud_seed_applied_version_v2j = 0;

    if (!aim_cloud_runtime_config_load_v2j()) {
        return;
    }

    /*
     * Cloud-SD v2j.1 policy:
     *
     * star_adaptive_enabled = 1:
     *   Cloud config is a one-round seed for the next Star round.
     *   After this seed round, Star returns to its original adaptive controller.
     *
     * star_adaptive_enabled = 0:
     *   Cloud config is a hard manual override.
     *   Every Star round uses cloud target/dwell until adaptive is enabled again.
     */
    bool hard_manual_override = (s_cloud_star_adaptive_v2j == 0);
    bool new_cloud_seed = (s_cloud_config_version_v2j > 0 &&
                           s_cloud_config_version_v2j != s_star_cloud_seed_applied_version_v2j);

    if (!hard_manual_override && !new_cloud_seed) {
        ESP_LOGI(TAG,
                 "Cloud-SD v2j.1 Star adaptive continues: version=%d already seeded, keep adaptive target_r=%d dwell=%lu load=%d",
                 s_cloud_config_version_v2j,
                 s_adaptive_target_r,
                 (unsigned long)s_adaptive_dwell_ms,
                 s_adaptive_load_score);

        // Cloud-SD v2j.2: sync Star header when adaptive continues.
        aim_star_sync_header_duration_v2j();
        return;
    }

    s_adaptive_target_r = s_cloud_star_target_r_v2j;
    s_adaptive_dwell_ms = s_cloud_star_dwell_ms_v2j;
    s_adaptive_cooldown_ms = (uint32_t)aim_cloud_clamp_int_v2j((int)(s_cloud_star_dwell_ms_v2j / 2U), 120, 500);
    s_adaptive_stable_radius_px = aim_cloud_clamp_int_v2j(s_cloud_star_target_r_v2j - 8, 28, 80);
    s_adaptive_target_expand_px = aim_cloud_clamp_int_v2j(s_cloud_star_target_r_v2j / 3, 8, 32);

    snprintf(s_adaptive_profile_summary,
             sizeof(s_adaptive_profile_summary),
             "云端v%d | 半径=%d | 悬停=%lums | 稳定=%d | 冷却=%lums",
             s_cloud_config_version_v2j,
             s_adaptive_target_r,
             (unsigned long)s_adaptive_dwell_ms,
             s_adaptive_stable_radius_px,
             (unsigned long)s_adaptive_cooldown_ms);

    // Cloud-SD v2j.2: sync Star header after cloud runtime config.
    aim_star_sync_header_duration_v2j();

    if (hard_manual_override) {
        ESP_LOGI(TAG,
                 "Cloud-SD v2j.1 Star manual override: version=%d target_r=%d dwell=%lu round=%lus adaptive=0",
                 s_cloud_config_version_v2j,
                 s_adaptive_target_r,
                 (unsigned long)s_adaptive_dwell_ms,
                 (unsigned long)(s_star_round_ms_v2j / 1000U));
    } else {
        s_star_cloud_seed_applied_version_v2j = s_cloud_config_version_v2j;

        ESP_LOGI(TAG,
                 "Cloud-SD v2j.1 Star seed applied once: version=%d target_r=%d dwell=%lu round=%lus adaptive=1",
                 s_cloud_config_version_v2j,
                 s_adaptive_target_r,
                 (unsigned long)s_adaptive_dwell_ms,
                 (unsigned long)(s_star_round_ms_v2j / 1000U));
    }
}

static void aim_cloud_apply_color_runtime_v2j(void)
{
    (void)aim_cloud_runtime_config_load_v2j();

    ESP_LOGI(TAG,
             "Cloud-SD v2j Color-Go applied: version=%d target_r=%d dwell=%lu bubbles=%d nogo=%d round=%lus",
             s_cloud_config_version_v2j,
             s_color_go_target_r_v2j,
             (unsigned long)s_color_go_dwell_ms_v2j,
             s_color_go_bubble_count_v2j,
             s_color_go_nogo_ratio_v2j,
             (unsigned long)(s_color_go_round_ms_v2j / 1000U));
}


static char s_adaptive_advice[96] = "正常训练";
static uint32_t s_last_round_avg_ms = 0;
static uint32_t s_last_round_fast_ms = 0;
static uint32_t s_last_round_hits = 0;

static const char *aim_adaptive_level_name(void)
{
    if (s_adaptive_load_score < 35) {
        return "低负荷";
    }

    if (s_adaptive_load_score > 70) {
        return "高负荷";
    }

    return "中等负荷";
}

static int aim_adaptive_clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static int aim_adaptive_lerp_int(int low_load_value, int high_load_value, int load)
{
    load = aim_adaptive_clamp_int(load, 0, 100);
    return low_load_value + ((high_load_value - low_load_value) * load + 50) / 100;
}

static void aim_refresh_adaptive_profile_delta_summary(int old_load,
                                                       int new_load)
{
    int old_target_r = aim_adaptive_lerp_int(68, 42, old_load);
    int old_min_dist = aim_adaptive_lerp_int(150, 230, old_load);
    int old_dwell = aim_adaptive_lerp_int(430, 240, old_load);
    int old_stable = aim_adaptive_lerp_int(62, 34, old_load);

    int new_target_r = aim_adaptive_lerp_int(68, 42, new_load);
    int new_min_dist = aim_adaptive_lerp_int(150, 230, new_load);
    int new_dwell = aim_adaptive_lerp_int(430, 240, new_load);
    int new_stable = aim_adaptive_lerp_int(62, 34, new_load);

    snprintf(s_adaptive_profile_delta_summary,
             sizeof(s_adaptive_profile_delta_summary),
             "负荷 %d->%d | 半径 %d->%d | 悬停 %d->%d",
             old_load,
             new_load,
             old_target_r,
             new_target_r,
             old_dwell,
             new_dwell,
             old_stable,
             new_stable,
             old_min_dist,
             new_min_dist);
}

static void aim_refresh_adaptive_profile_summary(void)
{
    snprintf(s_adaptive_profile_summary,
             sizeof(s_adaptive_profile_summary),
             "负荷=%d | 半径=%d | 悬停=%lums | 稳定=%d",
             s_adaptive_load_score,
             s_adaptive_target_r,
             (unsigned long)s_adaptive_dwell_ms,
             s_adaptive_stable_radius_px,
             (unsigned long)s_adaptive_cooldown_ms,
             s_adaptive_min_dist);
}

static void aim_apply_adaptive_level(void)
{
    s_adaptive_load_score = aim_adaptive_clamp_int(s_adaptive_load_score, 0, 100);

    /*
     * Continuous mapping from training load to concrete interaction parameters.
     *
     * Low load:
     *   larger target, longer hover confirmation, wider stability tolerance.
     *
     * High load:
     *   smaller target, shorter hover confirmation, stricter stability,
     *   wider spatial transition distance.
     */
    s_adaptive_target_r = aim_adaptive_lerp_int(68, 42, s_adaptive_load_score);
    s_adaptive_min_dist = aim_adaptive_lerp_int(150, 230, s_adaptive_load_score);

    s_adaptive_top_reserved = aim_adaptive_lerp_int(115, 70, s_adaptive_load_score);
    s_adaptive_bottom_reserved = aim_adaptive_lerp_int(65, 24, s_adaptive_load_score);
    s_adaptive_side_margin = aim_adaptive_lerp_int(95, 24, s_adaptive_load_score);

    s_adaptive_dwell_ms = (uint32_t)aim_adaptive_lerp_int(430, 240, s_adaptive_load_score);
    s_adaptive_cooldown_ms = (uint32_t)aim_adaptive_lerp_int(220, 120, s_adaptive_load_score);
    s_adaptive_stable_radius_px = aim_adaptive_lerp_int(62, 34, s_adaptive_load_score);
    s_adaptive_target_expand_px = aim_adaptive_lerp_int(22, 6, s_adaptive_load_score);

    /*
     * Coarse compatibility label only.
     * The actual control is continuous s_adaptive_load_score.
     */
    if (s_adaptive_load_score < 35) {
        s_adaptive_level = 0;
    } else if (s_adaptive_load_score > 70) {
        s_adaptive_level = 2;
    } else {
        s_adaptive_level = 1;
    }

    aim_refresh_adaptive_profile_summary();
}

static void aim_update_adaptive_from_round(uint32_t hits,
                                           uint32_t avg_ms,
                                           uint32_t fastest_ms)
{
    air_attention_heatmap_stats_t stats;
    bool stats_ok = air_attention_heatmap_get_stats(&stats);

    int stability_pct = 0;
    int focus_pct = 0;
    int coverage_pct = 0;

    if (stats_ok) {
        stability_pct = (int)(stats.recent_stability * 100.0f + 0.5f);
        focus_pct = (int)(stats.focus_score * 100.0f + 0.5f);
        coverage_pct = (int)(stats.coverage_score * 100.0f + 0.5f);
    }

    /*
     * Performance score estimates whether the next round should carry
     * a higher or lower interaction load. This no longer maps to fixed
     * Easy/Normal/Hard levels. It changes a continuous 0~100 load score.
     */
    int perf_score = 0;

    if (hits >= 40) {
        perf_score += 3;
    } else if (hits >= 28) {
        perf_score += 2;
    } else if (hits >= 18) {
        perf_score += 1;
    } else if (hits < 10) {
        perf_score -= 2;
    }

    if (avg_ms > 0 && avg_ms <= 900) {
        perf_score += 2;
    } else if (avg_ms <= 1150) {
        perf_score += 1;
    } else if (avg_ms >= 1700) {
        perf_score -= 2;
    } else if (avg_ms >= 1400) {
        perf_score -= 1;
    }

    if (fastest_ms > 0 && fastest_ms <= 650) {
        perf_score += 1;
    } else if (fastest_ms >= 1200) {
        perf_score -= 1;
    }

    if (stats_ok) {
        if (stability_pct >= 90) {
            perf_score += 1;
        } else if (stability_pct < 70) {
            perf_score -= 1;
        }

        /*
         * Coverage is not simply "higher is better".
         * Extremely low coverage suggests over-concentration; enough coverage
         * means the child can handle wider spatial distribution.
         */
        if (coverage_pct >= 16) {
            perf_score += 1;
        } else if (coverage_pct < 6) {
            perf_score -= 1;
        }
    }

    int old_load = s_adaptive_load_score;

    /*
     * Convert performance into a continuous load delta.
     * Strong performance increases load more; weak performance reduces load.
     */
    int delta = 0;

    if (perf_score >= 6) {
        delta = 12;
    } else if (perf_score >= 4) {
        delta = 8;
    } else if (perf_score >= 2) {
        delta = 4;
    } else if (perf_score <= -4) {
        delta = -12;
    } else if (perf_score <= -2) {
        delta = -8;
    } else if (perf_score < 0) {
        delta = -4;
    } else {
        delta = 0;
    }

    s_adaptive_load_score = aim_adaptive_clamp_int(s_adaptive_load_score + delta, 0, 100);

    if (delta > 0) {
        snprintf(s_adaptive_advice,
                 sizeof(s_adaptive_advice),
                 "升负荷：目标更小，悬停更短。");
    } else if (delta < 0) {
        snprintf(s_adaptive_advice,
                 sizeof(s_adaptive_advice),
                 "降负荷：目标更大，悬停更久。");
    } else {
        snprintf(s_adaptive_advice,
                 sizeof(s_adaptive_advice),
                 "负荷稳定：保持当前参数。");
    }

    aim_apply_adaptive_level();
    aim_refresh_adaptive_profile_delta_summary(old_load, s_adaptive_load_score);

    s_last_round_hits = hits;
    s_last_round_avg_ms = avg_ms;
    s_last_round_fast_ms = fastest_ms;

    ESP_LOGI(TAG,
             "Adaptive load update: old_load=%d new_load=%d delta=%d perf=%d hits=%lu avg=%lu fast=%lu stability=%d focus=%d coverage=%d",
             old_load,
             s_adaptive_load_score,
             delta,
             perf_score,
             (unsigned long)hits,
             (unsigned long)avg_ms,
             (unsigned long)fastest_ms,
             stability_pct,
             focus_pct,
             coverage_pct);

    ESP_LOGI(TAG,
             "Adaptive profile: load=%d target_r=%d dwell=%lu stable=%d cooldown=%lu expand=%d min_dist=%d label=%s",
             s_adaptive_load_score,
             s_adaptive_target_r,
             (unsigned long)s_adaptive_dwell_ms,
             s_adaptive_stable_radius_px,
             (unsigned long)s_adaptive_cooldown_ms,
             s_adaptive_target_expand_px,
             s_adaptive_min_dist,
             aim_adaptive_level_name());

    ESP_LOGI(TAG,
             "变化：%s",
             s_adaptive_profile_delta_summary);
}



static uint16_t s_hover_target_id = 0;
static uint16_t s_hover_progress = 0;

/*
 * Spatial Transition Attention Analysis
 * -------------------------------------
 * This block is intentionally documented in detail because it is the core
 * behavior-analysis part of the AirTouch training system.
 *
 * The old heatmap only answered:
 *   "Which screen areas were touched or visited more often?"
 *
 * This transition module answers a more valuable training question:
 *   "How does the child move attention/control from region A to region B?"
 *
 * In sequential single-target mode, only one target is shown at a time.
 * After the child hits the current target, the next target appears.
 * This creates a clear directed relation:
 *
 *      previous target region  --->  next target region
 *
 * For each transition, we record:
 *   - completion time: target spawn to hit
 *   - path length: actual pointer travel distance
 *   - straight distance: ideal shortest distance between target centers
 *   - straightness: straight distance / actual path length
 *   - maximum deviation: largest distance from the ideal straight line
 *
 * These metrics make the heatmap closer to an "attention mechanism":
 * it is no longer only a per-cell activity map, but also a relation map
 * between spatial regions. Later we can visualize this as arrows or a
 * 3x3 region-to-region transition matrix.
 */
#define AIM_TRANSITION_REGION_COLS 3
#define AIM_TRANSITION_REGION_ROWS 3
#define AIM_TRANSITION_MOVE_START_PX 24

/*
 * Weak-relation guided target generation.
 *
 * After one round ends, the transition analyzer stores the slowest directed
 * spatial relation, for example "LC->CC". The next round starts with a short
 * guided target sequence that alternates the two weak-relation regions before
 * falling back to random targets.
 */
#define AIM_GUIDED_PLAN_MAX 6

/*
 * Guided target generation v2:
 * use probability bias across the whole round instead of a fixed prefix plan.
 */
#define AIM_GUIDED_FORWARD_PCT 45
#define AIM_GUIDED_REVERSE_PCT 20
#define AIM_GUIDED_ENTRY_PCT 20
#define AIM_REGION_COVERAGE_COUNT 9
#define AIM_GUIDED_MAX_ACCEPT_PER_ROUND 3

#define AIM_GUIDED_MAX_STREAK 2
#define AIM_GUIDED_MAX_REVERSE_STREAK 1
/*
 * Result-page AirTouch virtual targets.
 *
 * These are not touch-screen buttons. They are visual targets that can be
 * selected by hovering the AirTouch pointer, keeping the whole result-page
 * interaction consistent with the no-contact interaction concept.
 */
#define AIM_RESULT_CTRL_ANALYSIS_ID 9001
#define AIM_RESULT_CTRL_NEXT_ROUND_ID 9002
#define AIM_RESULT_CTRL_HOME_ID 9003

#define AIM_RESULT_CTRL_ANALYSIS_X 246
#define AIM_RESULT_CTRL_NEXT_ROUND_X 512
#define AIM_RESULT_CTRL_HOME_X 778
#define AIM_RESULT_CTRL_Y 532
#define AIM_RESULT_CTRL_R 58

/*
 * Main-menu AirTouch virtual targets.
 * These are hover-confirmed targets, not touch-screen buttons.
 */
#define AIM_MENU_BTN_STAR_ID 8101
#define AIM_MENU_BTN_COLOR_GO_ID 8102
#define AIM_MENU_BTN_RECORD_ID 8103

#define AIM_MENU_BTN_STAR_X 191
#define AIM_MENU_BTN_COLOR_GO_X 509
#define AIM_MENU_BTN_RECORD_X 827
#define AIM_MENU_BTN_Y 407
#define AIM_MENU_BTN_R 112

static bool s_transition_active = false;
static int s_transition_from_x = 0;
static int s_transition_from_y = 0;
static int s_transition_to_x = 0;
static int s_transition_to_y = 0;
static uint32_t s_transition_start_ms = 0;
static uint32_t s_transition_first_move_ms = 0;

static bool s_transition_last_valid = false;
static int s_transition_last_x = 0;
static int s_transition_last_y = 0;
static float s_transition_path_len_px = 0.0f;
static float s_transition_max_dev_px = 0.0f;

static uint32_t s_transition_count = 0;
static uint32_t s_transition_total_completion_ms = 0;
static float s_transition_total_path_len_px = 0.0f;
static float s_transition_total_straightness = 0.0f;
static float s_transition_total_max_dev_px = 0.0f;

static char s_transition_weak_relation[48] = "none";
static uint32_t s_transition_slowest_ms = 0;

static char s_next_guided_relation[48] = "none";
static char s_guided_plan_regions[AIM_GUIDED_PLAN_MAX][3];
static int s_guided_plan_count = 0;
static int s_guided_plan_index = 0;

static uint32_t s_guided_bias_attempt_count = 0;
static uint32_t s_guided_bias_accept_count = 0;
static uint32_t s_guided_bias_forward_count = 0;
static uint32_t s_guided_bias_reverse_count = 0;
static uint32_t s_guided_bias_entry_count = 0;
static uint32_t s_guided_bias_streak_count = 0;
static uint32_t s_guided_bias_reverse_streak_count = 0;
static char s_guided_bias_active_relation[48] = "none";
static char s_guided_bias_summary[192] = "Bias relation: none | accepted: 0 / 0 | F/R/E: 0 / 0 / 0";

/*
 * Round target policy v3:
 * - cover all 9 regions first
 * - then use balanced random placement
 * - guided weak-relation compensation is only a small-dose insertion
 */
static char s_region_coverage_plan[AIM_REGION_COVERAGE_COUNT][3];
static uint8_t s_region_coverage_plan_index = 0;
static uint16_t s_region_spawn_count[AIM_REGION_COVERAGE_COUNT] = {0};
static uint32_t s_region_coverage_forced_count = 0;


/*
 * Forward declarations for transition-analysis helpers.
 * These functions are defined after the round UI functions, but C requires
 * declarations before first use.
 */
static void aim_transition_clear_round(void);
static void aim_transition_log_round_summary(void);
static void aim_transition_push_pointer(const air_input_state_t *st, uint32_t now);
static void aim_transition_finish(uint32_t hit_ms, uint32_t completion_ms);
static void aim_transition_begin(int from_x,
                                 int from_y,
                                 int to_x,
                                 int to_y,
                                 uint32_t start_ms);



static void start_new_round(void);
static void aim_show_boot_screen(void);
static void aim_boot_timer_cb(lv_timer_t *timer);
static void aim_delete_boot_page_if_needed(void);

static int aim_star_clamp_int_local(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static void aim_star_create_deco_circle(lv_obj_t *parent,
                                        int x,
                                        int y,
                                        int d,
                                        uint32_t color,
                                        lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        return;
    }

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, d, d);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, d / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void aim_delete_star_game_page_if_needed(void)
{
    if (s_star_feedback_label != NULL) {
        AIM_OBJ_DELETE(s_star_feedback_label);
        s_star_feedback_label = NULL;
    }

    if (s_star_page != NULL) {
        AIM_OBJ_DELETE(s_star_page);
        s_star_page = NULL;
    }

    s_star_feedback_until_ms = 0;
}

static void aim_star_show_hit_feedback(int x, int y, uint32_t reaction_ms, uint32_t now)
{
    if (s_star_feedback_label == NULL) {
        return;
    }

    lv_obj_clear_flag(s_star_feedback_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_star_feedback_label,
                   aim_star_clamp_int_local(x - 118, 28, AIM_SCREEN_W - 250),
                   aim_star_clamp_int_local(y - 108, 88, AIM_SCREEN_H - 150));

    airtouch_sfx_hit_v1d();

    if (reaction_ms < 800) {
        aim_label_set_text_fmt_cn_v2n2(s_star_feedback_label,
                              "很棒! +10  %lums",
                              (unsigned long)reaction_ms);
    } else if (reaction_ms < 1400) {
        aim_label_set_text_fmt_cn_v2n2(s_star_feedback_label,
                              "很棒! +10  %lums",
                              (unsigned long)reaction_ms);
    } else {
        aim_label_set_text_fmt_cn_v2n2(s_star_feedback_label,
                              "很棒! +10  %lums",
                              (unsigned long)reaction_ms);
    }

    s_star_feedback_until_ms = now + 760U;
    lv_obj_move_foreground(s_star_feedback_label);
}

static void aim_star_update_feedback(uint32_t now)
{
    if (s_star_feedback_label == NULL) {
        return;
    }

    if (s_star_feedback_until_ms == 0 || now > s_star_feedback_until_ms) {
        lv_obj_add_flag(s_star_feedback_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void aim_delete_menu_page_if_needed(void);
static void aim_show_star_game_page(void)
{
    /*
     * Entering Star Catcher from the storybook main menu.
     * Delete the full-screen menu page first; otherwise its white clouds/cards/header
     * can remain as a visible layer above or around the Star page.
     */
    aim_delete_menu_page_if_needed();
    aim_delete_star_game_page_if_needed();

    /*
     * Star Catcher page v4
     * --------------------
     * Clean rebuild of the page visual layer.
     *
     * Old issue:
     * - The previous Star page mixed root-screen HUD objects with a background page.
     * - Large white decorative circles made the page look like a white overlay.
     *
     * New rule:
     * - s_star_page is a clean full-screen visual background.
     * - No large white cloud layer.
     * - Global HUD objects are only positioned/styled above the page.
     * - Target objects remain unchanged and are still re-shown in start_new_round().
     */
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s_star_page = lv_obj_create(scr);
    if (s_star_page == NULL) {
        return;
    }

    lv_obj_remove_style_all(s_star_page);
    lv_obj_set_size(s_star_page, AIM_SCREEN_W, AIM_SCREEN_H);
    lv_obj_set_pos(s_star_page, 0, 0);
    lv_obj_set_style_bg_color(s_star_page, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_star_page, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_star_page, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_star_page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_star_page, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_star_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_star_page, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_star_page, LV_OBJ_FLAG_SCROLLABLE);

        /*
     * Irregular colorful dreamy dots on pure black background.
     * Sparse, playful, and child-friendly.
     */
    aim_star_create_deco_circle(s_star_page, 84, 72, 7, 0xFFE58A, (lv_opa_t)220);
    aim_star_create_deco_circle(s_star_page, 168, 132, 5, 0x9FD6FF, (lv_opa_t)210);
    aim_star_create_deco_circle(s_star_page, 290, 86, 6, 0xD8C2FF, (lv_opa_t)200);
    aim_star_create_deco_circle(s_star_page, 412, 148, 4, 0xFFB6D5, (lv_opa_t)210);
    aim_star_create_deco_circle(s_star_page, 598, 102, 7, 0xB8F2C8, (lv_opa_t)205);
    aim_star_create_deco_circle(s_star_page, 706, 78, 5, 0xFFE58A, (lv_opa_t)220);
    aim_star_create_deco_circle(s_star_page, 844, 128, 6, 0x9FD6FF, (lv_opa_t)205);
    aim_star_create_deco_circle(s_star_page, 930, 86, 4, 0xFFB6D5, (lv_opa_t)210);

    aim_star_create_deco_circle(s_star_page, 118, 242, 5, 0xD8C2FF, (lv_opa_t)190);
    aim_star_create_deco_circle(s_star_page, 914, 226, 6, 0xB8F2C8, (lv_opa_t)185);

    aim_star_create_deco_circle(s_star_page, 74, 412, 6, 0x9FD6FF, (lv_opa_t)200);
    aim_star_create_deco_circle(s_star_page, 212, 506, 5, 0xFFE58A, (lv_opa_t)190);
    aim_star_create_deco_circle(s_star_page, 384, 548, 4, 0xFFB6D5, (lv_opa_t)185);
    aim_star_create_deco_circle(s_star_page, 612, 518, 6, 0xD8C2FF, (lv_opa_t)190);
    aim_star_create_deco_circle(s_star_page, 772, 482, 5, 0xB8F2C8, (lv_opa_t)180);
    aim_star_create_deco_circle(s_star_page, 906, 534, 7, 0xFFE58A, (lv_opa_t)195);

/*
     * Put the clean page in front of old root background.
     * Global HUD and targets will then be moved above it.
     */
    lv_obj_move_foreground(s_star_page);

    if (s_title_label == NULL) {
        s_title_label = lv_label_create(scr);
    }

    if (s_title_label != NULL) {
        lv_obj_clear_flag(s_title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_title_label, 260);
        lv_obj_set_pos(s_title_label, 728, 18);
        lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        aim_label_set_text_cn_v2n2(s_title_label, "星星追踪");
        lv_obj_move_foreground(s_title_label);
    }

    if (s_info_label == NULL) {
        s_info_label = lv_label_create(scr);
    }

    if (s_info_label != NULL) {
        lv_obj_clear_flag(s_info_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_info_label, 540);
        lv_obj_set_pos(s_info_label, 36, 18);
        lv_obj_set_style_text_color(s_info_label, lv_color_hex(0xDDEBFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_info_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        aim_label_set_text_fmt_cn_v2n2(s_info_label, "时间 %lus    星星 0    平均 0ms", (unsigned long)(s_star_round_ms_v2j / 1000U));
        lv_obj_move_foreground(s_info_label);
    }

    if (s_status_label == NULL) {
        s_status_label = lv_label_create(scr);
    }

    if (s_status_label != NULL) {
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_status_label, 900);
        lv_obj_set_pos(s_status_label, 62, 562);
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xDDEBFF), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        aim_label_set_text_cn_v2n2(s_status_label, "移动到星星上方，保持悬停完成捕捉");
        lv_obj_move_foreground(s_status_label);
    }

    if (s_progress_bar == NULL) {
        s_progress_bar = lv_bar_create(scr);
    }

    if (s_progress_bar != NULL) {
        lv_obj_clear_flag(s_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_progress_bar, 340, 8);
        lv_obj_set_pos(s_progress_bar, 342, 538);
        lv_bar_set_range(s_progress_bar, 0, 1000);
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_radius(s_progress_bar, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x263040), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0xFFD56A), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_move_foreground(s_progress_bar);
    }

    s_star_feedback_label = lv_label_create(scr);
    if (s_star_feedback_label != NULL) {
        lv_obj_set_width(s_star_feedback_label, 250);
        lv_obj_set_style_text_color(s_star_feedback_label, lv_color_hex(0xFF9F1C), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_star_feedback_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        aim_label_set_text_cn_v2n2(s_star_feedback_label, "很棒! +10");
        lv_obj_add_flag(s_star_feedback_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_star_feedback_label);
    }

    ESP_LOGI(TAG, "Star page finalized v11: black sky, colorful dots, safe HUD zones");
}


static void aim_show_main_menu(void);
static void aim_menu_process_airtouch(uint32_t now);
static void aim_delete_menu_page_if_needed(void);
static void color_go_start_round(void);
static void color_go_process_airtouch(uint32_t now);
static void color_go_delete_pages(void);
static uint32_t color_go_speed_score_from_avg_ms(uint32_t avg_ms);
static void record_show_page(void);
static void record_process_airtouch(uint32_t now);
static void record_delete_page(void);
static void color_go_history_push(uint32_t accuracy,
                                  uint32_t avg_ms,
                                  uint32_t inhibition,
                                  uint32_t correct);






#define AIM_BOOT_FRAME_MS 120
#define AIM_BOOT_TOTAL_FRAMES 18

static void aim_boot_create_circle(lv_obj_t *parent,
                                   int x,
                                   int y,
                                   int d,
                                   uint32_t color,
                                   lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        return;
    }

    lv_obj_set_size(obj, d, d);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, d / 2, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *aim_boot_create_dot(lv_obj_t *parent,
                                     int x,
                                     int y,
                                     int d,
                                     uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    if (dot == NULL) {
        return NULL;
    }

    lv_obj_set_size(dot, d, d);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_radius(dot, d / 2, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(dot, 10, 0);
    lv_obj_set_style_shadow_opa(dot, (lv_opa_t)45, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    return dot;
}

static void aim_delete_boot_page_if_needed(void)
{
    if (s_boot_timer != NULL) {
        lv_timer_del(s_boot_timer);
        s_boot_timer = NULL;
    }

    if (s_boot_page != NULL) {
        AIM_OBJ_DELETE(s_boot_page);
        s_boot_page = NULL;
    }

    s_boot_loading_label = NULL;
    for (int i = 0; i < 4; i++) {
        s_boot_dots[i] = NULL;
    }

    s_boot_frame = 0;
}

static void aim_boot_update_visuals(void)
{
    const uint32_t colors[4] = {0x4F8DF7, 0xFFD56A, 0x26C6A3, 0xFF9AAE};
    uint32_t active = (s_boot_frame / 2U) % 4U;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = s_boot_dots[i];
        if (dot == NULL) {
            continue;
        }

        if ((uint32_t)i == active) {
            lv_obj_set_size(dot, 28, 28);
            lv_obj_set_pos(dot, 430 + i * 54, 368);
            lv_obj_set_style_radius(dot, 14, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(colors[i]), 0);
            lv_obj_set_style_shadow_color(dot, lv_color_hex(colors[i]), 0);
            lv_obj_set_style_shadow_width(dot, 16, 0);
            lv_obj_set_style_shadow_opa(dot, (lv_opa_t)70, 0);
        } else {
            lv_obj_set_size(dot, 20, 20);
            lv_obj_set_pos(dot, 434 + i * 54, 372);
            lv_obj_set_style_radius(dot, 10, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(colors[i]), 0);
            lv_obj_set_style_shadow_width(dot, 8, 0);
            lv_obj_set_style_shadow_opa(dot, (lv_opa_t)35, 0);
        }
    }

    if (s_boot_loading_label != NULL) {
        switch (s_boot_frame % 4U) {
        case 0:
            aim_label_set_text_cn_v2n2(s_boot_loading_label, "Loading");
            break;
        case 1:
            aim_label_set_text_cn_v2n2(s_boot_loading_label, "Loading.");
            break;
        case 2:
            aim_label_set_text_cn_v2n2(s_boot_loading_label, "Loading..");
            break;
        default:
            aim_label_set_text_cn_v2n2(s_boot_loading_label, "Loading...");
            break;
        }
    }
}

static void aim_boot_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_app_mode != AIM_APP_MODE_BOOT) {
        return;
    }

    s_boot_frame++;
    aim_boot_update_visuals();

    if (s_boot_frame >= AIM_BOOT_TOTAL_FRAMES) {
        lv_timer_t *done_timer = s_boot_timer;
        s_boot_timer = NULL;

        if (done_timer != NULL) {
            lv_timer_del(done_timer);
        }

        if (s_boot_page != NULL) {
            AIM_OBJ_DELETE(s_boot_page);
            s_boot_page = NULL;
        }

        s_boot_loading_label = NULL;
        for (int i = 0; i < 4; i++) {
            s_boot_dots[i] = NULL;
        }

        s_boot_frame = 0;

        ESP_LOGI(TAG, "Boot animation finished -> main menu");
        aim_show_main_menu();
    }
}

static void aim_show_boot_screen(void)
{
    aim_delete_boot_page_if_needed();

    s_app_mode = AIM_APP_MODE_BOOT;
    s_hover_target_id = 0;
    s_hover_progress = 0;
    s_menu_ctrl_wait_release = false;
    s_result_ctrl_wait_release = false;
    s_result_ctrl_last_click_ms = 0;

    air_attention_heatmap_set_enabled(false);

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }

    s_boot_page = lv_obj_create(lv_scr_act());
    if (s_boot_page == NULL) {
        aim_show_main_menu();
        return;
    }

    lv_obj_set_size(s_boot_page, AIM_SCREEN_W, AIM_SCREEN_H);
    lv_obj_align(s_boot_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_boot_page, lv_color_hex(0xDFF3FF), 0);
    lv_obj_set_style_bg_grad_color(s_boot_page, lv_color_hex(0xFFF6E8), 0);
    lv_obj_set_style_bg_grad_dir(s_boot_page, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_boot_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_boot_page, 0, 0);
    lv_obj_set_style_radius(s_boot_page, 0, 0);
    lv_obj_set_style_pad_all(s_boot_page, 0, 0);
    lv_obj_clear_flag(s_boot_page, LV_OBJ_FLAG_SCROLLABLE);

    /* soft storybook background */
    aim_boot_create_circle(s_boot_page, 70, 62, 70, 0xFFFFFF, (lv_opa_t)210);
    aim_boot_create_circle(s_boot_page, 122, 48, 48, 0xFFFFFF, (lv_opa_t)210);
    aim_boot_create_circle(s_boot_page, 790, 78, 88, 0xFFFFFF, (lv_opa_t)190);
    aim_boot_create_circle(s_boot_page, 868, 88, 54, 0xFFFFFF, (lv_opa_t)190);
    aim_boot_create_circle(s_boot_page, 864, 60, 36, 0xFFE27B, LV_OPA_COVER);
    aim_boot_create_circle(s_boot_page, 96, 480, 160, 0xD9F2E0, (lv_opa_t)220);
    aim_boot_create_circle(s_boot_page, 784, 500, 180, 0xD9F2E0, (lv_opa_t)220);

    lv_obj_t *panel = lv_obj_create(s_boot_page);
    if (panel != NULL) {
        lv_obj_set_size(panel, 680, 380);
        lv_obj_set_pos(panel, 172, 102);
        lv_obj_set_style_radius(panel, 46, 0);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(panel, (lv_opa_t)238, 0);
        lv_obj_set_style_border_width(panel, 0, 0);
        lv_obj_set_style_shadow_color(panel, lv_color_hex(0xABC2D7), 0);
        lv_obj_set_style_shadow_width(panel, 22, 0);
        lv_obj_set_style_shadow_opa(panel, (lv_opa_t)48, 0);
        lv_obj_set_style_shadow_ofs_y(panel, 8, 0);
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

        aim_boot_create_circle(panel, 266, 28, 72, 0x4F8DF7, LV_OPA_COVER);
        aim_boot_create_circle(panel, 324, 40, 54, 0xFFD56A, LV_OPA_COVER);
        aim_boot_create_circle(panel, 288, 92, 48, 0x26C6A3, LV_OPA_COVER);
        aim_boot_create_circle(panel, 358, 92, 42, 0xFF9AAE, LV_OPA_COVER);
        aim_boot_create_circle(panel, 314, 64, 46, 0xFFFFFF, (lv_opa_t)230);

        lv_obj_t *title = lv_label_create(panel);
        if (title != NULL) {
            lv_obj_set_width(title, 580);
            lv_obj_set_pos(title, 50, 156);
            lv_obj_set_style_text_color(title, lv_color_hex(0x2674B8), 0);
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
            aim_label_set_text_cn_v2n2(title, "AirTouch");
        }

        lv_obj_t *subtitle = lv_label_create(panel);
        if (subtitle != NULL) {
            lv_obj_set_width(subtitle, 580);
            lv_obj_set_pos(subtitle, 50, 208);
            lv_obj_set_style_text_color(subtitle, lv_color_hex(0x34506B), 0);
            lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
            aim_label_set_text_cn_v2n2(subtitle, "Touchless Play | Learn | Grow");
        }

        s_boot_loading_label = lv_label_create(panel);
        if (s_boot_loading_label != NULL) {
            lv_obj_set_width(s_boot_loading_label, 240);
            lv_obj_set_pos(s_boot_loading_label, 220, 304);
            lv_obj_set_style_text_color(s_boot_loading_label, lv_color_hex(0x6A8094), 0);
            lv_obj_set_style_text_align(s_boot_loading_label, LV_TEXT_ALIGN_CENTER, 0);
            aim_label_set_text_cn_v2n2(s_boot_loading_label, "Loading");
        }
    }

    s_boot_dots[0] = aim_boot_create_dot(s_boot_page, 434, 372, 20, 0x4F8DF7);
    s_boot_dots[1] = aim_boot_create_dot(s_boot_page, 488, 372, 20, 0xFFD56A);
    s_boot_dots[2] = aim_boot_create_dot(s_boot_page, 542, 372, 20, 0x26C6A3);
    s_boot_dots[3] = aim_boot_create_dot(s_boot_page, 596, 372, 20, 0xFF9AAE);

    aim_boot_update_visuals();

    s_boot_timer = lv_timer_create(aim_boot_timer_cb, AIM_BOOT_FRAME_MS, NULL);

    lv_obj_move_foreground(s_boot_page);

    ESP_LOGI(TAG, "Boot animation started v3 light storybook background");
}

static void aim_set_control_button_visible(const char *text, bool visible)
{
    s_app_mode = AIM_APP_MODE_STAR_CATCHER;
    aim_delete_menu_page_if_needed();
    if (s_control_button == NULL) {
        return;
    }

    if (text != NULL && s_control_button_label != NULL) {
        aim_label_set_text_cn_v2n2(s_control_button_label, text);
        lv_obj_center(s_control_button_label);
    }

    if (visible) {
        lv_obj_clear_flag(s_control_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_control_button);
    } else {
        lv_obj_add_flag(s_control_button, LV_OBJ_FLAG_HIDDEN);
    }
}

static void aim_control_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_game_state == AIM_STATE_RUNNING) {
        return;
    }

    start_new_round();
}




static void aim_delete_menu_page_if_needed(void)
{
    if (s_menu_page != NULL) {
        AIM_OBJ_DELETE(s_menu_page);
        s_menu_page = NULL;
    }

    s_menu_star_btn = NULL;
    s_menu_color_btn = NULL;
    s_menu_record_btn = NULL;
    s_menu_ctrl_wait_release = false;
}

typedef enum {
    AIM_MENU_CARD_KIND_STAR = 0,
    AIM_MENU_CARD_KIND_COLOR,
    AIM_MENU_CARD_KIND_RECORD,
} aim_menu_card_kind_t;

static void aim_menu_create_deco_circle(lv_obj_t *parent,
                                        int x,
                                        int y,
                                        int d,
                                        uint32_t color,
                                        lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (obj == NULL) {
        return;
    }

    lv_obj_set_size(obj, d, d);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, d / 2, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void aim_menu_create_pill(lv_obj_t *parent,
                                 int x,
                                 int y,
                                 int w,
                                 int h,
                                 const char *text,
                                 uint32_t text_color,
                                 uint32_t bg_color)
{
    lv_obj_t *pill = lv_obj_create(parent);
    if (pill == NULL) {
        return;
    }

    lv_obj_set_size(pill, w, h);
    lv_obj_set_pos(pill, x, y);
    lv_obj_set_style_radius(pill, h / 2, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(pill);
    if (label != NULL) {
        lv_obj_set_width(label, w - 12);
        lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(label, text);
        lv_obj_center(label);
    }
}

static void aim_menu_create_cloud(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *base = lv_obj_create(parent);
    if (base != NULL) {
        lv_obj_set_size(base, 86, 28);
        lv_obj_set_pos(base, x + 16, y + 22);
        lv_obj_set_style_radius(base, 14, 0);
        lv_obj_set_style_bg_color(base, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(base, 0, 0);
        lv_obj_set_style_pad_all(base, 0, 0);
        lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    }

    aim_menu_create_deco_circle(parent, x + 6,  y + 14, 30, color, LV_OPA_COVER);
    aim_menu_create_deco_circle(parent, x + 26, y,      38, color, LV_OPA_COVER);
    aim_menu_create_deco_circle(parent, x + 52, y + 12, 28, color, LV_OPA_COVER);
}


static void aim_menu_create_twinkle(lv_obj_t *parent,
                                    int x,
                                    int y,
                                    uint32_t color,
                                    const char *text)
{
    lv_obj_t *spark = lv_label_create(parent);
    if (spark == NULL) {
        return;
    }

    lv_obj_set_width(spark, 24);
    lv_obj_set_pos(spark, x, y);
    lv_obj_set_style_text_color(spark, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(spark, LV_TEXT_ALIGN_CENTER, 0);
    aim_label_set_text_cn_v2n2(spark, text);
}

static void aim_menu_create_flower(lv_obj_t *parent,
                                   int x,
                                   int y,
                                   uint32_t petal_color,
                                   uint32_t center_color)
{
    aim_menu_create_deco_circle(parent, x + 10, y,      12, petal_color, LV_OPA_COVER);
    aim_menu_create_deco_circle(parent, x,      y + 10, 12, petal_color, LV_OPA_COVER);
    aim_menu_create_deco_circle(parent, x + 20, y + 10, 12, petal_color, LV_OPA_COVER);
    aim_menu_create_deco_circle(parent, x + 10, y + 20, 12, petal_color, LV_OPA_COVER);
    aim_menu_create_deco_circle(parent, x + 10, y + 10, 12, center_color, LV_OPA_COVER);
}

static void aim_menu_create_rainbow(lv_obj_t *parent, int x, int y)
{
    const uint32_t colors[4] = {0xFF9AAE, 0xFFD56A, 0x7DC6FF, 0x9BE0A5};
    const int widths[4] = {92, 76, 60, 44};
    const int heights[4] = {50, 42, 34, 26};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *arc = lv_obj_create(parent);
        if (arc == NULL) {
            continue;
        }

        lv_obj_set_size(arc, widths[i], heights[i]);
        lv_obj_set_pos(arc, x + (92 - widths[i]) / 2, y + i * 4);
        lv_obj_set_style_radius(arc, 24, 0);
        lv_obj_set_style_bg_color(arc, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(arc, 0, 0);
        lv_obj_set_style_pad_all(arc, 0, 0);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *cover = lv_obj_create(parent);
    if (cover != NULL) {
        lv_obj_set_size(cover, 98, 28);
        lv_obj_set_pos(cover, x - 3, y + 24);
        lv_obj_set_style_radius(cover, 14, 0);
        lv_obj_set_style_bg_color(cover, lv_color_hex(0xDFF3FF), 0);
        lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cover, 0, 0);
        lv_obj_set_style_pad_all(cover, 0, 0);
        lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void aim_menu_create_card_icon(lv_obj_t *card,
                                      aim_menu_card_kind_t kind,
                                      uint32_t main_color)
{
    lv_obj_t *panel = lv_obj_create(card);
    if (panel == NULL) {
        return;
    }

    lv_obj_set_size(panel, 112, 74);
    lv_obj_set_pos(panel, 36, 18);
    lv_obj_set_style_radius(panel, 32, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xEAF1F7), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(main_color), 0);
    lv_obj_set_style_shadow_width(panel, 14, 0);
    lv_obj_set_style_shadow_opa(panel, (lv_opa_t)55, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    if (kind == AIM_MENU_CARD_KIND_STAR) {
        aim_menu_create_deco_circle(panel, 18, 18, 30, 0xFFD85E, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 46, 12, 18, 0x7EC7FF, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 68, 22, 16, 0xFF9FC2, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 34, 42, 14, 0x8FE3A8, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 60, 44, 12, 0xFFE98B, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 12, 48, 10, 0xB9D8FF, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 36, 26, 18, 0xFFFFFF, (lv_opa_t)220);

        lv_obj_t *spark = lv_label_create(panel);
        if (spark != NULL) {
            lv_obj_set_width(spark, 28);
            lv_obj_set_pos(spark, 24, 18);
            lv_obj_set_style_text_color(spark, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_align(spark, LV_TEXT_ALIGN_CENTER, 0);
            aim_label_set_text_cn_v2n2(spark, "*");
        }
    } else if (kind == AIM_MENU_CARD_KIND_COLOR) {
        aim_menu_create_deco_circle(panel, 16, 24, 22, 0x44A8FF, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 40, 16, 28, 0xFFCF5B, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 68, 26, 18, 0xFF7DA8, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 56, 42, 20, 0x41D6B5, LV_OPA_COVER);
        aim_menu_create_deco_circle(panel, 26, 46, 12, 0xFFFFFF, (lv_opa_t)215);
        aim_menu_create_twinkle(panel, 82, 12, 0xFFB95D, "*");
    } else {
        lv_obj_t *bar1 = lv_obj_create(panel);
        if (bar1 != NULL) {
            lv_obj_set_size(bar1, 16, 24);
            lv_obj_set_pos(bar1, 20, 34);
            lv_obj_set_style_radius(bar1, 8, 0);
            lv_obj_set_style_bg_color(bar1, lv_color_hex(0x7CC8FF), 0);
            lv_obj_set_style_bg_opa(bar1, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar1, 0, 0);
            lv_obj_clear_flag(bar1, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_obj_t *bar2 = lv_obj_create(panel);
        if (bar2 != NULL) {
            lv_obj_set_size(bar2, 16, 34);
            lv_obj_set_pos(bar2, 44, 24);
            lv_obj_set_style_radius(bar2, 8, 0);
            lv_obj_set_style_bg_color(bar2, lv_color_hex(0x9B87FF), 0);
            lv_obj_set_style_bg_opa(bar2, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar2, 0, 0);
            lv_obj_clear_flag(bar2, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_obj_t *bar3 = lv_obj_create(panel);
        if (bar3 != NULL) {
            lv_obj_set_size(bar3, 16, 42);
            lv_obj_set_pos(bar3, 68, 16);
            lv_obj_set_style_radius(bar3, 8, 0);
            lv_obj_set_style_bg_color(bar3, lv_color_hex(0x4CD8B1), 0);
            lv_obj_set_style_bg_opa(bar3, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar3, 0, 0);
            lv_obj_clear_flag(bar3, LV_OBJ_FLAG_SCROLLABLE);
        }

        aim_menu_create_deco_circle(panel, 16, 10, 10, 0xFFD266, LV_OPA_COVER);
        aim_menu_create_twinkle(panel, 80, 8, 0x9B87FF, "*");
    }
}

static void aim_menu_set_card_style(lv_obj_t *card, bool active, uint32_t base_color)
{
    if (card == NULL) {
        return;
    }

    lv_obj_set_style_border_color(card,
                                  active ? lv_color_hex(base_color) : lv_color_hex(0xDCEBFA),
                                  0);
    lv_obj_set_style_border_width(card, active ? 5 : 2, 0);

    lv_obj_set_style_shadow_color(card,
                                  active ? lv_color_hex(base_color) : lv_color_hex(0xABC2D7),
                                  0);
    lv_obj_set_style_shadow_width(card, active ? 30 : 14, 0);
    lv_obj_set_style_shadow_opa(card, active ? (lv_opa_t)85 : (lv_opa_t)38, 0);
    lv_obj_set_style_shadow_ofs_y(card, active ? 10 : 5, 0);
}

static lv_obj_t *aim_menu_create_card(lv_obj_t *parent,
                                      int x,
                                      int y,
                                      int w,
                                      int h,
                                      const char *title_text,
                                      const char *subtitle_text,
                                      const char *tag_text,
                                      uint32_t color,
                                      uint32_t card_bg,
                                      aim_menu_card_kind_t kind)
{
    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return NULL;
    }

    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 38, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(card_bg), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xDCEBFA), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0xABC2D7), 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_opa(card, (lv_opa_t)38, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(card);
    if (accent != NULL) {
        lv_obj_set_size(accent, 10, h - 52);
        lv_obj_set_pos(accent, 18, 26);
        lv_obj_set_style_radius(accent, 10, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);
    }

    aim_menu_create_card_icon(card, kind, color);

    lv_obj_t *arrow_bg = lv_obj_create(card);
    if (arrow_bg != NULL) {
        lv_obj_set_size(arrow_bg, 42, 42);
        lv_obj_set_pos(arrow_bg, w - 58, 36);
        lv_obj_set_style_radius(arrow_bg, 21, 0);
        lv_obj_set_style_bg_color(arrow_bg, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(arrow_bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(arrow_bg, lv_color_hex(0xE6EEF8), 0);
        lv_obj_set_style_border_width(arrow_bg, 1, 0);
        lv_obj_set_style_pad_all(arrow_bg, 0, 0);
        lv_obj_clear_flag(arrow_bg, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *arrow = lv_label_create(arrow_bg);
        if (arrow != NULL) {
            lv_obj_set_width(arrow, 26);
            lv_obj_set_style_text_color(arrow, lv_color_hex(color), 0);
            lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_CENTER, 0);
            aim_label_set_text_cn_v2n2(arrow, ">");
            lv_obj_center(arrow);
        }
    }

    lv_obj_t *title = lv_label_create(card);
    if (title != NULL) {
        lv_obj_set_width(title, w - 72);
        lv_obj_set_pos(title, 38, 98);
        lv_obj_set_style_text_color(title, lv_color_hex(0x203850), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(title, title_text);
    }

    lv_obj_t *subtitle = lv_label_create(card);
    if (subtitle != NULL) {
        lv_obj_set_width(subtitle, w - 76);
        lv_obj_set_pos(subtitle, 38, 132);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0x637B91), 0);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
        aim_label_set_text_cn_v2n2(subtitle, subtitle_text);
    }

    aim_menu_create_pill(card, 38, h - 34, 138, 28, tag_text, color, 0xFFFFFF);

    return card;
}

static void aim_show_main_menu(void)
{
    record_delete_page();
    aim_delete_star_game_page_if_needed();

    s_app_mode = AIM_APP_MODE_MENU;
    s_hover_target_id = 0;
    s_hover_progress = 0;
    s_menu_ctrl_wait_release = false;
    s_result_ctrl_wait_release = false;
    s_result_ctrl_last_click_ms = 0;

    air_attention_heatmap_set_enabled(false);

    air_input_config_t menu_cfg = {
        .dwell_ms = 520,
        .cooldown_ms = 260,
        .stable_radius_px = 54,
        .target_expand_px = 24,
    };
    air_input_set_config(&menu_cfg);

    if (s_result_layer != NULL) {
        AIM_OBJ_DELETE(s_result_layer);
        s_result_layer = NULL;
        s_result_analysis_btn = NULL;
        s_result_next_btn = NULL;
        s_result_home_btn = NULL;
    }

    if (s_result_page != NULL) {
        AIM_OBJ_DELETE(s_result_page);
        s_result_page = NULL;
    }

    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        if (s_targets[i].obj != NULL) {
            lv_obj_add_flag(s_targets[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    aim_delete_menu_page_if_needed();

    s_menu_page = lv_obj_create(lv_scr_act());
    if (s_menu_page == NULL) {
        return;
    }

    lv_obj_set_size(s_menu_page, AIM_SCREEN_W, AIM_SCREEN_H);
    lv_obj_align(s_menu_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_menu_page, lv_color_hex(0xDFF3FF), 0);
    lv_obj_set_style_bg_grad_color(s_menu_page, lv_color_hex(0xFFF5DF), 0);
    lv_obj_set_style_bg_grad_dir(s_menu_page, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_menu_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_menu_page, 0, 0);
    lv_obj_set_style_radius(s_menu_page, 0, 0);
    lv_obj_set_style_pad_all(s_menu_page, 0, 0);
    lv_obj_clear_flag(s_menu_page, LV_OBJ_FLAG_SCROLLABLE);

    /* storybook sky */
    aim_menu_create_cloud(s_menu_page, 58, 38, 0xFFFFFF);
    aim_menu_create_cloud(s_menu_page, 328, 54, 0xF8FCFF);
    aim_menu_create_cloud(s_menu_page, 762, 48, 0xFFFFFF);
    aim_menu_create_rainbow(s_menu_page, 864, 72);
    aim_menu_create_deco_circle(s_menu_page, 918, 40, 34, 0xFFE27B, LV_OPA_COVER);
    aim_menu_create_deco_circle(s_menu_page, 74, 118, 16, 0xFFE08D, (lv_opa_t)180);
    aim_menu_create_deco_circle(s_menu_page, 126, 98, 10, 0xA9D7FF, (lv_opa_t)180);
    aim_menu_create_deco_circle(s_menu_page, 874, 132, 14, 0xFFC7DB, (lv_opa_t)180);
    aim_menu_create_deco_circle(s_menu_page, 832, 112, 10, 0xB6E7C9, (lv_opa_t)180);
    aim_menu_create_twinkle(s_menu_page, 148, 84, 0xFFD56A, "*");
    aim_menu_create_twinkle(s_menu_page, 292, 72, 0x7DC6FF, "*");
    aim_menu_create_twinkle(s_menu_page, 816, 96, 0xFF9AAE, "*");

    /* grass / ground */
    lv_obj_t *ground = lv_obj_create(s_menu_page);
    if (ground != NULL) {
        lv_obj_set_size(ground, 980, 66);
        lv_obj_set_pos(ground, 22, 526);
        lv_obj_set_style_radius(ground, 30, 0);
        lv_obj_set_style_bg_color(ground, lv_color_hex(0xD9F2E0), 0);
        lv_obj_set_style_bg_opa(ground, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ground, 0, 0);
        lv_obj_set_style_pad_all(ground, 0, 0);
        lv_obj_clear_flag(ground, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* keep the bottom ground continuous; add light storybook flowers */
    aim_menu_create_flower(s_menu_page, 42, 544, 0xFFE0A8, 0xFFD56A);
    aim_menu_create_flower(s_menu_page, 938, 544, 0xFFD1E2, 0xFFB0C9);
    aim_menu_create_twinkle(s_menu_page, 176, 548, 0x9DD6A7, "*");
    aim_menu_create_twinkle(s_menu_page, 854, 548, 0x9DD6A7, "*");

    lv_obj_t *header = lv_obj_create(s_menu_page);
    if (header != NULL) {
        lv_obj_set_size(header, 900, 116);
        lv_obj_set_pos(header, 62, 46);
        lv_obj_set_style_radius(header, 34, 0);
        lv_obj_set_style_bg_color(header, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(header, (lv_opa_t)236, 0);
        lv_obj_set_style_border_width(header, 0, 0);
        lv_obj_set_style_shadow_color(header, lv_color_hex(0xB7CADC), 0);
        lv_obj_set_style_shadow_width(header, 16, 0);
        lv_obj_set_style_shadow_opa(header, (lv_opa_t)42, 0);
        lv_obj_set_style_shadow_ofs_y(header, 5, 0);
        lv_obj_set_style_pad_all(header, 0, 0);
        lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *brand = lv_label_create(header);
        if (brand != NULL) {
            lv_obj_set_width(brand, 240);
            lv_obj_set_pos(brand, 30, 22);
            lv_obj_set_style_text_color(brand, lv_color_hex(0x2674B8), 0);
            aim_label_set_text_cn_v2n2(brand, "AirTouch");
        }

        lv_obj_t *name = lv_label_create(header);
        if (name != NULL) {
            lv_obj_set_width(name, 340);
            lv_obj_set_pos(name, 30, 56);
            lv_obj_set_style_text_color(name, lv_color_hex(0x34506B), 0);
            aim_label_set_text_cn_v2n2(name, "趣味智训板");
        }

        lv_obj_t *desc = lv_label_create(header);
        if (desc != NULL) {
            lv_obj_set_width(desc, 410);
            lv_obj_set_pos(desc, 336, 36);
            lv_obj_set_style_text_color(desc, lv_color_hex(0x72879B), 0);
            lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
            aim_label_set_text_cn_v2n2(desc, "");
            lv_obj_add_flag(desc, LV_OBJ_FLAG_HIDDEN);
        }

        /* Main menu clean v2n.3.1: top-right SET pill removed. */
    }

    aim_menu_create_pill(s_menu_page, 82, 182, 120, 34, "开始训练", 0xD26A00, 0xFFF0BF);

    lv_obj_t *hello = lv_label_create(s_menu_page);
    if (hello != NULL) {
        lv_obj_set_width(hello, 860);
        lv_obj_set_pos(hello, 72, 224);
        lv_obj_set_style_text_color(hello, lv_color_hex(0x263E56), 0);
        aim_label_set_text_cn_v2n2(hello, "");
        lv_obj_add_flag(hello, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *hint = lv_label_create(s_menu_page);
    if (hint != NULL) {
        lv_obj_set_width(hint, 860);
        lv_obj_set_pos(hint, 72, 258);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x6A8094), 0);
        aim_label_set_text_cn_v2n2(hint, "选择一张卡片，悬停开始训练");
    }

    s_menu_star_btn = aim_menu_create_card(s_menu_page,
                                           46,
                                           300,
                                           290,
                                           214,
                                           "星星追踪",
                                           "抓星星，练反应",
                                           "",
                                           0x4F8DF7,
                                           0xEEF6FF,
                                           AIM_MENU_CARD_KIND_STAR);

    s_menu_color_btn = aim_menu_create_card(s_menu_page,
                                            364,
                                            300,
                                            290,
                                            214,
                                            "颜色反应",
                                            "看颜色，练专注",
                                            "",
                                            0x26C6A3,
                                            0xEEFFF8,
                                            AIM_MENU_CARD_KIND_COLOR);

    s_menu_record_btn = aim_menu_create_card(s_menu_page,
                                             682,
                                             300,
                                             290,
                                             214,
                                             "成长档案",
                                             "看进步，慢慢成长",
                                             "",
                                             0x8D75F9,
                                             0xF3F0FF,
                                             AIM_MENU_CARD_KIND_RECORD);

    aim_menu_create_twinkle(s_menu_page, 332, 324, 0x7DC6FF, "*");
    aim_menu_create_twinkle(s_menu_page, 650, 318, 0xFFD56A, "*");
    aim_menu_create_twinkle(s_menu_page, 968, 328, 0xFF9AAE, "*");

            
    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }

    if (s_title_label != NULL) {
        aim_label_set_text_cn_v2n2(s_title_label, "趣味智训板");
    }

    if (s_info_label != NULL) {
        aim_label_set_text_cn_v2n2(s_info_label, "训练项目");
    }

    if (s_status_label != NULL) {
        
    }

    lv_obj_move_foreground(s_menu_page);

    ESP_LOGI(TAG, "Main menu created: UI polish v5 light storybook restored");
}

static void aim_menu_process_airtouch(uint32_t now)
{
    (void)now;

    air_input_circle_target_t menu_targets[3];

    menu_targets[0].id = AIM_MENU_BTN_STAR_ID;
    menu_targets[0].cx = AIM_MENU_BTN_STAR_X;
    menu_targets[0].cy = AIM_MENU_BTN_Y;
    menu_targets[0].r = AIM_MENU_BTN_R;

    menu_targets[1].id = AIM_MENU_BTN_COLOR_GO_ID;
    menu_targets[1].cx = AIM_MENU_BTN_COLOR_GO_X;
    menu_targets[1].cy = AIM_MENU_BTN_Y;
    menu_targets[1].r = AIM_MENU_BTN_R;

    menu_targets[2].id = AIM_MENU_BTN_RECORD_ID;
    menu_targets[2].cx = AIM_MENU_BTN_RECORD_X;
    menu_targets[2].cy = AIM_MENU_BTN_Y;
    menu_targets[2].r = AIM_MENU_BTN_R;

    air_input_state_t st;
    memset(&st, 0, sizeof(st));

    air_input_update_circle_targets(menu_targets, 3, &st);

    s_hover_target_id = st.target_inside ? st.target_id : 0;
    s_hover_progress = st.hover_progress;

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, st.hover_progress, LV_ANIM_OFF);
    }

    aim_menu_set_card_style(s_menu_star_btn,
                            st.target_inside && st.target_id == AIM_MENU_BTN_STAR_ID,
                            0x4F8DF7);
    aim_menu_set_card_style(s_menu_color_btn,
                            st.target_inside && st.target_id == AIM_MENU_BTN_COLOR_GO_ID,
                            0x26C6A3);
    aim_menu_set_card_style(s_menu_record_btn,
                            st.target_inside && st.target_id == AIM_MENU_BTN_RECORD_ID,
                            0x8D75F9);

    if (!st.target_inside) {
        s_menu_ctrl_wait_release = false;
    }

    if (!st.click) {
        return;
    }

    if (s_menu_ctrl_wait_release) {
        return;
    }

    s_menu_ctrl_wait_release = true;

    if (st.target_id == AIM_MENU_BTN_STAR_ID) {
        ESP_LOGI(TAG, "Menu AirTouch: start Star Catcher");
        s_app_mode = AIM_APP_MODE_STAR_CATCHER;
        aim_delete_menu_page_if_needed();
        start_new_round();
    } else if (st.target_id == AIM_MENU_BTN_COLOR_GO_ID) {
        ESP_LOGI(TAG, "Menu AirTouch: start Color-Go");
        color_go_start_round();
    } else if (st.target_id == AIM_MENU_BTN_RECORD_ID) {
        airtouch_sfx_click_v1d();
        ESP_LOGI(TAG, "Menu AirTouch: open Growth Record");
        record_show_page();
    }
}

static uint32_t color_go_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void color_go_delete_pages(void)
{
    if (s_color_go_page != NULL) {
        AIM_OBJ_DELETE(s_color_go_page);
        s_color_go_page = NULL;
    }

    if (s_color_go_result_page != NULL) {
        AIM_OBJ_DELETE(s_color_go_result_page);
        s_color_go_result_page = NULL;
    }

    s_color_go_next_btn = NULL;
    s_color_go_home_btn = NULL;
    s_color_go_rule_label = NULL;
    s_color_go_score_label = NULL;

    for (int i = 0; i < s_color_go_bubble_count_v2j; i++) {
        s_color_go_bubbles[i].obj = NULL;
        s_color_go_bubbles[i].label = NULL;
    }

    s_color_go_running = false;
    s_color_go_result_active = false;
    s_color_go_wait_release = false;
}

static uint32_t color_go_kind_color(color_go_bubble_kind_t kind)
{
    switch (kind) {
    case COLOR_GO_KIND_GO:
        return 0x2787FF;
    case COLOR_GO_KIND_NOGO:
        return 0xFF3D4F;
    case COLOR_GO_KIND_DISTRACTOR:
    default:
        return 0xFFC94A;
    }
}

static const char *color_go_kind_label(color_go_bubble_kind_t kind)
{
    switch (kind) {
    case COLOR_GO_KIND_GO:
        return "GO";
    case COLOR_GO_KIND_NOGO:
        return "NO";
    case COLOR_GO_KIND_DISTRACTOR:
    default:
        return "X";
    }
}

static void color_go_update_bubble_style(int idx, bool active)
{
    if (idx < 0 || idx >= s_color_go_bubble_count_v2j) {
        return;
    }

    color_go_bubble_t *b = &s_color_go_bubbles[idx];

    if (b->obj == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(b->obj, lv_color_hex(color_go_kind_color(b->kind)), 0);
    lv_obj_set_style_bg_opa(b->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b->obj,
                                  active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x244057),
                                  0);
    lv_obj_set_style_border_width(b->obj, active ? 5 : 2, 0);
    lv_obj_set_style_radius(b->obj, LV_RADIUS_CIRCLE, 0);

    if (b->label != NULL) {
        aim_label_set_text_cn_v2n2(b->label, color_go_kind_label(b->kind));
        lv_obj_center(b->label);
    }
}

static bool color_go_position_ok(int idx, int x, int y, int r)
{
    for (int i = 0; i < idx; i++) {
        int dx = x - s_color_go_bubbles[i].cx;
        int dy = y - s_color_go_bubbles[i].cy;
        int min_dist = r + s_color_go_bubbles[i].r + 34;

        if (dx * dx + dy * dy < min_dist * min_dist) {
            return false;
        }
    }

    return true;
}

static void color_go_place_bubble(int idx, uint32_t now)
{
    if (idx < 0 || idx >= s_color_go_bubble_count_v2j) {
        return;
    }

    color_go_bubble_t *b = &s_color_go_bubbles[idx];
    int r = s_color_go_target_r_v2j;

    int min_x = r + 60;
    int max_x = AIM_SCREEN_W - r - 60;
    int min_y = 150;
    int max_y = AIM_SCREEN_H - r - 60;

    int x = AIM_SCREEN_W / 2;
    int y = AIM_SCREEN_H / 2;

    for (int attempt = 0; attempt < 48; attempt++) {
        x = min_x + (int)(esp_random() % (uint32_t)(max_x - min_x + 1));
        y = min_y + (int)(esp_random() % (uint32_t)(max_y - min_y + 1));

        if (color_go_position_ok(idx, x, y, r)) {
            break;
        }
    }

    /*
     * Color-Go v1 bubble safe zone:
     * keep targets away from top HUD and bottom rule text.
     */
    {
        const int left = 52;
        const int right = AIM_SCREEN_W - 52;
        const int top = 124;
        const int bottom = 506;

        if (x - r < left) {
            x = left + r;
        }
        if (x + r > right) {
            x = right - r;
        }
        if (y - r < top) {
            y = top + r;
        }
        if (y + r > bottom) {
            y = bottom - r;
        }
    }

    b->cx = x;
    b->cy = y;
    b->r = r;
    b->spawn_ms = now;

    if (b->obj != NULL) {
        lv_obj_set_size(b->obj, r * 2, r * 2);
        lv_obj_set_pos(b->obj, x - r, y - r);
        lv_obj_clear_flag(b->obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(b->obj);
    }

    color_go_update_bubble_style(idx, false);
}

static void color_go_spawn_bubbles(uint32_t now)
{
    int count = aim_cloud_clamp_int_v2j(s_color_go_bubble_count_v2j, 3, COLOR_GO_BUBBLE_MAX);
    s_color_go_bubble_count_v2j = count;

    color_go_bubble_kind_t kinds[COLOR_GO_BUBBLE_MAX];

    for (int i = 0; i < COLOR_GO_BUBBLE_MAX; i++) {
        kinds[i] = COLOR_GO_KIND_DISTRACTOR;
    }

    int nogo_count = (count * s_color_go_nogo_ratio_v2j + 99) / 100;
    nogo_count = aim_cloud_clamp_int_v2j(nogo_count, 1, count - 1);

    kinds[0] = COLOR_GO_KIND_GO;

    for (int i = 1; i <= nogo_count && i < count; i++) {
        kinds[i] = COLOR_GO_KIND_NOGO;
    }

    for (int i = count - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        color_go_bubble_kind_t tmp = kinds[i];
        kinds[i] = kinds[j];
        kinds[j] = tmp;
    }

    for (int i = 0; i < count; i++) {
        s_color_go_bubbles[i].kind = kinds[i];

        color_go_place_bubble(i, now);

        if (s_color_go_bubbles[i].kind == COLOR_GO_KIND_DISTRACTOR &&
            i % 2 == 1 &&
            s_color_go_bubbles[i].obj != NULL) {
            lv_obj_set_style_bg_color(s_color_go_bubbles[i].obj, lv_color_hex(0x34C759), 0);
        }

        ESP_LOGI(TAG,
                 "Color-Go bubble: idx=%d/%d kind=%d id=%lu x=%d y=%d r=%d",
                 i,
                 count,
                 (int)s_color_go_bubbles[i].kind,
                 (unsigned long)s_color_go_bubbles[i].id,
                 s_color_go_bubbles[i].cx,
                 s_color_go_bubbles[i].cy,
                 s_color_go_bubbles[i].r);
    }
}

static void color_go_update_score_label(uint32_t now)
{
    if (s_color_go_score_label == NULL) {
        return;
    }

    uint32_t elapsed = now - s_color_go_round_start_ms;
    uint32_t remain_ms = elapsed >= s_color_go_round_ms_v2j ? 0 : s_color_go_round_ms_v2j - elapsed;
    uint32_t total_clicks = s_color_go_correct_hits + s_color_go_wrong_hits + s_color_go_false_alarm_count;
    uint32_t accuracy = 0;

    if (total_clicks > 0) {
        accuracy = (s_color_go_correct_hits * 100U) / total_clicks;
    }

    char buf[192];
    snprintf(buf,
             sizeof(buf),
             "时间 %lus  正确 %lu  误触 %lu  No-Go %lu  准确 %lu%%",
             (unsigned long)(remain_ms / 1000U),
             (unsigned long)s_color_go_correct_hits,
             (unsigned long)s_color_go_wrong_hits,
             (unsigned long)s_color_go_false_alarm_count,
             (unsigned long)accuracy);

    aim_label_set_text_cn_v2n2(s_color_go_score_label, buf);
    lv_obj_set_style_text_font(s_color_go_score_label, UI_FONT_CN_SMALL, LV_PART_MAIN);
}

static void color_go_add_soft_background_dots(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    static const struct {
        int x;
        int y;
        int r;
        uint32_t color;
        lv_opa_t opa;
    } dots[] = {
        {64, 86, 5, 0x5ECFFF, 130},
        {158, 504, 4, 0xFFE58A, 105},
        {260, 70, 3, 0xB8F2C8, 115},
        {418, 520, 5, 0xFFB6D5, 95},
        {586, 86, 4, 0xD8C2FF, 115},
        {746, 504, 3, 0x5ECFFF, 120},
        {902, 98, 5, 0xFFE58A, 90},
        {936, 456, 4, 0xB8F2C8, 110},
    };

    for (int i = 0; i < (int)(sizeof(dots) / sizeof(dots[0])); i++) {
        lv_obj_t *dot = lv_obj_create(parent);
        if (dot == NULL) {
            continue;
        }

        lv_obj_set_size(dot, dots[i].r * 2, dots[i].r * 2);
        lv_obj_set_pos(dot, dots[i].x, dots[i].y);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(dots[i].color), 0);
        lv_obj_set_style_bg_opa(dot, dots[i].opa, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void color_go_create_game_page(void)
{
    color_go_delete_pages();

    s_color_go_page = lv_obj_create(lv_scr_act());
    if (s_color_go_page == NULL) {
        return;
    }

    lv_obj_set_size(s_color_go_page, AIM_SCREEN_W, AIM_SCREEN_H);
    lv_obj_align(s_color_go_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_color_go_page, lv_color_hex(0x061522), 0);
    lv_obj_set_style_bg_opa(s_color_go_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_color_go_page, 0, 0);
    lv_obj_set_style_radius(s_color_go_page, 0, 0);
    lv_obj_set_style_pad_all(s_color_go_page, 0, 0);
    lv_obj_clear_flag(s_color_go_page, LV_OBJ_FLAG_SCROLLABLE);

    /* Color-Go v1 clean no-card background. */
    color_go_add_soft_background_dots(s_color_go_page);

    lv_obj_t *title = lv_label_create(s_color_go_page);
    if (title != NULL) {
        lv_obj_set_width(title, 260);
        lv_obj_set_pos(title, 42, 22);
        lv_obj_set_style_text_color(title, lv_color_hex(0xEAF4FF), 0);
        aim_label_set_text_cn_v2n2(title, "颜色反应");
    }

    s_color_go_rule_label = lv_label_create(s_color_go_page);
    if (s_color_go_rule_label != NULL) {
        lv_obj_set_width(s_color_go_rule_label, 900);
        lv_obj_set_pos(s_color_go_rule_label, 62, 548);
        lv_obj_set_style_text_color(s_color_go_rule_label, lv_color_hex(0xBFD8F4), 0);
        lv_obj_set_style_text_align(s_color_go_rule_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_color_go_rule_label, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(s_color_go_rule_label,
                          "收集蓝色，避开红色");
    }

    s_color_go_score_label = lv_label_create(s_color_go_page);
    if (s_color_go_score_label != NULL) {
        lv_obj_set_width(s_color_go_score_label, 730);
        lv_obj_set_pos(s_color_go_score_label, 266, 24);
        lv_obj_set_style_text_color(s_color_go_score_label, lv_color_hex(0xD8F2FF), 0);
        lv_obj_set_style_text_align(s_color_go_score_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(s_color_go_score_label, UI_FONT_CN_SMALL, LV_PART_MAIN);
        lv_label_set_long_mode(s_color_go_score_label, LV_LABEL_LONG_CLIP);
        aim_label_set_text_fmt_cn_v2n2(s_color_go_score_label,
                          "时间 %lus  正确 0  误触 0  No-Go 0  准确 0%%",
                          (unsigned long)(s_color_go_round_ms_v2j / 1000U));
        lv_obj_set_style_text_font(s_color_go_score_label, UI_FONT_CN_SMALL, LV_PART_MAIN);
    }

    for (int i = 0; i < s_color_go_bubble_count_v2j; i++) {
        color_go_bubble_t *b = &s_color_go_bubbles[i];

        b->id = COLOR_GO_BUBBLE_ID_BASE + (uint32_t)i;
        b->r = s_color_go_target_r_v2j;
        b->obj = lv_obj_create(s_color_go_page);
        b->label = NULL;

        if (b->obj == NULL) {
            continue;
        }

        lv_obj_set_size(b->obj, b->r * 2, b->r * 2);
        lv_obj_set_style_radius(b->obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b->obj, lv_color_hex(0x2787FF), 0);
        lv_obj_set_style_bg_opa(b->obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b->obj, lv_color_hex(0x244057), 0);
        lv_obj_set_style_border_width(b->obj, 2, 0);
        lv_obj_clear_flag(b->obj, LV_OBJ_FLAG_SCROLLABLE);

        b->label = lv_label_create(b->obj);
        if (b->label != NULL) {
            lv_obj_set_style_text_color(b->label, lv_color_hex(0xFFFFFF), 0);
            aim_label_set_text_cn_v2n2(b->label, "GO");
            lv_obj_center(b->label);
        }
    }

    lv_obj_move_foreground(s_color_go_page);
    ESP_LOGI(TAG, "Color-Go game page v1: clean no-card UI");
}

static void color_go_start_round(void)
{
    record_delete_page();

    
    aim_cloud_apply_color_runtime_v2j();
uint32_t now = color_go_now_ms();

    s_app_mode = AIM_APP_MODE_COLOR_GO;
    s_game_state = AIM_STATE_RUNNING;
    s_color_go_running = true;
    s_color_go_result_active = false;
    s_color_go_wait_release = false;

    s_color_go_round_start_ms = now;
    s_color_go_correct_hits = 0;
    s_color_go_wrong_hits = 0;
    s_color_go_false_alarm_count = 0;
    s_color_go_miss_count = 0;
    s_color_go_total_reaction_ms = 0;
    s_color_go_fastest_reaction_ms = UINT32_MAX;

    aim_delete_menu_page_if_needed();

    if (s_result_layer != NULL) {
        AIM_OBJ_DELETE(s_result_layer);
        s_result_layer = NULL;
        s_result_analysis_btn = NULL;
        s_result_next_btn = NULL;
        s_result_home_btn = NULL;
    }

    if (s_result_page != NULL) {
        AIM_OBJ_DELETE(s_result_page);
        s_result_page = NULL;
    }

    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        if (s_targets[i].obj != NULL) {
            lv_obj_add_flag(s_targets[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    air_attention_heatmap_clear();
    air_attention_heatmap_set_enabled(true);

    air_input_config_t cfg = {
        .dwell_ms = s_color_go_dwell_ms_v2j,
        .cooldown_ms = 180,
        .stable_radius_px = aim_cloud_clamp_int_v2j(s_color_go_target_r_v2j - 6, 28, 80),
        .target_expand_px = aim_cloud_clamp_int_v2j(s_color_go_target_r_v2j / 3, 8, 32),
    };
    air_input_set_config(&cfg);

    color_go_create_game_page();

    /*
     * Re-enable Color-Go running state after page recreation.
     * color_go_create_game_page() calls color_go_delete_pages(), which clears
     * s_color_go_running to false. Without this, the UI appears but AirTouch
     * bubble clicks are ignored.
     */
    s_app_mode = AIM_APP_MODE_COLOR_GO;
    s_game_state = AIM_STATE_RUNNING;
    s_color_go_running = true;
    s_color_go_result_active = false;
    s_color_go_wait_release = false;

    color_go_spawn_bubbles(now);
    color_go_update_score_label(now);

    if (s_title_label != NULL) {
        aim_label_set_text_cn_v2n2(s_title_label, "颜色反应");
    }

    if (s_info_label != NULL) {
        aim_label_set_text_cn_v2n2(s_info_label, "收集蓝色，避开红色");
    }

    if (s_status_label != NULL) {
        aim_label_set_text_cn_v2n2(s_status_label, "颜色反应进行中，仅使用空中指针");
    }

    airtouch_sfx_start_v1d();
    ESP_LOGI(TAG, "Color-Go round started");
}

static int color_go_find_bubble_by_id(uint32_t id)
{
    for (int i = 0; i < s_color_go_bubble_count_v2j; i++) {
        if (s_color_go_bubbles[i].id == id) {
            return i;
        }
    }

    return -1;
}

static void color_go_set_result_button_style(lv_obj_t *btn, bool active)
{
    if (btn == NULL) {
        return;
    }

    if (active) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A6F8F), 0);
        lv_obj_set_style_bg_opa(btn, 245, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x8AE9FF), 0);
        lv_obj_set_style_border_width(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 14, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x8AE9FF), 0);
        lv_obj_set_style_shadow_opa(btn, 110, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x18435C), 0);
        lv_obj_set_style_bg_opa(btn, 230, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x5B8EAA), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_shadow_width(btn, 6, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x061522), 0);
        lv_obj_set_style_shadow_opa(btn, 70, 0);
    }
}


static lv_obj_t *color_go_create_result_button(lv_obj_t *parent,
                                               int cx,
                                               int cy,
                                               const char *text)
{
    lv_obj_t *btn = lv_obj_create(parent);
    if (btn == NULL) {
        return NULL;
    }

    lv_obj_set_size(btn, 170, 62);
    lv_obj_set_pos(btn, cx - 85, cy - 31);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A2A3E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x49708E), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    aim_label_set_text_cn_v2n2(label, text);
    lv_obj_center(label);

    return btn;
}

static lv_obj_t *color_go_result_create_card(lv_obj_t *parent,
                                                int x,
                                                int y,
                                                int w,
                                                int h,
                                                uint32_t bg_color,
                                                uint32_t border_color)
{
    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return NULL;
    }

    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(card, 238, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(border_color), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    return card;
}

static void color_go_result_add_metric_card(lv_obj_t *parent,
                                            int x,
                                            int y,
                                            int w,
                                            int h,
                                            const char *title,
                                            const char *value,
                                            const char *unit,
                                            uint32_t accent_color)
{
    (void)unit;

    lv_obj_t *card = color_go_result_create_card(parent, x, y, w, h, 0x1B425C, 0x6FA6C2);
    if (card == NULL) {
        return;
    }

    lv_obj_t *dot = lv_obj_create(card);
    if (dot != NULL) {
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_pos(dot, 16, 23);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(accent_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *t = lv_label_create(card);
    if (t != NULL) {
        lv_obj_set_width(t, w - 48);
        lv_obj_set_pos(t, 30, 12);
        lv_obj_set_style_text_color(t, lv_color_hex(0xD8F2FF), 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(t, title != NULL ? title : "");
        lv_obj_set_style_text_font(t, UI_FONT_CN_SMALL, LV_PART_MAIN);
    }

    lv_obj_t *v = lv_label_create(card);
    if (v != NULL) {
        lv_obj_set_width(v, w - 30);
        lv_obj_set_pos(v, 15, 55);
        lv_obj_set_style_text_color(v, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(v, value != NULL ? value : "");
        lv_obj_set_style_text_font(v, UI_FONT_CN_SMALL, LV_PART_MAIN);
    }
}





static void color_go_enter_result_state(void)
{
    uint32_t total_clicks = s_color_go_correct_hits +
                            s_color_go_wrong_hits +
                            s_color_go_false_alarm_count;
    uint32_t accuracy = 0;
    uint32_t avg_ms = 0;
    uint32_t fastest_ms = 0;
    uint32_t inhibition_score = 0;

    if (total_clicks > 0) {
        accuracy = (s_color_go_correct_hits * 100U) / total_clicks;
    }

    if (s_color_go_correct_hits > 0) {
        avg_ms = s_color_go_total_reaction_ms / s_color_go_correct_hits;
    }

    if (s_color_go_fastest_reaction_ms != UINT32_MAX) {
        fastest_ms = s_color_go_fastest_reaction_ms;
    }

    if (total_clicks > 0) {
        uint32_t false_alarm_penalty = s_color_go_false_alarm_count * 15U;
        inhibition_score = accuracy;

        if (false_alarm_penalty >= inhibition_score) {
            inhibition_score = 0;
        } else {
            inhibition_score -= false_alarm_penalty;
        }
    }

    s_color_go_latest_valid = true;
    s_color_go_latest_correct = s_color_go_correct_hits;
    s_color_go_latest_wrong = s_color_go_wrong_hits;
    s_color_go_latest_false_alarm = s_color_go_false_alarm_count;
    s_color_go_latest_miss = s_color_go_miss_count;
    s_color_go_latest_accuracy = accuracy;
    s_color_go_latest_avg_ms = avg_ms;
    s_color_go_latest_fastest_ms = fastest_ms;
    s_color_go_latest_inhibition = inhibition_score;
    color_go_history_push(accuracy, avg_ms, inhibition_score, s_color_go_correct_hits);

    uint32_t color_speed_score = color_go_speed_score_from_avg_ms(avg_ms);
    airtouch_color_record_t color_sd_rec = {
        .record_id = 0,
        .boot_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .correct = s_color_go_correct_hits,
        .wrong = s_color_go_wrong_hits,
        .false_alarm = s_color_go_false_alarm_count,
        .miss = s_color_go_miss_count,
        .accuracy = accuracy,
        .avg_ms = avg_ms,
        .fastest_ms = fastest_ms,
        .inhibition = inhibition_score,
        .speed_score = color_speed_score,
        .difficulty = (uint32_t)s_color_go_difficulty_v2j,
        .dwell_ms = s_color_go_dwell_ms_v2j,
        .bubble_count = (uint32_t)s_color_go_bubble_count_v2j,
        .nogo_ratio = (uint32_t)s_color_go_nogo_ratio_v2j,
        .round_duration_s = s_color_go_round_ms_v2j / 1000U,
        .adaptive_level = (uint32_t)s_color_go_adaptive_v2j,
    };
    airtouch_storage_append_color_record(&color_sd_rec);

    s_color_go_running = false;
    s_color_go_result_active = true;
    s_color_go_wait_release = false;
    s_game_state = AIM_STATE_RESULT;
    airtouch_sfx_result_v1d();

    air_attention_heatmap_set_enabled(false);

    if (s_color_go_page != NULL) {
        AIM_OBJ_DELETE(s_color_go_page);
        s_color_go_page = NULL;
    }

    lv_obj_t *result_root = lv_scr_act();
    if (result_root != NULL) {
        lv_obj_set_style_bg_color(result_root, lv_color_hex(0x061522), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(result_root, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_invalidate(result_root);
    }

    s_color_go_result_page = lv_obj_create(result_root);
    if (s_color_go_result_page == NULL) {
        return;
    }

    lv_obj_remove_style_all(s_color_go_result_page);
    lv_obj_set_size(s_color_go_result_page, AIM_SCREEN_W, AIM_SCREEN_H);
    lv_obj_align(s_color_go_result_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_color_go_result_page, lv_color_hex(0x061522), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s_color_go_result_page, lv_color_hex(0x12364C), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s_color_go_result_page, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_color_go_result_page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_color_go_result_page, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_color_go_result_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_color_go_result_page, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_color_go_result_page, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * v4: solid LVGL underlay.
     * Keep this object as the first child. Later title/cards/buttons are
     * created above it naturally. Do not call lv_obj_move_background().
     */
    lv_obj_t *result_bg = lv_obj_create(s_color_go_result_page);
    if (result_bg != NULL) {
        lv_obj_remove_style_all(result_bg);
        lv_obj_set_size(result_bg, AIM_SCREEN_W, AIM_SCREEN_H);
        lv_obj_set_pos(result_bg, 0, 0);
        lv_obj_set_style_radius(result_bg, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(result_bg, lv_color_hex(0x061522), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(result_bg, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(result_bg, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(result_bg, 0, LV_PART_MAIN);
        lv_obj_clear_flag(result_bg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(result_bg, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *title = lv_label_create(s_color_go_result_page);
    if (title != NULL) {
        lv_obj_set_width(title, 560);
        lv_obj_set_pos(title, 52, 34);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(title, "颜色反应结果");
        lv_obj_set_style_text_font(title, UI_FONT_CN_MEDIUM, LV_PART_MAIN);
    }

    lv_obj_t *subtitle = lv_label_create(s_color_go_result_page);
    if (subtitle != NULL) {
        lv_obj_set_width(subtitle, 720);
        lv_obj_set_pos(subtitle, 52, 70);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0xCFE8FF), 0);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(subtitle, "选择性注意与抑制控制总结");
        lv_obj_set_style_text_font(subtitle, UI_FONT_CN_SMALL, LV_PART_MAIN);
    }

    char correct_buf[32];
    char acc_buf[32];
    char false_buf[32];
    char avg_buf[32];

    snprintf(correct_buf, sizeof(correct_buf), "%lu", (unsigned long)s_color_go_correct_hits);
    snprintf(acc_buf, sizeof(acc_buf), "%lu%%", (unsigned long)accuracy);
    snprintf(false_buf, sizeof(false_buf), "%lu", (unsigned long)s_color_go_false_alarm_count);
    snprintf(avg_buf, sizeof(avg_buf), "%lu ms", (unsigned long)avg_ms);

    color_go_result_add_metric_card(s_color_go_result_page, 50, 116, 206, 128,
                                    "正确", correct_buf, "蓝色 Go 目标", 0x5ECFFF);
    color_go_result_add_metric_card(s_color_go_result_page, 282, 116, 206, 128,
                                    "准确率", acc_buf, "规则", 0xB8F2C8);
    color_go_result_add_metric_card(s_color_go_result_page, 514, 116, 206, 128,
                                    "误触", false_buf, "红色 No-Go 抑制", 0xFF7A7A);
    color_go_result_add_metric_card(s_color_go_result_page, 746, 116, 206, 128,
                                    "平均反应", avg_buf, "反应时间 / ms", 0xFFE58A);

    lv_obj_t *feedback = color_go_result_create_card(s_color_go_result_page,
                                                     88, 284, 848, 112,
                                                     0x173C54, 0x6FA6C2);
    if (feedback != NULL) {
        lv_obj_t *ft = lv_label_create(feedback);
        if (ft != NULL) {
            lv_obj_set_width(ft, 790);
            lv_obj_set_pos(ft, 30, 18);
            lv_obj_set_style_text_color(ft, lv_color_hex(0xFFFFFF), 0);
            lv_label_set_long_mode(ft, LV_LABEL_LONG_DOT);
            aim_label_set_text_cn_v2n2(ft, "训练反馈");
            lv_obj_set_style_text_font(ft, UI_FONT_CN_SMALL, LV_PART_MAIN);
        }

        lv_obj_t *fb = lv_label_create(feedback);
        if (fb != NULL) {
            lv_obj_set_width(fb, 790);
            lv_obj_set_pos(fb, 30, 52);
            lv_obj_set_style_text_color(fb, lv_color_hex(0xD8F2FF), 0);
            lv_obj_set_style_text_font(fb, UI_FONT_CN_SMALL, LV_PART_MAIN);
            lv_label_set_long_mode(fb, LV_LABEL_LONG_WRAP);

            if (s_color_go_false_alarm_count > 0) {
                aim_label_set_text_cn_v2n2(fb,
                                  "看到红色目标时先慢下来，确认规则后再悬停");
            } else if (accuracy >= 85) {
                aim_label_set_text_cn_v2n2(fb,
                                  "选择性注意表现不错，下一轮可以增加干扰或缩短反应时间");
            } else {
                aim_label_set_text_cn_v2n2(fb,
                                  "继续练习颜色选择和稳定悬停");
            }
        }
    }

    s_color_go_next_btn = color_go_create_result_button(s_color_go_result_page,
                                                        384,
                                                        520,
                                                        "继续训练");
    s_color_go_home_btn = color_go_create_result_button(s_color_go_result_page,
                                                        640,
                                                        520,
                                                        "返回主页");

    lv_obj_move_foreground(s_color_go_result_page);

    ESP_LOGI(TAG,
             "Color-Go result v4 solid LVGL background: correct=%lu wrong=%lu false_alarm=%lu accuracy=%lu%% avg=%lu fastest=%lu inhibition=%lu",
             (unsigned long)s_color_go_correct_hits,
             (unsigned long)s_color_go_wrong_hits,
             (unsigned long)s_color_go_false_alarm_count,
             (unsigned long)accuracy,
             (unsigned long)avg_ms,
             (unsigned long)fastest_ms,
             (unsigned long)inhibition_score);
}

static void color_go_process_result_airtouch(uint32_t now)
{
    (void)now;

    air_input_circle_target_t result_targets[2];

    result_targets[0].id = COLOR_GO_NEXT_ID;
    result_targets[0].cx = 384;
    result_targets[0].cy = 520;
    result_targets[0].r = 70;

    result_targets[1].id = COLOR_GO_HOME_ID;
    result_targets[1].cx = 640;
    result_targets[1].cy = 520;
    result_targets[1].r = 70;

    air_input_state_t st;
    memset(&st, 0, sizeof(st));

    air_input_update_circle_targets(result_targets, 2, &st);

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, st.hover_progress, LV_ANIM_OFF);
    }

    color_go_set_result_button_style(s_color_go_next_btn,
                                     st.target_inside && st.target_id == COLOR_GO_NEXT_ID);
    color_go_set_result_button_style(s_color_go_home_btn,
                                     st.target_inside && st.target_id == COLOR_GO_HOME_ID);

    if (!st.target_inside) {
        s_color_go_wait_release = false;
    }

    if (!st.click || s_color_go_wait_release) {
        return;
    }

    s_color_go_wait_release = true;

    if (st.target_id == COLOR_GO_NEXT_ID) {
        airtouch_sfx_click_v1d();
        ESP_LOGI(TAG, "Color-Go result: next round");
        color_go_start_round();
    } else if (st.target_id == COLOR_GO_HOME_ID) {
        airtouch_sfx_click_v1d();
        ESP_LOGI(TAG, "Color-Go result: home menu");
        color_go_delete_pages();
        aim_show_main_menu();
    }
}

static void color_go_process_airtouch(uint32_t now)
{
    if (s_color_go_result_active) {
        color_go_process_result_airtouch(now);
        return;
    }

    if (!s_color_go_running) {
        return;
    }

    if (now - s_color_go_round_start_ms >= s_color_go_round_ms_v2j) {
        color_go_enter_result_state();
        return;
    }

    color_go_update_score_label(now);

    air_input_circle_target_t bubble_targets[COLOR_GO_BUBBLE_MAX];

    for (int i = 0; i < s_color_go_bubble_count_v2j; i++) {
        bubble_targets[i].id = s_color_go_bubbles[i].id;
        bubble_targets[i].cx = s_color_go_bubbles[i].cx;
        bubble_targets[i].cy = s_color_go_bubbles[i].cy;
        bubble_targets[i].r = s_color_go_bubbles[i].r;
    }

    air_input_state_t st;
    memset(&st, 0, sizeof(st));

    air_input_update_circle_targets(bubble_targets, s_color_go_bubble_count_v2j, &st);

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, st.hover_progress, LV_ANIM_OFF);
    }

    for (int i = 0; i < s_color_go_bubble_count_v2j; i++) {
        color_go_update_bubble_style(i,
                                     st.target_inside &&
                                     st.target_id == s_color_go_bubbles[i].id);

        if (s_color_go_bubbles[i].kind == COLOR_GO_KIND_DISTRACTOR &&
            i % 2 == 1 &&
            s_color_go_bubbles[i].obj != NULL) {
            lv_obj_set_style_bg_color(s_color_go_bubbles[i].obj, lv_color_hex(0x34C759), 0);
        }
    }

    if (!st.target_inside) {
        s_color_go_wait_release = false;
    }

    if (!st.click || s_color_go_wait_release) {
        return;
    }

    s_color_go_wait_release = true;

    int idx = color_go_find_bubble_by_id(st.target_id);
    if (idx < 0) {
        return;
    }

    color_go_bubble_t *b = &s_color_go_bubbles[idx];
    uint32_t reaction_ms = now - b->spawn_ms;

    if (b->kind == COLOR_GO_KIND_GO) {
        s_color_go_correct_hits++;
        airtouch_sfx_hit_v1d();
        s_color_go_total_reaction_ms += reaction_ms;

        if (reaction_ms < s_color_go_fastest_reaction_ms) {
            s_color_go_fastest_reaction_ms = reaction_ms;
        }

        ESP_LOGI(TAG,
                 "Color-Go HIT correct idx=%d reaction=%lu correct=%lu",
                 idx,
                 (unsigned long)reaction_ms,
                 (unsigned long)s_color_go_correct_hits);
    } else if (b->kind == COLOR_GO_KIND_NOGO) {
        s_color_go_false_alarm_count++;
        airtouch_sfx_error_v1d();

        ESP_LOGI(TAG,
                 "Color-Go FALSE ALARM idx=%d reaction=%lu false_alarm=%lu",
                 idx,
                 (unsigned long)reaction_ms,
                 (unsigned long)s_color_go_false_alarm_count);
    } else {
        s_color_go_wrong_hits++;
        airtouch_sfx_error_v1d();

        ESP_LOGI(TAG,
                 "Color-Go WRONG distractor idx=%d reaction=%lu wrong=%lu",
                 idx,
                 (unsigned long)reaction_ms,
                 (unsigned long)s_color_go_wrong_hits);
    }

    color_go_spawn_bubbles(now);
    color_go_update_score_label(now);
}



static uint32_t color_go_speed_score_from_avg_ms(uint32_t avg_ms)
{
    /*
     * Convert reaction time to a 0-100 trend score.
     * Lower reaction time is better.
     */
    if (avg_ms == 0) {
        return 0;
    }

    if (avg_ms <= 800U) {
        return 100U;
    }

    if (avg_ms >= 3000U) {
        return 20U;
    }

    return 100U - ((avg_ms - 800U) * 80U) / 2200U;
}

static void color_go_history_push(uint32_t accuracy,
                                  uint32_t avg_ms,
                                  uint32_t inhibition,
                                  uint32_t correct)
{
    uint32_t write_idx = 0;

    if (s_color_go_history_count < COLOR_GO_HISTORY_MAX) {
        write_idx = s_color_go_history_count;
        s_color_go_history_count++;
    } else {
        for (uint32_t i = 1; i < COLOR_GO_HISTORY_MAX; i++) {
            s_color_go_history_accuracy[i - 1] = s_color_go_history_accuracy[i];
            s_color_go_history_avg_ms[i - 1] = s_color_go_history_avg_ms[i];
            s_color_go_history_inhibition[i - 1] = s_color_go_history_inhibition[i];
            s_color_go_history_correct[i - 1] = s_color_go_history_correct[i];
        }

        write_idx = COLOR_GO_HISTORY_MAX - 1U;
    }

    s_color_go_history_accuracy[write_idx] = accuracy;
    s_color_go_history_avg_ms[write_idx] = avg_ms;
    s_color_go_history_inhibition[write_idx] = inhibition;
    s_color_go_history_correct[write_idx] = correct;

    ESP_LOGI(TAG,
             "Color-Go history push: count=%lu latest_acc=%lu avg=%lu inhibition=%lu correct=%lu",
             (unsigned long)s_color_go_history_count,
             (unsigned long)accuracy,
             (unsigned long)avg_ms,
             (unsigned long)inhibition,
             (unsigned long)correct);
}

static void record_prepare_score_points(lv_point_t *points,
                                        const uint32_t *values,
                                        uint32_t count,
                                        int chart_w,
                                        int chart_h)
{
    if (points == NULL || values == NULL || count == 0) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t score = values[i];

        if (score > 100U) {
            score = 100U;
        }

        points[i].x = count <= 1U ? chart_w / 2 : (int)((i * (uint32_t)chart_w) / (count - 1U));
        points[i].y = chart_h - (int)((score * (uint32_t)chart_h) / 100U);
    }
}

static void record_prepare_speed_points(lv_point_t *points,
                                        uint32_t count,
                                        int chart_w,
                                        int chart_h)
{
    if (points == NULL || count == 0) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t score = color_go_speed_score_from_avg_ms(s_color_go_history_avg_ms[i]);

        if (score > 100U) {
            score = 100U;
        }

        points[i].x = count <= 1U ? chart_w / 2 : (int)((i * (uint32_t)chart_w) / (count - 1U));
        points[i].y = chart_h - (int)((score * (uint32_t)chart_h) / 100U);
    }
}

static void record_create_chart_grid(lv_obj_t *parent,
                                     int x,
                                     int y,
                                     int w,
                                     int h)
{
    if (parent == NULL) {
        return;
    }

    for (int i = 0; i <= 4; i++) {
        lv_obj_t *grid = lv_obj_create(parent);
        if (grid == NULL) {
            continue;
        }

        int gy = y + (h * i) / 4;
        lv_obj_set_size(grid, w, 1);
        lv_obj_set_pos(grid, x, gy);
        lv_obj_set_style_radius(grid, 0, 0);
        lv_obj_set_style_bg_color(grid, lv_color_hex(0x33516A), 0);
        lv_obj_set_style_bg_opa(grid, (lv_opa_t)(i == 4 ? 130 : 62), 0);
        lv_obj_set_style_border_width(grid, 0, 0);
        lv_obj_set_style_pad_all(grid, 0, 0);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void record_create_trend_line(lv_obj_t *parent,
                                     lv_point_t *points,
                                     uint32_t count,
                                     uint32_t color_hex,
                                     int x,
                                     int y)
{
    if (parent == NULL || points == NULL || count == 0) {
        return;
    }

    if (count > 1U) {
        lv_obj_t *line = lv_line_create(parent);
        if (line != NULL) {
            lv_line_set_points(line, points, (uint16_t)count);
            lv_obj_set_pos(line, x, y);
            lv_obj_set_style_line_width(line, 2, 0);
            lv_obj_set_style_line_color(line, lv_color_hex(color_hex), 0);
            lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_line_rounded(line, true, 0);
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *dot = lv_obj_create(parent);
        if (dot == NULL) {
            continue;
        }

        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, x + points[i].x - 4, y + points[i].y - 4);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(color_hex), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0xEAF6FF), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
}


// ================= Growth Record dot legend v2n.5.2 =================
static void record_create_dot_legend_item_v2n5_2(lv_obj_t *parent,
                                                 int x,
                                                 int y,
                                                 uint32_t dot_color,
                                                 uint32_t text_color,
                                                 const char *text)
{
    if (parent == NULL || text == NULL) {
        return;
    }

    lv_obj_t *dot = lv_obj_create(parent);
    if (dot != NULL) {
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_pos(dot, x, y + 6);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *label = lv_label_create(parent);
    if (label != NULL) {
        lv_obj_set_width(label, 88);
        lv_obj_set_pos(label, x + 16, y);
        lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(label, text);
        lv_obj_set_style_text_font(label, UI_FONT_CN_SMALL, LV_PART_MAIN);
    }
}


static void record_create_trend_panel(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    lv_obj_t *panel = lv_obj_create(parent);
    if (panel == NULL) {
        return;
    }

    lv_obj_set_size(panel, RECORD_TREND_PANEL_W, RECORD_TREND_PANEL_H);
    lv_obj_set_pos(panel, RECORD_TREND_PANEL_X, RECORD_TREND_PANEL_Y);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1A2A), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x315C9A), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    if (title != NULL) {
        lv_obj_set_width(title, 350);
        lv_obj_set_pos(title, 24, 18);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        aim_label_set_text_cn_v2n2(title, "颜色反应趋势");
    }

    record_create_dot_legend_item_v2n5_2(panel, 24, 44, 0x4DA3FF, 0xB9E2FF, "准确率");
    record_create_dot_legend_item_v2n5_2(panel, 142, 44, 0x7DFF9A, 0xB8F2C8, "抑制");
    record_create_dot_legend_item_v2n5_2(panel, 248, 44, 0xFFC94A, 0xFFE7A0, "速度");

    record_create_chart_grid(panel,
                             RECORD_TREND_CHART_X,
                             RECORD_TREND_CHART_Y,
                             RECORD_TREND_CHART_W,
                             RECORD_TREND_CHART_H);

    if (s_color_go_history_count == 0) {
        lv_obj_t *empty = lv_label_create(panel);
        if (empty != NULL) {
            lv_obj_set_width(empty, 330);
            lv_obj_set_pos(empty, 36, 144);
            lv_obj_set_style_text_color(empty, lv_color_hex(0xB9E2FF), 0);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            aim_label_set_text_cn_v2n2(empty, "趋势数据不足\n完成训练后生成趋势");
        }
        return;
    }

    record_prepare_score_points(s_record_accuracy_points,
                                s_color_go_history_accuracy,
                                s_color_go_history_count,
                                RECORD_TREND_CHART_W,
                                RECORD_TREND_CHART_H);

    record_prepare_score_points(s_record_inhibition_points,
                                s_color_go_history_inhibition,
                                s_color_go_history_count,
                                RECORD_TREND_CHART_W,
                                RECORD_TREND_CHART_H);

    record_prepare_speed_points(s_record_speed_points,
                                s_color_go_history_count,
                                RECORD_TREND_CHART_W,
                                RECORD_TREND_CHART_H);

    record_create_trend_line(panel,
                             s_record_accuracy_points,
                             s_color_go_history_count,
                             0x4DA3FF,
                             RECORD_TREND_CHART_X,
                             RECORD_TREND_CHART_Y);

    record_create_trend_line(panel,
                             s_record_inhibition_points,
                             s_color_go_history_count,
                             0x7DFF9A,
                             RECORD_TREND_CHART_X,
                             RECORD_TREND_CHART_Y);

    record_create_trend_line(panel,
                             s_record_speed_points,
                             s_color_go_history_count,
                             0xFFC94A,
                             RECORD_TREND_CHART_X,
                             RECORD_TREND_CHART_Y);

    /* v2n.5: bottom numeric legend removed.
     * Left card shows latest values; this panel focuses on trend lines only.
     */

}


static uint32_t star_hit_score_from_hits(uint32_t hits)
{
    /*
     * Star Catcher is a timed 45s task.
     * Use 40 hits as an approximate full-score anchor for trend visualization.
     */
    if (hits >= 40U) {
        return 100U;
    }

    return (hits * 100U) / 40U;
}

static void star_history_push(uint32_t hits,
                              uint32_t hit_score,
                              uint32_t avg_ms,
                              uint32_t fastest_ms,
                              uint32_t speed_score)
{
    uint32_t write_idx = 0;

    if (s_star_history_count < STAR_HISTORY_MAX) {
        write_idx = s_star_history_count;
        s_star_history_count++;
    } else {
        for (uint32_t i = 1; i < STAR_HISTORY_MAX; i++) {
            s_star_history_hits[i - 1] = s_star_history_hits[i];
            s_star_history_hit_score[i - 1] = s_star_history_hit_score[i];
            s_star_history_avg_ms[i - 1] = s_star_history_avg_ms[i];
            s_star_history_speed_score[i - 1] = s_star_history_speed_score[i];
        }

        write_idx = STAR_HISTORY_MAX - 1U;
    }

    s_star_history_hits[write_idx] = hits;
    s_star_history_hit_score[write_idx] = hit_score;
    s_star_history_avg_ms[write_idx] = avg_ms;
    s_star_history_speed_score[write_idx] = speed_score;

    s_star_latest_valid = true;
    s_star_latest_hits = hits;
    s_star_latest_hit_score = hit_score;
    s_star_latest_avg_ms = avg_ms;
    s_star_latest_fastest_ms = fastest_ms;
    s_star_latest_speed_score = speed_score;

    ESP_LOGI(TAG,
             "Star history push: count=%lu hits=%lu hit_score=%lu avg=%lu fastest=%lu speed=%lu",
             (unsigned long)s_star_history_count,
             (unsigned long)hits,
             (unsigned long)hit_score,
             (unsigned long)avg_ms,
             (unsigned long)fastest_ms,
             (unsigned long)speed_score);
}

static void record_create_bg_decor(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    /*
     * Four accent colors:
     * Blue  - Star / Accuracy
     * Green - Color-Go / Inhibition
     * Yellow- Speed
     * Pink  - Feedback / soft highlight
     */
    struct {
        int x;
        int y;
        int d;
        uint32_t color;
        lv_opa_t opa;
    } deco[] = {
        {-56, 54, 210, 0x1B6FAE, 28},
        {760, 360, 230, 0x22D0B0, 24},
        {378, 438, 160, 0xFF8BCB, 18},
        {890, 42, 90, 0xFFD166, 20},
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *obj = lv_obj_create(parent);
        if (obj == NULL) {
            continue;
        }

        lv_obj_set_size(obj, deco[i].d, deco[i].d);
        lv_obj_set_pos(obj, deco[i].x, deco[i].y);
        lv_obj_set_style_radius(obj, deco[i].d / 2, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(deco[i].color), 0);
        lv_obj_set_style_bg_opa(obj, deco[i].opa, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *record_create_metric_card(lv_obj_t *parent,
                                           int x,
                                           int y,
                                           int w,
                                           int h,
                                           const char *title,
                                           const char *value,
                                           const char *desc,
                                           uint32_t accent_color)
{
    (void)desc;

    uint32_t marker_color = 0xFF8BCB;
    uint32_t title_color = 0xFFD6EA;

    if (accent_color == 0x12395A) {
        marker_color = 0x4DA3FF;
        title_color = 0xB9E2FF;
    } else if (accent_color == 0x244A34) {
        marker_color = 0x7DFF9A;
        title_color = 0xB8F2C8;
    } else if (accent_color == 0x16243A) {
        marker_color = 0xFF8BCB;
        title_color = 0xFFD6EA;
    }

    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return NULL;
    }

    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)238, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x6BB7E8), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x06111F), 0);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_opa(card, (lv_opa_t)64, 0);
    lv_obj_set_style_shadow_ofs_y(card, 6, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * v4d: no title top line.
     * Four-color visual system:
     * blue / green / yellow / pink.
     */
    lv_obj_t *dot = lv_obj_create(card);
    if (dot != NULL) {
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_pos(dot, 28, 40);
        lv_obj_set_style_radius(dot, 6, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(marker_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(0xF0FAFF), 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_shadow_color(dot, lv_color_hex(marker_color), 0);
        lv_obj_set_style_shadow_width(dot, 8, 0);
        lv_obj_set_style_shadow_opa(dot, (lv_opa_t)100, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *title_label = lv_label_create(card);
    if (title_label != NULL) {
        lv_obj_set_width(title_label, w - 76);
        lv_obj_set_pos(title_label, 48, 36);
        lv_obj_set_style_text_color(title_label, lv_color_hex(title_color), 0);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        aim_label_set_text_cn_v2n2(title_label, title != NULL ? title : "");
    }

    lv_obj_t *value_label = lv_label_create(card);
    if (value_label != NULL) {
        lv_obj_set_width(value_label, w - 76);
        lv_obj_set_pos(value_label, 42, h >= 260 ? 98 : 78);
        lv_obj_set_style_text_color(value_label, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
        aim_label_set_text_cn_v2n2(value_label, value != NULL ? value : "");
    }

    return card;
}

static void record_set_button_style(lv_obj_t *btn, bool active);

static lv_obj_t *record_create_side_button(lv_obj_t *parent,
                                           int cx,
                                           int cy,
                                           const char *text)
{
    lv_obj_t *btn = lv_obj_create(parent);
    if (btn == NULL) {
        return NULL;
    }

    lv_obj_set_size(btn, 76, 54);
    lv_obj_set_pos(btn, cx - 38, cy - 27);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    record_set_button_style(btn, false);

    lv_obj_t *label = lv_label_create(btn);
    if (label != NULL) {
        lv_obj_set_width(label, 70);
        lv_obj_set_pos(label, 3, 17);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        aim_label_set_text_cn_v2n2(label, text != NULL ? text : "");
    }

    return btn;
}

static void record_create_scroll_indicator(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    s_record_up_btn = record_create_side_button(parent,
                                                RECORD_BTN_UP_X,
                                                RECORD_BTN_UP_Y,
                                                "UP");

    s_record_down_btn = record_create_side_button(parent,
                                                  RECORD_BTN_DOWN_X,
                                                  RECORD_BTN_DOWN_Y,
                                                  "DOWN");

    lv_obj_t *rail = lv_obj_create(parent);
    if (rail != NULL) {
        lv_obj_set_size(rail, 8, 142);
        lv_obj_set_pos(rail, RECORD_BTN_UP_X - 4, 194);
        lv_obj_set_style_radius(rail, 6, 0);
        lv_obj_set_style_bg_color(rail, lv_color_hex(0x2C4660), 0);
        lv_obj_set_style_bg_opa(rail, (lv_opa_t)160, 0);
        lv_obj_set_style_border_width(rail, 0, 0);
        lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    }

    int knob_y = 196;
    if (RECORD_PAGE_COUNT > 1) {
        knob_y += (int)((s_record_page_index * 104U) / (RECORD_PAGE_COUNT - 1U));
    }

    lv_obj_t *knob = lv_obj_create(parent);
    if (knob != NULL) {
        lv_obj_set_size(knob, 18, 38);
        lv_obj_set_pos(knob, RECORD_BTN_UP_X - 9, knob_y);
        lv_obj_set_style_radius(knob, 10, 0);
        lv_obj_set_style_bg_color(knob, lv_color_hex(0xC9F0FF), 0);
        lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(knob, 0, 0);
        lv_obj_set_style_shadow_color(knob, lv_color_hex(0x7EE7FF), 0);
        lv_obj_set_style_shadow_width(knob, 8, 0);
        lv_obj_set_style_shadow_opa(knob, (lv_opa_t)80, 0);
        lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    }

    char page_buf[32];
    snprintf(page_buf,
             sizeof(page_buf),
             "%u/%u",
             (unsigned int)(s_record_page_index + 1U),
             (unsigned int)RECORD_PAGE_COUNT);

    lv_obj_t *pill = lv_obj_create(parent);
    if (pill != NULL) {
        lv_obj_set_size(pill, 68, 32);
        lv_obj_set_pos(pill, RECORD_BTN_DOWN_X - 34, RECORD_BTN_DOWN_Y + 48);
        lv_obj_set_style_radius(pill, 16, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(0x102B46), 0);
        lv_obj_set_style_bg_opa(pill, (lv_opa_t)220, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(0x5E9FD0), 0);
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_pad_all(pill, 0, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *page = lv_label_create(pill);
        if (page != NULL) {
            lv_obj_set_width(page, 64);
            lv_obj_set_pos(page, 2, 8);
            lv_obj_set_style_text_color(page, lv_color_hex(0xCFE8FF), 0);
            lv_obj_set_style_text_align(page, LV_TEXT_ALIGN_CENTER, 0);
            aim_label_set_text_cn_v2n2(page, page_buf);
        }
    }
}

static void record_create_star_trend_panel(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    lv_obj_t *panel = lv_obj_create(parent);
    if (panel == NULL) {
        return;
    }

    lv_obj_set_size(panel, RECORD_TREND_PANEL_W, RECORD_TREND_PANEL_H);
    lv_obj_set_pos(panel, RECORD_TREND_PANEL_X, RECORD_TREND_PANEL_Y);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1A2A), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x315C9A), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    if (title != NULL) {
        lv_obj_set_width(title, 350);
        lv_obj_set_pos(title, 24, 18);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        aim_label_set_text_cn_v2n2(title, "星星追踪趋势");
    }

    record_create_dot_legend_item_v2n5_2(panel, 24, 44, 0x4DA3FF, 0xB9E2FF, "命中");
    record_create_dot_legend_item_v2n5_2(panel, 142, 44, 0xFFC94A, 0xFFE7A0, "速度");

    record_create_chart_grid(panel,
                             RECORD_TREND_CHART_X,
                             RECORD_TREND_CHART_Y,
                             RECORD_TREND_CHART_W,
                             RECORD_TREND_CHART_H);

    if (s_star_history_count == 0) {
        lv_obj_t *empty = lv_label_create(panel);
        if (empty != NULL) {
            lv_obj_set_width(empty, 330);
            lv_obj_set_pos(empty, 36, 144);
            lv_obj_set_style_text_color(empty, lv_color_hex(0xB9E2FF), 0);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            aim_label_set_text_cn_v2n2(empty, "趋势数据不足\n完成训练后生成趋势");
        }
        return;
    }

    record_prepare_score_points(s_record_star_hit_points,
                                s_star_history_hit_score,
                                s_star_history_count,
                                RECORD_TREND_CHART_W,
                                RECORD_TREND_CHART_H);

    record_prepare_score_points(s_record_star_speed_points,
                                s_star_history_speed_score,
                                s_star_history_count,
                                RECORD_TREND_CHART_W,
                                RECORD_TREND_CHART_H);

    record_create_trend_line(panel,
                             s_record_star_hit_points,
                             s_star_history_count,
                             0x4DA3FF,
                             RECORD_TREND_CHART_X,
                             RECORD_TREND_CHART_Y);

    record_create_trend_line(panel,
                             s_record_star_speed_points,
                             s_star_history_count,
                             0xFFC94A,
                             RECORD_TREND_CHART_X,
                             RECORD_TREND_CHART_Y);

    /* v2n.5: bottom numeric legend removed.
     * Left card shows latest values; this panel focuses on trend lines only.
     */

}

static void record_delete_page(void)
{
    if (s_record_page != NULL) {
        AIM_OBJ_DELETE(s_record_page);
        s_record_page = NULL;
    }

    s_record_star_btn = NULL;
    s_record_color_btn = NULL;
    s_record_home_btn = NULL;
    s_record_up_btn = NULL;
    s_record_down_btn = NULL;
    s_record_wait_release = false;
}

static void record_set_button_style(lv_obj_t *btn, bool active)
{
    if (btn == NULL) {
        return;
    }

    if (active) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F5F8A), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x7EE7FF), 0);
        lv_obj_set_style_border_width(btn, 3, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x7EE7FF), 0);
        lv_obj_set_style_shadow_width(btn, 16, 0);
        lv_obj_set_style_shadow_opa(btn, (lv_opa_t)120, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x14324A), 0);
        lv_obj_set_style_bg_opa(btn, (lv_opa_t)225, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x5E9FD0), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x06111F), 0);
        lv_obj_set_style_shadow_width(btn, 8, 0);
        lv_obj_set_style_shadow_opa(btn, (lv_opa_t)60, 0);
    }
}

static lv_obj_t *record_create_button(lv_obj_t *parent,
                                      int cx,
                                      int cy,
                                      const char *text)
{
    lv_obj_t *btn = lv_obj_create(parent);
    if (btn == NULL) {
        return NULL;
    }

    lv_obj_set_size(btn, 178, 64);
    lv_obj_set_pos(btn, cx - 89, cy - 32);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    record_set_button_style(btn, false);

    lv_obj_t *label = lv_label_create(btn);
    if (label != NULL) {
        lv_obj_set_width(label, 160);
        lv_obj_set_pos(label, 9, 21);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        aim_label_set_text_cn_v2n2(label, text != NULL ? text : "");
    }

    return btn;
}

static void record_show_page(void)
{
    s_app_mode = AIM_APP_MODE_RECORD;
    s_game_state = AIM_STATE_IDLE;
    s_record_wait_release = false;

    if (s_record_page_index >= RECORD_PAGE_COUNT) {
        s_record_page_index = 0;
    }

    aim_delete_menu_page_if_needed();
    color_go_delete_pages();

    if (s_result_layer != NULL) {
        AIM_OBJ_DELETE(s_result_layer);
        s_result_layer = NULL;
        s_result_analysis_btn = NULL;
        s_result_next_btn = NULL;
        s_result_home_btn = NULL;
    }

    if (s_result_page != NULL) {
        AIM_OBJ_DELETE(s_result_page);
        s_result_page = NULL;
    }

    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        if (s_targets[i].obj != NULL) {
            lv_obj_add_flag(s_targets[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    air_attention_heatmap_set_enabled(false);
    record_delete_page();

    s_record_page = lv_obj_create(lv_scr_act());
    if (s_record_page == NULL) {
        return;
    }

    lv_obj_set_size(s_record_page, AIM_SCREEN_W, AIM_SCREEN_H);
    lv_obj_align(s_record_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_record_page, lv_color_hex(0x061522), 0);
    lv_obj_set_style_bg_opa(s_record_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_record_page, 0, 0);
    lv_obj_set_style_radius(s_record_page, 0, 0);
    lv_obj_set_style_pad_all(s_record_page, 0, 0);
    lv_obj_clear_flag(s_record_page, LV_OBJ_FLAG_SCROLLABLE);

    record_create_bg_decor(s_record_page);

    lv_obj_t *title = lv_label_create(s_record_page);
    if (title != NULL) {
        lv_obj_set_width(title, 830);
        lv_obj_set_pos(title, 58, 26);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        aim_label_set_text_cn_v2n2(title, "成长档案");
    }

    lv_obj_t *subtitle = lv_label_create(s_record_page);
    if (subtitle != NULL) {
        lv_obj_set_width(subtitle, 820);
        lv_obj_set_pos(subtitle, 76, 68);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0xB9E2FF), 0);

        if (s_record_page_index == 0) {
            aim_label_set_text_cn_v2n2(subtitle, "训练概览");
        } else if (s_record_page_index == 1) {
            aim_label_set_text_cn_v2n2(subtitle, "星星追踪：空间定位与反应速度");
        } else if (s_record_page_index == 2) {
            aim_label_set_text_cn_v2n2(subtitle, "颜色反应：注意选择与抑制控制");
        } else {
            aim_label_set_text_cn_v2n2(subtitle, "自适应训练建议");
        }
    }

    if (s_record_page_index == 0) {
        char star_value[128];
        char color_value[128];
        char overview_buf[256];

        if (s_star_latest_valid) {
            snprintf(star_value,
                     sizeof(star_value),
                     "命中数 %lu\n平均反应 %lu ms\n命中评分 %lu\n速度评分 %lu",
                     (unsigned long)s_star_latest_hits,
                     (unsigned long)s_star_latest_avg_ms,
                     (unsigned long)s_star_latest_hit_score,
                     (unsigned long)s_star_latest_speed_score);
        } else {
            snprintf(star_value, sizeof(star_value), "暂无星星数据\n训练一次后生成记录");
        }

        if (s_color_go_latest_valid) {
            snprintf(color_value,
                     sizeof(color_value),
                     "准确率 %lu%%\n抑制控制 %lu\n平均反应 %lu ms\n轮次 %lu/12",
                     (unsigned long)s_color_go_latest_accuracy,
                     (unsigned long)s_color_go_latest_inhibition,
                     (unsigned long)s_color_go_latest_avg_ms,
                     (unsigned long)s_color_go_history_count);
        } else {
            snprintf(color_value, sizeof(color_value), "暂无颜色数据\n训练一次后生成记录");
        }

        record_create_metric_card(s_record_page,
                                  78,
                                  122,
                                  400,
                                  190,
                                  "星星追踪",
                                  star_value,
                                  "Spatial tracking record.",
                                  0x12395A);

        record_create_metric_card(s_record_page,
                                  502,
                                  122,
                                  400,
                                  190,
                                  "颜色反应",
                                  color_value,
                                  "Attention and inhibition record.",
                                  0x244A34);

        snprintf(overview_buf,
                 sizeof(overview_buf),
                 "训练概览\n星星轮次：%lu/12\n颜色轮次：%lu/12\n\n使用 UP / DOWN 切换趋势和反馈页面",
                 (unsigned long)s_star_history_count,
                 (unsigned long)s_color_go_history_count);

        lv_obj_t *overview = lv_label_create(s_record_page);
        if (overview != NULL) {
            lv_obj_set_width(overview, 800);
            lv_obj_set_pos(overview, 92, 336);
            lv_obj_set_style_text_color(overview, lv_color_hex(0xD6EFFF), 0);
            lv_label_set_long_mode(overview, LV_LABEL_LONG_WRAP);
            aim_label_set_text_cn_v2n2(overview, overview_buf);
        }
    } else if (s_record_page_index == 1) {
        char value_buf[160];
        char desc_buf[128];

        if (s_star_latest_valid) {
            snprintf(value_buf,
                     sizeof(value_buf),
                     "命中数 %lu\n平均反应 %lu ms\n命中评分 %lu\n速度评分 %lu",
                     (unsigned long)s_star_latest_hits,
                     (unsigned long)s_star_latest_avg_ms,
                     (unsigned long)s_star_latest_hit_score,
                     (unsigned long)s_star_latest_speed_score);
            snprintf(desc_buf, sizeof(desc_buf), "最近星星表现");
        } else {
            snprintf(value_buf, sizeof(value_buf), "暂无星星数据\n训练一次后生成记录");
            snprintf(desc_buf, sizeof(desc_buf), "Spatial tracking record.");
        }

        record_create_metric_card(s_record_page,
                                  78,
                                  128,
                                  386,
                                  328,
                                  "最近星星表现",
                                  value_buf,
                                  desc_buf,
                                  0x12395A);

        record_create_star_trend_panel(s_record_page);
    } else if (s_record_page_index == 2) {
        char color_buf[192];

        if (s_color_go_latest_valid) {
            snprintf(color_buf,
                     sizeof(color_buf),
                     "准确率 %lu%%\n抑制控制 %lu\n平均反应 %lu ms\n轮次 %lu/12",
                     (unsigned long)s_color_go_latest_accuracy,
                     (unsigned long)s_color_go_latest_inhibition,
                     (unsigned long)s_color_go_latest_avg_ms,
                     (unsigned long)s_color_go_history_count);
        } else {
            snprintf(color_buf,
                     sizeof(color_buf),
                     "暂无颜色数据\n训练一次后生成记录");
        }

        record_create_metric_card(s_record_page,
                                  78,
                                  128,
                                  386,
                                  328,
                                  "最近颜色表现",
                                  color_buf,
                                  "最近颜色表现",
                                  0x244A34);

        record_create_trend_panel(s_record_page);
    } else {
        char feedback_buf[512];

        if (!s_star_latest_valid && !s_color_go_latest_valid) {
            snprintf(feedback_buf,
                     sizeof(feedback_buf),
                     "暂无完整训练记录\n\n训练一次后生成反馈");
        } else {
            snprintf(feedback_buf,
                     sizeof(feedback_buf),
                     "当前记录\n星星轮次：%lu/12\n颜色轮次：%lu/12\n\n下一步\n- 星星速度偏低：可增大目标\n- 误触偏高：可延长悬停时间\n- 表现稳定提升：逐步提高难度",
                     (unsigned long)s_star_history_count,
                     (unsigned long)s_color_go_history_count);
        }

        lv_obj_t *fb = lv_obj_create(s_record_page);
        if (fb != NULL) {
            lv_obj_set_size(fb, 824, 338);
            lv_obj_set_pos(fb, 78, 118);
            lv_obj_set_style_radius(fb, 28, 0);
            lv_obj_set_style_bg_color(fb, lv_color_hex(0x16243A), 0);
            lv_obj_set_style_bg_opa(fb, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(fb, lv_color_hex(0x31506E), 0);
            lv_obj_set_style_border_width(fb, 2, 0);
            lv_obj_clear_flag(fb, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *txt = lv_label_create(fb);
            if (txt != NULL) {
                lv_obj_set_width(txt, 760);
                lv_obj_set_pos(txt, 32, 32);
                lv_obj_set_style_text_color(txt, lv_color_hex(0xF0FAFF), 0);
                lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
                aim_label_set_text_cn_v2n2(txt, feedback_buf);
            }
        }
    }

    record_create_scroll_indicator(s_record_page);

    s_record_star_btn = record_create_button(s_record_page,
                                             RECORD_BTN_STAR_X,
                                             RECORD_BTN_Y,
                                             "星星");
    s_record_color_btn = record_create_button(s_record_page,
                                              RECORD_BTN_COLOR_X,
                                              RECORD_BTN_Y,
                                              "颜色");
    s_record_home_btn = record_create_button(s_record_page,
                                             RECORD_BTN_HOME_X,
                                             RECORD_BTN_Y,
                                             "返回主页");

    if (s_title_label != NULL) {
        aim_label_set_text_cn_v2n2(s_title_label, "成长档案");
    }

    if (s_info_label != NULL) {
        aim_label_set_text_cn_v2n2(s_info_label, "使用 UP / DOWN 切换趋势和反馈页面");
    }

    if (s_status_label != NULL) {
        aim_label_set_text_cn_v2n2(s_status_label, "成长档案：星星 / 颜色 / 主页 / UP / DOWN");
    }

    lv_obj_move_foreground(s_record_page);

    ESP_LOGI(TAG,
             "Growth Record v4d four colors: page=%u star_rounds=%lu color_rounds=%lu",
             (unsigned int)(s_record_page_index + 1U),
             (unsigned long)s_star_history_count,
             (unsigned long)s_color_go_history_count);
}

static void record_process_airtouch(uint32_t now)
{
    (void)now;

    if (s_record_page == NULL) {
        return;
    }

    air_input_circle_target_t targets[5];

    targets[0].id = RECORD_BTN_STAR_ID;
    targets[0].cx = RECORD_BTN_STAR_X;
    targets[0].cy = RECORD_BTN_Y;
    targets[0].r = RECORD_BTN_R;

    targets[1].id = RECORD_BTN_COLOR_ID;
    targets[1].cx = RECORD_BTN_COLOR_X;
    targets[1].cy = RECORD_BTN_Y;
    targets[1].r = RECORD_BTN_R;

    targets[2].id = RECORD_BTN_HOME_ID;
    targets[2].cx = RECORD_BTN_HOME_X;
    targets[2].cy = RECORD_BTN_Y;
    targets[2].r = RECORD_BTN_R;

    targets[3].id = RECORD_BTN_UP_ID;
    targets[3].cx = RECORD_BTN_UP_X;
    targets[3].cy = RECORD_BTN_UP_Y;
    targets[3].r = RECORD_BTN_SCROLL_R;

    targets[4].id = RECORD_BTN_DOWN_ID;
    targets[4].cx = RECORD_BTN_DOWN_X;
    targets[4].cy = RECORD_BTN_DOWN_Y;
    targets[4].r = RECORD_BTN_SCROLL_R;

    air_input_state_t st;
    memset(&st, 0, sizeof(st));

    air_input_update_circle_targets(targets, 5, &st);

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, st.hover_progress, LV_ANIM_OFF);
    }

    record_set_button_style(s_record_star_btn,
                            st.target_inside && st.target_id == RECORD_BTN_STAR_ID);
    record_set_button_style(s_record_color_btn,
                            st.target_inside && st.target_id == RECORD_BTN_COLOR_ID);
    record_set_button_style(s_record_home_btn,
                            st.target_inside && st.target_id == RECORD_BTN_HOME_ID);
    record_set_button_style(s_record_up_btn,
                            st.target_inside && st.target_id == RECORD_BTN_UP_ID);
    record_set_button_style(s_record_down_btn,
                            st.target_inside && st.target_id == RECORD_BTN_DOWN_ID);

    if (!st.target_inside) {
        s_record_wait_release = false;
    }

    if (!st.click || s_record_wait_release) {
        return;
    }

    s_record_wait_release = true;

    if (st.target_id == RECORD_BTN_STAR_ID) {
        airtouch_sfx_click_v1d();
        ESP_LOGI(TAG, "Growth Record: start Star Catcher");
        record_delete_page();
        start_new_round();
    } else if (st.target_id == RECORD_BTN_COLOR_ID) {
        airtouch_sfx_click_v1d();
        ESP_LOGI(TAG, "Growth Record: start Color-Go");
        record_delete_page();
        color_go_start_round();
    } else if (st.target_id == RECORD_BTN_HOME_ID) {
        ESP_LOGI(TAG, "Growth Record: home menu");
        record_delete_page();
        aim_show_main_menu();
    } else if (st.target_id == RECORD_BTN_UP_ID) {
        airtouch_sfx_click_v1d();
        if (s_record_page_index > 0) {
            s_record_page_index--;
        }
        ESP_LOGI(TAG, "Growth Record: page up -> %u", (unsigned int)(s_record_page_index + 1U));
        record_show_page();
        s_record_wait_release = true;
    } else if (st.target_id == RECORD_BTN_DOWN_ID) {
        airtouch_sfx_click_v1d();
        if (s_record_page_index + 1U < RECORD_PAGE_COUNT) {
            s_record_page_index++;
        }
        ESP_LOGI(TAG, "Growth Record: page down -> %u", (unsigned int)(s_record_page_index + 1U));
        record_show_page();
        s_record_wait_release = true;
    }
}


static void aim_result_set_virtual_button_style(lv_obj_t *btn, bool active)
{
    if (btn == NULL) {
        return;
    }

    if (active) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F4E78), 0);
        lv_obj_set_style_bg_opa(btn, 245, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x7EE7FF), 0);
        lv_obj_set_style_border_width(btn, 4, 0);
        lv_obj_set_style_shadow_width(btn, 16, 0);
        lv_obj_set_style_shadow_spread(btn, 1, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x7EE7FF), 0);
        lv_obj_set_style_shadow_opa(btn, 120, 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x182235), 0);
        lv_obj_set_style_bg_opa(btn, 220, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x4B6F8D), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_shadow_spread(btn, 0, 0);
        lv_obj_set_style_shadow_opa(btn, 0, 0);
    }
}


static lv_obj_t *aim_result_create_virtual_button(lv_obj_t *parent,
                                                  const char *text,
                                                  int x_offset)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 260, 68);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x_offset, -16);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_pad_all(btn, 8, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    aim_result_set_virtual_button_style(btn, false);

    lv_obj_t *label = lv_label_create(btn);
    aim_label_set_text_cn_v2n2(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);

    return btn;
}

static void aim_result_create_card_accent(lv_obj_t *card, int width, uint32_t color_hex)
{
    /*
     * v19: subtle card accent.
     * Use a small dot instead of a long strip. It adds hierarchy without
     * cutting through the card or competing with text on the real LCD.
     */
    (void)width;

    if (card == NULL) {
        return;
    }

    lv_obj_t *dot = lv_obj_create(card);
    if (dot == NULL) {
        return;
    }

    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_pos(dot, 9, 23);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color_hex), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
}



// ================= Star Result UI typography v2n.7 =================
// Result page has dense cards. Do not let the generic Chinese wrapper
// auto-promote short labels to title fonts; explicitly control hierarchy.
static void aim_result_set_text_small_v2n7(lv_obj_t *label, const char *txt)
{
    if (label == NULL) {
        return;
    }

    aim_label_set_text_cn_v2n2(label, txt != NULL ? txt : "");
    lv_obj_set_style_text_font(label, UI_FONT_CN_SMALL, LV_PART_MAIN);
}

static void aim_result_set_text_medium_v2n7(lv_obj_t *label, const char *txt)
{
    if (label == NULL) {
        return;
    }

    aim_label_set_text_cn_v2n2(label, txt != NULL ? txt : "");
    lv_obj_set_style_text_font(label, UI_FONT_CN_MEDIUM, LV_PART_MAIN);
}

static void aim_result_set_text_fmt_small_v2n7(lv_obj_t *label, const char *fmt, ...)
{
    if (label == NULL || fmt == NULL) {
        return;
    }

    char buf[512];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    buf[sizeof(buf) - 1] = '\0';

    aim_label_set_text_cn_v2n2(label, buf);
    lv_obj_set_style_text_font(label, UI_FONT_CN_SMALL, LV_PART_MAIN);
}



// ================= Star result explore score v2n.8 =================
// "coverage_score" can be very small after thresholding/rounding,
// while the heatmap is visibly active. For display, use a visual
// exploration score derived from raw heatmap cells.
static int aim_result_compute_explore_pct_v2n8(bool hm_valid,
                                               const air_attention_heatmap_stats_t *stats)
{
    if (!hm_valid || stats == NULL || stats->valid_sample_count == 0) {
        return 0;
    }

    float max_v = 0.0f;

    for (int r = 0; r < AIR_ATTENTION_HEATMAP_ROWS; r++) {
        for (int c = 0; c < AIR_ATTENTION_HEATMAP_COLS; c++) {
            float v = air_attention_heatmap_get_cell(r, c);
            if (v > max_v) {
                max_v = v;
            }
        }
    }

    if (max_v <= 0.0001f) {
        int pct = (int)(stats->coverage_score * 100.0f + 0.5f);
        return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    }

    /*
     * Count cells that are visibly part of the heatmap.
     * 12% of the peak matches the visible red heatmap better than the older
     * strict coverage score, and avoids showing 0 when there is a real cluster.
     */
    const float threshold = max_v * 0.12f;
    int active = 0;
    const int total = AIR_ATTENTION_HEATMAP_ROWS * AIR_ATTENTION_HEATMAP_COLS;

    for (int r = 0; r < AIR_ATTENTION_HEATMAP_ROWS; r++) {
        for (int c = 0; c < AIR_ATTENTION_HEATMAP_COLS; c++) {
            if (air_attention_heatmap_get_cell(r, c) >= threshold) {
                active++;
            }
        }
    }

    int visual_pct = (active * 100 + total / 2) / total;

    /*
     * Keep old coverage as a fallback lower bound when it is larger.
     * This keeps behavior stable while fixing the 0% visual contradiction.
     */
    int old_pct = (int)(stats->coverage_score * 100.0f + 0.5f);

    int pct = visual_pct > old_pct ? visual_pct : old_pct;

    if (active > 0 && pct <= 0) {
        pct = 1;
    }
    if (pct > 100) {
        pct = 100;
    }
    if (pct < 0) {
        pct = 0;
    }

    return pct;
}


static void aim_result_render_view(void)
{
    if (s_result_page == NULL) {
        return;
    }

    /*
     * Result page v12 layout rule:
     *
     * Screen: 1024 x 600
     * Layer : 980 x 560, centered
     *
     * 0   - 64  : title / page indicator
     * 72  - 450 : content only
     * 480 - 560 : AirTouch control buttons only
     *
     * Never place long text or cards in the bottom control zone.
     */
    if (s_result_layer != NULL) {
        AIM_OBJ_DELETE(s_result_layer);
        s_result_layer = NULL;
    }

    s_result_analysis_btn = NULL;
    s_result_next_btn = NULL;
    s_result_home_btn = NULL;

    uint32_t avg_ms = 0;
    uint32_t fastest = 0;

    if (s_hits > 0) {
        avg_ms = s_total_reaction_ms / s_hits;
    }

    if (s_fastest_reaction_ms != UINT32_MAX) {
        fastest = s_fastest_reaction_ms;
    }

    uint32_t hit_score = star_hit_score_from_hits(s_hits);
    uint32_t speed_score = color_go_speed_score_from_avg_ms(avg_ms);

    uint32_t ui_avg_transfer_ms = 0;
    int ui_avg_straight_pct = 0;
    int ui_avg_path_px = 0;
    int ui_avg_dev_px = 0;

    if (s_transition_count > 0) {
        ui_avg_transfer_ms = s_transition_total_completion_ms / s_transition_count;
        ui_avg_straight_pct =
            (int)((s_transition_total_straightness * 100.0f /
                   (float)s_transition_count) + 0.5f);
        ui_avg_path_px =
            (int)((s_transition_total_path_len_px /
                   (float)s_transition_count) + 0.5f);
        ui_avg_dev_px =
            (int)((s_transition_total_max_dev_px /
                   (float)s_transition_count) + 0.5f);
    }

    air_attention_heatmap_stats_t hm_stats;
    memset(&hm_stats, 0, sizeof(hm_stats));
    bool hm_valid = air_attention_heatmap_get_stats(&hm_stats);

    int focus_pct = hm_valid ? (int)(hm_stats.focus_score * 100.0f + 0.5f) : 0;
    int stability_pct = hm_valid ? (int)(hm_stats.recent_stability * 100.0f + 0.5f) : 0;
    int coverage_pct = aim_result_compute_explore_pct_v2n8(hm_valid, &hm_stats);

    s_result_layer = lv_obj_create(s_result_page);
    if (s_result_layer == NULL) {
        return;
    }

    lv_obj_set_size(s_result_layer, 980, 560);
    lv_obj_align(s_result_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_result_layer, lv_color_hex(0x050B14), 0);
    lv_obj_set_style_bg_opa(s_result_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_result_layer, 0, 0);
    lv_obj_set_style_radius(s_result_layer, 0, 0);
    lv_obj_set_style_pad_all(s_result_layer, 0, 0);
    lv_obj_clear_flag(s_result_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_result_layer);
    if (title != NULL) {
        lv_obj_set_width(title, 720);
        lv_obj_set_pos(title, 34, 16);
        lv_obj_set_style_text_color(title, lv_color_hex(0xEAF4FF), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    }

    lv_obj_t *page = lv_label_create(s_result_layer);
    if (page != NULL) {
        lv_obj_set_width(page, 260);
        lv_obj_set_pos(page, 684, 18);
        lv_obj_set_style_text_color(page, lv_color_hex(0x8EC8FF), 0);
        lv_obj_set_style_text_align(page, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(page, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(page, UI_FONT_CN_SMALL, LV_PART_MAIN);
    }

    lv_obj_t *sep = lv_obj_create(s_result_layer);
    if (sep != NULL) {
        lv_obj_set_size(sep, 912, 2);
        lv_obj_set_pos(sep, 34, 58);
        lv_obj_set_style_bg_color(sep, lv_color_hex(0x243A56), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    }

    /*
     * Result page v13 visual polish:
     * sparse colorful dots behind cards, consistent with Star Catcher night-sky UI.
     * Created before content cards so they stay in the background.
     */
    aim_star_create_deco_circle(s_result_layer, 72, 82, 4, 0xFFE58A, (lv_opa_t)190);
    aim_star_create_deco_circle(s_result_layer, 170, 438, 4, 0x9FD6FF, (lv_opa_t)170);
    aim_star_create_deco_circle(s_result_layer, 322, 72, 3, 0xFFB6D5, (lv_opa_t)165);
    aim_star_create_deco_circle(s_result_layer, 514, 444, 4, 0xB8F2C8, (lv_opa_t)160);
    aim_star_create_deco_circle(s_result_layer, 760, 76, 3, 0xD8C2FF, (lv_opa_t)170);
    aim_star_create_deco_circle(s_result_layer, 918, 420, 4, 0xFFE58A, (lv_opa_t)170);

    if (s_result_view == AIM_RESULT_VIEW_SUMMARY) {
        if (title != NULL) {
            aim_result_set_text_medium_v2n7(title, "星星追踪结果");
        }
        if (page != NULL) {
            aim_result_set_text_small_v2n7(page, "1/4 总结");
        }

        const char *card_titles[4] = {
            "星星",
            "平均反应",
            "最快反应",
            "下一轮负荷",
        };

        char card_values[4][64];
        snprintf(card_values[0], sizeof(card_values[0]), "%lu",
                 (unsigned long)s_hits);
        snprintf(card_values[1], sizeof(card_values[1]), "%lu ms",
                 (unsigned long)avg_ms);
        snprintf(card_values[2], sizeof(card_values[2]), "%lu ms",
                 (unsigned long)fastest);
        snprintf(card_values[3], sizeof(card_values[3]), "%s",
                 aim_adaptive_level_name());

        const char *card_desc[4] = {
            "",
            "",
            "",
            "",
        };

        int card_x[4] = {44, 275, 506, 737};
        uint32_t card_accent[4] = {0xFFE58A, 0x9FD6FF, 0xFFB6D5, 0xB8F2C8};

        for (int i = 0; i < 4; i++) {
            lv_obj_t *card = lv_obj_create(s_result_layer);
            if (card == NULL) {
                continue;
            }

            lv_obj_set_size(card, 199, 146);
            lv_obj_set_pos(card, card_x[i], 82);
            lv_obj_set_style_radius(card, 22, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x142033), 0);
            lv_obj_set_style_bg_opa(card, 235, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(0x315C9A), 0);
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            aim_result_create_card_accent(card, 199, card_accent[i]);

            lv_obj_t *ct = lv_label_create(card);
            if (ct != NULL) {
                lv_obj_set_width(ct, 160);
                lv_obj_set_pos(ct, 18, 14);
                lv_obj_set_style_text_color(ct, lv_color_hex(0x8EC8FF), 0);
                aim_result_set_text_small_v2n7(ct, card_titles[i]);
            }

            lv_obj_t *cv = lv_label_create(card);
            if (cv != NULL) {
                lv_obj_set_width(cv, 168);
                lv_obj_set_pos(cv, 18, 46);
                lv_obj_set_style_text_color(cv, lv_color_hex(0xFFFFFF), 0);
                aim_result_set_text_small_v2n7(cv, card_values[i]);
            }

            lv_obj_t *cd = lv_label_create(card);
            if (cd != NULL) {
                lv_obj_set_width(cd, 168);
                lv_obj_set_pos(cd, 18, 96);
                lv_obj_set_style_text_color(cd, lv_color_hex(0xBFD8F4), 0);
                aim_result_set_text_small_v2n7(cd, "");
                lv_obj_add_flag(cd, LV_OBJ_FLAG_HIDDEN);
            }
        }

        const char *mid_titles[3] = {
            "反应评分",
            "注意状态",
            "自适应决策",
        };

        char mid_values[3][128];
        snprintf(mid_values[0],
                 sizeof(mid_values[0]),
                 "命中评分 %lu / 100\n速度评分 %lu / 100",
                 (unsigned long)hit_score,
                 (unsigned long)speed_score);
        snprintf(mid_values[1],
                 sizeof(mid_values[1]),
                 "专注 %d%%\n稳定 %d%%\n探索 %d%%",
                 focus_pct,
                 stability_pct,
                 coverage_pct);
        snprintf(mid_values[2],
                 sizeof(mid_values[2]),
                 "%s",
                 s_adaptive_advice);

        int mid_x[3] = {44, 353, 662};
        uint32_t mid_accent[3] = {0xFFE58A, 0x9FD6FF, 0xB8F2C8};

        for (int i = 0; i < 3; i++) {
            lv_obj_t *card = lv_obj_create(s_result_layer);
            if (card == NULL) {
                continue;
            }

            lv_obj_set_size(card, 274, 170);
            lv_obj_set_pos(card, mid_x[i], 250);
            lv_obj_set_style_radius(card, 22, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x101A2A), 0);
            lv_obj_set_style_bg_opa(card, 235, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(0x294A70), 0);
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            aim_result_create_card_accent(card, 274, mid_accent[i]);

            lv_obj_t *ct = lv_label_create(card);
            if (ct != NULL) {
                lv_obj_set_width(ct, 230);
                lv_obj_set_pos(ct, 18, 16);
                lv_obj_set_style_text_color(ct, lv_color_hex(0xFFE58A), 0);
                aim_result_set_text_small_v2n7(ct, mid_titles[i]);
            }

            lv_obj_t *cv = lv_label_create(card);
            if (cv != NULL) {
                lv_obj_set_width(cv, 236);
                lv_obj_set_pos(cv, 18, 54);
                lv_obj_set_style_text_color(cv, lv_color_hex(0xDCEBFF), 0);
                lv_label_set_long_mode(cv, LV_LABEL_LONG_WRAP);
                aim_result_set_text_small_v2n7(cv, mid_values[i]);
            }
        }

        lv_obj_t *hint = lv_label_create(s_result_layer);
        if (hint != NULL) {
            lv_obj_set_width(hint, 880);
            lv_obj_set_pos(hint, 50, 438);
            lv_obj_set_style_text_color(hint, lv_color_hex(0xBFD8F4), 0);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
            aim_result_set_text_small_v2n7(hint, "悬停分析，切换页面");
        }
    } else if (s_result_view == AIM_RESULT_VIEW_TRANSFER) {
        if (title != NULL) {
            aim_result_set_text_medium_v2n7(title, "空间转移分析");
        }
        if (page != NULL) {
            aim_result_set_text_small_v2n7(page, "2/4 转移");
        }

        char weak_from[3] = "--";
        char weak_to[3] = "--";

        if (strlen(s_transition_weak_relation) >= 6 &&
            s_transition_weak_relation[2] == '-' &&
            s_transition_weak_relation[3] == '>') {
            weak_from[0] = s_transition_weak_relation[0];
            weak_from[1] = s_transition_weak_relation[1];
            weak_from[2] = '\0';
            weak_to[0] = s_transition_weak_relation[4];
            weak_to[1] = s_transition_weak_relation[5];
            weak_to[2] = '\0';
        }

        const char *region_names[9] = {
            "LT", "CT", "RT",
            "LC", "CC", "RC",
            "LB", "CB", "RB",
        };

        const int grid_x = 64;
        const int grid_y = 96;
        const int cell_w = 96;
        const int cell_h = 82;
        const int gap = 14;

        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                int idx = r * 3 + c;
                bool is_from = (strcmp(region_names[idx], weak_from) == 0);
                bool is_to = (strcmp(region_names[idx], weak_to) == 0);

                lv_obj_t *cell = lv_obj_create(s_result_layer);
                if (cell == NULL) {
                    continue;
                }

                lv_obj_set_size(cell, cell_w, cell_h);
                lv_obj_set_pos(cell,
                               grid_x + c * (cell_w + gap),
                               grid_y + r * (cell_h + gap));
                lv_obj_set_style_radius(cell, 18, 0);
                lv_obj_set_style_bg_opa(cell, 235, 0);
                lv_obj_set_style_border_width(cell, 2, 0);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

                if (is_from) {
                    lv_obj_set_style_bg_color(cell, lv_color_hex(0x245C8F), 0);
                    lv_obj_set_style_border_color(cell, lv_color_hex(0x8EC8FF), 0);
                } else if (is_to) {
                    lv_obj_set_style_bg_color(cell, lv_color_hex(0x876018), 0);
                    lv_obj_set_style_border_color(cell, lv_color_hex(0xFFD56A), 0);
                } else {
                    lv_obj_set_style_bg_color(cell, lv_color_hex(0x121C2C), 0);
                    lv_obj_set_style_border_color(cell, lv_color_hex(0x2E4B70), 0);
                }

                lv_obj_t *lab = lv_label_create(cell);
                if (lab != NULL) {
                    lv_obj_set_width(lab, cell_w);
                    lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), 0);
                    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
                    aim_result_set_text_small_v2n7(lab, region_names[idx]);
                    lv_obj_center(lab);
                }
            }
        }

        lv_obj_t *metric = lv_obj_create(s_result_layer);
        if (metric != NULL) {
            lv_obj_set_size(metric, 500, 294);
            lv_obj_set_pos(metric, 424, 96);
            lv_obj_set_style_radius(metric, 24, 0);
            lv_obj_set_style_bg_color(metric, lv_color_hex(0x142033), 0);
            lv_obj_set_style_bg_opa(metric, 235, 0);
            lv_obj_set_style_border_color(metric, lv_color_hex(0x315C9A), 0);
            lv_obj_set_style_border_width(metric, 2, 0);
            lv_obj_clear_flag(metric, LV_OBJ_FLAG_SCROLLABLE);
            aim_result_create_card_accent(metric, 500, 0x9FD6FF);

            lv_obj_t *m = lv_label_create(metric);
            if (m != NULL) {
                lv_obj_set_width(m, 450);
                lv_obj_set_pos(m, 24, 24);
                lv_obj_set_style_text_color(m, lv_color_hex(0xFFFFFF), 0);
                lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);

                if (s_transition_count > 0) {
                    aim_result_set_text_fmt_small_v2n7(m,
                                          "最弱转移\n"
                                          "%s\n\n"
                                          "均时 %lu ms\n"
                                          "直线 %d%%\n"
                                          "路径 %d px\n"
                                          "偏移 %d px\n"
                                          "最慢 %lu ms",
                                          s_transition_weak_relation,
                                          (unsigned long)ui_avg_transfer_ms,
                                          ui_avg_straight_pct,
                                          ui_avg_path_px,
                                          ui_avg_dev_px,
                                          (unsigned long)s_transition_slowest_ms);
                } else {
                    aim_result_set_text_small_v2n7(m,
                                      "最弱转移\n"
                                      "数据不足\n\n"
                                      "至少完成两次命中。");
                }
            }
        }

        lv_obj_t *hint = lv_label_create(s_result_layer);
        if (hint != NULL) {
            lv_obj_set_width(hint, 860);
            lv_obj_set_pos(hint, 62, 420);
            lv_obj_set_style_text_color(hint, lv_color_hex(0xBFD8F4), 0);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
            aim_result_set_text_small_v2n7(hint, "蓝色表示起点区域，黄色表示目标区域");
        }
    } else if (s_result_view == AIM_RESULT_VIEW_HEATMAP) {
        if (title != NULL) {
            aim_result_set_text_medium_v2n7(title, "注意力热力图");
        }
        if (page != NULL) {
            aim_result_set_text_small_v2n7(page, "3/4 热力图");
        }

        /*
         * Fix only:
         * 1) no overlap
         * 2) use one-color heatmap strength rendering
         * 3) keep bottom AirTouch buttons clear
         */
        float hm_max = 0.0f;

        for (int r = 0; r < AIR_ATTENTION_HEATMAP_ROWS; r++) {
            for (int c = 0; c < AIR_ATTENTION_HEATMAP_COLS; c++) {
                float v = air_attention_heatmap_get_cell(r, c);
                if (v > hm_max) {
                    hm_max = v;
                }
            }
        }

        lv_obj_t *grid_panel = lv_obj_create(s_result_layer);
        if (grid_panel != NULL) {
            lv_obj_set_size(grid_panel, 560, 304);
            lv_obj_set_pos(grid_panel, 42, 86);
            lv_obj_set_style_radius(grid_panel, 24, 0);
            lv_obj_set_style_bg_color(grid_panel, lv_color_hex(0x101824), 0);
            lv_obj_set_style_bg_opa(grid_panel, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(grid_panel, lv_color_hex(0x315C9A), 0);
            lv_obj_set_style_border_width(grid_panel, 2, 0);
            lv_obj_clear_flag(grid_panel, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *gt = lv_label_create(grid_panel);
            if (gt != NULL) {
                lv_obj_set_width(gt, 500);
                lv_obj_set_pos(gt, 24, 16);
                lv_obj_set_style_text_color(gt, lv_color_hex(0xFFFFFF), 0);
                aim_result_set_text_small_v2n7(gt, "16 x 10 注意力热力图");
            }

            /*
             * Compact grid that fully fits in the left panel:
             * width  = 16*27 + 15*5 = 507
             * height = 10*18 + 9*5  = 225
             */
            const int cell_x = 22;
            const int cell_y = 50;
            const int cell_w = 26;
            const int cell_h = 17;
            const int gap = 5;

            for (int r = 0; r < AIR_ATTENTION_HEATMAP_ROWS; r++) {
                for (int c = 0; c < AIR_ATTENTION_HEATMAP_COLS; c++) {
                    float v = air_attention_heatmap_get_cell(r, c);
                    float norm = 0.0f;

                    if (hm_max > 0.0001f) {
                        norm = v / hm_max;
                        if (norm < 0.0f) {
                            norm = 0.0f;
                        }
                        if (norm > 1.0f) {
                            norm = 1.0f;
                        }
                    }

                    /* single-color red gradient: same red hue, different opacity */
                    int opa_i = 45 + (int)(norm * 210.0f);
                    if (opa_i > 255) {
                        opa_i = 255;
                    }
                    if (opa_i < 0) {
                        opa_i = 0;
                    }
                    lv_opa_t opa = (lv_opa_t)opa_i;

                    lv_obj_t *cell = lv_obj_create(grid_panel);
                    if (cell == NULL) {
                        continue;
                    }

                    lv_obj_set_size(cell, cell_w, cell_h);
                    lv_obj_set_pos(cell,
                                   cell_x + c * (cell_w + gap),
                                   cell_y + r * (cell_h + gap));
                    lv_obj_set_style_radius(cell, 5, 0);
                    lv_obj_set_style_bg_color(cell, lv_color_hex(0xFF5A5A), 0);
                    lv_obj_set_style_bg_opa(cell, opa, 0);
                    lv_obj_set_style_border_width(cell, 0, 0);
                    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
                }
            }
}

        const char *metric_titles[4] = {
            "专注",
            "稳定",
            "探索",
            "样本",
        };

        char metric_values[4][64];
        snprintf(metric_values[0], sizeof(metric_values[0]), "%d%%", focus_pct);
        snprintf(metric_values[1], sizeof(metric_values[1]), "%d%%", stability_pct);
        snprintf(metric_values[2], sizeof(metric_values[2]), "%d%%", coverage_pct);
        snprintf(metric_values[3], sizeof(metric_values[3]), "%lu",
                 hm_valid ? (unsigned long)hm_stats.sample_count : 0UL);

        int mx[2] = {634, 792};
        int my[2] = {96, 246};
        uint32_t metric_accent[4] = {0xFF5A5A, 0xFFE58A, 0x9FD6FF, 0xB8F2C8};

        for (int i = 0; i < 4; i++) {
            lv_obj_t *card = lv_obj_create(s_result_layer);
            if (card == NULL) {
                continue;
            }

            lv_obj_set_size(card, 146, 118);
            lv_obj_set_pos(card, mx[i % 2], my[i / 2]);
            lv_obj_set_style_radius(card, 20, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x142033), 0);
            lv_obj_set_style_bg_opa(card, 235, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(0x315C9A), 0);
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            aim_result_create_card_accent(card, 146, metric_accent[i]);

            lv_obj_t *ct = lv_label_create(card);
            if (ct != NULL) {
                lv_obj_set_width(ct, 110);
                lv_obj_set_pos(ct, 16, 12);
                lv_obj_set_style_text_color(ct, lv_color_hex(0x8EC8FF), 0);
                aim_result_set_text_small_v2n7(ct, metric_titles[i]);
            }

            lv_obj_t *cv = lv_label_create(card);
            if (cv != NULL) {
                lv_obj_set_width(cv, 110);
                lv_obj_set_pos(cv, 16, 44);
                lv_obj_set_style_text_color(cv, lv_color_hex(0xFFFFFF), 0);
                aim_result_set_text_small_v2n7(cv, metric_values[i]);
            }
        }

        /*
         * Heatmap strength scale: one red hue, brightness means strength.
         */
        lv_obj_t *scale = lv_obj_create(s_result_layer);
        if (scale != NULL) {
            lv_obj_set_size(scale, 304, 30);
            lv_obj_set_pos(scale, 634, 386);
            lv_obj_set_style_bg_opa(scale, 0, 0);
            lv_obj_set_style_border_width(scale, 0, 0);
            lv_obj_set_style_pad_all(scale, 0, 0);
            lv_obj_clear_flag(scale, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *low = lv_label_create(scale);
            if (low != NULL) {
                lv_obj_set_width(low, 42);
                lv_obj_set_pos(low, 0, 5);
                lv_obj_set_style_text_color(low, lv_color_hex(0xBFD8F4), 0);
                aim_result_set_text_small_v2n7(low, "低");
            }

            for (int i = 0; i < 5; i++) {
                lv_obj_t *box = lv_obj_create(scale);
                if (box == NULL) {
                    continue;
                }

                lv_obj_set_size(box, 26, 12);
                lv_obj_set_pos(box, 48 + i * 34, 8);
                lv_obj_set_style_radius(box, 4, 0);
                lv_obj_set_style_bg_color(box, lv_color_hex(0xFF5A5A), 0);
                lv_obj_set_style_bg_opa(box, (lv_opa_t)(55 + i * 45), 0);
                lv_obj_set_style_border_width(box, 0, 0);
                lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            }

            lv_obj_t *high = lv_label_create(scale);
            if (high != NULL) {
                lv_obj_set_width(high, 54);
                lv_obj_set_pos(high, 226, 5);
                lv_obj_set_style_text_color(high, lv_color_hex(0xBFD8F4), 0);
                aim_result_set_text_small_v2n7(high, "高");
            }
        }

        lv_obj_t *hint = lv_label_create(s_result_layer);
        if (hint != NULL) {
            lv_obj_set_width(hint, 860);
            lv_obj_set_pos(hint, 62, 424);
            lv_obj_set_style_text_color(hint, lv_color_hex(0xBFD8F4), 0);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
            aim_result_set_text_small_v2n7(hint, "红色亮度表示注意力强度，探索表示活动范围");
        }

    } else {
        if (title != NULL) {
            aim_result_set_text_medium_v2n7(title, "自适应训练反馈");
        }
        if (page != NULL) {
            aim_result_set_text_small_v2n7(page, "4/4 自适应");
        }

        const char *titles[3] = {
            "训练概览",
            "决策",
            "下一轮",
        };

        char values[3][256];
        snprintf(values[0],
                 sizeof(values[0]),
                 "星星 %lu\n平均 %lu ms\n最快 %lu ms\n专注 %d%%  稳定 %d%%",
                 (unsigned long)s_hits,
                 (unsigned long)avg_ms,
                 (unsigned long)fastest,
                 focus_pct,
                 stability_pct);

        snprintf(values[1],
                 sizeof(values[1]),
                 "%s\n负荷 %d/100\n%s",
                 aim_adaptive_level_name(),
                 s_adaptive_load_score,
                 s_adaptive_advice);

        snprintf(values[2],
                 sizeof(values[2]),
                 "半径 %d px\n悬停 %lu ms\n稳定 %d px\n间距 %d px",
                 s_adaptive_target_r,
                 (unsigned long)s_adaptive_dwell_ms,
                 s_adaptive_stable_radius_px,
                 s_adaptive_min_dist);

        int x[3] = {48, 352, 656};
        uint32_t adaptive_accent[3] = {0xFFE58A, 0x9FD6FF, 0xB8F2C8};

        for (int i = 0; i < 3; i++) {
            lv_obj_t *card = lv_obj_create(s_result_layer);
            if (card == NULL) {
                continue;
            }

            lv_obj_set_size(card, 276, 220);
            lv_obj_set_pos(card, x[i], 104);
            lv_obj_set_style_radius(card, 24, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x142033), 0);
            lv_obj_set_style_bg_opa(card, 235, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(0x315C9A), 0);
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            aim_result_create_card_accent(card, 276, adaptive_accent[i]);

            lv_obj_t *ct = lv_label_create(card);
            if (ct != NULL) {
                lv_obj_set_width(ct, 232);
                lv_obj_set_pos(ct, 22, 18);
                lv_obj_set_style_text_color(ct, lv_color_hex(0xFFE58A), 0);
                aim_result_set_text_small_v2n7(ct, titles[i]);
            }

            lv_obj_t *cv = lv_label_create(card);
            if (cv != NULL) {
                lv_obj_set_width(cv, 232);
                lv_obj_set_pos(cv, 22, 58);
                lv_obj_set_style_text_color(cv, lv_color_hex(0xDCEBFF), 0);
                lv_label_set_long_mode(cv, LV_LABEL_LONG_WRAP);
                aim_result_set_text_small_v2n7(cv, values[i]);
            }
        }

        lv_obj_t *profile = lv_obj_create(s_result_layer);
        if (profile != NULL) {
            lv_obj_set_size(profile, 884, 88);
            lv_obj_set_pos(profile, 48, 354);
            lv_obj_set_style_radius(profile, 22, 0);
            lv_obj_set_style_bg_color(profile, lv_color_hex(0x101A2A), 0);
            lv_obj_set_style_bg_opa(profile, 235, 0);
            lv_obj_set_style_border_color(profile, lv_color_hex(0x294A70), 0);
            lv_obj_set_style_border_width(profile, 2, 0);
            lv_obj_clear_flag(profile, LV_OBJ_FLAG_SCROLLABLE);
            aim_result_create_card_accent(profile, 884, 0xD8C2FF);

            lv_obj_t *pt = lv_label_create(profile);
            if (pt != NULL) {
                lv_obj_set_width(pt, 830);
                lv_obj_set_pos(pt, 24, 18);
                lv_obj_set_style_text_color(pt, lv_color_hex(0xDCEBFF), 0);
                lv_label_set_long_mode(pt, LV_LABEL_LONG_WRAP);
                aim_result_set_text_fmt_small_v2n7(pt,
                                      "下一轮：%s",
                                      s_adaptive_advice);
            }
        }
    }

    s_result_analysis_btn = aim_result_create_virtual_button(s_result_layer,
                                                             "分析",
                                                             -266);
    s_result_next_btn = aim_result_create_virtual_button(s_result_layer,
                                                         "继续训练",
                                                         0);
    s_result_home_btn = aim_result_create_virtual_button(s_result_layer,
                                                         "返回主页",
                                                         266);

    lv_obj_move_foreground(s_result_layer);

    ESP_LOGI(TAG,
             "Result page rendered v19: view=%d hits=%lu avg=%lu fast=%lu focus=%d stability=%d coverage=%d",
             (int)s_result_view,
             (unsigned long)s_hits,
             (unsigned long)avg_ms,
             (unsigned long)fastest,
             focus_pct,
             stability_pct,
             coverage_pct);
}


static void aim_result_process_airtouch(uint32_t now)
{
    /*
     * Result-page AirTouch control.
     *
     * In result state, we no longer return immediately after updating the
     * header. We create two virtual circular targets and let AirInput handle
     * dwell confirmation exactly like it handles game targets.
     *
     *   ANALYSIS   : switch result layer
     *   NEXT ROUND : start the next training round
     */
    if (now - s_result_start_ms < 500) {
        return;
    }

    air_input_circle_target_t result_targets[3];

    result_targets[0].id = AIM_RESULT_CTRL_ANALYSIS_ID;
    result_targets[0].cx = AIM_RESULT_CTRL_ANALYSIS_X;
    result_targets[0].cy = AIM_RESULT_CTRL_Y;
    result_targets[0].r = AIM_RESULT_CTRL_R;

    result_targets[1].id = AIM_RESULT_CTRL_NEXT_ROUND_ID;
    result_targets[1].cx = AIM_RESULT_CTRL_NEXT_ROUND_X;
    result_targets[1].cy = AIM_RESULT_CTRL_Y;
    result_targets[1].r = AIM_RESULT_CTRL_R;

    result_targets[2].id = AIM_RESULT_CTRL_HOME_ID;
    result_targets[2].cx = AIM_RESULT_CTRL_HOME_X;
    result_targets[2].cy = AIM_RESULT_CTRL_Y;
    result_targets[2].r = AIM_RESULT_CTRL_R;

    air_input_state_t st;
    memset(&st, 0, sizeof(st));

    air_input_update_circle_targets(result_targets, 3, &st);

    s_hover_target_id = st.target_inside ? st.target_id : 0;
    s_hover_progress = st.hover_progress;

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, st.hover_progress, LV_ANIM_OFF);
    }

    aim_result_set_virtual_button_style(
        s_result_analysis_btn,
        st.target_inside && st.target_id == AIM_RESULT_CTRL_ANALYSIS_ID);

    aim_result_set_virtual_button_style(
        s_result_next_btn,
        st.target_inside && st.target_id == AIM_RESULT_CTRL_NEXT_ROUND_ID);

    aim_result_set_virtual_button_style(
        s_result_home_btn,
        st.target_inside && st.target_id == AIM_RESULT_CTRL_HOME_ID);

    /*
     * Anti-repeat behavior:
     * After one result-page virtual target is selected, require the pointer
     * to leave all result controls before another click is accepted.
     * This prevents ANALYSIS from toggling 1->0->1 while the pointer stays
     * inside the same button.
     */
    if (!st.target_inside) {
        s_result_ctrl_wait_release = false;
    }

    if (st.click) {
        if (s_result_ctrl_wait_release) {
            return;
        }

        s_result_ctrl_wait_release = true;
        s_result_ctrl_last_click_ms = now;

        if (st.target_id == AIM_RESULT_CTRL_ANALYSIS_ID) {
            s_result_view =
                (aim_result_view_t)(((int)s_result_view + 1) %
                                    AIM_RESULT_VIEW_COUNT);

            ESP_LOGI(TAG,
                     "Result AirTouch: switch analysis view=%d",
                     (int)s_result_view);

            aim_result_render_view();
        } else if (st.target_id == AIM_RESULT_CTRL_NEXT_ROUND_ID) {
            ESP_LOGI(TAG, "Result AirTouch: next round");
            start_new_round();
        } else if (st.target_id == AIM_RESULT_CTRL_HOME_ID) {
            ESP_LOGI(TAG, "Result AirTouch: home menu");
            aim_show_main_menu();
        }
    }
}


static inline uint32_t now_ms_aim(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int rand_range_aim(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }

    uint32_t r = esp_random();
    return lo + (int)(r % (uint32_t)(hi - lo + 1));
}

static int dist2_aim(int x1, int y1, int x2, int y2)
{
    int dx = x1 - x2;
    int dy = y1 - y2;
    return dx * dx + dy * dy;
}

static int aim_star_visual_size_from_radius(int r);
static int aim_star_line_width_from_size(int size, bool is_hover);
static void aim_star_update_line_points(int idx, int size);

static bool target_position_ok(int idx, int cx, int cy)
{
    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        if (i == idx) {
            continue;
        }

        if (s_targets[i].cx < 0 || s_targets[i].cy < 0) {
            continue;
        }

        int d2 = dist2_aim(cx, cy, s_targets[i].cx, s_targets[i].cy);
        if (d2 < s_adaptive_min_dist * s_adaptive_min_dist) {
            return false;
        }
    }

    return true;
}

static void set_target_position(aim_target_t *target, int cx, int cy)
{
    if (target == NULL || target->obj == NULL) {
        return;
    }

    int visual_size = aim_star_visual_size_from_radius(target->r);
    int visual_center = visual_size / 2;

    int side_margin = s_adaptive_side_margin;
    int top_reserved = s_adaptive_top_reserved;
    int bottom_reserved = s_adaptive_bottom_reserved;

    /*
     * Star Catcher HUD safe zones:
     * top: Time / Stars / Avg + title
     * bottom: progress bar + hint text
     */
    if (side_margin < 44) {
        side_margin = 44;
    }
    if (top_reserved < 92) {
        top_reserved = 92;
    }
    if (bottom_reserved < 90) {
        bottom_reserved = 90;
    }

    int min_x = side_margin + visual_center;
    int max_x = AIM_SCREEN_W - side_margin - visual_center;
    int min_y = top_reserved + visual_center;
    int max_y = AIM_SCREEN_H - bottom_reserved - visual_center;

    if (cx < min_x) {
        cx = min_x;
    } else if (cx > max_x) {
        cx = max_x;
    }

    if (cy < min_y) {
        cy = min_y;
    } else if (cy > max_y) {
        cy = max_y;
    }

    target->cx = cx;
    target->cy = cy;

    lv_obj_set_pos(target->obj,
                   cx - visual_center,
                   cy - visual_center);
}

static bool aim_guided_relation_valid(const char *relation)
{
    return relation != NULL &&
           strlen(relation) == 6 &&
           relation[2] == '-' &&
           relation[3] == '>';
}

static bool aim_region_center_from_name(const char *name, int *out_x, int *out_y)
{
    if (name == NULL || out_x == NULL || out_y == NULL) {
        return false;
    }

    static const char *names[AIM_TRANSITION_REGION_ROWS][AIM_TRANSITION_REGION_COLS] = {
        {"LT", "CT", "RT"},
        {"LC", "CC", "RC"},
        {"LB", "CB", "RB"},
    };

    for (int row = 0; row < AIM_TRANSITION_REGION_ROWS; row++) {
        for (int col = 0; col < AIM_TRANSITION_REGION_COLS; col++) {
            if (strcmp(name, names[row][col]) == 0) {
                *out_x = (col * 2 + 1) * AIM_SCREEN_W / (AIM_TRANSITION_REGION_COLS * 2);
                *out_y = (row * 2 + 1) * AIM_SCREEN_H / (AIM_TRANSITION_REGION_ROWS * 2);
                return true;
            }
        }
    }

    return false;
}


static const char *aim_guided_region_from_xy(int x, int y)
{
    static const char *names[AIM_TRANSITION_REGION_ROWS][AIM_TRANSITION_REGION_COLS] = {
        {"LT", "CT", "RT"},
        {"LC", "CC", "RC"},
        {"LB", "CB", "RB"},
    };

    int col = (x * AIM_TRANSITION_REGION_COLS) / AIM_SCREEN_W;
    int row = (y * AIM_TRANSITION_REGION_ROWS) / AIM_SCREEN_H;

    if (col < 0) {
        col = 0;
    } else if (col >= AIM_TRANSITION_REGION_COLS) {
        col = AIM_TRANSITION_REGION_COLS - 1;
    }

    if (row < 0) {
        row = 0;
    } else if (row >= AIM_TRANSITION_REGION_ROWS) {
        row = AIM_TRANSITION_REGION_ROWS - 1;
    }

    return names[row][col];
}

static int aim_clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static bool aim_place_target_in_region(int idx, const char *region_name)
{
    if (idx < 0 || idx >= AIM_TARGET_COUNT || region_name == NULL) {
        return false;
    }

    int cx = 0;
    int cy = 0;

    if (!aim_region_center_from_name(region_name, &cx, &cy)) {
        return false;
    }

    int min_x = s_adaptive_side_margin + s_adaptive_target_r;
    int max_x = AIM_SCREEN_W - s_adaptive_side_margin - s_adaptive_target_r;

    int min_y = s_adaptive_top_reserved + s_adaptive_target_r;
    int max_y = AIM_SCREEN_H - s_adaptive_bottom_reserved - s_adaptive_target_r;

    uint32_t rnd = esp_random();
    int jitter_x = (int)(rnd % 71U) - 35;
    int jitter_y = (int)((rnd >> 8) % 51U) - 25;

    cx = aim_clamp_int(cx + jitter_x, min_x, max_x);
    cy = aim_clamp_int(cy + jitter_y, min_y, max_y);

    set_target_position(&s_targets[idx], cx, cy);
    s_targets[idx].spawn_ms = now_ms_aim();

    ESP_LOGI(TAG,
             "Guided target placement: sample=%d region=%s x=%d y=%d relation=%s",
             s_guided_plan_index + 1,
             region_name,
             cx,
             cy,
             s_next_guided_relation);

    return true;
}


static void aim_guided_refresh_summary_text(void)
{
    uint32_t rate_pct = 0;

    if (s_guided_bias_attempt_count > 0) {
        rate_pct = (s_guided_bias_accept_count * 100U) / s_guided_bias_attempt_count;
    }

    if (!aim_guided_relation_valid(s_guided_bias_active_relation)) {
        snprintf(s_guided_bias_summary,
                 sizeof(s_guided_bias_summary),
                 "Bias relation: none | accepted: 0 / 0 | F/R/E: 0 / 0 / 0");
        return;
    }

    snprintf(s_guided_bias_summary,
             sizeof(s_guided_bias_summary),
             "Bias relation: %s | accepted: %lu / %lu (%lu%%) | F/R/E: %lu / %lu / %lu",
             s_guided_bias_active_relation,
             (unsigned long)s_guided_bias_accept_count,
             (unsigned long)s_guided_bias_attempt_count,
             (unsigned long)rate_pct,
             (unsigned long)s_guided_bias_forward_count,
             (unsigned long)s_guided_bias_reverse_count,
             (unsigned long)s_guided_bias_entry_count);
}

static void aim_guided_reset_stats(const char *relation)
{
    s_guided_bias_attempt_count = 0;
    s_guided_bias_accept_count = 0;
    s_guided_bias_forward_count = 0;
    s_guided_bias_reverse_count = 0;
    s_guided_bias_entry_count = 0;

    if (aim_guided_relation_valid(relation)) {
        snprintf(s_guided_bias_active_relation,
                 sizeof(s_guided_bias_active_relation),
                 "%s",
                 relation);
    } else {
        snprintf(s_guided_bias_active_relation,
                 sizeof(s_guided_bias_active_relation),
                 "none");
    }

    s_guided_bias_streak_count = 0;
    s_guided_bias_reverse_streak_count = 0;

    aim_guided_refresh_summary_text();
}

static void aim_guided_log_round_summary(void)
{
    uint32_t rate_pct = 0;

    if (s_guided_bias_attempt_count > 0) {
        rate_pct = (s_guided_bias_accept_count * 100U) / s_guided_bias_attempt_count;
    }

    aim_guided_refresh_summary_text();

    ESP_LOGI(TAG,
             "Guided bias summary: relation=%s attempts=%lu accepted=%lu rate=%lu%% forward=%lu reverse=%lu entry=%lu",
             s_guided_bias_active_relation,
             (unsigned long)s_guided_bias_attempt_count,
             (unsigned long)s_guided_bias_accept_count,
             (unsigned long)rate_pct,
             (unsigned long)s_guided_bias_forward_count,
             (unsigned long)s_guided_bias_reverse_count,
             (unsigned long)s_guided_bias_entry_count);
}

static void aim_guided_build_plan_for_new_round(void)
{
    s_guided_plan_count = 0;
    s_guided_plan_index = 0;

    memset(s_guided_plan_regions, 0, sizeof(s_guided_plan_regions));

    aim_guided_reset_stats(s_next_guided_relation);

    if (!aim_guided_relation_valid(s_next_guided_relation)) {
        ESP_LOGI(TAG, "Guided target bias: disabled relation=%s", s_next_guided_relation);
        return;
    }

    char from_region[3] = {
        s_next_guided_relation[0],
        s_next_guided_relation[1],
        '\0',
    };

    char to_region[3] = {
        s_next_guided_relation[4],
        s_next_guided_relation[5],
        '\0',
    };

    int dummy_x = 0;
    int dummy_y = 0;

    if (!aim_region_center_from_name(from_region, &dummy_x, &dummy_y) ||
        !aim_region_center_from_name(to_region, &dummy_x, &dummy_y)) {
        snprintf(s_next_guided_relation, sizeof(s_next_guided_relation), "none");
        aim_guided_reset_stats(s_next_guided_relation);
        ESP_LOGI(TAG, "Guided target bias: disabled invalid relation");
        return;
    }

    ESP_LOGI(TAG,
             "Guided target bias: enabled relation=%s forward=%d%% reverse=%d%% entry=%d%%",
             s_next_guided_relation,
             AIM_GUIDED_FORWARD_PCT,
             AIM_GUIDED_REVERSE_PCT,
             AIM_GUIDED_ENTRY_PCT);
}


static const char *aim_coverage_region_name_from_index(int idx)
{
    static const char names[AIM_REGION_COVERAGE_COUNT][3] = {
        "LT", "CT", "RT",
        "LC", "CC", "RC",
        "LB", "CB", "RB",
    };

    if (idx < 0 || idx >= AIM_REGION_COVERAGE_COUNT) {
        return "CC";
    }

    return names[idx];
}

static int aim_coverage_region_index_from_name(const char *name)
{
    if (name == NULL || strlen(name) < 2) {
        return -1;
    }

    if (strcmp(name, "LT") == 0) return 0;
    if (strcmp(name, "CT") == 0) return 1;
    if (strcmp(name, "RT") == 0) return 2;
    if (strcmp(name, "LC") == 0) return 3;
    if (strcmp(name, "CC") == 0) return 4;
    if (strcmp(name, "RC") == 0) return 5;
    if (strcmp(name, "LB") == 0) return 6;
    if (strcmp(name, "CB") == 0) return 7;
    if (strcmp(name, "RB") == 0) return 8;

    return -1;
}

static void aim_region_coverage_note_name(const char *name)
{
    int idx = aim_coverage_region_index_from_name(name);

    if (idx >= 0 && idx < AIM_REGION_COVERAGE_COUNT) {
        if (s_region_spawn_count[idx] < UINT16_MAX) {
            s_region_spawn_count[idx]++;
        }
    }
}

static bool aim_region_coverage_done(void)
{
    return s_region_coverage_plan_index >= AIM_REGION_COVERAGE_COUNT;
}

static void aim_region_coverage_reset_plan(void)
{
    static const char base[AIM_REGION_COVERAGE_COUNT][3] = {
        "LT", "CT", "RT",
        "LC", "CC", "RC",
        "LB", "CB", "RB",
    };

    for (int i = 0; i < AIM_REGION_COVERAGE_COUNT; i++) {
        snprintf(s_region_coverage_plan[i],
                 sizeof(s_region_coverage_plan[i]),
                 "%s",
                 base[i]);
        s_region_spawn_count[i] = 0;
    }

    for (int i = AIM_REGION_COVERAGE_COUNT - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        char tmp[3];

        snprintf(tmp, sizeof(tmp), "%s", s_region_coverage_plan[i]);
        snprintf(s_region_coverage_plan[i],
                 sizeof(s_region_coverage_plan[i]),
                 "%s",
                 s_region_coverage_plan[j]);
        snprintf(s_region_coverage_plan[j],
                 sizeof(s_region_coverage_plan[j]),
                 "%s",
                 tmp);
    }

    s_region_coverage_plan_index = 0;
    s_region_coverage_forced_count = 0;

    ESP_LOGI(TAG,
             "Region coverage plan reset: %s,%s,%s,%s,%s,%s,%s,%s,%s",
             s_region_coverage_plan[0],
             s_region_coverage_plan[1],
             s_region_coverage_plan[2],
             s_region_coverage_plan[3],
             s_region_coverage_plan[4],
             s_region_coverage_plan[5],
             s_region_coverage_plan[6],
             s_region_coverage_plan[7],
             s_region_coverage_plan[8]);
}

static bool aim_try_coverage_target_position(int idx)
{
    if (idx < 0 || idx >= AIM_TARGET_COUNT) {
        return false;
    }

    while (s_region_coverage_plan_index < AIM_REGION_COVERAGE_COUNT) {
        const char *region = s_region_coverage_plan[s_region_coverage_plan_index];

        s_region_coverage_plan_index++;

        if (aim_place_target_in_region(idx, region)) {
            s_region_coverage_forced_count++;
            aim_region_coverage_note_name(region);

            ESP_LOGI(TAG,
                     "Region coverage placement: index=%u/%d region=%s x=%d y=%d",
                     (unsigned int)s_region_coverage_plan_index,
                     AIM_REGION_COVERAGE_COUNT,
                     region,
                     s_targets[idx].cx,
                     s_targets[idx].cy);

            return true;
        }

        ESP_LOGI(TAG,
                 "Region coverage placement failed: region=%s index=%u/%d",
                 region,
                 (unsigned int)s_region_coverage_plan_index,
                 AIM_REGION_COVERAGE_COUNT);
    }

    return false;
}

static bool aim_try_balanced_region_target_position(int idx)
{
    if (idx < 0 || idx >= AIM_TARGET_COUNT) {
        return false;
    }

    uint16_t min_count = UINT16_MAX;
    int candidates[AIM_REGION_COVERAGE_COUNT];
    int candidate_count = 0;

    for (int i = 0; i < AIM_REGION_COVERAGE_COUNT; i++) {
        if (s_region_spawn_count[i] < min_count) {
            min_count = s_region_spawn_count[i];
            candidate_count = 0;
            candidates[candidate_count++] = i;
        } else if (s_region_spawn_count[i] == min_count &&
                   candidate_count < AIM_REGION_COVERAGE_COUNT) {
            candidates[candidate_count++] = i;
        }
    }

    if (candidate_count <= 0) {
        return false;
    }

    int selected = candidates[esp_random() % (uint32_t)candidate_count];
    const char *region = aim_coverage_region_name_from_index(selected);

    if (aim_place_target_in_region(idx, region)) {
        aim_region_coverage_note_name(region);

        ESP_LOGI(TAG,
                 "Balanced region placement: region=%s count_before=%u x=%d y=%d",
                 region,
                 (unsigned int)min_count,
                 s_targets[idx].cx,
                 s_targets[idx].cy);

        return true;
    }

    return false;
}

static bool aim_try_guided_target_position(int idx)
{
    if (idx < 0 || idx >= AIM_TARGET_COUNT) {
        return false;
    }

    if (!aim_guided_relation_valid(s_next_guided_relation)) {
        return false;
    }

    if (!aim_region_coverage_done()) {
        ESP_LOGI(TAG,
                 "Guided skipped: region coverage not finished index=%u/%d relation=%s",
                 (unsigned int)s_region_coverage_plan_index,
                 AIM_REGION_COVERAGE_COUNT,
                 s_next_guided_relation);
        return false;
    }

    if (s_guided_bias_accept_count >= AIM_GUIDED_MAX_ACCEPT_PER_ROUND) {
        s_guided_bias_streak_count = 0;
        s_guided_bias_reverse_streak_count = 0;

        ESP_LOGI(TAG,
                 "Guided quota reached: accepted=%lu max=%d relation=%s, fallback to balanced random",
                 (unsigned long)s_guided_bias_accept_count,
                 AIM_GUIDED_MAX_ACCEPT_PER_ROUND,
                 s_next_guided_relation);

        return false;
    }

    char from_region[3] = {
        s_next_guided_relation[0],
        s_next_guided_relation[1],
        '\0',
    };

    char to_region[3] = {
        s_next_guided_relation[4],
        s_next_guided_relation[5],
        '\0',
    };

    const char *current_region =
        aim_guided_region_from_xy(s_targets[idx].cx, s_targets[idx].cy);

    const char *target_region = NULL;
    const char *mode = "none";
    int threshold_pct = 0;
    uint32_t roll = esp_random() % 100U;

    s_guided_bias_attempt_count++;

    if (strcmp(current_region, from_region) == 0) {
        threshold_pct = AIM_GUIDED_FORWARD_PCT;
        if ((int)roll < threshold_pct) {
            target_region = to_region;
            mode = "forward";
        }
    } else if (strcmp(current_region, to_region) == 0) {
        threshold_pct = AIM_GUIDED_REVERSE_PCT;
        if ((int)roll < threshold_pct) {
            target_region = from_region;
            mode = "reverse";
        }
    } else {
        threshold_pct = AIM_GUIDED_ENTRY_PCT;
        if ((int)roll < threshold_pct) {
            target_region = from_region;
            mode = "entry";
        }
    }

    if (target_region == NULL) {
        s_guided_bias_streak_count = 0;
        s_guided_bias_reverse_streak_count = 0;

        ESP_LOGI(TAG,
                 "Guided diversity guard: probability fallback to random current=%s roll=%lu threshold=%d relation=%s",
                 current_region,
                 (unsigned long)roll,
                 threshold_pct,
                 s_next_guided_relation);

        return false;
    }

    if (s_guided_bias_streak_count >= AIM_GUIDED_MAX_STREAK) {
        ESP_LOGI(TAG,
                 "Guided diversity guard: force random after %lu guided placements relation=%s current=%s candidate=%s mode=%s",
                 (unsigned long)s_guided_bias_streak_count,
                 s_next_guided_relation,
                 current_region,
                 target_region,
                 mode);

        s_guided_bias_streak_count = 0;
        s_guided_bias_reverse_streak_count = 0;
        return false;
    }

    if (strcmp(mode, "reverse") == 0 &&
        s_guided_bias_reverse_streak_count >= AIM_GUIDED_MAX_REVERSE_STREAK) {
        ESP_LOGI(TAG,
                 "Guided diversity guard: suppress repeated reverse relation=%s current=%s candidate=%s reverse_streak=%lu",
                 s_next_guided_relation,
                 current_region,
                 target_region,
                 (unsigned long)s_guided_bias_reverse_streak_count);

        s_guided_bias_streak_count = 0;
        s_guided_bias_reverse_streak_count = 0;
        return false;
    }

    ESP_LOGI(TAG,
             "Guided bias accepted: current=%s target=%s mode=%s roll=%lu threshold=%d relation=%s",
             current_region,
             target_region,
             mode,
             (unsigned long)roll,
             threshold_pct,
             s_next_guided_relation);

    if (aim_place_target_in_region(idx, target_region)) {
        s_guided_bias_accept_count++;
        aim_region_coverage_note_name(target_region);
        s_guided_bias_streak_count++;

        if (strcmp(mode, "reverse") == 0) {
            s_guided_bias_reverse_streak_count++;
        } else {
            s_guided_bias_reverse_streak_count = 0;
        }

        if (strcmp(mode, "forward") == 0) {
            s_guided_bias_forward_count++;
        } else if (strcmp(mode, "reverse") == 0) {
            s_guided_bias_reverse_count++;
        } else if (strcmp(mode, "entry") == 0) {
            s_guided_bias_entry_count++;
        }

        aim_guided_refresh_summary_text();

        s_guided_plan_index++;
        return true;
    }

    s_guided_bias_streak_count = 0;
    s_guided_bias_reverse_streak_count = 0;

    ESP_LOGI(TAG,
             "Guided diversity guard: placement failed fallback to random relation=%s current=%s candidate=%s mode=%s",
             s_next_guided_relation,
             current_region,
             target_region != NULL ? target_region : "none",
             mode);

    return false;
}

static void randomize_target_position(int idx)
{
    if (idx < 0 || idx >= AIM_TARGET_COUNT) {
        return;
    }

    
    if (aim_try_coverage_target_position(idx)) {
        return;
    }

    if (aim_try_guided_target_position(idx)) {
        return;
    }

    if (aim_try_balanced_region_target_position(idx)) {
        return;
    }
int min_x = s_adaptive_side_margin + s_adaptive_target_r;
    int max_x = AIM_SCREEN_W - s_adaptive_side_margin - s_adaptive_target_r;

    int min_y = s_adaptive_top_reserved + s_adaptive_target_r;
    int max_y = AIM_SCREEN_H - s_adaptive_bottom_reserved - s_adaptive_target_r;

    int cx = 0;
    int cy = 0;

    bool ok = false;

    for (int attempt = 0; attempt < 80; attempt++) {
        cx = rand_range_aim(min_x, max_x);
        cy = rand_range_aim(min_y, max_y);

        if (target_position_ok(idx, cx, cy)) {
            ok = true;
            break;
        }
    }

    if (!ok) {
        /*
         * Sequential single-target mode fallback:
         * If random placement fails, place the only active target near center.
         */
        cx = AIM_SCREEN_W / 2;
        cy = AIM_SCREEN_H / 2;
    }

    set_target_position(&s_targets[idx], cx, cy);
    s_targets[idx].spawn_ms = now_ms_aim();
}


static int aim_star_visual_size_from_radius(int r)
{
    if (r < 1) {
        r = AIM_TARGET_R_BASE;
    }

    return r * 2;
}

static int aim_star_line_width_from_size(int size, bool is_hover)
{
    int w = 4;

    if (size >= 128) {
        w = 7;
    } else if (size >= 112) {
        w = 6;
    } else if (size >= 96) {
        w = 5;
    } else {
        w = 4;
    }

    if (is_hover) {
        w += 1;
    }

    return w;
}

static void aim_star_update_line_points(int idx, int size)
{
    if (idx < 0 || idx >= AIM_TARGET_COUNT || size <= 0) {
        return;
    }

    /*
     * Balanced pentagram inside the adaptive circle.
     * The points are inset from the ring so the thick line does not exceed it.
     *
     * Order:
     * top -> lower-right -> upper-left -> upper-right -> lower-left -> top
     */
    s_star_line_points[idx][0].x = size * 50 / 100;
    s_star_line_points[idx][0].y = size * 13 / 100;

    s_star_line_points[idx][1].x = size * 75 / 100;
    s_star_line_points[idx][1].y = size * 74 / 100;

    s_star_line_points[idx][2].x = size * 18 / 100;
    s_star_line_points[idx][2].y = size * 39 / 100;

    s_star_line_points[idx][3].x = size * 82 / 100;
    s_star_line_points[idx][3].y = size * 39 / 100;

    s_star_line_points[idx][4].x = size * 25 / 100;
    s_star_line_points[idx][4].y = size * 74 / 100;

    s_star_line_points[idx][5].x = size * 50 / 100;
    s_star_line_points[idx][5].y = size * 13 / 100;
}

static void update_target_style(int idx)
{
    if (idx < 0 || idx >= AIM_TARGET_COUNT) {
        return;
    }

    aim_target_t *t = &s_targets[idx];

    if (t->obj == NULL || t->label == NULL) {
        return;
    }

    bool is_hover = (s_hover_target_id == t->id && s_hover_progress > 0);

    int visual_size = aim_star_visual_size_from_radius(t->r);
    int visual_center = visual_size / 2;
    int line_width = aim_star_line_width_from_size(visual_size, is_hover);

    lv_obj_set_size(t->obj, visual_size, visual_size);
    lv_obj_set_style_radius(t->obj, visual_center, LV_PART_MAIN);
    lv_obj_set_style_bg_color(t->obj,
                              lv_color_hex(is_hover ? 0xFFF3BD : 0xFFFBEA),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(t->obj, is_hover ? (lv_opa_t)210 : (lv_opa_t)140, LV_PART_MAIN);
    lv_obj_set_style_border_color(t->obj,
                                  lv_color_hex(is_hover ? 0x6DB7FF : 0xF2D56B),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(t->obj, is_hover ? 4 : 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(t->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(t->obj, 0, LV_PART_MAIN);

    if (t->star_line != NULL) {
        aim_star_update_line_points(idx, visual_size);
        lv_line_set_points(t->star_line, s_star_line_points[idx], 6);
        lv_obj_set_style_line_color(t->star_line,
                                    lv_color_hex(is_hover ? 0xF2B800 : 0xE8B800),
                                    LV_PART_MAIN);
        lv_obj_set_style_line_width(t->star_line, line_width, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(t->star_line, true, LV_PART_MAIN);
        lv_obj_clear_flag(t->star_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(t->star_line);
    }

    lv_obj_set_width(t->label, visual_size);
    lv_obj_set_style_text_color(t->label, lv_color_hex(0xD89B00), LV_PART_MAIN);
    lv_obj_set_style_text_align(t->label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

#if LV_FONT_MONTSERRAT_14
    lv_obj_set_style_text_font(t->label, &lv_font_montserrat_14, LV_PART_MAIN);
#endif

    if (is_hover) {
        aim_label_set_text_fmt_cn_v2n2(t->label, "%d%%", s_hover_progress / 10);
        lv_obj_clear_flag(t->label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(t->label, 0, visual_size - 22);
        lv_obj_move_foreground(t->label);
    } else {
        aim_label_set_text_cn_v2n2(t->label, "");
        lv_obj_add_flag(t->label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_all_target_styles(void)
{
    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        update_target_style(i);
    }
}

static void create_targets(lv_obj_t *scr)
{
    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        s_targets[i].id = (uint16_t)(i + 1);
        s_targets[i].cx = -1;
        s_targets[i].cy = -1;
        s_targets[i].r = s_adaptive_target_r;
        s_targets[i].spawn_ms = 0;
        s_targets[i].star_line = NULL;

        int visual_size = aim_star_visual_size_from_radius(s_targets[i].r);
        int visual_center = visual_size / 2;

        s_targets[i].obj = lv_obj_create(scr);
        lv_obj_set_size(s_targets[i].obj, visual_size, visual_size);
        lv_obj_set_style_radius(s_targets[i].obj, visual_center, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_targets[i].obj, lv_color_hex(0xFFFBEA), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_targets[i].obj, (lv_opa_t)140, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_targets[i].obj, lv_color_hex(0xF2D56B), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_targets[i].obj, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_targets[i].obj, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_targets[i].obj, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s_targets[i].obj, LV_OBJ_FLAG_SCROLLABLE);

        aim_star_update_line_points(i, visual_size);

        s_targets[i].star_line = lv_line_create(s_targets[i].obj);
        if (s_targets[i].star_line != NULL) {
            lv_line_set_points(s_targets[i].star_line, s_star_line_points[i], 6);
            lv_obj_set_style_line_color(s_targets[i].star_line, lv_color_hex(0xE8B800), LV_PART_MAIN);
            lv_obj_set_style_line_width(s_targets[i].star_line,
                                        aim_star_line_width_from_size(visual_size, false),
                                        LV_PART_MAIN);
            lv_obj_set_style_line_rounded(s_targets[i].star_line, true, LV_PART_MAIN);
            lv_obj_clear_flag(s_targets[i].star_line, LV_OBJ_FLAG_SCROLLABLE);
        }

        s_targets[i].label = lv_label_create(s_targets[i].obj);
        if (s_targets[i].label != NULL) {
            lv_obj_set_width(s_targets[i].label, visual_size);
            lv_obj_set_style_text_color(s_targets[i].label, lv_color_hex(0xD89B00), LV_PART_MAIN);
            lv_obj_set_style_text_align(s_targets[i].label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
#if LV_FONT_MONTSERRAT_14
            lv_obj_set_style_text_font(s_targets[i].label, &lv_font_montserrat_14, LV_PART_MAIN);
#endif
            aim_label_set_text_cn_v2n2(s_targets[i].label, "");
            lv_obj_add_flag(s_targets[i].label, LV_OBJ_FLAG_HIDDEN);
        }

        update_target_style(i);
    }
}

static int find_target_index_by_id(uint16_t id)
{
    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        if (s_targets[i].id == id) {
            return i;
        }
    }

    return -1;
}

static uint16_t clamp_reaction_to_u16(uint32_t reaction_ms)
{
    if (reaction_ms == 0) {
        return 1;
    }

    if (reaction_ms > 65535) {
        return 65535;
    }

    return (uint16_t)reaction_ms;
}

static void delete_result_page_if_needed(void)
{
    if (s_result_page != NULL) {
        AIM_OBJ_DELETE(s_result_page);
        s_result_page = NULL;
    }
}


static void aim_apply_adaptive_target_radius(void)
{
    static int s_last_logged_r = -1;
    static int s_last_logged_visual = -1;

    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        s_targets[i].r = s_adaptive_target_r;

        if (s_targets[i].obj != NULL) {
            int visual_size = aim_star_visual_size_from_radius(s_targets[i].r);
            int visual_center = visual_size / 2;

            lv_obj_set_size(s_targets[i].obj,
                            visual_size,
                            visual_size);

            lv_obj_set_pos(s_targets[i].obj,
                           s_targets[i].cx - visual_center,
                           s_targets[i].cy - visual_center);

            if (i == 0 &&
                (s_last_logged_r != s_targets[i].r ||
                 s_last_logged_visual != visual_size)) {
                ESP_LOGI("air_aim_trainer",
                         "adaptive target size: r=%d visual=%d",
                         s_targets[i].r,
                         visual_size);
                s_last_logged_r = s_targets[i].r;
                s_last_logged_visual = visual_size;
            }
        }

        update_target_style(i);
    }
}

static void start_new_round(void)
{
    record_delete_page();
    aim_delete_menu_page_if_needed();
    aim_show_star_game_page();

    /*
     * Keep Star page visible above any old full-screen page, then targets are moved above it.
     */
    if (s_star_page != NULL) {
        lv_obj_move_foreground(s_star_page);
    }

    /*
     * Star round must re-show target objects hidden by main menu.
     * Menu mode hides the training targets to keep the home screen clean.
     */
    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        if (s_targets[i].obj != NULL) {
            lv_obj_clear_flag(s_targets[i].obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_targets[i].obj);
        }
    }

    if (s_title_label != NULL) {
        lv_obj_move_foreground(s_title_label);
    }
    if (s_info_label != NULL) {
        lv_obj_move_foreground(s_info_label);
    }
    if (s_status_label != NULL) {
        lv_obj_move_foreground(s_status_label);
    }
    if (s_progress_bar != NULL) {
        lv_obj_move_foreground(s_progress_bar);
    }
    if (s_star_feedback_label != NULL) {
        lv_obj_move_foreground(s_star_feedback_label);
    }

    ESP_LOGI(TAG, "Star target objects re-shown after menu");


    aim_set_control_button_visible(NULL, false);

    delete_result_page_if_needed();

    s_result_layer = NULL;
    s_result_analysis_btn = NULL;
    s_result_next_btn = NULL;
    s_result_home_btn = NULL;
    s_result_view = AIM_RESULT_VIEW_SUMMARY;
    s_result_ctrl_wait_release = false;
    s_result_ctrl_last_click_ms = 0;

    s_game_state = AIM_STATE_RUNNING;

    s_round_start_ms = now_ms_aim();
    s_result_start_ms = 0;

    s_hits = 0;
    s_total_reaction_ms = 0;
    s_fastest_reaction_ms = UINT32_MAX;
    
    /*
     * Cache the previous round's weakest transition before clearing
     * transition statistics. This is the handoff from analysis result
     * to the next round's guided target generation.
     */
    if (aim_guided_relation_valid(s_transition_weak_relation)) {
        snprintf(s_next_guided_relation,
                 sizeof(s_next_guided_relation),
                 "%s",
                 s_transition_weak_relation);

        ESP_LOGI(TAG,
                 "Guided relation cached before round reset: %s",
                 s_next_guided_relation);
    } else if (!aim_guided_relation_valid(s_next_guided_relation)) {
        snprintf(s_next_guided_relation,
                 sizeof(s_next_guided_relation),
                 "none");
    }

    aim_transition_clear_round();

    /*
     * Start a new per-round attention map.
     * The previous round has already been evaluated before this function runs.
     */
    air_attention_heatmap_clear();
    air_attention_heatmap_set_enabled(true);

    s_hover_target_id = 0;
    s_hover_progress = 0;

    air_heatmap_reset();

    aim_apply_adaptive_level();
    aim_cloud_apply_star_runtime_v2j();
    aim_apply_adaptive_target_radius();

    air_input_config_t cfg = {
        .dwell_ms = s_adaptive_dwell_ms,
        .cooldown_ms = s_adaptive_cooldown_ms,
        .stable_radius_px = s_adaptive_stable_radius_px,
        .target_expand_px = s_adaptive_target_expand_px,
    };
    air_input_set_config(&cfg);

    
    aim_region_coverage_reset_plan();
    aim_guided_build_plan_for_new_round();

    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        randomize_target_position(i);
    }

    update_all_target_styles();

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }

    if (s_status_label != NULL) {
        aim_label_set_text_fmt_cn_v2n2(s_status_label,
                              "等级:%s  悬停目标",
                              aim_adaptive_level_name());
    }

    if (s_info_label != NULL) {
        aim_label_set_text_fmt_cn_v2n2(s_info_label,
                              "时间 %lus    星星 0    平均 0ms",
                              (unsigned long)(s_star_round_ms_v2j / 1000U));
    }

    airtouch_sfx_start_v1d();
    ESP_LOGI(TAG, "New aim round started");
}

static void enter_result_state(void)
{
    s_game_state = AIM_STATE_RESULT;
    airtouch_sfx_result_v1d();
    s_result_start_ms = now_ms_aim();

    s_hover_target_id = 0;
    s_hover_progress = 0;
    s_result_ctrl_wait_release = false;
    s_result_ctrl_last_click_ms = 0;

    update_all_target_styles();

    uint32_t avg_ms = 0;
    if (s_hits > 0) {
        avg_ms = s_total_reaction_ms / s_hits;
    }

    uint32_t fastest = 0;
    if (s_fastest_reaction_ms != UINT32_MAX) {
        fastest = s_fastest_reaction_ms;
    }

    uint32_t star_hit_score = star_hit_score_from_hits(s_hits);
    uint32_t star_speed_score = color_go_speed_score_from_avg_ms(avg_ms);
    star_history_push(s_hits, star_hit_score, avg_ms, fastest, star_speed_score);

    airtouch_star_record_t star_sd_rec = {
        .record_id = 0,
        .boot_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .hits = s_hits,
        .avg_ms = avg_ms,
        .fastest_ms = fastest,
        .hit_score = star_hit_score,
        .speed_score = star_speed_score,
        .difficulty = (uint32_t)s_adaptive_load_score,
        .dwell_ms = s_adaptive_dwell_ms,
        .target_radius = (uint32_t)s_adaptive_target_r,
        .round_duration_s = s_star_round_ms_v2j / 1000U,
        .adaptive_level = (uint32_t)s_adaptive_level,
    };
    airtouch_storage_append_star_record(&star_sd_rec);

    aim_update_adaptive_from_round(s_hits, avg_ms, fastest);
    aim_transition_log_round_summary();
    aim_guided_log_round_summary();
    /*
     * Stop collecting attention data while waiting on result page.
     * The next round will explicitly re-enable it after START/NEXT.
     */
    air_attention_heatmap_set_enabled(false);


    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }

    if (s_status_label != NULL) {
        /*
         * Transition result UI v2
         * -----------------------
         * The backend already records directed spatial transfer relations:
         *      previous target region -> current target region
         *
         * This result text exposes those relation-level metrics on screen.
         * It is intentionally kept as text first; later we can upgrade it
         * into arrows or a 3x3 region-to-region matrix.
         */
        if (s_transition_count > 0) {
            uint32_t ui_avg_transfer_ms =
                s_transition_total_completion_ms / s_transition_count;

            int ui_avg_straight_pct =
                (int)((s_transition_total_straightness * 100.0f /
                       (float)s_transition_count) + 0.5f);

            int ui_avg_dev_px =
                (int)((s_transition_total_max_dev_px /
                       (float)s_transition_count) + 0.5f);

            aim_label_set_text_fmt_cn_v2n2(s_status_label,
                                  "RESULT  Next:%s  Hits:%lu  Avg:%lums  Fast:%lums\n"
                                  "Transfer:%lums  Straight:%d%%  Dev:%dpx  Weak:%s/%lums",
                                  aim_adaptive_level_name(),
                                  (unsigned long)s_hits,
                                  (unsigned long)avg_ms,
                                  (unsigned long)fastest,
                                  (unsigned long)ui_avg_transfer_ms,
                                  ui_avg_straight_pct,
                                  ui_avg_dev_px,
                                  s_transition_weak_relation,
                                  (unsigned long)s_transition_slowest_ms);
        } else {
            aim_label_set_text_fmt_cn_v2n2(s_status_label,
                                  "RESULT  Next:%s  Hits:%lu  Avg:%lums  Fast:%lums\n"
                                  "Transfer: not enough transition data",
                                  aim_adaptive_level_name(),
                                  (unsigned long)s_hits,
                                  (unsigned long)avg_ms,
                                  (unsigned long)fastest);
        }
    }

    delete_result_page_if_needed();

    s_result_page = lv_obj_create(lv_scr_act());
    if (s_result_page != NULL) {
        lv_obj_set_size(s_result_page, AIM_SCREEN_W, AIM_SCREEN_H);
        lv_obj_align(s_result_page, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(s_result_page, lv_color_hex(0x050B14), 0);
        lv_obj_set_style_bg_opa(s_result_page, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_result_page, 0, 0);
        lv_obj_set_style_radius(s_result_page, 0, 0);
        lv_obj_set_style_pad_all(s_result_page, 0, 0);
        lv_obj_clear_flag(s_result_page, LV_OBJ_FLAG_SCROLLABLE);
    }

    s_result_view = AIM_RESULT_VIEW_SUMMARY;
    aim_result_render_view();



    if (s_result_page != NULL) {
        lv_obj_move_foreground(s_result_page);
    }

    aim_set_control_button_visible(NULL, false);


    ESP_LOGI(TAG,
             "Round result: hits=%lu avg=%lu ms fast=%lu ms, load=%d profile=%s target_r=%d dwell=%lu ms, blank result page created",
             (unsigned long)s_hits,
             (unsigned long)avg_ms,
             (unsigned long)fastest,
             s_adaptive_load_score,
             aim_adaptive_level_name(),
             s_adaptive_target_r,
             (unsigned long)s_adaptive_dwell_ms);
}

static void update_header_running(uint32_t now)
{
    uint32_t elapsed = now - s_round_start_ms;
    uint32_t remain = 0;
    uint32_t round_ms = s_star_round_ms_v2j;

    if (round_ms < 1000U) {
        round_ms = AIM_ROUND_MS;
    }

    if (elapsed < round_ms) {
        remain = round_ms - elapsed;
    }

    uint32_t remain_s = (remain + 999) / 1000;

    uint32_t avg_ms = 0;
    if (s_hits > 0) {
        avg_ms = s_total_reaction_ms / s_hits;
    }

    if (s_info_label != NULL) {
        lv_obj_clear_flag(s_info_label, LV_OBJ_FLAG_HIDDEN);
        aim_label_set_text_fmt_cn_v2n2(s_info_label,
                              "时间 %lus    星星 %lu    平均 %lums",
                              (unsigned long)remain_s,
                              (unsigned long)s_hits,
                              (unsigned long)avg_ms);
        lv_obj_move_foreground(s_info_label);
    }
}

static void update_header_result(uint32_t now)
{
    (void)now;

    if (s_info_label != NULL) {
        aim_label_set_text_fmt_cn_v2n2(s_info_label,
                              "悬停继续训练   %s",
                              s_adaptive_advice);
    }
}


static float aim_dist_float(int x1, int y1, int x2, int y2)
{
    float dx = (float)(x1 - x2);
    float dy = (float)(y1 - y2);
    return sqrtf(dx * dx + dy * dy);
}

static const char *aim_region_name_from_xy(int x, int y)
{
    int col = (x * AIM_TRANSITION_REGION_COLS) / AIM_SCREEN_W;
    int row = (y * AIM_TRANSITION_REGION_ROWS) / AIM_SCREEN_H;

    if (col < 0) {
        col = 0;
    } else if (col >= AIM_TRANSITION_REGION_COLS) {
        col = AIM_TRANSITION_REGION_COLS - 1;
    }

    if (row < 0) {
        row = 0;
    } else if (row >= AIM_TRANSITION_REGION_ROWS) {
        row = AIM_TRANSITION_REGION_ROWS - 1;
    }

    static const char *names[AIM_TRANSITION_REGION_ROWS][AIM_TRANSITION_REGION_COLS] = {
        {"LT", "CT", "RT"},
        {"LC", "CC", "RC"},
        {"LB", "CB", "RB"},
    };

    return names[row][col];
}

static void aim_transition_clear_round(void)
{
    /*
     * Clear only transition-analysis statistics for the current round.
     * This does not clear the visual hit heatmap or the attention heatmap.
     */
    s_transition_active = false;
    s_transition_first_move_ms = 0;
    s_transition_last_valid = false;
    s_transition_path_len_px = 0.0f;
    s_transition_max_dev_px = 0.0f;

    s_transition_count = 0;
    s_transition_total_completion_ms = 0;
    s_transition_total_path_len_px = 0.0f;
    s_transition_total_straightness = 0.0f;
    s_transition_total_max_dev_px = 0.0f;

    s_transition_slowest_ms = 0;
    snprintf(s_transition_weak_relation,
             sizeof(s_transition_weak_relation),
             "none");
}

static void aim_transition_begin(int from_x,
                                 int from_y,
                                 int to_x,
                                 int to_y,
                                 uint32_t start_ms)
{
    /*
     * Begin tracking one directed spatial relation:
     *     previous target center -> next target center
     */
    s_transition_active = true;

    s_transition_from_x = from_x;
    s_transition_from_y = from_y;
    s_transition_to_x = to_x;
    s_transition_to_y = to_y;
    s_transition_start_ms = start_ms;
    s_transition_first_move_ms = 0;

    s_transition_last_valid = false;
    s_transition_last_x = 0;
    s_transition_last_y = 0;
    s_transition_path_len_px = 0.0f;
    s_transition_max_dev_px = 0.0f;

    ESP_LOGI(TAG,
             "Transition begin: %s(%d,%d) -> %s(%d,%d)",
             aim_region_name_from_xy(from_x, from_y),
             from_x,
             from_y,
             aim_region_name_from_xy(to_x, to_y),
             to_x,
             to_y);
}

static void aim_transition_push_pointer(const air_input_state_t *st, uint32_t now)
{
    if (!s_transition_active || st == NULL || !st->pointer_valid) {
        return;
    }

    int x = st->x;
    int y = st->y;

    if (s_transition_first_move_ms == 0) {
        int d2_from = dist2_aim(x, y, s_transition_from_x, s_transition_from_y);
        if (d2_from >= AIM_TRANSITION_MOVE_START_PX * AIM_TRANSITION_MOVE_START_PX) {
            s_transition_first_move_ms = now;
        }
    }

    if (s_transition_last_valid) {
        s_transition_path_len_px += aim_dist_float(s_transition_last_x,
                                                   s_transition_last_y,
                                                   x,
                                                   y);
    }

    /*
     * Maximum deviation from the ideal straight path:
     * distance from current point to the line segment approximation
     * formed by previous-target center and current-target center.
     */
    float ax = (float)s_transition_from_x;
    float ay = (float)s_transition_from_y;
    float bx = (float)s_transition_to_x;
    float by = (float)s_transition_to_y;
    float px = (float)x;
    float py = (float)y;

    float vx = bx - ax;
    float vy = by - ay;
    float wx = px - ax;
    float wy = py - ay;
    float line_len = sqrtf(vx * vx + vy * vy);

    if (line_len > 1.0f) {
        float cross = fabsf(vx * wy - vy * wx);
        float dev = cross / line_len;

        if (dev > s_transition_max_dev_px) {
            s_transition_max_dev_px = dev;
        }
    }

    s_transition_last_x = x;
    s_transition_last_y = y;
    s_transition_last_valid = true;
}

static void aim_transition_finish(uint32_t hit_ms, uint32_t completion_ms)
{
    if (!s_transition_active) {
        /*
         * The first target in a round has no previous target, so it does
         * not form an inter-region transition relation.
         */
        return;
    }

    float straight_px = aim_dist_float(s_transition_from_x,
                                       s_transition_from_y,
                                       s_transition_to_x,
                                       s_transition_to_y);

    float path_px = s_transition_path_len_px;

    if (path_px < straight_px) {
        /*
         * Sampling may miss the first part of a very fast movement.
         * Keep straightness within a meaningful range.
         */
        path_px = straight_px;
    }

    float straightness = 1.0f;
    if (path_px > 1.0f) {
        straightness = straight_px / path_px;
    }

    if (straightness < 0.0f) {
        straightness = 0.0f;
    } else if (straightness > 1.0f) {
        straightness = 1.0f;
    }

    uint32_t reaction_to_move_ms = 0;
    if (s_transition_first_move_ms > 0 && s_transition_first_move_ms >= s_transition_start_ms) {
        reaction_to_move_ms = s_transition_first_move_ms - s_transition_start_ms;
    }

    s_transition_count++;
    s_transition_total_completion_ms += completion_ms;
    s_transition_total_path_len_px += path_px;
    s_transition_total_straightness += straightness;
    s_transition_total_max_dev_px += s_transition_max_dev_px;

    if (completion_ms > s_transition_slowest_ms) {
        s_transition_slowest_ms = completion_ms;
        snprintf(s_transition_weak_relation,
                 sizeof(s_transition_weak_relation),
                 "%s->%s",
                 aim_region_name_from_xy(s_transition_from_x, s_transition_from_y),
                 aim_region_name_from_xy(s_transition_to_x, s_transition_to_y));
    }

    ESP_LOGI(TAG,
             "Transition finish: %s->%s completion=%lu ms move_reaction=%lu ms straight=%d%% path=%d px dev=%d px",
             aim_region_name_from_xy(s_transition_from_x, s_transition_from_y),
             aim_region_name_from_xy(s_transition_to_x, s_transition_to_y),
             (unsigned long)completion_ms,
             (unsigned long)reaction_to_move_ms,
             (int)(straightness * 100.0f + 0.5f),
             (int)(path_px + 0.5f),
             (int)(s_transition_max_dev_px + 0.5f));

    s_transition_active = false;
    (void)hit_ms;
}

static void aim_transition_log_round_summary(void)
{
    if (s_transition_count == 0) {
        ESP_LOGI(TAG, "Transition summary: no inter-target transition in this round");
        return;
    }

    uint32_t avg_completion_ms = s_transition_total_completion_ms / s_transition_count;
    int avg_straight_pct = (int)((s_transition_total_straightness * 100.0f /
                                  (float)s_transition_count) + 0.5f);
    int avg_path_px = (int)((s_transition_total_path_len_px /
                             (float)s_transition_count) + 0.5f);
    int avg_dev_px = (int)((s_transition_total_max_dev_px /
                            (float)s_transition_count) + 0.5f);

    ESP_LOGI(TAG,
             "Transition summary: count=%lu avg_completion=%lu ms avg_straight=%d%% avg_path=%d px avg_dev=%d px weakest=%s slowest=%lu ms",
             (unsigned long)s_transition_count,
             (unsigned long)avg_completion_ms,
             avg_straight_pct,
             avg_path_px,
             avg_dev_px,
             s_transition_weak_relation,
             (unsigned long)s_transition_slowest_ms);
}


static void aim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint32_t now = now_ms_aim();

    if (s_app_mode == AIM_APP_MODE_BOOT) {
        return;
    }

    if (s_app_mode == AIM_APP_MODE_MENU) {
        aim_menu_process_airtouch(now);
        return;
    }

    if (s_app_mode == AIM_APP_MODE_COLOR_GO) {
        color_go_process_airtouch(now);
        return;
    }

    if (s_app_mode == AIM_APP_MODE_RECORD) {
        record_process_airtouch(now);
        return;
    }


    if (s_game_state == AIM_STATE_IDLE) {
        return;
    }

    if (s_game_state == AIM_STATE_RESULT) {
        update_header_result(now);
        aim_result_process_airtouch(now);
        return;
    }

    if (now - s_round_start_ms >= s_star_round_ms_v2j) {
        enter_result_state();
        return;
    }

    air_input_circle_target_t circle_targets[AIM_TARGET_COUNT];

    for (int i = 0; i < AIM_TARGET_COUNT; i++) {
        circle_targets[i].id = s_targets[i].id;
        circle_targets[i].cx = s_targets[i].cx;
        circle_targets[i].cy = s_targets[i].cy;
        circle_targets[i].r = s_targets[i].r;
    }

    air_input_state_t st;
    memset(&st, 0, sizeof(st));

    air_input_update_circle_targets(circle_targets, AIM_TARGET_COUNT, &st);

    aim_transition_push_pointer(&st, now);

    s_hover_target_id = st.target_inside ? st.target_id : 0;
    s_hover_progress = st.hover_progress;

    if (s_progress_bar != NULL) {
        lv_bar_set_value(s_progress_bar, st.hover_progress, LV_ANIM_OFF);
    }

    if (st.click) {
        int idx = find_target_index_by_id(st.target_id);

        if (idx >= 0) {
            uint32_t reaction_ms = now - s_targets[idx].spawn_ms;
            uint16_t reaction_ms_u16 = clamp_reaction_to_u16(reaction_ms);

            s_hits++;
            s_total_reaction_ms += reaction_ms;

            if (reaction_ms < s_fastest_reaction_ms) {
                s_fastest_reaction_ms = reaction_ms;
            }

            air_heatmap_record_hit((int16_t)s_targets[idx].cx,
                                   (int16_t)s_targets[idx].cy,
                                   reaction_ms_u16);

            ESP_LOGI(TAG,
                     "AIM HIT target=%u x=%d y=%d reaction=%lu ms hits=%lu",
                     st.target_id,
                     s_targets[idx].cx,
                     s_targets[idx].cy,
                     (unsigned long)reaction_ms,
                     (unsigned long)s_hits);

            /*
             * Sequential single-target mode:
             * The current target is the "from" point. After it is hit,
             * a new target is generated and becomes the "to" point.
             * This creates one directed spatial transition relation.
             */
            int transition_from_x = s_targets[idx].cx;
            int transition_from_y = s_targets[idx].cy;

            aim_transition_finish(now, reaction_ms);

            aim_star_show_hit_feedback(transition_from_x,
                                       transition_from_y,
                                       reaction_ms,
                                       now);

            randomize_target_position(idx);

            aim_transition_begin(transition_from_x,
                                 transition_from_y,
                                 s_targets[idx].cx,
                                 s_targets[idx].cy,
                                 now);
        }
    }

    aim_star_update_feedback(now);

    if (s_status_label != NULL) {
        if (!st.pointer_valid) {
            aim_label_set_text_cn_v2n2(s_status_label, "等待空中指针");
        } else if (st.target_inside) {
            aim_label_set_text_fmt_cn_v2n2(s_status_label,
                                  "保持悬停   %d%%",
                                  st.hover_progress / 10);
        } else {
            aim_label_set_text_cn_v2n2(s_status_label, "移动到星星上方，保持悬停完成捕捉");
        }
    }

    update_header_running(now);
    update_all_target_styles();
}

static void airtouch_record_restore_from_sd(void)
{
    if (!airtouch_storage_is_ready()) {
        ESP_LOGW(TAG, "SD history restore skipped: storage not ready");
        return;
    }

    airtouch_star_record_t star_records[STAR_HISTORY_MAX];
    airtouch_color_record_t color_records[COLOR_GO_HISTORY_MAX];

    uint32_t star_count = airtouch_storage_load_recent_star_records(star_records,
                                                                    STAR_HISTORY_MAX);
    uint32_t color_count = airtouch_storage_load_recent_color_records(color_records,
                                                                      COLOR_GO_HISTORY_MAX);

    s_star_history_count = 0;
    s_star_latest_valid = false;
    s_star_latest_hits = 0;
    s_star_latest_hit_score = 0;
    s_star_latest_avg_ms = 0;
    s_star_latest_fastest_ms = 0;
    s_star_latest_speed_score = 0;
    memset(s_star_history_hits, 0, sizeof(s_star_history_hits));
    memset(s_star_history_hit_score, 0, sizeof(s_star_history_hit_score));
    memset(s_star_history_avg_ms, 0, sizeof(s_star_history_avg_ms));
    memset(s_star_history_speed_score, 0, sizeof(s_star_history_speed_score));

    for (uint32_t i = 0; i < star_count; i++) {
        star_history_push(star_records[i].hits,
                          star_records[i].hit_score,
                          star_records[i].avg_ms,
                          star_records[i].fastest_ms,
                          star_records[i].speed_score);
    }

    s_color_go_history_count = 0;
    s_color_go_latest_valid = false;
    s_color_go_latest_correct = 0;
    s_color_go_latest_wrong = 0;
    s_color_go_latest_false_alarm = 0;
    s_color_go_latest_miss = 0;
    s_color_go_latest_accuracy = 0;
    s_color_go_latest_avg_ms = 0;
    s_color_go_latest_fastest_ms = 0;
    s_color_go_latest_inhibition = 0;
    memset(s_color_go_history_accuracy, 0, sizeof(s_color_go_history_accuracy));
    memset(s_color_go_history_avg_ms, 0, sizeof(s_color_go_history_avg_ms));
    memset(s_color_go_history_inhibition, 0, sizeof(s_color_go_history_inhibition));
    memset(s_color_go_history_correct, 0, sizeof(s_color_go_history_correct));

    for (uint32_t i = 0; i < color_count; i++) {
        color_go_history_push(color_records[i].accuracy,
                              color_records[i].avg_ms,
                              color_records[i].inhibition,
                              color_records[i].correct);
    }

    if (color_count > 0) {
        const airtouch_color_record_t *last_color = &color_records[color_count - 1U];

        s_color_go_latest_valid = true;
        s_color_go_latest_correct = last_color->correct;
        s_color_go_latest_wrong = last_color->wrong;
        s_color_go_latest_false_alarm = last_color->false_alarm;
        s_color_go_latest_miss = last_color->miss;
        s_color_go_latest_accuracy = last_color->accuracy;
        s_color_go_latest_avg_ms = last_color->avg_ms;
        s_color_go_latest_fastest_ms = last_color->fastest_ms;
        s_color_go_latest_inhibition = last_color->inhibition;
    }

    ESP_LOGI(TAG,
             "AirTouch SD history restored: star=%lu color=%lu",
             (unsigned long)star_count,
             (unsigned long)color_count);
}

void air_aim_trainer_ui_start(void)
{
    if (s_timer != NULL) {
        return;
    }

    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    s_title_label = lv_label_create(scr);
    aim_label_set_text_cn_v2n2(s_title_label, "AirTouch Sequential Target Trainer");
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 16);

    s_info_label = lv_label_create(scr);
    aim_label_set_text_cn_v2n2(s_info_label, "时间:45s   命中:0   平均:0ms");
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(0xDDDDDD), LV_PART_MAIN);
    lv_obj_align(s_info_label, LV_ALIGN_TOP_MID, 0, 46);

    s_status_label = lv_label_create(scr);
    aim_label_set_text_cn_v2n2(s_status_label, "悬停目标");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xCCCCCC), LV_PART_MAIN);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -16);

    s_progress_bar = lv_bar_create(scr);
    lv_obj_set_size(s_progress_bar, 360, 12);
    lv_obj_align(s_progress_bar, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_bar_set_range(s_progress_bar, 0, 1000);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0xFFD166), LV_PART_INDICATOR);

    create_targets(scr);



    s_control_button = lv_btn_create(scr);
    lv_obj_set_size(s_control_button, 260, 64);
    lv_obj_align(s_control_button, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_add_event_cb(s_control_button,
                        aim_control_button_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    s_control_button_label = lv_label_create(s_control_button);
    aim_label_set_text_cn_v2n2(s_control_button_label, "开始");
    lv_obj_center(s_control_button_label);

    s_game_state = AIM_STATE_IDLE;
    air_attention_heatmap_set_enabled(false);
    aim_set_control_button_visible("开始", true);

    s_timer = lv_timer_create(aim_timer_cb, AIM_TIMER_MS, NULL);

    airtouch_record_restore_from_sd();

    aim_show_boot_screen();

    ESP_LOGI(TAG, "AirTouch aim trainer UI started");
}













