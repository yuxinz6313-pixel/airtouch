#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIR_ATTENTION_HEATMAP_COLS 16
#define AIR_ATTENTION_HEATMAP_ROWS 10
#define AIR_ATTENTION_HEATMAP_CELL_COUNT (AIR_ATTENTION_HEATMAP_COLS * AIR_ATTENTION_HEATMAP_ROWS)

typedef struct {
    uint32_t sample_count;
    uint32_t valid_sample_count;

    float total_energy;
    float max_value;
    int max_row;
    int max_col;

    float center_x;
    float center_y;

    float entropy_norm;
    float focus_score;
    float coverage_score;

    float mean_jitter_px;
    float recent_stability;
} air_attention_heatmap_stats_t;

void air_attention_heatmap_init(void);

void air_attention_heatmap_set_enabled(bool enabled);

void air_attention_heatmap_push_sample(
    bool valid,
    int screen_x,
    int screen_y,
    int confidence,
    int area,
    uint16_t distance_mm,
    bool distance_valid
);

bool air_attention_heatmap_get_stats(air_attention_heatmap_stats_t *out_stats);

float air_attention_heatmap_get_cell(int row, int col);

void air_attention_heatmap_clear(void);

#ifdef __cplusplus
}
#endif