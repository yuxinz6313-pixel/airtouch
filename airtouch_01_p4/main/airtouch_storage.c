#include "airtouch_storage.h"
#include "airtouch_cloud_uart.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "bsp/wt99p4c5_s1_board.h"

#ifndef BSP_SD_MOUNT_POINT
#define BSP_SD_MOUNT_POINT "/sdcard"
#endif

#define AIRTOUCH_STORAGE_ROOT_DIR      BSP_SD_MOUNT_POINT "/airtouch"
#define AIRTOUCH_STORAGE_USERS_DIR     BSP_SD_MOUNT_POINT "/airtouch/users"
#define AIRTOUCH_STORAGE_DEFAULT_USER  "child_001"
#define AIRTOUCH_STORAGE_USER_DIR      BSP_SD_MOUNT_POINT "/airtouch/users/child_001"

#define AIRTOUCH_STORAGE_STAR_CSV      AIRTOUCH_STORAGE_USER_DIR "/STAR.CSV"
#define AIRTOUCH_STORAGE_COLOR_CSV     AIRTOUCH_STORAGE_USER_DIR "/COLOR.CSV"
#define AIRTOUCH_STORAGE_SUM_TXT       AIRTOUCH_STORAGE_USER_DIR "/SUM.TXT"
#define AIRTOUCH_STORAGE_QUEUE_JSL     AIRTOUCH_STORAGE_USER_DIR "/QUEUE.JSL"
#define AIRTOUCH_STORAGE_FW_TAG        "v0.4d_sd_v5a"

/* --------------------------------------------------------------------------
 * Cloud-SD v1g.3 file system check
 *
 * Purpose:
 *   Formalize the SD-side data file layout.
 *   This version only manages local files and status snapshots.
 *   It does not change game logic, UI logic, or cloud upload behavior.
 * -------------------------------------------------------------------------- */

#ifndef AIRTOUCH_STORAGE_SYNC_TXT
#define AIRTOUCH_STORAGE_SYNC_TXT AIRTOUCH_STORAGE_USER_DIR "/SYNC.TXT"
#endif
#ifndef AIRTOUCH_STORAGE_CONFIG_TXT
#define AIRTOUCH_STORAGE_CONFIG_TXT AIRTOUCH_STORAGE_USER_DIR "/CONFIG.TXT"
#endif

#ifndef AIRTOUCH_STORAGE_STATUS_TXT
#define AIRTOUCH_STORAGE_STATUS_TXT AIRTOUCH_STORAGE_USER_DIR "/STATUS.TXT"
#endif

#define AIRTOUCH_FILESYS_TAG "airtouch_filesys"
#define AIRTOUCH_FILESYS_SOURCE_V1G3 "p4_cloud_sd_v1g3"

static bool airtouch_file_exists_v1g3(const char *path)
{
    if (!path) {
        return false;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }

    fclose(fp);
    return true;
}

static bool airtouch_ensure_text_file_v1g3(const char *path, const char *default_content)
{
    if (!path) {
        return false;
    }

    FILE *fp = fopen(path, "r");
    if (fp) {
        fclose(fp);
        ESP_LOGI(AIRTOUCH_FILESYS_TAG, "File exists: %s", path);
        return true;
    }

    fp = fopen(path, "w");
    if (!fp) {
        ESP_LOGW(AIRTOUCH_FILESYS_TAG, "Create file failed: %s", path);
        return false;
    }

    if (default_content && default_content[0] != '\0') {
        fputs(default_content, fp);
    }

    fclose(fp);

    ESP_LOGI(AIRTOUCH_FILESYS_TAG, "File created: %s", path);
    return true;
}

static const char *airtouch_default_star_csv_header_v1g3(void)
{
    return "rid,boot_ms,hits,avg_ms,fastest_ms,hit_score,speed_score,difficulty,dwell_ms,target_r,duration_s,level,fw\n";
}

static const char *airtouch_default_color_csv_header_v1g3(void)
{
    return "rid,boot_ms,correct,wrong,false_alarm,miss,accuracy,avg_ms,fastest_ms,inhibit,speed_score,difficulty,dwell_ms,bubble_count,nogo_ratio,duration_s,level,fw\n";
}

static const char *airtouch_default_config_txt_v1g3(void)
{
    return
        "config_file_version=1\n"
        "config_version=0\n"
        "cloud_config_enabled=0\n"
        "adaptive_enabled=1\n"
        "\n"
        "star_target_radius=56\n"
        "star_dwell_ms=336\n"
        "star_duration_s=45\n"
        "star_difficulty=50\n"
        "\n"
        "color_target_radius=54\n"
        "color_dwell_ms=520\n"
        "color_bubble_count=4\n"
        "color_nogo_ratio=25\n"
        "color_duration_s=45\n"
        "color_difficulty=1\n"
        "\n"
        "global_difficulty_floor=1\n"
        "global_difficulty_ceiling=5\n";
}

static bool airtouch_filesys_update_status_v1g3(void)
{
    FILE *fp = fopen(AIRTOUCH_STORAGE_STATUS_TXT, "w");
    if (!fp) {
        ESP_LOGW(AIRTOUCH_FILESYS_TAG,
                 "STATUS.TXT update failed: %s",
                 AIRTOUCH_STORAGE_STATUS_TXT);
        return false;
    }

    const uint32_t boot_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    fprintf(fp, "status_file_version=1\n");
    fprintf(fp, "source=%s\n", AIRTOUCH_FILESYS_SOURCE_V1G3);
    fprintf(fp, "boot_ms=%lu\n", (unsigned long)boot_ms);
    fprintf(fp, "user_id=%s\n", AIRTOUCH_STORAGE_DEFAULT_USER);
    fprintf(fp, "storage_ready=1\n");

    fprintf(fp, "star_csv=%u\n", airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_STAR_CSV) ? 1 : 0);
    fprintf(fp, "color_csv=%u\n", airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_COLOR_CSV) ? 1 : 0);
    fprintf(fp, "sum_txt=%u\n", airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_SUM_TXT) ? 1 : 0);
    fprintf(fp, "queue_jsl=%u\n", airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_QUEUE_JSL) ? 1 : 0);
    fprintf(fp, "sync_txt=%u\n", airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_SYNC_TXT) ? 1 : 0);
    fprintf(fp, "config_txt=%u\n", airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_CONFIG_TXT) ? 1 : 0);

    fprintf(fp, "fw=%s\n", AIRTOUCH_STORAGE_FW_TAG);

    fclose(fp);

    ESP_LOGI(AIRTOUCH_FILESYS_TAG,
             "STATUS.TXT updated: %s",
             AIRTOUCH_STORAGE_STATUS_TXT);

    return true;
}

static void airtouch_filesys_check_v1g3(void)
{
    ESP_LOGI(AIRTOUCH_FILESYS_TAG, "Cloud-SD v1g.3 file system check begin");

    airtouch_ensure_text_file_v1g3(AIRTOUCH_STORAGE_STAR_CSV,
                                   airtouch_default_star_csv_header_v1g3());

    airtouch_ensure_text_file_v1g3(AIRTOUCH_STORAGE_COLOR_CSV,
                                   airtouch_default_color_csv_header_v1g3());

    airtouch_ensure_text_file_v1g3(AIRTOUCH_STORAGE_QUEUE_JSL,
                                   "");

    airtouch_ensure_text_file_v1g3(AIRTOUCH_STORAGE_CONFIG_TXT,
                                   airtouch_default_config_txt_v1g3());

    if (airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_SUM_TXT)) {
        ESP_LOGI(AIRTOUCH_FILESYS_TAG, "File exists: %s", AIRTOUCH_STORAGE_SUM_TXT);
    } else {
        ESP_LOGW(AIRTOUCH_FILESYS_TAG, "SUM.TXT not found yet: %s", AIRTOUCH_STORAGE_SUM_TXT);
    }

    if (airtouch_file_exists_v1g3(AIRTOUCH_STORAGE_SYNC_TXT)) {
        ESP_LOGI(AIRTOUCH_FILESYS_TAG, "File exists: %s", AIRTOUCH_STORAGE_SYNC_TXT);
    } else {
        ESP_LOGW(AIRTOUCH_FILESYS_TAG, "SYNC.TXT not found yet: %s", AIRTOUCH_STORAGE_SYNC_TXT);
    }

    airtouch_filesys_update_status_v1g3();

    ESP_LOGI(AIRTOUCH_FILESYS_TAG, "Cloud-SD v1g.3 file system check done");
}


