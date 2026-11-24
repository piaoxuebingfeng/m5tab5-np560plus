/*
 * dmcu_clock.h - DMCU 时钟域。
 *
 * 从 qemu-dmcu/target/dmcu/clock.c 移植。PLL/CLKCON 选择逻辑、外部数据
 * 等待状态与已文档化的指令成本表保持原语义。
 */
#ifndef DMCU_CLOCK_H
#define DMCU_CLOCK_H

#include "dmcu_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 当前系统时钟频率（PLL 4MHz×N/D 与 CLKCON 选择，或 32.768k/16.384M/24.576M）。 */
uint32_t dmcu_system_clock_hz(const DMCUCpu *cpu);

/* 已文档化指令成本（clock.c dmcu_documented_cycles）。0 表示尚未确立。 */
unsigned dmcu_decode_cycles(uint16_t word, uint32_t pc);

/* 推进虚拟周期计数。 */
void dmcu_clock_charge(DMCUCpu *cpu, uint64_t cycles);

#ifdef __cplusplus
}
#endif

#endif /* DMCU_CLOCK_H */
