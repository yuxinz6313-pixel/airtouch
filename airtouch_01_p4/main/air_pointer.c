#include "air_pointer.h"
#include "air_espdet_probe.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "air_pointer";

/*
 * Camera raw frame -> reduced mask:
 *   1280x960 -> 160x120
 */
#define AIR_MASK_W                      160
#define AIR_MASK_H                      120

/*
 * LCD screen:
 *   1024x600
 */
#define AIR_SCREEN_W                    1024
#define AIR_SCREEN_H                    600

/*
 * Current physical relation:
 *
 * Camera raw image to displayed screen direction:
 *   rotate 180 degrees + horizontal mirror
 *
 * Equivalent coordinate transform:
 *   display_x = camera_x
 *   display_y = AIR_MASK_H - 1 - camera_y
 *
 * So only Y needs to be inverted.
 */
#define AIR_ORIENT_INVERT_X             0
#define AIR_ORIENT_INVERT_Y             1

/*
 * Marker area threshold in 160x120 mask space.
 * If your pen tip is detected but unstable, this may be lowered.
 */
#define AIR_MIN_COMPONENT_AREA          18

/*
 * If target is lost for a few frames, then invalidate.
 */
#define AIR_LOST_MAX_FRAMES             3

/*
 * Smoothing:
 * filtered = (old * (N-1) + new) / N
 */
#define AIR_SMOOTH_N                    4

/*
 * Full-screen mapping active range in DISPLAY-oriented mask coordinates.
 */
#define AIR_ACTIVE_X_MIN          0
#define AIR_ACTIVE_X_MAX          150
#define AIR_CALIB_X_OFFSET_PX      4
#define AIR_ACTIVE_Y_MIN                6
#define AIR_ACTIVE_Y_MAX                114

static uint8_t s_mask[AIR_MASK_W * AIR_MASK_H];
static uint8_t s_visited[AIR_MASK_W * AIR_MASK_H];
static uint16_t s_queue_x[AIR_MASK_W * AIR_MASK_H];
static uint16_t s_queue_y[AIR_MASK_W * AIR_MASK_H];

static air_pointer_result_t s_latest = {0};

static bool s_inited = false;
static uint32_t s_frame_count = 0;
static uint32_t s_valid_count = 0;
static uint32_t s_lost_frames = 0;

/*
 * Smoothed center in display-oriented mask coordinates.
 */
static bool s_have_filtered = false;
static int s_filtered_x = 0;
static int s_filtered_y = 0;

/*
 * Motion-aware local reacquisition.
 * Global color search is still the main path.
 * When global search temporarily fails during fast motion, predict the next
 * camera-mask location and search a small local window with a lower area threshold.
 */
#define AIR_LOCAL_SEARCH_RADIUS          44
#define AIR_LOCAL_SEARCH_MIN_AREA        5
#define AIR_LOCAL_TRACK_MAX_GAP_FRAMES   18
#define AIR_LOCAL_MAX_STEP               56

static bool s_motion_have_cam = false;
static int s_motion_prev_cam_x = 0;
static int s_motion_prev_cam_y = 0;
static int s_motion_last_cam_x = 0;
static int s_motion_last_cam_y = 0;
static uint32_t s_motion_last_frame = 0;


static inline uint32_t air_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline int air_clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }

    if (v > hi) {
        return hi;
    }

    return v;
}

static inline int air_map_range(int v, int in_min, int in_max, int out_min, int out_max)
{
    if (in_max <= in_min) {
        return out_min;
    }

    if (v < in_min) {
        v = in_min;
    }

    if (v > in_max) {
        v = in_max;
    }

    int num = (v - in_min) * (out_max - out_min);
    int den = in_max - in_min;

    return out_min + num / den;
}

static inline void rgb565_to_rgb888(uint16_t p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r5 = (p >> 11) & 0x1F;
    uint8_t g6 = (p >> 5) & 0x3F;
    uint8_t b5 = p & 0x1F;

    *r = (uint8_t)((r5 * 255 + 15) / 31);
    *g = (uint8_t)((g6 * 255 + 31) / 63);
    *b = (uint8_t)((b5 * 255 + 15) / 31);
}