static const char *TAG = "airtouch_storage";
static bool s_storage_ready = false;
static uint32_t s_star_record_counter = 0;
static uint32_t s_color_record_counter = 0;

static bool make_dir_if_needed(const char *path)
{
    struct stat st = {0};

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "Directory exists: %s", path);
            return true;
        }

        ESP_LOGE(TAG, "Path exists but is not directory: %s", path);
        return false;
    }

    if (mkdir(path, 0775) == 0) {
        ESP_LOGI(TAG, "Directory created: %s", path);
        return true;
    }

    if (errno == EEXIST) {
        ESP_LOGI(TAG, "Directory already exists: %s", path);
        return true;
    }

    ESP_LOGE(TAG, "mkdir failed: %s errno=%d", path, errno);
    return false;
}

static bool file_needs_header(const char *path)
{
    struct stat st = {0};

    if (stat(path, &st) != 0) {
        return true;
    }

    return st.st_size == 0;
}

static uint32_t load_last_record_id_from_csv(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGI(TAG, "No existing CSV yet: %s", path);
        return 0;
    }

    char line[256];
    uint32_t last_rid = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') {
            continue;
        }

        if (line[0] < '0' || line[0] > '9') {
            continue;
        }

        char *endptr = NULL;
        unsigned long rid = strtoul(line, &endptr, 10);
        if (endptr != line && rid > last_rid) {
            last_rid = (uint32_t)rid;
        }
    }

    fclose(fp);

    ESP_LOGI(TAG, "CSV counter loaded: %s last_rid=%lu",
             path,
             (unsigned long)last_rid);

    return last_rid;
}

static bool write_boot_test_file(void)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/boot_test.txt", AIRTOUCH_STORAGE_USER_DIR);

    FILE *fp = fopen(path, "a");
    if (!fp) {
        ESP_LOGE(TAG, "Open boot_test failed: %s errno=%d", path, errno);
        return false;
    }

    fprintf(fp, "AirTouch SD boot test OK. user=%s\n", AIRTOUCH_STORAGE_DEFAULT_USER);
    fflush(fp);
    fclose(fp);

    ESP_LOGI(TAG, "Boot test written: %s", path);
    return true;
}

static bool ensure_storage_ready_for_write(void)
{
    if (!s_storage_ready) {
        ESP_LOGW(TAG, "Storage not ready, skip SD append");
        return false;
    }

    return true;
}

bool airtouch_storage_init(void)
{
    if (s_storage_ready) {
        ESP_LOGI(TAG, "Storage already ready");
        return true;
    }

    ESP_LOGI(TAG, "Initializing SD storage...");
    ESP_LOGI(TAG, "Mount point: %s", BSP_SD_MOUNT_POINT);

    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_mount failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "SD storage disabled, system will use RAM-only records");
        s_storage_ready = false;
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted");

    if (!make_dir_if_needed(AIRTOUCH_STORAGE_ROOT_DIR)) {
        s_storage_ready = false;
        return false;
    }

    if (!make_dir_if_needed(AIRTOUCH_STORAGE_USERS_DIR)) {
        s_storage_ready = false;
        return false;
    }

    if (!make_dir_if_needed(AIRTOUCH_STORAGE_USER_DIR)) {
        s_storage_ready = false;
        return false;
    }

    if (!write_boot_test_file()) {
        s_storage_ready = false;
        return false;
    }

    s_star_record_counter = load_last_record_id_from_csv(AIRTOUCH_STORAGE_STAR_CSV);
    s_color_record_counter = load_last_record_id_from_csv(AIRTOUCH_STORAGE_COLOR_CSV);

    ESP_LOGI(TAG,
             "Record counters restored: star=%lu color=%lu",
             (unsigned long)s_star_record_counter,
             (unsigned long)s_color_record_counter);

    s_storage_ready = true;
    airtouch_storage_rebuild_summary();
    airtouch_filesys_check_v1g3();

    ESP_LOGI(TAG, "AirTouch SD storage ready");
    return true;
}

bool airtouch_storage_is_ready(void)
{
    return s_storage_ready;
}

const char *airtouch_storage_mount_point(void)
{
    return BSP_SD_MOUNT_POINT;
}

const char *airtouch_storage_current_user(void)
{
    return AIRTOUCH_STORAGE_DEFAULT_USER;
}

static bool parse_star_csv_line(const char *line, airtouch_star_record_t *rec)
{
    if (line == NULL || rec == NULL) {
        return false;
    }

    unsigned long rid = 0;
    unsigned long boot_ms = 0;
    unsigned long hits = 0;
    unsigned long avg_ms = 0;
    unsigned long fastest_ms = 0;
    unsigned long hit_score = 0;
    unsigned long speed_score = 0;
    unsigned long difficulty = 0;
    unsigned long dwell_ms = 0;
    unsigned long target_radius = 0;
    unsigned long round_duration_s = 0;
    unsigned long adaptive_level = 0;

    int n = sscanf(line,
                   "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                   &rid,
                   &boot_ms,
                   &hits,
                   &avg_ms,
                   &fastest_ms,
                   &hit_score,
                   &speed_score,
                   &difficulty,
                   &dwell_ms,
                   &target_radius,
                   &round_duration_s,
                   &adaptive_level);

    if (n != 12) {
        return false;
    }

    rec->record_id = (uint32_t)rid;
    rec->boot_ms = (uint32_t)boot_ms;
    rec->hits = (uint32_t)hits;
    rec->avg_ms = (uint32_t)avg_ms;
    rec->fastest_ms = (uint32_t)fastest_ms;
    rec->hit_score = (uint32_t)hit_score;
    rec->speed_score = (uint32_t)speed_score;
    rec->difficulty = (uint32_t)difficulty;
    rec->dwell_ms = (uint32_t)dwell_ms;
    rec->target_radius = (uint32_t)target_radius;
    rec->round_duration_s = (uint32_t)round_duration_s;
    rec->adaptive_level = (uint32_t)adaptive_level;

    return true;
}

static bool parse_color_csv_line(const char *line, airtouch_color_record_t *rec)
{
    if (line == NULL || rec == NULL) {
        return false;
    }

    unsigned long rid = 0;
    unsigned long boot_ms = 0;
    unsigned long correct = 0;
    unsigned long wrong = 0;
    unsigned long false_alarm = 0;
    unsigned long miss = 0;
    unsigned long accuracy = 0;
    unsigned long avg_ms = 0;
    unsigned long fastest_ms = 0;
    unsigned long inhibition = 0;
    unsigned long speed_score = 0;
    unsigned long difficulty = 0;
    unsigned long dwell_ms = 0;
    unsigned long bubble_count = 0;
    unsigned long nogo_ratio = 0;
    unsigned long round_duration_s = 0;
    unsigned long adaptive_level = 0;

    int n = sscanf(line,
                   "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                   &rid,
                   &boot_ms,
                   &correct,
                   &wrong,
                   &false_alarm,
                   &miss,
                   &accuracy,
                   &avg_ms,
                   &fastest_ms,
                   &inhibition,
                   &speed_score,
                   &difficulty,
                   &dwell_ms,
                   &bubble_count,
                   &nogo_ratio,
                   &round_duration_s,
                   &adaptive_level);

    if (n != 17) {
        return false;
    }

    rec->record_id = (uint32_t)rid;
    rec->boot_ms = (uint32_t)boot_ms;
    rec->correct = (uint32_t)correct;
    rec->wrong = (uint32_t)wrong;
    rec->false_alarm = (uint32_t)false_alarm;
    rec->miss = (uint32_t)miss;
    rec->accuracy = (uint32_t)accuracy;
    rec->avg_ms = (uint32_t)avg_ms;
    rec->fastest_ms = (uint32_t)fastest_ms;
    rec->inhibition = (uint32_t)inhibition;
    rec->speed_score = (uint32_t)speed_score;
    rec->difficulty = (uint32_t)difficulty;
    rec->dwell_ms = (uint32_t)dwell_ms;
    rec->bubble_count = (uint32_t)bubble_count;
    rec->nogo_ratio = (uint32_t)nogo_ratio;
    rec->round_duration_s = (uint32_t)round_duration_s;
    rec->adaptive_level = (uint32_t)adaptive_level;

    return true;
}

