#include "airtouch_tip_detect.hpp"

#include "dl_detect_postprocessor.hpp"
#include "dl_math.hpp"
#include "esp_log.h"

#include <algorithm>
#include <cmath>

extern const uint8_t airtouch_tip_detect_espdl[] asm("_binary_airtouch_tip_detect_espdl_start");
static const char *path = (const char *)airtouch_tip_detect_espdl;

namespace airtouch_tip_detect {

static const char *TAG_AT_PICO = "airtouch_pico_pp";

class AirTouchPicoPostprocessor : public dl::detect::AnchorPointDetectPostprocessor {
private:
    template <typename T>
    void debug_score_stage(dl::TensorBase *score, const char *name)
    {
        if (score == nullptr) {
            ESP_LOGW(TAG_AT_PICO, "%s is null", name);
            return;
        }

        int H = score->shape[1];
        int W = score->shape[2];
        int C = score->shape[3];

        T *ptr = (T *)score->data;
        int total = H * W * C;

        T max_q = ptr[0];
        int max_i = 0;

        for (int i = 1; i < total; i++) {
            if (ptr[i] > max_q) {
                max_q = ptr[i];
                max_i = i;
            }
        }

        float exp = DL_SCALE(score->exponent);
        float max_raw = dl::dequantize(max_q, exp);
        float score_f = sqrtf(max_raw > 0 ? max_raw : 0);

        int c = max_i % C;
        int x = (max_i / C) % W;
        int y = (max_i / C) / W;

        ESP_LOGI(TAG_AT_PICO,
                 "%s shape=[%d,%d,%d] dtype=%d exponent=%d max_q=%d max_raw=%.6f score=%.6f at y=%d x=%d c=%d",
                 name,
                 H, W, C,
                 score->dtype,
                 score->exponent,
                 (int)max_q,
                 max_raw,
                 score_f,
                 y, x, c);
    }

    template <typename T>
    void parse_stage_with_box_name(dl::TensorBase *score,
                                   dl::TensorBase *box,
                                   const int stage_index)
    {
        if (score == nullptr || box == nullptr) {
            return;
        }

        int stride_y = m_stages[stage_index].stride_y;
        int stride_x = m_stages[stage_index].stride_x;

        int offset_y = m_stages[stage_index].offset_y;
        int offset_x = m_stages[stage_index].offset_x;

        int H = score->shape[1];
        int W = score->shape[2];
        int C = score->shape[3];

        T *score_ptr = (T *)score->data;
        T *box_ptr = (T *)box->data;

        float score_exp = DL_SCALE(score->exponent);
        float box_exp = DL_SCALE(box->exponent);

        T score_thr_quant = dl::quantize<T>(m_score_thr * m_score_thr, 1.f / score_exp);

        float inv_resize_scale_x = 1.f / m_resize_scale_x;
        float inv_resize_scale_y = 1.f / m_resize_scale_y;

        for (size_t y = 0; y < H; y++) {
            for (size_t x = 0; x < W; x++) {
                for (size_t c = 0; c < C; c++) {
                    if (*score_ptr > score_thr_quant) {
                        int center_y = y * stride_y + offset_y;
                        int center_x = x * stride_x + offset_x;

                        float box_data[32];
                        for (int i = 0; i < 32; i++) {
                            box_data[i] = dl::dequantize(box_ptr[i], box_exp);
                        }

                        dl::detect::result_t new_box = {
                            (int)c,
                            sqrtf(dl::dequantize(*score_ptr, score_exp)),
                            {
                                (int)((center_x - dl::math::dfl_integral(box_data, 7) * stride_x) * inv_resize_scale_x),
                                (int)((center_y - dl::math::dfl_integral(box_data + 8, 7) * stride_y) * inv_resize_scale_y),
                                (int)((center_x + dl::math::dfl_integral(box_data + 16, 7) * stride_x) * inv_resize_scale_x),
                                (int)((center_y + dl::math::dfl_integral(box_data + 24, 7) * stride_y) * inv_resize_scale_y),
                            },
                            {}
                        };

                        m_box_list.insert(
                            std::upper_bound(m_box_list.begin(),
                                             m_box_list.end(),
                                             new_box,
                                             dl::detect::greater_box),
                            new_box
                        );
                    }
                }

                score_ptr++;
                box_ptr += 32;
            }
        }
    }

public:
    using dl::detect::AnchorPointDetectPostprocessor::AnchorPointDetectPostprocessor;

    void postprocess() override
    {
        dl::TensorBase *score0 = m_model->get_intermediate("score0");
        dl::TensorBase *box0   = m_model->get_intermediate("box0");
        dl::TensorBase *score1 = m_model->get_intermediate("score1");
        dl::TensorBase *box1   = m_model->get_intermediate("box1");
        dl::TensorBase *score2 = m_model->get_intermediate("score2");
        dl::TensorBase *box2   = m_model->get_intermediate("box2");

        if (score0 == nullptr || box0 == nullptr ||
            score1 == nullptr || box1 == nullptr ||
            score2 == nullptr || box2 == nullptr) {
            ESP_LOGW(TAG_AT_PICO,
                     "missing tensor: score0=%p box0=%p score1=%p box1=%p score2=%p box2=%p",
                     score0, box0, score1, box1, score2, box2);
            return;
        }

        static int s_pp_count = 0;
        s_pp_count++;

        if ((s_pp_count % 10) == 1) {
            if (score0->dtype == dl::DATA_TYPE_INT8) {
                debug_score_stage<int8_t>(score0, "score0");
                debug_score_stage<int8_t>(score1, "score1");
                debug_score_stage<int8_t>(score2, "score2");
            } else {
                debug_score_stage<int16_t>(score0, "score0");
                debug_score_stage<int16_t>(score1, "score1");
                debug_score_stage<int16_t>(score2, "score2");
            }
        }

        if (score0->dtype == dl::DATA_TYPE_INT8) {
            parse_stage_with_box_name<int8_t>(score0, box0, 0);
            parse_stage_with_box_name<int8_t>(score1, box1, 1);
            parse_stage_with_box_name<int8_t>(score2, box2, 2);
        } else {
            parse_stage_with_box_name<int16_t>(score0, box0, 0);
            parse_stage_with_box_name<int16_t>(score1, box1, 1);
            parse_stage_with_box_name<int16_t>(score2, box2, 2);
        }

        nms();
    }
};

Pico::Pico(const char *model_name)
{
    m_model = new dl::Model(
        path,
        model_name,
        fbs::MODEL_LOCATION_IN_FLASH_RODATA
    );

#if CONFIG_IDF_TARGET_ESP32P4
    m_image_preprocessor =
        new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {1, 1, 1}, 0);
#else
    m_image_preprocessor =
        new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {1, 1, 1});
#endif

    m_postprocessor =
        new AirTouchPicoPostprocessor(
            m_model,
            0.15,
            0.5,
            10,
            {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}}
        );
}

} // namespace airtouch_tip_detect

AirTouchTipDetect::AirTouchTipDetect(const char *sdcard_model_dir, model_type_t model_type)
{
    (void)sdcard_model_dir;

    switch (model_type) {
    case model_type_t::PICO_S8_V1:
    default:
        m_model = new airtouch_tip_detect::Pico("airtouch_tip_detect_pico_s8_v1.espdl");
        break;
    }
}







