#include "air_heatmap.h"

#include <stdio.h>
#include <string.h>

#define RT_INIT_BEST 0xFFFF

static air_heatmap_stats_t s_stats;

static const char *s_cell_name[AIR_HEATMAP_GRID_N] = {
    "LT", "TOP", "RT",
    "LEFT", "MID", "RIGHT",
    "LB", "BOTTOM", "RB"
};

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static int xy_to_cell(int16_t x, int16_t y)
{
    int cx = clamp_int((int)x, 0, AIR_HEATMAP_SCREEN_W - 1);
    int cy = clamp_int((int)y, 0, AIR_HEATMAP_SCREEN_H - 1);

    int gx = cx * 3 / AIR_HEATMAP_SCREEN_W;
    int gy = cy * 3 / AIR_HEATMAP_SCREEN_H;

    gx = clamp_int(gx, 0, 2);
    gy = clamp_int(gy, 0, 2);

    return gy * 3 + gx;
}

static uint16_t cell_avg_ms(int idx)
{
    if (idx < 0 || idx >= AIR_HEATMAP_GRID_N) {
        return 0;
    }

    if (s_stats.hit_count[idx] == 0) {
        return 0;
    }

    return (uint16_t)(s_stats.rt_sum_ms[idx] / s_stats.hit_count[idx]);
}

static void update_weakest_cell(void)
{
    int weakest = -1;
    uint16_t weakest_avg = 0;

    for (int i = 0; i < AIR_HEATMAP_GRID_N; i++) {
        if (s_stats.hit_count[i] == 0) {
            continue;
        }

        uint16_t avg = cell_avg_ms(i);

        if (weakest < 0 || avg > weakest_avg) {
            weakest = i;
            weakest_avg = avg;
        }
    }

    s_stats.weakest_cell = weakest;
    s_stats.weakest_avg_ms = weakest_avg;
}

static lv_color_t color_for_cell(int idx)
{
    if (idx < 0 || idx >= AIR_HEATMAP_GRID_N || s_stats.hit_count[idx] == 0) {
        return lv_color_hex(0x182131);
    }

    uint16_t avg = cell_avg_ms(idx);

    /*
     * Color meaning:
     * Green / cyan: fast
     * Yellow / orange: medium
     * Red: slow
     *
     * Avoid using large-area magenta here,
     * because magenta is reserved for the AirTouch pen tip.
     */
    if (avg <= 650) {
        return lv_color_hex(0x13A89E);
    } else if (avg <= 900) {
        return lv_color_hex(0x54B96F);
    } else if (avg <= 1200) {
        return lv_color_hex(0xD9A441);
    } else {
        return lv_color_hex(0xC94B4B);
    }
}

static void style_fullscreen_bg(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, AIR_HEATMAP_SCREEN_W, AIR_HEATMAP_SCREEN_H);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x08111F), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_card(lv_obj_t *obj, lv_color_t bg, int radius)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x2D3E56), 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            const char *txt,
                            lv_color_t color,
                            int font_size_hint)
{
    (void)font_size_hint;

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    return label;
}

void air_heatmap_reset(void)
{
    memset(&s_stats, 0, sizeof(s_stats));

    for (int i = 0; i < AIR_HEATMAP_GRID_N; i++) {
        s_stats.rt_best_ms[i] = RT_INIT_BEST;
    }

    s_stats.best_rt_ms = RT_INIT_BEST;
    s_stats.worst_rt_ms = 0;
    s_stats.weakest_cell = -1;
    s_stats.weakest_avg_ms = 0;
}

void air_heatmap_record_hit(int16_t target_x,
                            int16_t target_y,
                            uint16_t reaction_ms)
{
    int idx = xy_to_cell(target_x, target_y);

    if (reaction_ms == 0) {
        reaction_ms = 1;
    }

    s_stats.hit_count[idx]++;
    s_stats.rt_sum_ms[idx] += reaction_ms;

    if (reaction_ms < s_stats.rt_best_ms[idx]) {
        s_stats.rt_best_ms[idx] = reaction_ms;
    }

    if (reaction_ms > s_stats.rt_worst_ms[idx]) {
        s_stats.rt_worst_ms[idx] = reaction_ms;
    }

    s_stats.total_hit_count++;
    s_stats.total_rt_sum_ms += reaction_ms;

    if (reaction_ms < s_stats.best_rt_ms) {
        s_stats.best_rt_ms = reaction_ms;
    }

    if (reaction_ms > s_stats.worst_rt_ms) {
        s_stats.worst_rt_ms = reaction_ms;
    }

    update_weakest_cell();
}

