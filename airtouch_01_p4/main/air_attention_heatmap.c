#include "air_attention_heatmap.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AIR_SCREEN_W 1024
#define AIR_SCREEN_H 600

#define HEATMAP_DECAY_PER_SAMPLE 0.985f
#define HEATMAP_BASE_GAIN        1.00f
#define HEATMAP_SIGMA_CELL       1.10f

#define HEATMAP_LOG_INTERVAL     40

static const char *TAG = "attention_heatmap";

static bool s_attention_heatmap_enabled = false;

static float s_map[AIR_ATTENTION_HEATMAP_ROWS][AIR_ATTENTION_HEATMAP_COLS];

static bool s_inited = false;
static bool s_has_last_pos = false;

static int s_last_x = -1;
static int s_last_y = -1;

static float s_recent_jitter = 0.0f;
static float s_recent_stability = 0.0f;

static uint32_t s_sample_count = 0;
static uint32_t s_valid_sample_count = 0;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static float attention_confidence_weight(int confidence)
{
    float w = (float)confidence / 100.0f;
    return clampf_local(w, 0.20f, 1.20f);
}

static float attention_area_weight(int area)
{
    if (area <= 0) {
        return 0.40f;
    }

    float w = sqrtf((float)area) / 12.0f;
    return clampf_local(w, 0.50f, 1.25f);
}

static float attention_distance_weight(uint16_t distance_mm, bool distance_valid)
{
    if (!distance_valid) {
        return 0.85f;
    }

    if (distance_mm < 120) {
        return 0.45f;
    }

    if (distance_mm < 200) {
        return 0.75f;
    }

    if (distance_mm <= 550) {
        return 1.10f;
    }

    if (distance_mm <= 900) {
        return 0.85f;
    }

    return 0.55f;
}

static float attention_stability_weight(int x, int y)
{
    if (!s_has_last_pos) {
        s_has_last_pos = true;
        s_last_x = x;
        s_last_y = y;
        s_recent_jitter = 0.0f;
        s_recent_stability = 0.5f;
        return 0.80f;
    }

    int dx = x - s_last_x;
    int dy = y - s_last_y;

    float dist = sqrtf((float)(dx * dx + dy * dy));

    s_recent_jitter = 0.85f * s_recent_jitter + 0.15f * dist;

    float stability = 1.0f - clampf_local(s_recent_jitter / 120.0f, 0.0f, 1.0f);
    s_recent_stability = stability;

    s_last_x = x;
    s_last_y = y;

    return clampf_local(0.55f + 0.75f * stability, 0.55f, 1.30f);
}

static void heatmap_apply_decay(void)
{
    for (int r = 0; r < AIR_ATTENTION_HEATMAP_ROWS; r++) {
        for (int c = 0; c < AIR_ATTENTION_HEATMAP_COLS; c++) {
            s_map[r][c] *= HEATMAP_DECAY_PER_SAMPLE;

            if (s_map[r][c] < 0.0001f) {
                s_map[r][c] = 0.0f;
            }
        }
    }
}

static void heatmap_deposit_gaussian(float fx_cell, float fy_cell, float weight)
{
    int center_col = (int)(fx_cell + 0.5f);
    int center_row = (int)(fy_cell + 0.5f);

    for (int dr = -2; dr <= 2; dr++) {
        for (int dc = -2; dc <= 2; dc++) {
            int r = center_row + dr;
            int c = center_col + dc;

            if (r < 0 || r >= AIR_ATTENTION_HEATMAP_ROWS || c < 0 || c >= AIR_ATTENTION_HEATMAP_COLS) {
                continue;
            }

            float dx = fx_cell - (float)c;
            float dy = fy_cell - (float)r;
            float d2 = dx * dx + dy * dy;

            float g = expf(-d2 / (2.0f * HEATMAP_SIGMA_CELL * HEATMAP_SIGMA_CELL));

            s_map[r][c] += weight * g;
        }
    }
}

static void heatmap_compute_stats_locked(air_attention_heatmap_stats_t *out)
{
    memset(out, 0, sizeof(*out));

    out->sample_count = s_sample_count;
    out->valid_sample_count = s_valid_sample_count;
    out->max_row = -1;
    out->max_col = -1;

    float total = 0.0f;
    float max_v = 0.0f;

    for (int r = 0; r < AIR_ATTENTION_HEATMAP_ROWS; r++) {
        for (int c = 0; c < AIR_ATTENTION_HEATMAP_COLS; c++) {
            float v = s_map[r][c];
            total += v;

            if (v > max_v) {
                max_v = v;
                out->max_row = r;
                out->max_col = c;
            }
        }
    }

    out->total_energy = total;
    out->max_value = max_v;
    out->mean_jitter_px = s_recent_jitter;
    out->recent_stability = s_recent_stability;

    if (total <= 0.0001f) {
        out->center_x = -1.0f;
        out->center_y = -1.0f;
        out->entropy_norm = 1.0f;
        out->focus_score = 0.0f;
        out->coverage_score = 0.0f;
        return;
    }

    float cx = 0.0f;
    float cy = 0.0f;
    float entropy = 0.0f;
    int active_cells = 0;

    for (int r = 0; r < AIR_ATTENTION_HEATMAP_ROWS; r++) {
        for (int c = 0; c < AIR_ATTENTION_HEATMAP_COLS; c++) {
            float v = s_map[r][c];
            float p = v / total;

            if (p > 0.00001f) {
                entropy += -p * logf(p);
            }

            if (v > max_v * 0.25f && v > 0.05f) {
                active_cells++;
            }

            float cell_center_x = ((float)c + 0.5f) * ((float)AIR_SCREEN_W / AIR_ATTENTION_HEATMAP_COLS);
            float cell_center_y = ((float)r + 0.5f) * ((float)AIR_SCREEN_H / AIR_ATTENTION_HEATMAP_ROWS);

            cx += p * cell_center_x;
            cy += p * cell_center_y;
        }
    }

    float max_entropy = logf((float)AIR_ATTENTION_HEATMAP_CELL_COUNT);
    float entropy_norm = entropy / max_entropy;

    out->center_x = cx;
    out->center_y = cy;
    out->entropy_norm = clampf_local(entropy_norm, 0.0f, 1.0f);
    out->focus_score = clampf_local(1.0f - out->entropy_norm, 0.0f, 1.0f);
    out->coverage_score = clampf_local((float)active_cells / (float)AIR_ATTENTION_HEATMAP_CELL_COUNT, 0.0f, 1.0f);
}

