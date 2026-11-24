/*
 * dmcu_irq.h - ICR 中断控制器 / DTU 数据传输单元 / 中断嵌套。
 *
 * 移植自 qemu-dmcu/target/dmcu：
 *   cpu.c  dmcu_irq_priority(34)/dmcu_irq_select(39)/dmcu_irq_or_dtu_pending(75)
 *          dmcu_irq_update(110)/dmcu_irq_raise(122)
 *          dmcu_interrupt_push(161)/dmcu_interrupt_suspend(205)
 *          dmcu_interrupt_complete(216)/dmcu_interrupt_enter(242)
 *          dmcu_software_trap(388)/dmcu_cpu_exec_interrupt(404)
 *   dtu.c  dmcu_dtu_request(77)/dmcu_dtu_address(100)/dmcu_dtu_run(118)
 *
 * 语义依据：doc/cpu/np560_interrupt_nesting.md（嵌套/重复影子/帧格式）、
 * doc/cpu/np560_icr_vector_dispatch.md（向量表）、doc/cpu/np560_dtu.md（DTU）、
 * CN1916884A 图 7B/12/13/16/17 与 pp.93/120/122。
 */
#ifndef DMCU_IRQ_H
#define DMCU_IRQ_H

#include "dmcu_exec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ICR 寄存器组 REG[0x50..0x6F]（cpu.h:336-341）。 */
#define DMCU_ICR_FIRST  0x50u
#define DMCU_ICR_COUNT  32u
#define DMCU_ICR_EN     0x0001u
#define DMCU_ICR_RQ     0x0002u
#define DMCU_ICR_PRI    0x0030u
#define DMCU_ICR_ED     0x0040u

/* DTU 通道控制 REG[0x21..0x24]（cpu.h:342-350）。 */
#define DMCU_DTUC_FIRST 0x21u
#define DMCU_DTU_CHANNELS 4u
#define DMCU_DTUC_COUNT 0x03ffu
#define DMCU_DTUC_DDS   0x0800u
#define DMCU_DTUC_SDS   0x1000u
#define DMCU_DTUC_WBT   0x2000u
#define DMCU_DTUC_INC   0xc000u

/* 硬件 PSR 位（cpu.h:332-335；低 4 位 Z,V,C,N 由 sfr 转换层处理）。 */
#define DMCU_PSR_MV     0x0040u
#define DMCU_PSR_MS     0x0080u
#define DMCU_PSR_CPRI   0x3000u
#define DMCU_PSR_CIRQ   0x4000u

/* SOCR.RAM（cpu.h:328）：向量基址选择（RAM=1 用段 0，RAM=0 用段 1）。 */
#define DMCU_SOCR_RAM   0x0002u
/* SOCR.MAS（cpu.h:331）：MAC 结果自动饱和。 */
#define DMCU_SOCR_MAS   0x0010u

/* 中断向量（np560_icr_vector_dispatch.md / cpu.c:446）。 */
#define DMCU_IRQ_VECTOR(source) \
    ((source) < 16 ? 0x40u + (source) * 8u : 0xc0u + ((source) - 16u) * 4u)

/*
 * 嵌套上下文 DMCUIrqContext 定义于 dmcu_cpu.h（与 DMCUCpu 同处）。
 * guest 寄存器/PSR/PC/SP/FP 不在上下文中保存——CPU 帧与 ISR 自己负责
 * （np560_interrupt_nesting.md 改动 3）。
 */

/* 正常递增的四种优先级可形成四层；软件改 PSR 后不假定同一上限。 */
#define DMCU_IRQ_MAX_DEPTH 8

/* 优先级仲裁（cpu.c:34/39/75）。 */
unsigned dmcu_irq_priority(unsigned icr);
int dmcu_irq_select(const DMCUCpu *cpu);
bool dmcu_irq_or_dtu_pending(const DMCUCpu *cpu);

/* 外设请求入口（cpu.c:122）。置 RQ、跑一次 DTU、标记需要重新仲裁。 */
void dmcu_irq_raise(Np560Bus *bus, DMCUCpu *cpu, unsigned source);

/* DTU 传输引擎（dtu.c:118 原样移植；返回是否处理了请求）。 */
bool dmcu_dtu_run(Np560Bus *bus, DMCUCpu *cpu);

/*
 * 指令边界仲裁（cpu.c dmcu_cpu_exec_interrupt 404-453 语义）。
 * CRIT/IGNDS 窗口期间拒绝进入；IE=0 不进入；DTU 传输不受限。
 * 由 dmcu_exec_step 在 irq_state_changed 边界调用。
 */
void dmcu_irq_service(Np560Bus *bus, DMCUCpu *cpu);

/* 同步软件陷阱（cpu.c:388；TRAP 0x0f，CN1916884A p124）。 */
void dmcu_software_trap(Np560Bus *bus, DMCUCpu *cpu, uint32_t return_pc,
                        unsigned trap);

/* RETI 簿记（helper.c:19879 Retire 段 + cpu.c:216）：frame_sp 为弹栈前
 * 的 SP（helper.c:19882）；匹配硬件帧才结算。 */
void dmcu_irq_reti_retire(Np560Bus *bus, DMCUCpu *cpu, uint32_t frame_sp);

#ifdef __cplusplus
}
#endif

#endif /* DMCU_IRQ_H */
