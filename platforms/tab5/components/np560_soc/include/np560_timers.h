/*
 * np560_timers.h - NP560+ 三路周期定时器 + 32.768kHz 时基。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560_timer.c（ptimer → CPU 周期累加器）。
 * CN1916884A 图 23/24/25：计数/比较/重装架构；原厂 08:3930/47A4 提供分频
 * 与周期-1 装载；24:97AC/9A1E 用 T0/T1/T2 做 DAC 传输。
 *
 * 语义要点（np560_timer.c 注释原样保留）：
 *  - TxC 仅支持周期模式 (control & ~TON) == 0x0080；RQ 是锁存器，
 *    EN/IE 屏蔽投递但不清除——由 CPU 接受中断源时清零。
 *  - 计数器是上计数视图 limit - count；写 Tx 等价于设置下计数值。
 *  - 启用时从完整周期重启（重编程相位未在文档中定义）。
 *  - 时基：32768 >> select Hz，selector 15 未用；limit 恒为 1，
 *    每个 tick 触发（TBC 的 0494/0496/0498 三字是 guest RAM，不是设备）。
 */
#ifndef NP560_TIMERS_H
#define NP560_TIMERS_H

#include <stdbool.h>
#include <stdint.h>
#include "dmcu_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP560_PERIODIC_TIMERS 3

typedef struct Np560PeriodicTimer {
    DMCUCpu *cpu;
    unsigned index;          /* 0..2；时基为 3 */
    uint32_t frequency_hz;   /* 定时器节拍频率 */
    uint32_t limit;          /* TxP + 1（ptimer limit） */
    uint32_t down_count;     /* ptimer 下计数值 */
    bool running;
    /* 分数节拍累加器：acc_num += delta_cpu_cycles * frequency_hz，
     * acc_num >= cpu_hz 时推进一个 timer tick（精确整数运算）。 */
    uint64_t acc_num;
    uint64_t expirations;
    uint64_t unsupported_configs;
} Np560PeriodicTimer;

typedef struct Np560Timers {
    DMCUCpu *cpu;
    Np560PeriodicTimer channel[NP560_PERIODIC_TIMERS];
    Np560PeriodicTimer timebase;
} Np560Timers;

/* 初始化并挂接 SFR 设备钩子（board 的 np560_sfr_device_* 分发由调用方
 * 组合 np560_timers_* 与 np560_audio_* 完成）。 */
void np560_timers_init(Np560Timers *timers, DMCUCpu *cpu);

/* SFR 设备接口（由板级分发调用）。 */
uint16_t np560_timers_read(Np560Timers *timers, unsigned reg, uint16_t value);
void np560_timers_write(Np560Timers *timers, unsigned reg,
                        uint16_t old_value, uint16_t value);
void np560_timers_reset(Np560Timers *timers);

/* 时间推进：host 主循环 / 测试在每批指令后调用。到期触发
 * dmcu_irq_raise(cpu, 1+index)（时基为源 0）。 */
void np560_timers_advance(Np560Timers *timers, uint64_t delta_cpu_cycles);

#ifdef __cplusplus
}
#endif

#endif /* NP560_TIMERS_H */
