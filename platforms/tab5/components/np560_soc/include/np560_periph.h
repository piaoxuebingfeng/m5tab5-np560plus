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

#include <stdbool.h>
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
    /* 由宿主键盘任务置位、由模拟器任务消费；避免跨任务直接改 guest ICR。
     * 语义是"剩余需要补发的时基 tick 数"，不是一次性的 bool：原厂去抖要
     * 连续 2 次键盘回调、每次间隔 4 个 64Hz tick（共 8 tick）；icount 语义
     * 下每个 tick 要 38.4 万指令（真机 ~0.58s），单纯 raise 一次 IRQ 远不够。
     * 按住期间持续补 tick 才能把响应压回原厂的 ~125ms。 */
    volatile uint32_t key_wake_ticks;
    uint32_t stat_key_wake_irq;  /* 实际注入并被接受时基 IRQ 累计数（诊断） */
    /* ADC 电量比较器（对应 QEMU NP560ADCState，F2D0）。 */
    uint16_t adc_control;
    uint8_t adc_level;      /* 0..8，对齐 QEMU adc-level 属性，默认 8 满电 */
    uint32_t adc_reads;
    uint32_t adc_writes;
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

/* 请求一次键盘按下唤醒。模拟器任务在后续 burst 内注入时基 tick（补足
 * 去抖需要的 8 次真实 ISR），促使原厂扫描器尽快读取 F160。 */
void np560_periph_request_key_wake(Np560Periph *periph);

/* 请求一次键盘释放唤醒。原厂去抖门用 2A8C 计数 + 2A94 rearm 锁存：
 * 按键释放后必须至少跑一次"无键扫描"才能重新置 2A94=1、清 2A8C=0，
 * 否则下次短按会走连发路径（计数需到 6）而不响应。补 8 tick（2 次
 * 扫描）让 rearm 立即完成，消除"按一次有、紧接着按一次无"的交替现象。 */
void np560_periph_request_key_release(Np560Periph *periph);

/* ADC 电量比较器（F2D0，对齐 QEMU np560_adc_read/write）。固件采样器
 * 4F:C336 写 (threshold<<2)|2 后读回，bit7 为比较结果；level=8 时所有
 * threshold 0..7 都断言，采样器返回满电等级 8，不再触发低电事件。 */
uint16_t np560_adc_read_reg(Np560Periph *periph, unsigned offset, unsigned size);
void np560_adc_write_reg(Np560Periph *periph, unsigned offset, uint16_t value,
                         unsigned size);

#ifdef __cplusplus
}
#endif

#endif /* NP560_PERIPH_H */