static inline bool is_target_magenta(uint8_t r, uint8_t g, uint8_t b)
{
    int maxc = r;

    if (g > maxc) {
        maxc = g;
    }

    if (b > maxc) {
        maxc = b;
    }

    int minc = r;

    if (g < minc) {
        minc = g;
    }

    if (b < minc) {
        minc = b;
    }

    int sat = maxc - minc;

    if (sat < 35) {
        return false;
    }

    if (r < 110) {
        return false;
    }

    if (b < 65) {
        return false;
    }

    if ((r - g) < 20) {
        return false;
    }

    if ((b - g) < 5) {
        return false;
    }

    if ((((int)r + (int)b) / 2 - (int)g) < 20) {
        return false;
    }

    /*
     * Reject pure red / orange.
     */
    if (b + 50 < r && b < 90) {
        return false;
    }

    return true;
}

static inline void camera_mask_to_display_mask(int cam_x,
                                               int cam_y,
                                               int *disp_x,
                                               int *disp_y)
{
#if AIR_ORIENT_INVERT_X
    *disp_x = AIR_MASK_W - 1 - cam_x;
#else
    *disp_x = cam_x;
#endif

#if AIR_ORIENT_INVERT_Y
    *disp_y = AIR_MASK_H - 1 - cam_y;
#else
    *disp_y = cam_y;
#endif
}

static void air_pointer_set_invalid(void)
{
    s_latest.valid = false;
    s_latest.confidence = 0;
    s_latest.area = 0;
}

esp_err_t air_pointer_init(void)
{
    memset(s_mask, 0, sizeof(s_mask));
    memset(s_visited, 0, sizeof(s_visited));
    memset(&s_latest, 0, sizeof(s_latest));

    s_frame_count = 0;
    s_valid_count = 0;
    s_lost_frames = 0;

    s_have_filtered = false;
    s_filtered_x = 0;
    s_filtered_y = 0;

    s_inited = true;

    ESP_LOGI(TAG, "Air pointer init done: magenta pen mode, invert_x=%d, invert_y=%d",
             AIR_ORIENT_INVERT_X,
             AIR_ORIENT_INVERT_Y);

    return ESP_OK;
}


static inline int air_motion_abs_int(int v)
{
    return (v < 0) ? -v : v;
}

static inline int air_motion_clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void air_motion_update_cam_track(int cam_x, int cam_y)
{
    cam_x = air_motion_clamp_int(cam_x, 0, AIR_MASK_W - 1);
    cam_y = air_motion_clamp_int(cam_y, 0, AIR_MASK_H - 1);

    if (!s_motion_have_cam) {
        s_motion_prev_cam_x = cam_x;
        s_motion_prev_cam_y = cam_y;
        s_motion_last_cam_x = cam_x;
        s_motion_last_cam_y = cam_y;
        s_motion_have_cam = true;
    } else {
        s_motion_prev_cam_x = s_motion_last_cam_x;
        s_motion_prev_cam_y = s_motion_last_cam_y;
        s_motion_last_cam_x = cam_x;
        s_motion_last_cam_y = cam_y;
    }

    s_motion_last_frame = s_frame_count;
}

static bool air_motion_predict_cam(int *out_x, int *out_y)
{
    if (out_x == NULL || out_y == NULL) {
        return false;
    }

    if (!s_motion_have_cam) {
        return false;
    }

    uint32_t gap = s_frame_count - s_motion_last_frame;
    if (gap > AIR_LOCAL_TRACK_MAX_GAP_FRAMES) {
        return false;
    }

    int vx = s_motion_last_cam_x - s_motion_prev_cam_x;
    int vy = s_motion_last_cam_y - s_motion_prev_cam_y;

    vx = air_motion_clamp_int(vx, -AIR_LOCAL_MAX_STEP, AIR_LOCAL_MAX_STEP);
    vy = air_motion_clamp_int(vy, -AIR_LOCAL_MAX_STEP, AIR_LOCAL_MAX_STEP);

    int pred_x = s_motion_last_cam_x + vx;
    int pred_y = s_motion_last_cam_y + vy;

    *out_x = air_motion_clamp_int(pred_x, 0, AIR_MASK_W - 1);
    *out_y = air_motion_clamp_int(pred_y, 0, AIR_MASK_H - 1);

    return true;
}

