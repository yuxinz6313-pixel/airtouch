#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "lvgl.h"

#define AIR_HEATMAP_SCREEN_W 1024
#define AIR_HEATMAP_SCREEN_H 600
#define AIR_HEATMAP_GRID_N   9

typedef struct {
    uint16_t hit_count[AIR_HEATMAP_GRID_N];
    uint32_t rt_sum_ms[AIR_HEATMAP_GRID_N];
    uint16_t rt_best_ms[AIR_HEATMAP_GRID_N];
    uint16_t rt_worst_ms[AIR_HEATMAP_GRID_N];

    uint32_t total_hit_count;
    uint32_t total_rt_sum_ms;

    uint16_t best_rt_ms;
    uint16_t worst_rt_ms;

    int weakest_cell;
    uint16_t weakest_avg_ms;
} air_heatmap_stats_t;

void air_heatmap_reset(void);

void air_heatmap_record_hit(int16_t target_x,
                            int16_t target_y,
                            uint16_t reaction_ms);

const air_heatmap_stats_t *air_heatmap_get_stats(void);

uint16_t air_heatmap_get_avg_ms(void);

int air_heatmap_get_weakest_cell(void);

void air_heatmap_fill_demo_data(void);

lv_obj_t *air_heatmap_create_result_page(lv_obj_t *parent,
                                         int score,
                                         int difficulty_level,
                                         int target_radius,
                                         int dwell_ms);

#ifdef __cplusplus
}
#endif