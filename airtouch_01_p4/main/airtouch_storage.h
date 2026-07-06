#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t star_total_rounds;
    uint32_t star_best_hits;
    uint32_t star_avg_hits;
    uint32_t star_best_speed;

    uint32_t color_total_rounds;
    uint32_t color_best_accuracy;
    uint32_t color_avg_accuracy;
    uint32_t color_best_inhibition;

    uint32_t last_update_boot_ms;
} airtouch_summary_t;
typedef struct {
    uint32_t record_id;
    uint32_t boot_ms;

    uint32_t hits;
    uint32_t avg_ms;
    uint32_t fastest_ms;
    uint32_t hit_score;
    uint32_t speed_score;

    uint32_t difficulty;
    uint32_t dwell_ms;
    uint32_t target_radius;
    uint32_t round_duration_s;
    uint32_t adaptive_level;
} airtouch_star_record_t;

typedef struct {
    uint32_t record_id;
    uint32_t boot_ms;

    uint32_t correct;
    uint32_t wrong;
    uint32_t false_alarm;
    uint32_t miss;

    uint32_t accuracy;
    uint32_t avg_ms;
    uint32_t fastest_ms;
    uint32_t inhibition;
    uint32_t speed_score;

    uint32_t difficulty;
    uint32_t dwell_ms;
    uint32_t bubble_count;
    uint32_t nogo_ratio;
    uint32_t round_duration_s;
    uint32_t adaptive_level;
} airtouch_color_record_t;

bool airtouch_storage_init(void);
bool airtouch_storage_is_ready(void);
const char *airtouch_storage_mount_point(void);
const char *airtouch_storage_current_user(void);

bool airtouch_storage_append_star_record(const airtouch_star_record_t *rec);
bool airtouch_storage_append_color_record(const airtouch_color_record_t *rec);

uint32_t airtouch_storage_load_recent_star_records(airtouch_star_record_t *out_records,
                                                   uint32_t max_count);
uint32_t airtouch_storage_load_recent_color_records(airtouch_color_record_t *out_records,
                                                    uint32_t max_count);
bool airtouch_storage_rebuild_summary(void);
bool airtouch_storage_load_summary(airtouch_summary_t *out_summary);

bool airtouch_storage_cloud_start_replay_task(void);

#ifdef __cplusplus
}
#endif