static bool find_local_component_near_prediction(int pred_x,
                                                 int pred_y,
                                                 int radius,
                                                 int min_area,
                                                 int *out_cx,
                                                 int *out_cy,
                                                 int *out_area)
{
    if (out_cx == NULL || out_cy == NULL || out_area == NULL) {
        return false;
    }

    pred_x = air_motion_clamp_int(pred_x, 0, AIR_MASK_W - 1);
    pred_y = air_motion_clamp_int(pred_y, 0, AIR_MASK_H - 1);

    int x0 = air_motion_clamp_int(pred_x - radius, 0, AIR_MASK_W - 1);
    int x1 = air_motion_clamp_int(pred_x + radius, 0, AIR_MASK_W - 1);
    int y0 = air_motion_clamp_int(pred_y - radius, 0, AIR_MASK_H - 1);
    int y1 = air_motion_clamp_int(pred_y + radius, 0, AIR_MASK_H - 1);

    memset(s_visited, 0, sizeof(s_visited));

    int best_area = 0;
    int best_sum_x = 0;
    int best_sum_y = 0;
    int best_score = -2147483647;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int idx = y * AIR_MASK_W + x;

            if (!s_mask[idx] || s_visited[idx]) {
                continue;
            }

            int head = 0;
            int tail = 0;

            s_queue_x[tail] = (uint16_t)x;
            s_queue_y[tail] = (uint16_t)y;
            tail++;

            s_visited[idx] = 1;

            int area = 0;
            int sum_x = 0;
            int sum_y = 0;

            while (head < tail) {
                int cx = s_queue_x[head];
                int cy = s_queue_y[head];
                head++;

                area++;
                sum_x += cx;
                sum_y += cy;

                static const int dx[4] = {1, -1, 0, 0};
                static const int dy[4] = {0, 0, 1, -1};

                for (int k = 0; k < 4; k++) {
                    int nx = cx + dx[k];
                    int ny = cy + dy[k];

                    if (nx < x0 || nx > x1 || ny < y0 || ny > y1) {
                        continue;
                    }

                    int nidx = ny * AIR_MASK_W + nx;

                    if (!s_mask[nidx] || s_visited[nidx]) {
                        continue;
                    }

                    s_visited[nidx] = 1;
                    s_queue_x[tail] = (uint16_t)nx;
                    s_queue_y[tail] = (uint16_t)ny;
                    tail++;
                }
            }

            if (area < min_area) {
                continue;
            }

            int cx = sum_x / area;
            int cy = sum_y / area;
            int ddx = cx - pred_x;
            int ddy = cy - pred_y;
            int dist2 = ddx * ddx + ddy * ddy;

            /*
             * Prefer larger components, but among similar sizes prefer the one
             * closest to the predicted motion position.
             */
            int score = area * 100 - dist2;

            if (score > best_score) {
                best_score = score;
                best_area = area;
                best_sum_x = sum_x;
                best_sum_y = sum_y;
            }
        }
    }

    if (best_area < min_area) {
        return false;
    }

    *out_cx = best_sum_x / best_area;
    *out_cy = best_sum_y / best_area;
    *out_area = best_area;
    return true;
}


static void build_mask_from_rgb565(const uint8_t *camera_buf,
                                   uint32_t width,
                                   uint32_t height)
{
    for (int my = 0; my < AIR_MASK_H; my++) {
        uint32_t sy = ((uint32_t)my * height) / AIR_MASK_H;

        if (sy >= height) {
            sy = height - 1;
        }

        for (int mx = 0; mx < AIR_MASK_W; mx++) {
            uint32_t sx = ((uint32_t)mx * width) / AIR_MASK_W;

            if (sx >= width) {
                sx = width - 1;
            }

            size_t off = ((size_t)sy * width + sx) * 2U;

            uint16_t p = (uint16_t)camera_buf[off] |
                         ((uint16_t)camera_buf[off + 1] << 8);

            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;

            rgb565_to_rgb888(p, &r, &g, &b);

            s_mask[my * AIR_MASK_W + mx] = is_target_magenta(r, g, b) ? 1 : 0;
        }
    }
}