uint32_t airtouch_storage_load_recent_star_records(airtouch_star_record_t *out_records,
                                                   uint32_t max_count)
{
    if (!s_storage_ready || out_records == NULL || max_count == 0) {
        return 0;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_STAR_CSV, "r");
    if (!fp) {
        ESP_LOGW(TAG, "Open STAR.CSV for read failed: %s errno=%d",
                 AIRTOUCH_STORAGE_STAR_CSV,
                 errno);
        return 0;
    }

    char line[256];
    uint32_t count = 0;
    uint32_t total_valid = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        airtouch_star_record_t rec;
        if (!parse_star_csv_line(line, &rec)) {
            continue;
        }

        total_valid++;

        if (count < max_count) {
            out_records[count++] = rec;
        } else {
            for (uint32_t i = 1; i < max_count; i++) {
                out_records[i - 1] = out_records[i];
            }
            out_records[max_count - 1] = rec;
        }
    }

    fclose(fp);

    ESP_LOGI(TAG,
             "STAR.CSV recent loaded: count=%lu total_valid=%lu",
             (unsigned long)count,
             (unsigned long)total_valid);

    return count;
}

uint32_t airtouch_storage_load_recent_color_records(airtouch_color_record_t *out_records,
                                                    uint32_t max_count)
{
    if (!s_storage_ready || out_records == NULL || max_count == 0) {
        return 0;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_COLOR_CSV, "r");
    if (!fp) {
        ESP_LOGW(TAG, "Open COLOR.CSV for read failed: %s errno=%d",
                 AIRTOUCH_STORAGE_COLOR_CSV,
                 errno);
        return 0;
    }

    char line[320];
    uint32_t count = 0;
    uint32_t total_valid = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        airtouch_color_record_t rec;
        if (!parse_color_csv_line(line, &rec)) {
            continue;
        }

        total_valid++;

        if (count < max_count) {
            out_records[count++] = rec;
        } else {
            for (uint32_t i = 1; i < max_count; i++) {
                out_records[i - 1] = out_records[i];
            }
            out_records[max_count - 1] = rec;
        }
    }

    fclose(fp);

    ESP_LOGI(TAG,
             "COLOR.CSV recent loaded: count=%lu total_valid=%lu",
             (unsigned long)count,
             (unsigned long)total_valid);

    return count;
}

static bool compute_summary_from_csv(airtouch_summary_t *summary)
{
    if (summary == NULL) {
        return false;
    }

    memset(summary, 0, sizeof(*summary));
    summary->last_update_boot_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    FILE *star_fp = fopen(AIRTOUCH_STORAGE_STAR_CSV, "r");
    if (star_fp) {
        char line[256];
        uint32_t star_hits_sum = 0;

        while (fgets(line, sizeof(line), star_fp) != NULL) {
            airtouch_star_record_t rec;
            if (!parse_star_csv_line(line, &rec)) {
                continue;
            }

            summary->star_total_rounds++;
            star_hits_sum += rec.hits;

            if (rec.hits > summary->star_best_hits) {
                summary->star_best_hits = rec.hits;
            }

            if (rec.speed_score > summary->star_best_speed) {
                summary->star_best_speed = rec.speed_score;
            }
        }

        fclose(star_fp);

        if (summary->star_total_rounds > 0) {
            summary->star_avg_hits = star_hits_sum / summary->star_total_rounds;
        }
    }

    FILE *color_fp = fopen(AIRTOUCH_STORAGE_COLOR_CSV, "r");
    if (color_fp) {
        char line[320];
        uint32_t color_acc_sum = 0;

        while (fgets(line, sizeof(line), color_fp) != NULL) {
            airtouch_color_record_t rec;
            if (!parse_color_csv_line(line, &rec)) {
                continue;
            }

            summary->color_total_rounds++;
            color_acc_sum += rec.accuracy;

            if (rec.accuracy > summary->color_best_accuracy) {
                summary->color_best_accuracy = rec.accuracy;
            }

            if (rec.inhibition > summary->color_best_inhibition) {
                summary->color_best_inhibition = rec.inhibition;
            }
        }

        fclose(color_fp);

        if (summary->color_total_rounds > 0) {
            summary->color_avg_accuracy = color_acc_sum / summary->color_total_rounds;
        }
    }

    return true;
}

static bool write_summary_file(const airtouch_summary_t *summary)
{
    if (summary == NULL) {
        return false;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_SUM_TXT, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Open SUM.TXT for write failed: %s errno=%d",
                 AIRTOUCH_STORAGE_SUM_TXT,
                 errno);
        return false;
    }

    fprintf(fp, "fw=%s\n", AIRTOUCH_STORAGE_FW_TAG);
    fprintf(fp, "star_total_rounds=%lu\n", (unsigned long)summary->star_total_rounds);
    fprintf(fp, "star_best_hits=%lu\n", (unsigned long)summary->star_best_hits);
    fprintf(fp, "star_avg_hits=%lu\n", (unsigned long)summary->star_avg_hits);
    fprintf(fp, "star_best_speed=%lu\n", (unsigned long)summary->star_best_speed);
    fprintf(fp, "color_total_rounds=%lu\n", (unsigned long)summary->color_total_rounds);
    fprintf(fp, "color_best_accuracy=%lu\n", (unsigned long)summary->color_best_accuracy);
    fprintf(fp, "color_avg_accuracy=%lu\n", (unsigned long)summary->color_avg_accuracy);
    fprintf(fp, "color_best_inhibition=%lu\n", (unsigned long)summary->color_best_inhibition);
    fprintf(fp, "last_update_boot_ms=%lu\n", (unsigned long)summary->last_update_boot_ms);

    fclose(fp);

    ESP_LOGI(TAG,
             "SUM.TXT updated: star_rounds=%lu star_best=%lu color_rounds=%lu color_best_acc=%lu",
             (unsigned long)summary->star_total_rounds,
             (unsigned long)summary->star_best_hits,
             (unsigned long)summary->color_total_rounds,
             (unsigned long)summary->color_best_accuracy);

    return true;
}

bool airtouch_storage_rebuild_summary(void)
{
    if (!s_storage_ready) {
        ESP_LOGW(TAG, "SUM.TXT rebuild skipped: storage not ready");
        return false;
    }

    airtouch_summary_t summary;
    if (!compute_summary_from_csv(&summary)) {
        return false;
    }

    return write_summary_file(&summary);
}

bool airtouch_storage_load_summary(airtouch_summary_t *out_summary)
{
    if (!s_storage_ready || out_summary == NULL) {
        return false;
    }

    return compute_summary_from_csv(out_summary);
}
static bool load_last_star_record_from_csv(airtouch_star_record_t *out_record)
{
    if (out_record == NULL) {
        return false;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_STAR_CSV, "r");
    if (!fp) {
        return false;
    }

    char line[256];
    bool found = false;

    while (fgets(line, sizeof(line), fp) != NULL) {
        airtouch_star_record_t rec;
        if (parse_star_csv_line(line, &rec)) {
            *out_record = rec;
            found = true;
        }
    }

    fclose(fp);
    return found;
}

static bool load_last_color_record_from_csv(airtouch_color_record_t *out_record)
{
    if (out_record == NULL) {
        return false;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_COLOR_CSV, "r");
    if (!fp) {
        return false;
    }

    char line[320];
    bool found = false;

    while (fgets(line, sizeof(line), fp) != NULL) {
        airtouch_color_record_t rec;
        if (parse_color_csv_line(line, &rec)) {
            *out_record = rec;
            found = true;
        }
    }

    fclose(fp);
    return found;
}