const air_heatmap_stats_t *air_heatmap_get_stats(void)
{
    return &s_stats;
}

uint16_t air_heatmap_get_avg_ms(void)
{
    if (s_stats.total_hit_count == 0) {
        return 0;
    }

    return (uint16_t)(s_stats.total_rt_sum_ms / s_stats.total_hit_count);
}

int air_heatmap_get_weakest_cell(void)
{
    return s_stats.weakest_cell;
}

void air_heatmap_fill_demo_data(void)
{
    air_heatmap_reset();

    /*
     * Demo data:
     * Simulate that the lower-right area is slower,
     * so we can directly check the result page style
     * without waiting for a real training round.
     */
    air_heatmap_record_hit(120,  90,  620);
    air_heatmap_record_hit(480,  80,  710);
    air_heatmap_record_hit(850,  90,  760);

    air_heatmap_record_hit(130, 300,  820);
    air_heatmap_record_hit(520, 290,  560);
    air_heatmap_record_hit(850, 310,  930);

    air_heatmap_record_hit(150, 500, 1050);
    air_heatmap_record_hit(520, 500,  880);
    air_heatmap_record_hit(870, 500, 1380);
    air_heatmap_record_hit(900, 520, 1510);
    air_heatmap_record_hit(880, 470, 1290);
}

