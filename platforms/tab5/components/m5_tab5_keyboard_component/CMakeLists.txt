idf_component_register(
    SRCS
        "src/m5_tab5_keyboard.cpp"
    INCLUDE_DIRS
        "src"
    REQUIRES
        "esp_driver_gpio"
        "esp_driver_i2c"
)

# np560 移植裁剪：不引入 espressif/i2c_bus，仅用原生 i2c_master 路径。
target_compile_definitions(${COMPONENT_LIB} PUBLIC M5_TAB5_KB_NO_I2C_BUS)