static bool append_star_record_to_queue(const airtouch_star_record_t *rec)
{
    if (rec == NULL) {
        return false;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_QUEUE_JSL, "a");
    if (!fp) {
        ESP_LOGE(TAG, "Open QUEUE.JSL for star append failed: %s errno=%d",
                 AIRTOUCH_STORAGE_QUEUE_JSL,
                 errno);
        return false;
    }

    fprintf(fp,
            "{\"synced\":0,\"type\":\"star\",\"rid\":%lu,\"boot_ms\":%lu,\"hits\":%lu,\"avg_ms\":%lu,\"fastest_ms\":%lu,\"hit_score\":%lu,\"speed_score\":%lu,\"difficulty\":%lu,\"dwell_ms\":%lu,\"target_r\":%lu,\"duration_s\":%lu,\"level\":%lu,\"fw\":\"%s\"}\n",
            (unsigned long)rec->record_id,
            (unsigned long)rec->boot_ms,
            (unsigned long)rec->hits,
            (unsigned long)rec->avg_ms,
            (unsigned long)rec->fastest_ms,
            (unsigned long)rec->hit_score,
            (unsigned long)rec->speed_score,
            (unsigned long)rec->difficulty,
            (unsigned long)rec->dwell_ms,
            (unsigned long)rec->target_radius,
            (unsigned long)rec->round_duration_s,
            (unsigned long)rec->adaptive_level,
            AIRTOUCH_STORAGE_FW_TAG);

    fclose(fp);

    ESP_LOGI(TAG,
             "QUEUE.JSL appended: type=star rid=%lu",
             (unsigned long)rec->record_id);

    return true;
}

static bool append_color_record_to_queue(const airtouch_color_record_t *rec)
{
    if (rec == NULL) {
        return false;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_QUEUE_JSL, "a");
    if (!fp) {
        ESP_LOGE(TAG, "Open QUEUE.JSL for color append failed: %s errno=%d",
                 AIRTOUCH_STORAGE_QUEUE_JSL,
                 errno);
        return false;
    }

    fprintf(fp,
            "{\"synced\":0,\"type\":\"color\",\"rid\":%lu,\"boot_ms\":%lu,\"correct\":%lu,\"wrong\":%lu,\"false_alarm\":%lu,\"miss\":%lu,\"accuracy\":%lu,\"avg_ms\":%lu,\"fastest_ms\":%lu,\"inhibition\":%lu,\"speed_score\":%lu,\"difficulty\":%lu,\"dwell_ms\":%lu,\"bubble_count\":%lu,\"nogo_ratio\":%lu,\"duration_s\":%lu,\"level\":%lu,\"fw\":\"%s\"}\n",
            (unsigned long)rec->record_id,
            (unsigned long)rec->boot_ms,
            (unsigned long)rec->correct,
            (unsigned long)rec->wrong,
            (unsigned long)rec->false_alarm,
            (unsigned long)rec->miss,
            (unsigned long)rec->accuracy,
            (unsigned long)rec->avg_ms,
            (unsigned long)rec->fastest_ms,
            (unsigned long)rec->inhibition,
            (unsigned long)rec->speed_score,
            (unsigned long)rec->difficulty,
            (unsigned long)rec->dwell_ms,
            (unsigned long)rec->bubble_count,
            (unsigned long)rec->nogo_ratio,
            (unsigned long)rec->round_duration_s,
            (unsigned long)rec->adaptive_level,
            AIRTOUCH_STORAGE_FW_TAG);

    fclose(fp);

    ESP_LOGI(TAG,
             "QUEUE.JSL appended: type=color rid=%lu",
             (unsigned long)rec->record_id);

    return true;
}

static bool append_last_star_record_to_queue(void)
{
    airtouch_star_record_t rec;
    if (!load_last_star_record_from_csv(&rec)) {
        ESP_LOGW(TAG, "QUEUE.JSL star append skipped: no valid STAR.CSV record");
        return false;
    }

    return append_star_record_to_queue(&rec);
}

static bool append_last_color_record_to_queue(void)
{
    airtouch_color_record_t rec;
    if (!load_last_color_record_from_csv(&rec)) {
        ESP_LOGW(TAG, "QUEUE.JSL color append skipped: no valid COLOR.CSV record");
        return false;
    }

    return append_color_record_to_queue(&rec);
}

static void airtouch_storage_cloud_upload_last_star(void)
{
    if (!airtouch_cloud_uart_is_ready()) {
        ESP_LOGW(TAG, "Cloud-B v2c: UART not ready, skip STAR upload");
        return;
    }

    airtouch_star_record_t rec;
    if (!load_last_star_record_from_csv(&rec)) {
        ESP_LOGW(TAG, "Cloud-B v2c: no last STAR record for upload");
        return;
    }

    char json[768];
    snprintf(json, sizeof(json),
             "{\"device_id\":\"airtouch_001\","
             "\"user_id\":\"%s\","
             "\"source\":\"p4_cloud_b_v2c\","
             "\"type\":\"star\","
             "\"rid\":%lu,"
             "\"boot_ms\":%lu,"
             "\"hits\":%lu,"
             "\"avg_ms\":%lu,"
             "\"fastest_ms\":%lu,"
             "\"hit_score\":%lu,"
             "\"speed_score\":%lu,"
             "\"difficulty\":%lu,"
             "\"dwell_ms\":%lu,"
             "\"target_r\":%lu,"
             "\"duration_s\":%lu,"
             "\"level\":%lu,"
             "\"fw\":\"p4_cloud_b_v2c\"}",
             airtouch_storage_current_user(),
             (unsigned long)rec.record_id,
             (unsigned long)rec.boot_ms,
             (unsigned long)rec.hits,
             (unsigned long)rec.avg_ms,
             (unsigned long)rec.fastest_ms,
             (unsigned long)rec.hit_score,
             (unsigned long)rec.speed_score,
             (unsigned long)rec.difficulty,
             (unsigned long)rec.dwell_ms,
             (unsigned long)rec.target_radius,
             (unsigned long)rec.round_duration_s,
             (unsigned long)rec.adaptive_level);

    uint32_t seq = 100000UL + rec.record_id;
    bool ok = airtouch_cloud_uart_send_json(seq, json, 3500);

    ESP_LOGI(TAG, "Cloud-B v2c STAR upload: rid=%lu seq=%lu ok=%d",
             (unsigned long)rec.record_id,
             (unsigned long)seq,
             ok ? 1 : 0);
}

static void airtouch_storage_cloud_upload_last_color(void)
{
    if (!airtouch_cloud_uart_is_ready()) {
        ESP_LOGW(TAG, "Cloud-B v2c: UART not ready, skip COLOR upload");
        return;
    }

    airtouch_color_record_t rec;
    if (!load_last_color_record_from_csv(&rec)) {
        ESP_LOGW(TAG, "Cloud-B v2c: no last COLOR record for upload");
        return;
    }

    char json[896];
    snprintf(json, sizeof(json),
             "{\"device_id\":\"airtouch_001\","
             "\"user_id\":\"%s\","
             "\"source\":\"p4_cloud_b_v2c\","
             "\"type\":\"color_go\","
             "\"rid\":%lu,"
             "\"boot_ms\":%lu,"
             "\"correct\":%lu,"
             "\"wrong\":%lu,"
             "\"false_alarm\":%lu,"
             "\"miss\":%lu,"
             "\"accuracy\":%lu,"
             "\"avg_ms\":%lu,"
             "\"fastest_ms\":%lu,"
             "\"inhibition\":%lu,"
             "\"speed_score\":%lu,"
             "\"difficulty\":%lu,"
             "\"dwell_ms\":%lu,"
             "\"bubble_count\":%lu,"
             "\"nogo_ratio\":%lu,"
             "\"duration_s\":%lu,"
             "\"level\":%lu,"
             "\"fw\":\"p4_cloud_b_v2c\"}",
             airtouch_storage_current_user(),
             (unsigned long)rec.record_id,
             (unsigned long)rec.boot_ms,
             (unsigned long)rec.correct,
             (unsigned long)rec.wrong,
             (unsigned long)rec.false_alarm,
             (unsigned long)rec.miss,
             (unsigned long)rec.accuracy,
             (unsigned long)rec.avg_ms,
             (unsigned long)rec.fastest_ms,
             (unsigned long)rec.inhibition,
             (unsigned long)rec.speed_score,
             (unsigned long)rec.difficulty,
             (unsigned long)rec.dwell_ms,
             (unsigned long)rec.bubble_count,
             (unsigned long)rec.nogo_ratio,
             (unsigned long)rec.round_duration_s,
             (unsigned long)rec.adaptive_level);

    uint32_t seq = 200000UL + rec.record_id;
    bool ok = airtouch_cloud_uart_send_json(seq, json, 3500);

    ESP_LOGI(TAG, "Cloud-B v2c COLOR upload: rid=%lu seq=%lu ok=%d",
             (unsigned long)rec.record_id,
             (unsigned long)seq,
             ok ? 1 : 0);
}