lv_obj_t *air_heatmap_create_result_page(lv_obj_t *parent,
                                         int score,
                                         int difficulty_level,
                                         int target_radius,
                                         int dwell_ms)
{
    update_weakest_cell();

    lv_obj_t *root = lv_obj_create(parent);
    style_fullscreen_bg(root);

    lv_obj_t *title = make_label(root,
                                 "AirTouch Training Result",
                                 lv_color_hex(0xEAF2FF),
                                 32);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *sub = make_label(root,
                               "Reaction heatmap based on 3x3 screen regions",
                               lv_color_hex(0x8EA4C2),
                               18);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 62);

    uint16_t avg_ms = air_heatmap_get_avg_ms();
    uint16_t best_ms = (s_stats.best_rt_ms == RT_INIT_BEST) ? 0 : s_stats.best_rt_ms;

    const char *grade = "C";
    if (s_stats.total_hit_count >= 25 && avg_ms > 0 && avg_ms <= 700) {
        grade = "S";
    } else if (s_stats.total_hit_count >= 18 && avg_ms > 0 && avg_ms <= 900) {
        grade = "A";
    } else if (s_stats.total_hit_count >= 10 && avg_ms > 0 && avg_ms <= 1200) {
        grade = "B";
    }

    lv_obj_t *left_card = lv_obj_create(root);
    lv_obj_set_size(left_card, 300, 390);
    lv_obj_align(left_card, LV_ALIGN_LEFT_MID, 40, 25);
    style_card(left_card, lv_color_hex(0x101C2E), 18);

    lv_obj_t *summary_title = make_label(left_card,
                                         "SUMMARY",
                                         lv_color_hex(0x8EA4C2),
                                         18);
    lv_obj_align(summary_title, LV_ALIGN_TOP_LEFT, 2, 0);

    lv_obj_t *score_label = make_label(left_card,
                                       "",
                                       lv_color_hex(0xFFFFFF),
                                       28);
    lv_label_set_text_fmt(score_label, "Hits: %d", score);
    lv_obj_align(score_label, LV_ALIGN_TOP_LEFT, 2, 48);

    lv_obj_t *avg_label = make_label(left_card,
                                     "",
                                     lv_color_hex(0xEAF2FF),
                                     24);
    if (avg_ms > 0) {
        lv_label_set_text_fmt(avg_label, "Avg RT: %u ms", (unsigned int)avg_ms);
    } else {
        lv_label_set_text(avg_label, "Avg RT: --");
    }
    lv_obj_align(avg_label, LV_ALIGN_TOP_LEFT, 2, 92);

    lv_obj_t *best_label = make_label(left_card,
                                      "",
                                      lv_color_hex(0xEAF2FF),
                                      24);
    if (best_ms > 0) {
        lv_label_set_text_fmt(best_label, "Best RT: %u ms", (unsigned int)best_ms);
    } else {
        lv_label_set_text(best_label, "Best RT: --");
    }
    lv_obj_align(best_label, LV_ALIGN_TOP_LEFT, 2, 136);

    lv_obj_t *grade_label = make_label(left_card,
                                       "",
                                       lv_color_hex(0x36D399),
                                       30);
    lv_label_set_text_fmt(grade_label, "Grade: %s", grade);
    lv_obj_align(grade_label, LV_ALIGN_TOP_LEFT, 2, 190);

    lv_obj_t *weak_label = make_label(left_card,
                                      "",
                                      lv_color_hex(0xF3C969),
                                      20);
    if (s_stats.weakest_cell >= 0) {
        lv_label_set_text_fmt(weak_label,
                              "Weak area: %s / %u ms",
                              s_cell_name[s_stats.weakest_cell],
                              (unsigned int)s_stats.weakest_avg_ms);
    } else {
        lv_label_set_text(weak_label, "Weak area: --");
    }
    lv_obj_align(weak_label, LV_ALIGN_TOP_LEFT, 2, 248);

    lv_obj_t *diff_label = make_label(left_card,
                                      "",
                                      lv_color_hex(0x8EA4C2),
                                      18);
    lv_label_set_text_fmt(diff_label,
                          "D%d  R%d  Hold%dms",
                          difficulty_level,
                          target_radius,
                          dwell_ms);
    lv_obj_align(diff_label, LV_ALIGN_TOP_LEFT, 2, 310);

    lv_obj_t *heat_card = lv_obj_create(root);
    lv_obj_set_size(heat_card, 600, 390);
    lv_obj_align(heat_card, LV_ALIGN_RIGHT_MID, -40, 25);
    style_card(heat_card, lv_color_hex(0x101C2E), 18);

    lv_obj_t *heat_title = make_label(heat_card,
                                      "3x3 REACTION HEATMAP",
                                      lv_color_hex(0x8EA4C2),
                                      18);
    lv_obj_align(heat_title, LV_ALIGN_TOP_LEFT, 4, 0);

    const int grid_x0 = 24;
    const int grid_y0 = 50;
    const int cell_w = 170;
    const int cell_h = 92;
    const int gap = 12;

    for (int i = 0; i < AIR_HEATMAP_GRID_N; i++) {
        int gx = i % 3;
        int gy = i / 3;

        lv_obj_t *cell = lv_obj_create(heat_card);
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_pos(cell,
                       grid_x0 + gx * (cell_w + gap),
                       grid_y0 + gy * (cell_h + gap));

        style_card(cell, color_for_cell(i), 14);
        lv_obj_set_style_border_color(cell, lv_color_hex(0x42556E), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_pad_all(cell, 8, 0);

        lv_obj_t *name = make_label(cell,
                                    s_cell_name[i],
                                    lv_color_hex(0xFFFFFF),
                                    16);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *value = make_label(cell,
                                     "",
                                     lv_color_hex(0xFFFFFF),
                                     22);

        if (s_stats.hit_count[i] > 0) {
            uint16_t avg = cell_avg_ms(i);
            lv_label_set_text_fmt(value,
                                  "%u ms\n%u hits",
                                  (unsigned int)avg,
                                  (unsigned int)s_stats.hit_count[i]);
        } else {
            lv_label_set_text(value, "--\n0 hits");
        }

        lv_obj_align(value, LV_ALIGN_CENTER, 0, 10);
    }

    lv_obj_t *suggest_card = lv_obj_create(root);
    lv_obj_set_size(suggest_card, 944, 82);
    lv_obj_align(suggest_card, LV_ALIGN_BOTTOM_MID, 0, -24);
    style_card(suggest_card, lv_color_hex(0x0F1A2A), 16);

    lv_obj_t *suggest = make_label(suggest_card,
                                   "",
                                   lv_color_hex(0xEAF2FF),
                                   18);

    if (s_stats.total_hit_count == 0) {
        lv_label_set_text(suggest,
                          "Next: no valid data. Keep current difficulty and run one complete round.");
    } else if (avg_ms <= 750 && score >= 18) {
        lv_label_set_text(suggest,
                          "Next: performance is good. Reduce target size or shorten hover time.");
    } else if (avg_ms >= 1200 || score < 10) {
        lv_label_set_text(suggest,
                          "Next: performance is weak. Increase target size or extend hover time.");
    } else if (s_stats.weakest_cell >= 0) {
        lv_label_set_text_fmt(suggest,
                              "Next: keep difficulty. Increase target probability in %s area.",
                              s_cell_name[s_stats.weakest_cell]);
    } else {
        lv_label_set_text(suggest,
                          "Next: keep difficulty and continue training.");
    }

    lv_obj_align(suggest, LV_ALIGN_CENTER, 0, 0);

    return root;
}