static bool find_largest_component(int *out_cx,
                                   int *out_cy,
                                   int *out_area)
{
    memset(s_visited, 0, sizeof(s_visited));

    int best_area = 0;
    int best_sum_x = 0;
    int best_sum_y = 0;

    for (int y = 0; y < AIR_MASK_H; y++) {
        for (int x = 0; x < AIR_MASK_W; x++) {
            int idx = y * AIR_MASK_W + x;

            if (!s_mask[idx] || s_visited[idx]) {
                continue;
            }

            int head = 0;
            int tail = 0;

            s_queue_x[tail] = (uint16_t)x;
            s_queue_y[tail] = (uint16_t)y;
            tail++;

            s_visited[idx] = 1;

            int area = 0;
            int sum_x = 0;
            int sum_y = 0;

            while (head < tail) {
                int cx = s_queue_x[head];
                int cy = s_queue_y[head];
                head++;

                area++;
                sum_x += cx;
                sum_y += cy;

                static const int dx[4] = {1, -1, 0, 0};
                static const int dy[4] = {0, 0, 1, -1};

                for (int k = 0; k < 4; k++) {
                    int nx = cx + dx[k];
                    int ny = cy + dy[k];

                    if (nx < 0 || nx >= AIR_MASK_W || ny < 0 || ny >= AIR_MASK_H) {
                        continue;
                    }

                    int nidx = ny * AIR_MASK_W + nx;

                    if (!s_mask[nidx] || s_visited[nidx]) {
                        continue;
                    }

                    s_visited[nidx] = 1;

                    s_queue_x[tail] = (uint16_t)nx;
                    s_queue_y[tail] = (uint16_t)ny;
                    tail++;
                }
            }

            if (area > best_area) {
                best_area = area;
                best_sum_x = sum_x;
                best_sum_y = sum_y;
            }
        }
    }

    if (best_area < AIR_MIN_COMPONENT_AREA) {
        return false;
    }

    *out_area = best_area;
    *out_cx = best_sum_x / best_area;
    *out_cy = best_sum_y / best_area;

    return true;
}

static void update_filtered_center(int raw_x, int raw_y)
{
    if (!s_have_filtered) {
        s_filtered_x = raw_x;
        s_filtered_y = raw_y;
        s_have_filtered = true;
        return;
    }

    s_filtered_x = ((AIR_SMOOTH_N - 1) * s_filtered_x + raw_x) / AIR_SMOOTH_N;
    s_filtered_y = ((AIR_SMOOTH_N - 1) * s_filtered_y + raw_y) / AIR_SMOOTH_N;
}

void air_pointer_process_rgb565(const uint8_t *camera_buf,
                                uint32_t width,
                                uint32_t height,
                                size_t camera_buf_len)
{
    if (!s_inited) {
        return;
    }

    if (camera_buf == NULL || width == 0 || height == 0) {
        air_pointer_set_invalid();
        return;
    }

    size_t expected_len = (size_t)width * (size_t)height * 2U;

    if (camera_buf_len < expected_len) {
        air_pointer_set_invalid();
        return;
    }

    s_frame_count++;

    build_mask_from_rgb565(camera_buf, width, height);

    int cam_raw_x = -1;
    int cam_raw_y = -1;
    int area = 0;

    bool local_reacquired = false;
    bool ok = find_largest_component(&cam_raw_x, &cam_raw_y, &area);

    if (!ok) {
        int pred_x = -1;
        int pred_y = -1;

        if (air_motion_predict_cam(&pred_x, &pred_y)) {
            ok = find_local_component_near_prediction(pred_x,
                                                      pred_y,
                                                      AIR_LOCAL_SEARCH_RADIUS,
                                                      AIR_LOCAL_SEARCH_MIN_AREA,
                                                      &cam_raw_x,
                                                      &cam_raw_y,
                                                      &area);
            local_reacquired = ok;
        }
    }

    if (!ok) {
        s_lost_frames++;

        if (s_lost_frames > AIR_LOST_MAX_FRAMES) {
            air_pointer_set_invalid();
            s_have_filtered = false;
        }

        if ((s_frame_count % 20) == 0) {
            ESP_LOGI(TAG,
                     "valid=0 cam_raw=(-1,-1) disp_raw=(-1,-1) screen=(-1,-1) area=0 frames=%lu",
                     (unsigned long)s_frame_count);
        }

        return;
    }

    air_motion_update_cam_track(cam_raw_x, cam_raw_y);

    if (local_reacquired && ((s_frame_count % 10) == 0)) {
        ESP_LOGI(TAG,
                 "local_reacquire cam_raw=(%d,%d) area=%d frames=%lu",
                 cam_raw_x,
                 cam_raw_y,
                 area,
                 (unsigned long)s_frame_count);
    }

    s_lost_frames = 0;
    s_valid_count++;

    int disp_raw_x = 0;
    int disp_raw_y = 0;

    camera_mask_to_display_mask(cam_raw_x,
                                cam_raw_y,
                                &disp_raw_x,
                                &disp_raw_y);

    update_filtered_center(disp_raw_x, disp_raw_y);

    int screen_x = air_map_range(
        s_filtered_x,
        AIR_ACTIVE_X_MIN,
        AIR_ACTIVE_X_MAX,
        0,
        AIR_SCREEN_W - 1
    );

    int screen_y = air_map_range(
        s_filtered_y,
        AIR_ACTIVE_Y_MIN,
        AIR_ACTIVE_Y_MAX,
        0,
        AIR_SCREEN_H - 1
    );

    screen_x += AIR_CALIB_X_OFFSET_PX;

    screen_x = air_clamp_int(screen_x, 0, AIR_SCREEN_W - 1);
    screen_y = air_clamp_int(screen_y, 0, AIR_SCREEN_H - 1);

    s_latest.valid = true;

    /*
     * Store display-oriented raw coordinate.
     * This is more useful for debug because it matches screen movement.
     */
    s_latest.raw_x = (int16_t)disp_raw_x;
    s_latest.raw_y = (int16_t)disp_raw_y;

    s_latest.screen_x = (int16_t)screen_x;
    s_latest.screen_y = (int16_t)screen_y;
    s_latest.area = (uint16_t)area;
    s_latest.timestamp_ms = air_now_ms();

    int conf = area * 3;

    if (conf > 100) {
        conf = 100;
    }

    if (conf < 10) {
        conf = 10;
    }

    s_latest.confidence = (uint8_t)conf;

    if ((s_frame_count % 10) == 0) {
        ESP_LOGI(TAG,
                 "valid=1 cam_raw=(%d,%d) disp_raw=(%d,%d) screen=(%d,%d) area=%d conf=%d frames=%lu",
                 cam_raw_x,
                 cam_raw_y,
                 disp_raw_x,
                 disp_raw_y,
                 screen_x,
                 screen_y,
                 area,
                 conf,
                 (unsigned long)s_frame_count);
    }
}