bool airtouch_storage_append_star_record(const airtouch_star_record_t *rec)
{
    if (!rec) {
        ESP_LOGE(TAG, "append star failed: rec is NULL");
        return false;
    }

    if (!ensure_storage_ready_for_write()) {
        return false;
    }

    const bool need_header = file_needs_header(AIRTOUCH_STORAGE_STAR_CSV);

    FILE *fp = fopen(AIRTOUCH_STORAGE_STAR_CSV, "a");
    if (!fp) {
        ESP_LOGE(TAG, "Open STAR.CSV failed: %s errno=%d", AIRTOUCH_STORAGE_STAR_CSV, errno);
        return false;
    }

    if (need_header) {
        fprintf(fp, "rid,boot_ms,hits,avg_ms,fastest_ms,hit_score,speed_score,difficulty,dwell_ms,target_r,duration_s,level,fw\n");
    }

    uint32_t rid = rec->record_id;
    if (rid == 0) {
        rid = ++s_star_record_counter;
    }

    fprintf(
        fp,
        "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s\n",
        (unsigned long)rid,
        (unsigned long)rec->boot_ms,
        (unsigned long)rec->hits,
        (unsigned long)rec->avg_ms,
        (unsigned long)rec->fastest_ms,
        (unsigned long)rec->hit_score,
        (unsigned long)rec->speed_score,
        (unsigned long)rec->difficulty,
        (unsigned long)rec->dwell_ms,
        (unsigned long)rec->target_radius,
        (unsigned long)rec->round_duration_s,
        (unsigned long)rec->adaptive_level,
        AIRTOUCH_STORAGE_FW_TAG
    );

    fflush(fp);
    fclose(fp);

    ESP_LOGI(TAG,
             "STAR.CSV appended: rid=%lu hits=%lu avg=%lu hit_score=%lu speed_score=%lu",
             (unsigned long)rid,
             (unsigned long)rec->hits,
             (unsigned long)rec->avg_ms,
             (unsigned long)rec->hit_score,
             (unsigned long)rec->speed_score);

    append_last_star_record_to_queue();
    airtouch_storage_cloud_upload_last_star();
    airtouch_storage_rebuild_summary();
    return true;
}

bool airtouch_storage_append_color_record(const airtouch_color_record_t *rec)
{
    if (!rec) {
        ESP_LOGE(TAG, "append color failed: rec is NULL");
        return false;
    }

    if (!ensure_storage_ready_for_write()) {
        return false;
    }

    const bool need_header = file_needs_header(AIRTOUCH_STORAGE_COLOR_CSV);

    FILE *fp = fopen(AIRTOUCH_STORAGE_COLOR_CSV, "a");
    if (!fp) {
        ESP_LOGE(TAG, "Open COLOR.CSV failed: %s errno=%d", AIRTOUCH_STORAGE_COLOR_CSV, errno);
        return false;
    }

    if (need_header) {
        fprintf(fp, "rid,boot_ms,correct,wrong,false_alarm,miss,accuracy,avg_ms,fastest_ms,inhibit,speed_score,difficulty,dwell_ms,bubble_count,nogo_ratio,duration_s,level,fw\n");
    }

    uint32_t rid = rec->record_id;
    if (rid == 0) {
        rid = ++s_color_record_counter;
    }

    fprintf(
        fp,
        "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s\n",
        (unsigned long)rid,
        (unsigned long)rec->boot_ms,
        (unsigned long)rec->correct,
        (unsigned long)rec->wrong,
        (unsigned long)rec->false_alarm,
        (unsigned long)rec->miss,
        (unsigned long)rec->accuracy,
        (unsigned long)rec->avg_ms,
        (unsigned long)rec->fastest_ms,
        (unsigned long)rec->inhibition,
        (unsigned long)rec->speed_score,
        (unsigned long)rec->difficulty,
        (unsigned long)rec->dwell_ms,
        (unsigned long)rec->bubble_count,
        (unsigned long)rec->nogo_ratio,
        (unsigned long)rec->round_duration_s,
        (unsigned long)rec->adaptive_level,
        AIRTOUCH_STORAGE_FW_TAG
    );

    fflush(fp);
    fclose(fp);

    ESP_LOGI(TAG,
             "COLOR.CSV appended: rid=%lu correct=%lu wrong=%lu false=%lu accuracy=%lu inhibition=%lu",
             (unsigned long)rid,
             (unsigned long)rec->correct,
             (unsigned long)rec->wrong,
             (unsigned long)rec->false_alarm,
             (unsigned long)rec->accuracy,
             (unsigned long)rec->inhibition);

    append_last_color_record_to_queue();
    airtouch_storage_cloud_upload_last_color();
    airtouch_storage_rebuild_summary();
    return true;
}







/* --------------------------------------------------------------------------
 * Cloud-B v1g SD history replay
 *
 * Purpose:
 *   SD card is the authoritative edge-side record book.
 *   Cloud D1 is a synchronized replica.
 *
 * Strategy:
 *   Replay historical STAR.CSV and COLOR.CSV rows after boot.
 *   Cloud D1 has a unique index on device_id + user_id + type + rid,
 *   so repeated uploads are safe and will not create duplicate records.
 * -------------------------------------------------------------------------- */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AIRTOUCH_REPLAY_MAX_PER_KIND       128
#define AIRTOUCH_REPLAY_BOOT_DELAY_MS      8000
#define AIRTOUCH_REPLAY_BETWEEN_POST_MS    700
#define AIRTOUCH_REPLAY_TIMEOUT_MS         8000
#define AIRTOUCH_REPLAY_DEVICE_ID          "airtouch_001"
#define AIRTOUCH_REPLAY_SOURCE             "p4_cloud_sd_v1g2"
#define AIRTOUCH_REPLAY_FW                 "p4_cloud_sd_v1g2"

static const char *AIRTOUCH_REPLAY_TAG = "airtouch_replay";
static bool s_airtouch_replay_task_started = false;

static bool airtouch_replay_send_star_line_v1g(const char *line)
{
    unsigned long rid = 0;
    unsigned long boot_ms = 0;
    unsigned long hits = 0;
    unsigned long avg_ms = 0;
    unsigned long fastest_ms = 0;
    unsigned long hit_score = 0;
    unsigned long speed_score = 0;
    unsigned long difficulty = 0;
    unsigned long dwell_ms = 0;
    unsigned long target_r = 0;
    unsigned long duration_s = 0;
    unsigned long level = 0;

    int n = sscanf(line,
                   "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
                   &rid,
                   &boot_ms,
                   &hits,
                   &avg_ms,
                   &fastest_ms,
                   &hit_score,
                   &speed_score,
                   &difficulty,
                   &dwell_ms,
                   &target_r,
                   &duration_s,
                   &level);

    if (n != 12 || rid == 0) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "skip bad STAR.CSV row: %s", line);
        return false;
    }

    char json[640];

    int len = snprintf(json, sizeof(json),
                       "{\"device_id\":\"%s\","
                       "\"user_id\":\"%s\","
                       "\"source\":\"%s\","
                       "\"type\":\"star\","
                       "\"rid\":%lu,"
                       "\"boot_ms\":%lu,"
                       "\"hits\":%lu,"
                       "\"avg_ms\":%lu,"
                       "\"fastest_ms\":%lu,"
                       "\"hit_score\":%lu,"
                       "\"speed_score\":%lu,"
                       "\"difficulty\":%lu,"
                       "\"dwell_ms\":%lu,"
                       "\"target_r\":%lu,"
                       "\"duration_s\":%lu,"
                       "\"level\":%lu,"
                       "\"fw\":\"%s\"}",
                       AIRTOUCH_REPLAY_DEVICE_ID,
                       AIRTOUCH_STORAGE_DEFAULT_USER,
                       AIRTOUCH_REPLAY_SOURCE,
                       rid,
                       boot_ms,
                       hits,
                       avg_ms,
                       fastest_ms,
                       hit_score,
                       speed_score,
                       difficulty,
                       dwell_ms,
                       target_r,
                       duration_s,
                       level,
                       AIRTOUCH_REPLAY_FW);

    if (len <= 0 || len >= (int)sizeof(json)) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "STAR replay JSON overflow, rid=%lu", rid);
        return false;
    }

    uint32_t seq = (uint32_t)(100000UL + rid);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG, "replay STAR rid=%lu seq=%lu", rid, (unsigned long)seq);

    bool ok = airtouch_cloud_uart_send_json(seq, json, AIRTOUCH_REPLAY_TIMEOUT_MS);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "replay STAR rid=%lu result=%s",
             rid,
             ok ? "ACK" : "NACK/TIMEOUT");

    return ok;
}

