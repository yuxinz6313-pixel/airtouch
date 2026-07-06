include(${COMPONENT_DIR}/cmake/utilities.cmake)

set(src_dirs        .)
set(include_dirs    .)
set(requires        esp-dl)

set(models_dir ${COMPONENT_DIR}/models/p4)
set(model_file ${models_dir}/airtouch_tip_classifier_medium_96_0to1_defaultq.espdl)
set(packed_model ${BUILD_DIR}/espdl_models/airtouch_tip_detect.espdl)

idf_component_register(
    SRC_DIRS ${src_dirs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
)

file(MAKE_DIRECTORY ${BUILD_DIR}/espdl_models)

set(pack_model_exe ${COMPONENT_DIR}/pack_model.py)

add_custom_command(
    OUTPUT ${packed_model}
    COMMENT "Move and Pack AirTouch tip classifier model..."
    COMMAND python ${pack_model_exe} --model_path ${model_file} --out_file ${packed_model}
    DEPENDS ${model_file}
    VERBATIM
)

target_add_aligned_binary_data(${COMPONENT_LIB} ${packed_model} BINARY)
