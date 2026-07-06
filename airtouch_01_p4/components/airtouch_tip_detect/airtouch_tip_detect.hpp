#pragma once

#include "dl_detect_base.hpp"
#include "dl_detect_pico_postprocessor.hpp"

namespace airtouch_tip_detect {
class Pico : public dl::detect::DetectImpl {
public:
    Pico(const char *model_name);
};
} // namespace airtouch_tip_detect

class AirTouchTipDetect : public dl::detect::DetectWrapper {
public:
    typedef enum { PICO_S8_V1 } model_type_t;
    AirTouchTipDetect(const char *sdcard_model_dir = nullptr,
                      model_type_t model_type = PICO_S8_V1);
};