#include "air_espdet_probe.h"
#include "air_pointer.h"

#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <map>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"

static const char *TAG = "air_cls_probe";

#define CLS_IN_W 96
#define CLS_IN_H 96
#define AIR_MASK_W 160
#define AIR_MASK_H 120

/*
 * Current camera is 1280x960, while the classifier dataset was generated
 * from 320x240 images with 96x96 crops.
 * 96 / 320 = 384 / 1280, so use 384x384 source crop and resize to 96x96.
 */
#define CLS_SRC_CROP_SIZE 384

#define CLS_ACCEPT_THR 0.70f
#define CLS_GATE_RECENT_MS 1500
#define CLS_GATE_RAW_RADIUS 36
#define CLS_FAST_UNLOCK_MS 320

extern const uint8_t airtouch_tip_detect_espdl[] asm("_binary_airtouch_tip_detect_espdl_start");
extern const uint8_t airtouch_tip_detect_espdl_end[] asm("_binary_airtouch_tip_detect_espdl_end");

static dl::Model *s_cls_model = nullptr;
static dl::TensorBase *s_model_input = nullptr;
static dl::TensorBase *s_model_output = nullptr;
static dl::TensorBase *s_input_float_tensor = nullptr;
static dl::TensorBase *s_output_float_tensor = nullptr;

static float *s_input_f32 = nullptr;

static bool s_started = false;
static uint32_t s_process_count = 0;
static uint32_t s_infer_count = 0;
static bool s_infer_busy = false;

static volatile bool s_ai_has_accept = false;
static volatile uint32_t s_ai_accept_ms = 0;
static volatile int16_t s_ai_accept_raw_x = -1;
static volatile int16_t s_ai_accept_raw_y = -1;
static volatile float s_ai_latest_p_real = 0.0f;
static volatile bool s_ai_latest_accept = false;

static inline int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static inline int abs_int(int v)
{
    return (v < 0) ? -v : v;
}

