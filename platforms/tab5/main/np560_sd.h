/*
 * np560_sd.h - NP560+ 模拟器 microSD 存储层（ESP32 侧）。
 */
#pragma once

#include <cstdint>

extern "C" {
struct Np560Bus;
struct Np560Nand;

/* 挂载 TF 卡到 /sd（用户约定：模拟器文件放 /sd/noahnp560/）。 */
bool np560_sd_mount();

/* 装载真实固件（NP560.PRG -> phys 0x30000、resident.bin -> code ROM、
 * data 平面 ROM 镜像），并确保 system.nand 就绪。失败返回 false。 */
bool np560_sd_load_firmware(Np560Bus *bus);

/* 打开 system.nand 并作为 NAND 控制器介质挂接。 */
bool np560_sd_attach_nand(Np560Nand *nand);
}
