/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
/*
 * NP560+ 模拟器入口：复用官方 HAL 完成硬件初始化
 * （摄像头/I2C/编解码/显示/触摸/USB），随后启动模拟器任务。
 * 原 mooncake 应用框架流程已停用（见 app/ 目录，保留编译）。
 */
#include "hal/hal_esp32.h"
#include "np560_emu.h"
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_log.h"

extern "C" void app_main(void)
{
    // 官方 HAL：一次性完成全部板级硬件初始化（含 LVGL 显示与触摸）
    auto hal = std::make_unique<HalEsp32>();
    hal->init();

    // NP560+ 模拟器
    np560_emu_start();

    while (true) {
        vTaskDelay(1000);
    }
}