static bool airtouch_replay_send_color_line_v1g(const char *line)
{
    unsigned long rid = 0;
    unsigned long boot_ms = 0;
    unsigned long correct = 0;
    unsigned long wrong = 0;
    unsigned long false_alarm = 0;
    unsigned long miss = 0;
    unsigned long accuracy = 0;
    unsigned long avg_ms = 0;
    unsigned long fastest_ms = 0;
    unsigned long inhibition = 0;
    unsigned long speed_score = 0;
    unsigned long difficulty = 0;
    unsigned long dwell_ms = 0;
    unsigned long bubble_count = 0;
    unsigned long nogo_ratio = 0;
    unsigned long duration_s = 0;
    unsigned long level = 0;

    int n = sscanf(line,
                   "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
                   &rid,
                   &boot_ms,
                   &correct,
                   &wrong,
                   &false_alarm,
                   &miss,
                   &accuracy,
                   &avg_ms,
                   &fastest_ms,
                   &inhibition,
                   &speed_score,
                   &difficulty,
                   &dwell_ms,
                   &bubble_count,
                   &nogo_ratio,
                   &duration_s,
                   &level);

    if (n != 17 || rid == 0) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "skip bad COLOR.CSV row: %s", line);
        return false;
    }

    char json[768];

    int len = snprintf(json, sizeof(json),
                       "{\"device_id\":\"%s\","
                       "\"user_id\":\"%s\","
                       "\"source\":\"%s\","
                       "\"type\":\"color_go\","
                       "\"rid\":%lu,"
                       "\"boot_ms\":%lu,"
                       "\"correct\":%lu,"
                       "\"wrong\":%lu,"
                       "\"false_alarm\":%lu,"
                       "\"miss\":%lu,"
                       "\"accuracy\":%lu,"
                       "\"avg_ms\":%lu,"
                       "\"fastest_ms\":%lu,"
                       "\"inhibition\":%lu,"
                       "\"speed_score\":%lu,"
                       "\"difficulty\":%lu,"
                       "\"dwell_ms\":%lu,"
                       "\"bubble_count\":%lu,"
                       "\"nogo_ratio\":%lu,"
                       "\"duration_s\":%lu,"
                       "\"level\":%lu,"
                       "\"fw\":\"%s\"}",
                       AIRTOUCH_REPLAY_DEVICE_ID,
                       AIRTOUCH_STORAGE_DEFAULT_USER,
                       AIRTOUCH_REPLAY_SOURCE,
                       rid,
                       boot_ms,
                       correct,
                       wrong,
                       false_alarm,
                       miss,
                       accuracy,
                       avg_ms,
                       fastest_ms,
                       inhibition,
                       speed_score,
                       difficulty,
                       dwell_ms,
                       bubble_count,
                       nogo_ratio,
                       duration_s,
                       level,
                       AIRTOUCH_REPLAY_FW);

    if (len <= 0 || len >= (int)sizeof(json)) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "COLOR replay JSON overflow, rid=%lu", rid);
        return false;
    }

    uint32_t seq = (uint32_t)(200000UL + rid);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG, "replay COLOR rid=%lu seq=%lu", rid, (unsigned long)seq);

    bool ok = airtouch_cloud_uart_send_json(seq, json, AIRTOUCH_REPLAY_TIMEOUT_MS);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "replay COLOR rid=%lu result=%s",
             rid,
             ok ? "ACK" : "NACK/TIMEOUT");

    return ok;
}

static void airtouch_replay_csv_file_v1g(const char *path,
                                         const char *kind,
                                         bool (*send_line)(const char *line),
                                         uint32_t max_records,
                                         uint32_t *out_attempt,
                                         uint32_t *out_ack)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "replay open failed: %s", path);
        return;
    }

    char line[900];
    uint32_t attempt = 0;
    uint32_t ack = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '\0') {
            continue;
        }

        if (strncmp(line, "rid,", 4) == 0) {
            continue;
        }

        if (attempt >= max_records) {
            ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                     "replay %s reach max_records=%lu",
                     kind,
                     (unsigned long)max_records);
            break;
        }

        attempt++;

        bool ok = send_line(line);
        if (ok) {
            ack++;
        } else {
            ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                     "replay %s stopped on failed row attempt=%lu",
                     kind,
                     (unsigned long)attempt);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(AIRTOUCH_REPLAY_BETWEEN_POST_MS));
    }

    fclose(fp);

    if (out_attempt) {
        *out_attempt = attempt;
    }

    if (out_ack) {
        *out_ack = ack;
    }

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "replay %s done: attempt=%lu ack=%lu",
             kind,
             (unsigned long)attempt,
             (unsigned long)ack);
}

static void airtouch_storage_cloud_replay_task_v1g(void *arg)
{
    (void)arg;

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "Cloud-B v1g SD history replay task boot delay %d ms",
             AIRTOUCH_REPLAY_BOOT_DELAY_MS);

    vTaskDelay(pdMS_TO_TICKS(AIRTOUCH_REPLAY_BOOT_DELAY_MS));

    if (!airtouch_storage_is_ready()) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "replay cancelled: SD storage not ready");
        vTaskDelete(NULL);
        return;
    }

    if (!airtouch_cloud_uart_is_ready()) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "replay cancelled: cloud UART not ready");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "Cloud-B v1g SD history replay begin, cloud dedup enabled");

    uint32_t star_attempt = 0;
    uint32_t star_ack = 0;
    uint32_t color_attempt = 0;
    uint32_t color_ack = 0;

    airtouch_replay_csv_file_v1g(AIRTOUCH_STORAGE_STAR_CSV,
                                 "STAR",
                                 airtouch_replay_send_star_line_v1g,
                                 AIRTOUCH_REPLAY_MAX_PER_KIND,
                                 &star_attempt,
                                 &star_ack);

    vTaskDelay(pdMS_TO_TICKS(1000));

    airtouch_replay_csv_file_v1g(AIRTOUCH_STORAGE_COLOR_CSV,
                                 "COLOR",
                                 airtouch_replay_send_color_line_v1g,
                                 AIRTOUCH_REPLAY_MAX_PER_KIND,
                                 &color_attempt,
                                 &color_ack);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "Cloud-B v1g SD history replay finished: star %lu/%lu, color %lu/%lu",
             (unsigned long)star_ack,
             (unsigned long)star_attempt,
             (unsigned long)color_ack,
             (unsigned long)color_attempt);

    vTaskDelete(NULL);
}


/* --------------------------------------------------------------------------
 * Cloud-SD v1g.2 sync state
 *
 * Purpose:
 *   Keep edge-side cloud sync progress in SYNC.TXT.
 *   SD remains the authoritative record book.
 *   Cloud D1 remains an idempotent synchronized replica.
 *
 * Behavior:
 *   1. Load star_ack_rid / color_ack_rid from SYNC.TXT.
 *   2. Skip already acknowledged CSV rows.
 *   3. Upload only records with rid greater than ack rid.
 *   4. After each ACK, update SYNC.TXT immediately.
 *   5. On failure, stop current replay and continue next boot.
 * -------------------------------------------------------------------------- */

#ifndef AIRTOUCH_STORAGE_SYNC_TXT
#define AIRTOUCH_STORAGE_SYNC_TXT AIRTOUCH_STORAGE_USER_DIR "/SYNC.TXT"
#endif