static inline void rgb565_to_rgb888_from_buf(const uint8_t *p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);

    uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((v >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(v & 0x1F);

    *r = (uint8_t)((r5 * 255 + 15) / 31);
    *g = (uint8_t)((g6 * 255 + 31) / 63);
    *b = (uint8_t)((b5 * 255 + 15) / 31);
}

static void print_tensor_info(const char *name, dl::TensorBase *t)
{
    if (t == nullptr) {
        ESP_LOGW(TAG, "%s tensor is null", name);
        return;
    }

    std::vector<int> shape = t->get_shape();

    char shape_buf[96];
    int off = 0;
    off += snprintf(shape_buf + off, sizeof(shape_buf) - off, "[");
    for (size_t i = 0; i < shape.size() && off < (int)sizeof(shape_buf); i++) {
        off += snprintf(shape_buf + off,
                        sizeof(shape_buf) - off,
                        "%s%d",
                        (i == 0) ? "" : ",",
                        shape[i]);
    }
    snprintf(shape_buf + off, sizeof(shape_buf) - off, "]");

    ESP_LOGI(TAG,
             "%s shape=%s dtype=%s exp=%d bytes=%d",
             name,
             shape_buf,
             t->get_dtype_string(),
             t->get_exponent(),
             t->get_bytes());
}

static bool prepare_roi_from_pointer(const uint8_t *camera_buf,
                                     uint32_t camera_w,
                                     uint32_t camera_h,
                                     size_t camera_buf_len,
                                     int *out_center_x,
                                     int *out_center_y,
                                     int16_t *out_raw_x,
                                     int16_t *out_raw_y)
{
    if (camera_buf == nullptr || camera_w == 0 || camera_h == 0) {
        return false;
    }

    if (camera_buf_len < (size_t)camera_w * camera_h * 2) {
        ESP_LOGW(TAG,
                 "camera buf too small: len=%u need=%u",
                 (unsigned)camera_buf_len,
                 (unsigned)(camera_w * camera_h * 2));
        return false;
    }

    air_pointer_result_t p = {};
    bool has_pointer = air_pointer_get_latest_raw(&p);

    if (!has_pointer || !p.valid) {
        return false;
    }

    if (out_raw_x) {
        *out_raw_x = p.raw_x;
    }
    if (out_raw_y) {
        *out_raw_y = p.raw_y;
    }

    /*
     * air_pointer.h stores display-oriented raw coordinate.
     * Current config invert_y=1, so convert display raw back to camera mask raw.
     */
    int cam_mask_x = (int)p.raw_x;
    int cam_mask_y = (AIR_MASK_H - 1) - (int)p.raw_y;

    cam_mask_x = clamp_int(cam_mask_x, 0, AIR_MASK_W - 1);
    cam_mask_y = clamp_int(cam_mask_y, 0, AIR_MASK_H - 1);

    int center_x = (int)(((int64_t)cam_mask_x * camera_w + camera_w / 2) / AIR_MASK_W);
    int center_y = (int)(((int64_t)cam_mask_y * camera_h + camera_h / 2) / AIR_MASK_H);

    center_x = clamp_int(center_x, 0, (int)camera_w - 1);
    center_y = clamp_int(center_y, 0, (int)camera_h - 1);

    if (out_center_x) {
        *out_center_x = center_x;
    }
    if (out_center_y) {
        *out_center_y = center_y;
    }

    const int src_size = CLS_SRC_CROP_SIZE;
    const int src_half = src_size / 2;

    for (int oy = 0; oy < CLS_IN_H; oy++) {
        int sy = center_y - src_half + (oy * src_size + src_size / 2) / CLS_IN_H;
        sy = clamp_int(sy, 0, (int)camera_h - 1);

        for (int ox = 0; ox < CLS_IN_W; ox++) {
            int sx = center_x - src_half + (ox * src_size + src_size / 2) / CLS_IN_W;
            sx = clamp_int(sx, 0, (int)camera_w - 1);

            const uint8_t *pix = camera_buf + ((size_t)sy * camera_w + sx) * 2;

            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            rgb565_to_rgb888_from_buf(pix, &r, &g, &b);

            int idx = oy * CLS_IN_W + ox;

            /*
             * ESP-DL exported input is NHWC: [1,96,96,3].
             */
            int base = idx * 3;
            s_input_f32[base + 0] = (float)r / 255.0f;
            s_input_f32[base + 1] = (float)g / 255.0f;
            s_input_f32[base + 2] = (float)b / 255.0f;
        }
    }

    return true;
}

extern "C" esp_err_t air_espdet_probe_start(void)
{
    if (s_started) {
        ESP_LOGI(TAG, "already started");
        return ESP_OK;
    }

    const size_t model_len = (size_t)(airtouch_tip_detect_espdl_end - airtouch_tip_detect_espdl);

    ESP_LOGI(TAG, "AirTouch classifier load begin");
    ESP_LOGI(TAG, "packed espdl addr=%p len=%u", airtouch_tip_detect_espdl, (unsigned)model_len);
    ESP_LOGI(TAG, "model name: airtouch_tip_classifier_medium_96_0to1_defaultq.espdl");

    s_cls_model = new dl::Model((const char *)airtouch_tip_detect_espdl,
                                "airtouch_tip_classifier_medium_96_0to1_defaultq.espdl",
                                fbs::MODEL_LOCATION_IN_FLASH_RODATA);

    if (s_cls_model == nullptr) {
        ESP_LOGE(TAG, "new dl::Model returned nullptr");
        return ESP_FAIL;
    }

    std::map<std::string, dl::TensorBase *> &inputs = s_cls_model->get_inputs();
    std::map<std::string, dl::TensorBase *> &outputs = s_cls_model->get_outputs();

    if (inputs.size() != 1 || outputs.size() != 1) {
        ESP_LOGE(TAG,
                 "unexpected input/output count: inputs=%u outputs=%u",
                 (unsigned)inputs.size(),
                 (unsigned)outputs.size());
        return ESP_FAIL;
    }

    s_model_input = inputs.begin()->second;
    s_model_output = outputs.begin()->second;

    print_tensor_info("model_input", s_model_input);
    print_tensor_info("model_output", s_model_output);

    s_input_f32 = (float *)heap_caps_malloc(CLS_IN_W * CLS_IN_H * 3 * sizeof(float),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_input_f32 == nullptr) {
        ESP_LOGE(TAG, "failed to alloc input float buffer");
        return ESP_ERR_NO_MEM;
    }

    s_input_float_tensor = new dl::TensorBase({1, CLS_IN_H, CLS_IN_W, 3},
                                              s_input_f32,
                                              0,
                                              dl::DATA_TYPE_FLOAT,
                                              false);

    s_output_float_tensor = new dl::TensorBase(s_model_output->get_shape(),
                                               nullptr,
                                               0,
                                               dl::DATA_TYPE_FLOAT);

    if (s_input_float_tensor == nullptr || s_output_float_tensor == nullptr) {
        ESP_LOGE(TAG, "failed to create float tensors");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;

    ESP_LOGI(TAG, "classifier model loaded OK");
    ESP_LOGI(TAG, "AI gate enabled: public pointer is filtered by classifier");

    return ESP_OK;
}

extern "C" esp_err_t air_espdet_probe_process_rgb565_once(uint8_t *camera_buf,
                                                          uint32_t camera_w,
                                                          uint32_t camera_h,
                                                          size_t camera_buf_len)
{
    if (!s_started || s_cls_model == nullptr || s_model_input == nullptr || s_model_output == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_infer_busy) {
        return ESP_OK;
    }

    s_infer_busy = true;
    s_process_count++;

    int center_x = -1;
    int center_y = -1;
    int16_t raw_x = -1;
    int16_t raw_y = -1;

    bool roi_ok = prepare_roi_from_pointer(camera_buf,
                                           camera_w,
                                           camera_h,
                                           camera_buf_len,
                                           &center_x,
                                           &center_y,
                                           &raw_x,
                                           &raw_y);

    if (!roi_ok) {
        if ((s_process_count % 20) == 0) {
            ESP_LOGI(TAG, "CLS skip: no valid pointer candidate, probe_count=%u", (unsigned)s_process_count);
        }
        s_infer_busy = false;
        return ESP_OK;
    }

    int64_t t0 = esp_timer_get_time();

    bool assign_ok = s_model_input->assign(s_input_float_tensor);
    if (!assign_ok) {
        ESP_LOGE(TAG, "model_input assign failed");
        s_infer_busy = false;
        return ESP_FAIL;
    }

    s_cls_model->run();

    bool out_ok = s_output_float_tensor->assign(s_model_output);
    if (!out_ok) {
        ESP_LOGE(TAG, "model_output dequant assign failed");
        s_infer_busy = false;
        return ESP_FAIL;
    }

    int64_t t1 = esp_timer_get_time();

    float *out = s_output_float_tensor->get_element_ptr<float>();
    if (out == nullptr) {
        ESP_LOGE(TAG, "output ptr is null");
        s_infer_busy = false;
        return ESP_FAIL;
    }

    /*
     * Output order: [noise, real_tip]
     */
    float logit_noise = out[0];
    float logit_real = out[1];

    float m = (logit_noise > logit_real) ? logit_noise : logit_real;
    float e0 = expf(logit_noise - m);
    float e1 = expf(logit_real - m);
    float p_real = e1 / (e0 + e1 + 1e-6f);

    bool accept = (p_real >= CLS_ACCEPT_THR);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_ai_latest_p_real = p_real;
    s_ai_latest_accept = accept;

    if (accept) {
        s_ai_has_accept = true;
        s_ai_accept_ms = now_ms;
        s_ai_accept_raw_x = raw_x;
        s_ai_accept_raw_y = raw_y;
    }

    s_infer_count++;

    ESP_LOGI(TAG,
             "CLS #%u center=(%d,%d) logits=[noise=%.3f real=%.3f] p_real=%.3f accept=%d time=%lld us",
             (unsigned)s_infer_count,
             center_x,
             center_y,
             logit_noise,
             logit_real,
             p_real,
             accept ? 1 : 0,
             (long long)(t1 - t0));

    s_infer_busy = false;
    return ESP_OK;
}


extern "C" bool air_espdet_probe_ai_accept_recent_for_pointer(int16_t raw_x,
                                                              int16_t raw_y)
{
    if (!s_started) {
        return true;
    }

    if (!s_ai_has_accept) {
        return false;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t age_ms = now_ms - s_ai_accept_ms;

    if (age_ms > CLS_GATE_RECENT_MS) {
        return false;
    }

    /*
     * Fast-motion unlock:
     * Right after AI confirms a real tip, allow the color tracker to move fast
     * for a very short time. This prevents long-distance fast motion from being
     * rejected only because it is far from the last AI-confirmed coordinate.
     * Keep this lower than dwell time to avoid stable false click.
     */
    if (age_ms <= CLS_FAST_UNLOCK_MS) {
        return true;
    }

    int dx = abs_int((int)raw_x - (int)s_ai_accept_raw_x);
    int dy = abs_int((int)raw_y - (int)s_ai_accept_raw_y);

    if (dx > CLS_GATE_RAW_RADIUS || dy > CLS_GATE_RAW_RADIUS) {
        return false;
    }

    return true;
}

