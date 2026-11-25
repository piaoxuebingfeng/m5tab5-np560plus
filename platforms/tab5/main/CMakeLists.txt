file(GLOB_RECURSE APP_LAYER_SRCS
    ../../../app/*.c
    ../../../app/*.cc
    ../../../app/*.cpp
)

set(APP_LAYER_INCS
    ../../../app
)

file(GLOB_RECURSE MY_HAL_SRCS
    ./hal/*.c
    ./hal/*.cc
    ./hal/*.cpp
)

idf_component_register(SRCS "app_main.cpp" "np560_emu.cpp" "np560_tab5_keyboard.cpp" "np560_tab5_touch.cpp" "np560_sd.cpp" ${APP_LAYER_SRCS} ${MY_HAL_SRCS}
                    INCLUDE_DIRS "." ${APP_LAYER_INCS}
                    PRIV_REQUIRES dmcu_core np560_soc m5_tab5_keyboard_component m5stack_tab5 lvgl mooncake mooncake_log smooth_ui_toolkit esp_lvgl_port power_monitor_ina226 sensor_bmi270 sensor_icm20602 esp_cam_sensor esp_video esp_sccb_intf imlib esp_wifi esp_netif esp_http_server usb esp_lcd_touch esp_lcd_touch_gt911 usb_host_hid nvs_flash esp_wifi_remote esp_hosted esp_event esp_driver_i2c esp_driver_sdmmc fatfs
                    EMBED_TXTFILES "../audio/canon_in_d.mp3" "../audio/startup_sfx.mp3" "../audio/shutdown_sfx.mp3")