void air_attention_heatmap_init(void)
{
    portENTER_CRITICAL(&s_lock);

    memset(s_map, 0, sizeof(s_map));
    s_inited = true;
    s_has_last_pos = false;
    s_last_x = -1;
    s_last_y = -1;
    s_recent_jitter = 0.0f;
    s_recent_stability = 0.0f;
    s_sample_count = 0;
    s_valid_sample_count = 0;

    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG,
             "Attention heatmap init: grid=%dx%d, screen=%dx%d",
             AIR_ATTENTION_HEATMAP_COLS,
             AIR_ATTENTION_HEATMAP_ROWS,
             AIR_SCREEN_W,
             AIR_SCREEN_H);
}


void air_attention_heatmap_set_enabled(bool enabled)
{
    s_attention_heatmap_enabled = enabled;
    ESP_LOGI(TAG,
             "Attention heatmap %s",
             enabled ? "enabled" : "disabled");
}

void air_attention_heatmap_clear(void)
{
    air_attention_heatmap_init();
}

void air_attention_heatmap_push_sample(
    bool valid,
    int screen_x,
    int screen_y,
    int confidence,
    int area,
    uint16_t distance_mm,
    bool distance_valid
)
{
    if (!s_attention_heatmap_enabled) {
        return;
    }


    if (!s_inited) {
        air_attention_heatmap_init();
    }

    bool need_log = false;
    air_attention_heatmap_stats_t log_stats = {0};

    portENTER_CRITICAL(&s_lock);

    s_sample_count++;

    heatmap_apply_decay();

    if (!valid || screen_x < 0 || screen_y < 0 || screen_x >= AIR_SCREEN_W || screen_y >= AIR_SCREEN_H) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    s_valid_sample_count++;

    float fx_cell = ((float)screen_x / (float)AIR_SCREEN_W) * (float)AIR_ATTENTION_HEATMAP_COLS;
    float fy_cell = ((float)screen_y / (float)AIR_SCREEN_H) * (float)AIR_ATTENTION_HEATMAP_ROWS;

    fx_cell = clampf_local(fx_cell, 0.0f, (float)(AIR_ATTENTION_HEATMAP_COLS - 1));
    fy_cell = clampf_local(fy_cell, 0.0f, (float)(AIR_ATTENTION_HEATMAP_ROWS - 1));

    float w_conf = attention_confidence_weight(confidence);
    float w_area = attention_area_weight(area);
    float w_dist = attention_distance_weight(distance_mm, distance_valid);
    float w_stable = attention_stability_weight(screen_x, screen_y);

    float weight = HEATMAP_BASE_GAIN * w_conf * w_area * w_dist * w_stable;

    heatmap_deposit_gaussian(fx_cell, fy_cell, weight);

    if ((s_valid_sample_count % HEATMAP_LOG_INTERVAL) == 0) {
        heatmap_compute_stats_locked(&log_stats);
        need_log = true;
    }

    portEXIT_CRITICAL(&s_lock);

    if (need_log) {
        ESP_LOGI(TAG,
                 "stats: samples=%lu valid=%lu top=(r%d,c%d) center=(%.1f,%.1f) focus=%.3f entropy=%.3f coverage=%.3f jitter=%.1f stability=%.3f",
                 (unsigned long)log_stats.sample_count,
                 (unsigned long)log_stats.valid_sample_count,
                 log_stats.max_row,
                 log_stats.max_col,
                 log_stats.center_x,
                 log_stats.center_y,
                 log_stats.focus_score,
                 log_stats.entropy_norm,
                 log_stats.coverage_score,
                 log_stats.mean_jitter_px,
                 log_stats.recent_stability);
    }
}

bool air_attention_heatmap_get_stats(air_attention_heatmap_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return false;
    }

    if (!s_inited) {
        air_attention_heatmap_init();
    }

    portENTER_CRITICAL(&s_lock);
    heatmap_compute_stats_locked(out_stats);
    portEXIT_CRITICAL(&s_lock);

    return true;
}

float air_attention_heatmap_get_cell(int row, int col)
{
    if (row < 0 || row >= AIR_ATTENTION_HEATMAP_ROWS || col < 0 || col >= AIR_ATTENTION_HEATMAP_COLS) {
        return 0.0f;
    }

    if (!s_inited) {
        air_attention_heatmap_init();
    }

    portENTER_CRITICAL(&s_lock);
    float v = s_map[row][col];
    portEXIT_CRITICAL(&s_lock);

    return v;
}