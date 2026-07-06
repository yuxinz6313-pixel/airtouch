idf_component_register(
    SRCS 
        
        
        wm8960_codec.c
        app_audio.c
        main.cpp
        air_espdet_probe.cpp

        app_camera.c
        air_pointer.c
        air_cursor_ui.c
        air_input.c
        air_aim_trainer_ui.c
        airtouch_storage.c
        airtouch_cloud_net.c
                    airtouch_cloud_uart.c
        air_heatmap.c
        air_tof.c
	air_distance_guard_ui.c
	air_attention_heatmap.c

        ui_font_cn_20.c
        ui_font_cn_26.c
        ui_font_cn_32.c

    INCLUDE_DIRS 
        .

    PRIV_REQUIRES esp_driver_uart
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_gpio
        nvs_flash
        fatfs
        wt99p4c5_s1_board
        apps

        airtouch_tip_detect
        espressif__esp_hosted
        esp_event
        esp_netif
        esp_wifi
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE 
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

if(CONFIG_ESP_BROOKESIA_MEMORY_USE_CUSTOM)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "esp-brookesia" OR COMPONENT MATCHES "espressif__esp-brookesia")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component 'esp-brookesia' not found.")
    else()
        idf_component_get_property(ESP_BROOKESIA_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
    endif()

    target_compile_options(
        ${ESP_BROOKESIA_LIB}
        PUBLIC
            "-DESP_BROOKESIA_MEMORY_CUSTOM_MALLOC(x)=heap_caps_aligned_alloc(1, x, MALLOC_CAP_SPIRAM)"
            "-DESP_BROOKESIA_MEMORY_CUSTOM_FREE=free"
    )
endif()

if(CONFIG_LV_MEM_CUSTOM)
    target_compile_options(
        ${LVGL_LIB} 
        PRIVATE 
            "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
    )
endif()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)







