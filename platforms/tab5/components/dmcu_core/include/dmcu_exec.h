/*
 * dmcu_exec.h - 指令执行接口。
 */
#ifndef DMCU_EXEC_H
#define DMCU_EXEC_H

#include "dmcu_cpu.h"
#include "dmcu_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 取指并执行一条指令。
 * 返回 DMCU_EXEC_NEXT / LOOKUP / EXIT；遇到未移植指令族时置 halted 并返回
 * DMCU_EXEC_EXIT（对应 QEMU unsupported() + EXCP_HLT）。
 *
 * 移植状态（对齐 helper.c 指令族函数）：
 *   [x] exec_move    0x08..0xb9（helper.c:7102）
 *   [x] exec_alu     ADD/SUB/CMP/AND/OR/XOR 族（helper.c:7023）
 *   [x] exec_shift   SL/SR/ASR/ASL/ROL/ROR/OSB 族（helper.c:7238）
 *   [x] exec_bit     BST/BCLD/BAND/BOR/BXOR/BMOV/BMOVN/BCMP（helper.c:7415）
 *   [x] exec_control 条件跳转/SJMP/SCALL/RETS/RETF/RETI（helper.c:18346 语义核心）
 *   [x] MUL/UMUL（0xc0/0xc1）PUSH/POP SFR（0x1f）LINK/UNLINK（0x2e/0x2f）
 *   [x] NEG（0x3e）CRIT/IGNDS（0x3f）REPB/REP（0xae/0xaf/0xbf/0xcf）RESET（0x3fff）
 *   [x] exec_compare_update  helper.c:20006（0x4e..0x9f 同族）
 *   [x] exec_mac             helper.c:20046（0xbe，X/Y 指针 + R5:R6:R7 40 位累加器）
 *   [x] exec_divide          helper.c:20174（0xc2..0xc5）
 *   [x] exec_exponent        helper.c:20215（0x1e，结果写 RB10）
 *   [x] TRAP（0x0f）+ 中断系统（ICR 仲裁/嵌套/DTU，见 dmcu_irq.c）
 *   [ ] 中断真实外设来源（定时器/DAC 事件 → dmcu_irq_raise，板级接线）
 */
uint32_t dmcu_exec_step(Np560Bus *bus, DMCUCpu *cpu);

/* 访问原语（供各指令族使用；dmcu_data.c / dmcu_regs.c 实现）。 */
uint32_t dmcu_reg_read(DMCUCpu *cpu, unsigned reg, int bits);
void dmcu_reg_write(DMCUCpu *cpu, unsigned reg, uint32_t value, int bits);
uint32_t dmcu_sfr_read(DMCUCpu *cpu, unsigned reg, int bits);
void dmcu_sfr_write(DMCUCpu *cpu, unsigned reg, uint32_t value, int bits);

uint32_t dmcu_data_read(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, int bits);
void dmcu_data_write(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, uint32_t value, int bits);

uint32_t dmcu_pointer_read(Np560Bus *bus, DMCUCpu *cpu, unsigned reg, uint32_t addr, int bits);
void dmcu_pointer_write(Np560Bus *bus, DMCUCpu *cpu, unsigned reg, uint32_t addr, uint32_t value, int bits);
uint32_t dmcu_pointer_postincrement(DMCUCpu *cpu, unsigned reg, uint32_t addr, int32_t increment);

/* 循环缓冲 AGU（dmcu_data.c；memory MAC 使用，helper.c:6571）。 */
uint32_t dmcu_pointer_modifier(const DMCUCpu *cpu, unsigned reg);
bool dmcu_pointer_circular_valid(const DMCUCpu *cpu, unsigned reg, uint32_t addr);

void dmcu_push(Np560Bus *bus, DMCUCpu *cpu, uint32_t value);
uint32_t dmcu_pop(Np560Bus *bus, DMCUCpu *cpu);

/* 位访问（dmcu_data.c；exec_bit/位测试跳转使用）。 */
uint32_t dmcu_bit_read(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, unsigned bitno);
void dmcu_bit_write(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, unsigned bitno, bool value);

/* ALU（dmcu_alu.c）。operation: 0=ADD 1=ADDC 2=SUB 3=SUBC 4=AND 5=OR 6=CMP 7=XOR */
uint32_t dmcu_alu_op(DMCUCpu *cpu, unsigned operation,
                     uint32_t left, uint32_t right, int bits);

/* 标志原语（dmcu_alu.c；供 shift/bit/控制流族使用）。 */
void dmcu_set_flag(DMCUCpu *cpu, uint32_t mask, bool value);
void dmcu_logic_flags(DMCUCpu *cpu, uint32_t result, int bits);

/* 未支持指令停机（helper.c unsupported() 的移植）。 */
void dmcu_unsupported(Np560Bus *bus, DMCUCpu *cpu, uint32_t pc, uint16_t word);

#ifdef __cplusplus
}
#endif

#endif /* DMCU_EXEC_H */
