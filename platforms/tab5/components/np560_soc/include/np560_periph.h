/*
 * np560_periph.h - NP560+ 板级外设组合层。
 *
 * 对应 qemu-dmcu/hw/dmcu/np560.c 的 np560_sfr_device_read/write 分发：
 *   DAC0..DACC / AUDIO_PLL -> np560_audio_write
 *   其余计时寄存器（DTUC..TBC 含 T0..T2C/TBC）-> np560_timers_*
 * 并提供统一的时间推进入口（host 主循环 / ESP 任务循环调用）。
 */
#ifndef NP560_PERIPH_H
#define NP560_PERIPH_H

#include <stdint.h>
#include "dmcu_cpu.h"
#include "dmcu_bus.h"
#include "np560_timers.h"
#include "np560_audio.h"
#include "np560_lcd.h"
#include "np560_keypad.h"
#include "np560_ecc.h"
#include "np560_nand.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Np560Periph {
    Np560Timers timers;
    Np560Audio audio;
    Np560Lcd lcd;
    Np560Keypad keypad;
    Np560Ecc ecc;
    Np560Nand nand;
    DMCUCpu *cpu;
} Np560Periph;

/* 初始化并把 SFR 设备钩子挂到 CPU、LCD 字节 MMIO 与 word MMIO（键盘/
 * ECC/NAND/SOCR 视图）钩子挂到总线（对应 np560_timers_init +
 * np560_audio_init + np560_st7529_reset + np560_keyboard_reset +
 * np560_nand_reset + cpu->sfr_device_* / bus->mmio_*8 / bus->mmio_*）。 */
void np560_periph_init(Np560Periph *periph, DMCUCpu *cpu, Np560Bus *bus);

/* 节点②诊断：取走 SFR/MMIO 读写热点计数（各 512 项：0x00..0xFF=SFR reg，
 * 0x100..0x1FF=MMIO word 区（physical>>1），取走即清零）。 */
void np560_periph_diag_take(Np560Periph *s, uint32_t *reads, uint32_t *writes);

/* 统一时间推进：定时器到期 + 音频请求节拍。主循环每批指令后调用，
 * delta_cpu_cycles 为自上次调用以来执行的 guest 周期数。 */
void np560_periph_advance(Np560Periph *periph, uint64_t delta_cpu_cycles);

#ifdef __cplusplus
}
#endif

#endif /* NP560_PERIPH_H */