#define AIRTOUCH_SYNC_RESULT_OK      "ok"
#define AIRTOUCH_SYNC_RESULT_FAIL    "fail"
#define AIRTOUCH_SYNC_SOURCE_V1G2    "p4_cloud_sd_v1g2"

typedef struct {
    uint32_t star_ack_rid;
    uint32_t color_ack_rid;
    uint32_t last_sync_boot_ms;
    char last_sync_result[16];
    char last_sync_source[32];
} airtouch_sync_state_v1g2_t;

static void airtouch_sync_state_set_default_v1g2(airtouch_sync_state_v1g2_t *st)
{
    if (!st) {
        return;
    }

    memset(st, 0, sizeof(*st));
    st->star_ack_rid = 0;
    st->color_ack_rid = 0;
    st->last_sync_boot_ms = 0;
    snprintf(st->last_sync_result, sizeof(st->last_sync_result), "none");
    snprintf(st->last_sync_source, sizeof(st->last_sync_source), "%s", AIRTOUCH_SYNC_SOURCE_V1G2);
}

static bool airtouch_sync_load_state_v1g2(airtouch_sync_state_v1g2_t *st)
{
    if (!st) {
        return false;
    }

    airtouch_sync_state_set_default_v1g2(st);

    FILE *fp = fopen(AIRTOUCH_STORAGE_SYNC_TXT, "r");
    if (!fp) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                 "SYNC.TXT not found, start from rid=0: %s",
                 AIRTOUCH_STORAGE_SYNC_TXT);
        return false;
    }

    char line[128];

    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        unsigned long v = 0;

        if (sscanf(line, "star_ack_rid=%lu", &v) == 1) {
            st->star_ack_rid = (uint32_t)v;
            continue;
        }

        if (sscanf(line, "color_ack_rid=%lu", &v) == 1) {
            st->color_ack_rid = (uint32_t)v;
            continue;
        }

        if (sscanf(line, "last_sync_boot_ms=%lu", &v) == 1) {
            st->last_sync_boot_ms = (uint32_t)v;
            continue;
        }

        if (strncmp(line, "last_sync_result=", 17) == 0) {
            snprintf(st->last_sync_result,
                     sizeof(st->last_sync_result),
                     "%s",
                     line + 17);
            continue;
        }

        if (strncmp(line, "last_sync_source=", 17) == 0) {
            snprintf(st->last_sync_source,
                     sizeof(st->last_sync_source),
                     "%s",
                     line + 17);
            continue;
        }
    }

    fclose(fp);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "SYNC.TXT load: star_ack_rid=%lu color_ack_rid=%lu result=%s source=%s",
             (unsigned long)st->star_ack_rid,
             (unsigned long)st->color_ack_rid,
             st->last_sync_result,
             st->last_sync_source);

    return true;
}

static bool airtouch_sync_save_state_v1g2(const airtouch_sync_state_v1g2_t *st)
{
    if (!st) {
        return false;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_SYNC_TXT, "w");
    if (!fp) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                 "SYNC.TXT save failed: %s",
                 AIRTOUCH_STORAGE_SYNC_TXT);
        return false;
    }

    fprintf(fp, "star_ack_rid=%lu\n", (unsigned long)st->star_ack_rid);
    fprintf(fp, "color_ack_rid=%lu\n", (unsigned long)st->color_ack_rid);
    fprintf(fp, "last_sync_boot_ms=%lu\n", (unsigned long)st->last_sync_boot_ms);
    fprintf(fp, "last_sync_result=%s\n", st->last_sync_result);
    fprintf(fp, "last_sync_source=%s\n", st->last_sync_source);

    fclose(fp);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "SYNC.TXT save: star_ack_rid=%lu color_ack_rid=%lu result=%s",
             (unsigned long)st->star_ack_rid,
             (unsigned long)st->color_ack_rid,
             st->last_sync_result);

    return true;
}

static uint32_t airtouch_sync_parse_rid_from_csv_line_v1g2(const char *line)
{
    if (!line || line[0] == '\0') {
        return 0;
    }

    unsigned long rid = 0;

    if (sscanf(line, "%lu,", &rid) == 1) {
        return (uint32_t)rid;
    }

    return 0;
}


/* --------------------------------------------------------------------------
 * Cloud-SD v1g.4 CSV health check
 *
 * Purpose:
 *   Make historical replay tolerant to malformed CSV rows.
 *   Bad rows should be skipped and logged, but valid rows after them should
 *   still be replayed normally.
 * -------------------------------------------------------------------------- */

#define AIRTOUCH_CSV_HEALTH_TAG "airtouch_csv_health"

#define AIRTOUCH_STAR_CSV_MIN_FIELDS_V1G4   13
#define AIRTOUCH_COLOR_CSV_MIN_FIELDS_V1G4  18

static uint32_t airtouch_csv_count_fields_v1g4(const char *line)
{
    if (!line || line[0] == '\0') {
        return 0;
    }

    uint32_t fields = 1;

    for (const char *p = line; *p != '\0'; ++p) {
        if (*p == ',') {
            fields++;
        }
    }

    return fields;
}

static bool airtouch_csv_line_starts_with_digit_v1g4(const char *line)
{
    if (!line || line[0] == '\0') {
        return false;
    }

    return line[0] >= '0' && line[0] <= '9';
}

static bool airtouch_validate_star_csv_line_v1g4(const char *line, uint32_t *out_rid)
{
    if (out_rid) {
        *out_rid = 0;
    }

    if (!line || line[0] == '\0') {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG, "STAR skip empty row");
        return false;
    }

    if (strncmp(line, "rid,", 4) == 0) {
        return false;
    }

    if (!airtouch_csv_line_starts_with_digit_v1g4(line)) {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG, "STAR skip non-data row: %s", line);
        return false;
    }

    const uint32_t field_count = airtouch_csv_count_fields_v1g4(line);
    if (field_count < AIRTOUCH_STAR_CSV_MIN_FIELDS_V1G4) {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG,
                 "STAR skip malformed row: fields=%lu need>=%u row=%s",
                 (unsigned long)field_count,
                 AIRTOUCH_STAR_CSV_MIN_FIELDS_V1G4,
                 line);
        return false;
    }

    uint32_t rid = airtouch_sync_parse_rid_from_csv_line_v1g2(line);
    if (rid == 0) {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG, "STAR skip bad rid row: %s", line);
        return false;
    }

    if (out_rid) {
        *out_rid = rid;
    }

    return true;
}

static bool airtouch_validate_color_csv_line_v1g4(const char *line, uint32_t *out_rid)
{
    if (out_rid) {
        *out_rid = 0;
    }

    if (!line || line[0] == '\0') {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG, "COLOR skip empty row");
        return false;
    }

    if (strncmp(line, "rid,", 4) == 0) {
        return false;
    }

    if (!airtouch_csv_line_starts_with_digit_v1g4(line)) {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG, "COLOR skip non-data row: %s", line);
        return false;
    }

    const uint32_t field_count = airtouch_csv_count_fields_v1g4(line);
    if (field_count < AIRTOUCH_COLOR_CSV_MIN_FIELDS_V1G4) {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG,
                 "COLOR skip malformed row: fields=%lu need>=%u row=%s",
                 (unsigned long)field_count,
                 AIRTOUCH_COLOR_CSV_MIN_FIELDS_V1G4,
                 line);
        return false;
    }

    uint32_t rid = airtouch_sync_parse_rid_from_csv_line_v1g2(line);
    if (rid == 0) {
        ESP_LOGW(AIRTOUCH_CSV_HEALTH_TAG, "COLOR skip bad rid row: %s", line);
        return false;
    }

    if (out_rid) {
        *out_rid = rid;
    }

    return true;
}