bool air_pointer_get_latest_raw(air_pointer_result_t *out_result)
{
    if (out_result == NULL) {
        return false;
    }

    *out_result = s_latest;
    return s_latest.valid;
}


#define AIR_AI_BRIDGE_HOLD_MS           260
#define AIR_AI_BRIDGE_MIN_CONF          35

static air_pointer_result_t s_ai_bridge_last;
static bool s_ai_bridge_has_last = false;
static int64_t s_ai_bridge_last_us = 0;

static bool air_pointer_try_bridge_latest(air_pointer_result_t *out_result)
{
    if (out_result == NULL) {
        return false;
    }

    if (!s_ai_bridge_has_last) {
        return false;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t age_ms = (now_us - s_ai_bridge_last_us) / 1000;

    if (age_ms < 0 || age_ms > AIR_AI_BRIDGE_HOLD_MS) {
        return false;
    }

    *out_result = s_ai_bridge_last;
    out_result->valid = true;

    int conf = (int)s_ai_bridge_last.confidence;
    int decay = (int)((age_ms * 60) / AIR_AI_BRIDGE_HOLD_MS);
    conf -= decay;

    if (conf < AIR_AI_BRIDGE_MIN_CONF) {
        conf = AIR_AI_BRIDGE_MIN_CONF;
    }

    out_result->confidence = (uint8_t)conf;
    return true;
}

static void air_pointer_update_bridge_latest(const air_pointer_result_t *result)
{
    if (result == NULL || !result->valid) {
        return;
    }

    s_ai_bridge_last = *result;
    s_ai_bridge_last_us = esp_timer_get_time();
    s_ai_bridge_has_last = true;
}


bool air_pointer_get_latest(air_pointer_result_t *out_result)
{
    air_pointer_result_t raw = {0};

    if (!air_pointer_get_latest_raw(&raw)) {
        return air_pointer_try_bridge_latest(out_result);
    }

    if (!air_espdet_probe_ai_accept_recent_for_pointer(raw.raw_x,
                                                       raw.raw_y)) {
        return air_pointer_try_bridge_latest(out_result);
    }

    *out_result = raw;
    air_pointer_update_bridge_latest(&raw);
    return true;
}