static void airtouch_replay_star_csv_file_v1g2(airtouch_sync_state_v1g2_t *state,
                                               uint32_t max_records,
                                               uint32_t *out_attempt,
                                               uint32_t *out_ack,
                                               uint32_t *out_skip)
{
    if (!state) {
        return;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_STAR_CSV, "r");
    if (!fp) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "STAR replay open failed: %s", AIRTOUCH_STORAGE_STAR_CSV);
        return;
    }

    char line[900];
    uint32_t attempt = 0;
    uint32_t ack = 0;
    uint32_t skip = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '\0') {
            continue;
        }

        if (strncmp(line, "rid,", 4) == 0) {
            continue;
        }

        uint32_t rid = 0;

        if (!airtouch_validate_star_csv_line_v1g4(line, &rid)) {
            continue;
        }

        if (rid <= state->star_ack_rid) {
            skip++;
            ESP_LOGI(AIRTOUCH_REPLAY_TAG,
                     "STAR replay skip synced rid=%lu ack=%lu",
                     (unsigned long)rid,
                     (unsigned long)state->star_ack_rid);
            continue;
        }

        if (attempt >= max_records) {
            ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                     "STAR replay reach max_records=%lu",
                     (unsigned long)max_records);
            break;
        }

        attempt++;

        bool ok = airtouch_replay_send_star_line_v1g(line);
        state->last_sync_boot_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        snprintf(state->last_sync_source, sizeof(state->last_sync_source), "%s", AIRTOUCH_SYNC_SOURCE_V1G2);

        if (ok) {
            ack++;
            state->star_ack_rid = rid;
            snprintf(state->last_sync_result, sizeof(state->last_sync_result), "%s", AIRTOUCH_SYNC_RESULT_OK);
            airtouch_sync_save_state_v1g2(state);
        } else {
            snprintf(state->last_sync_result, sizeof(state->last_sync_result), "%s", AIRTOUCH_SYNC_RESULT_FAIL);
            airtouch_sync_save_state_v1g2(state);

            ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                     "STAR replay stopped: rid=%lu failed",
                     (unsigned long)rid);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(AIRTOUCH_REPLAY_BETWEEN_POST_MS));
    }

    fclose(fp);

    if (out_attempt) {
        *out_attempt = attempt;
    }

    if (out_ack) {
        *out_ack = ack;
    }

    if (out_skip) {
        *out_skip = skip;
    }

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "STAR v1g.2 replay done: skip=%lu attempt=%lu ack=%lu final_ack_rid=%lu",
             (unsigned long)skip,
             (unsigned long)attempt,
             (unsigned long)ack,
             (unsigned long)state->star_ack_rid);
}

static void airtouch_replay_color_csv_file_v1g2(airtouch_sync_state_v1g2_t *state,
                                                uint32_t max_records,
                                                uint32_t *out_attempt,
                                                uint32_t *out_ack,
                                                uint32_t *out_skip)
{
    if (!state) {
        return;
    }

    FILE *fp = fopen(AIRTOUCH_STORAGE_COLOR_CSV, "r");
    if (!fp) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "COLOR replay open failed: %s", AIRTOUCH_STORAGE_COLOR_CSV);
        return;
    }

    char line[900];
    uint32_t attempt = 0;
    uint32_t ack = 0;
    uint32_t skip = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (line[0] == '\0') {
            continue;
        }

        if (strncmp(line, "rid,", 4) == 0) {
            continue;
        }

        uint32_t rid = 0;

        if (!airtouch_validate_color_csv_line_v1g4(line, &rid)) {
            continue;
        }

        if (rid <= state->color_ack_rid) {
            skip++;
            ESP_LOGI(AIRTOUCH_REPLAY_TAG,
                     "COLOR replay skip synced rid=%lu ack=%lu",
                     (unsigned long)rid,
                     (unsigned long)state->color_ack_rid);
            continue;
        }

        if (attempt >= max_records) {
            ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                     "COLOR replay reach max_records=%lu",
                     (unsigned long)max_records);
            break;
        }

        attempt++;

        bool ok = airtouch_replay_send_color_line_v1g(line);
        state->last_sync_boot_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        snprintf(state->last_sync_source, sizeof(state->last_sync_source), "%s", AIRTOUCH_SYNC_SOURCE_V1G2);

        if (ok) {
            ack++;
            state->color_ack_rid = rid;
            snprintf(state->last_sync_result, sizeof(state->last_sync_result), "%s", AIRTOUCH_SYNC_RESULT_OK);
            airtouch_sync_save_state_v1g2(state);
        } else {
            snprintf(state->last_sync_result, sizeof(state->last_sync_result), "%s", AIRTOUCH_SYNC_RESULT_FAIL);
            airtouch_sync_save_state_v1g2(state);

            ESP_LOGW(AIRTOUCH_REPLAY_TAG,
                     "COLOR replay stopped: rid=%lu failed",
                     (unsigned long)rid);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(AIRTOUCH_REPLAY_BETWEEN_POST_MS));
    }

    fclose(fp);

    if (out_attempt) {
        *out_attempt = attempt;
    }

    if (out_ack) {
        *out_ack = ack;
    }

    if (out_skip) {
        *out_skip = skip;
    }

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "COLOR v1g.2 replay done: skip=%lu attempt=%lu ack=%lu final_ack_rid=%lu",
             (unsigned long)skip,
             (unsigned long)attempt,
             (unsigned long)ack,
             (unsigned long)state->color_ack_rid);
}

static void airtouch_storage_cloud_replay_task_v1g2(void *arg)
{
    (void)arg;

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "Cloud-SD v1g.2 sync-state replay task boot delay %d ms",
             AIRTOUCH_REPLAY_BOOT_DELAY_MS);

    vTaskDelay(pdMS_TO_TICKS(AIRTOUCH_REPLAY_BOOT_DELAY_MS));

    if (!airtouch_storage_is_ready()) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "v1g.2 replay cancelled: SD storage not ready");
        vTaskDelete(NULL);
        return;
    }

    if (!airtouch_cloud_uart_is_ready()) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "v1g.2 replay cancelled: cloud UART not ready");
        vTaskDelete(NULL);
        return;
    }

    airtouch_sync_state_v1g2_t state;
    airtouch_sync_load_state_v1g2(&state);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "Cloud-SD v1g.2 replay begin: star_ack=%lu color_ack=%lu",
             (unsigned long)state.star_ack_rid,
             (unsigned long)state.color_ack_rid);

    uint32_t star_attempt = 0;
    uint32_t star_ack = 0;
    uint32_t star_skip = 0;
    uint32_t color_attempt = 0;
    uint32_t color_ack = 0;
    uint32_t color_skip = 0;

    airtouch_replay_star_csv_file_v1g2(&state,
                                       AIRTOUCH_REPLAY_MAX_PER_KIND,
                                       &star_attempt,
                                       &star_ack,
                                       &star_skip);

    vTaskDelay(pdMS_TO_TICKS(1000));

    airtouch_replay_color_csv_file_v1g2(&state,
                                        AIRTOUCH_REPLAY_MAX_PER_KIND,
                                        &color_attempt,
                                        &color_ack,
                                        &color_skip);

    ESP_LOGI(AIRTOUCH_REPLAY_TAG,
             "Cloud-SD v1g.2 replay finished: STAR skip=%lu ack=%lu/%lu, COLOR skip=%lu ack=%lu/%lu, final star_ack=%lu color_ack=%lu",
             (unsigned long)star_skip,
             (unsigned long)star_ack,
             (unsigned long)star_attempt,
             (unsigned long)color_skip,
             (unsigned long)color_ack,
             (unsigned long)color_attempt,
             (unsigned long)state.star_ack_rid,
             (unsigned long)state.color_ack_rid);

    vTaskDelete(NULL);
}

bool airtouch_storage_cloud_start_replay_task(void)
{
    if (s_airtouch_replay_task_started) {
        ESP_LOGW(AIRTOUCH_REPLAY_TAG, "Cloud-B v1g replay task already started");
        return true;
    }

    BaseType_t ret = xTaskCreate(airtouch_storage_cloud_replay_task_v1g2,
                                 "air_sync_v1g2",
                                 8192,
                                 NULL,
                                 4,
                                 NULL);

    if (ret != pdPASS) {
        ESP_LOGE(AIRTOUCH_REPLAY_TAG, "Cloud-B v1g replay task create failed");
        return false;
    }

    s_airtouch_replay_task_started = true;

    ESP_LOGI(AIRTOUCH_REPLAY_TAG, "Cloud-SD v1g.2 sync replay task created");

    return true;
}









