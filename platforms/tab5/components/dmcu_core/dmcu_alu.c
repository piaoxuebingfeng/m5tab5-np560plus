/*
 * dmcu_alu.c - ALU 与条件码。
 *
 * 移植自 helper.c alu_add (6780)/alu_sub (6797)/alu (6814)/condition (6850)。
 * C 标志是借位（sub）；Z 链接（chained_z）用于 SUBC/ADDC 保持旧 Z。
 */
#include "dmcu_exec.h"

static inline bool dmcu_flag(const DMCUCpu *cpu, uint32_t mask)
{
    return (cpu->psw & mask) != 0;
}

void dmcu_set_flag(DMCUCpu *cpu, uint32_t mask, bool value)
{
    cpu->psw = value ? cpu->psw | mask : cpu->psw & ~mask;
}

void dmcu_logic_flags(DMCUCpu *cpu, uint32_t result, int bits)
{
    uint32_t mask = dmcu_width_mask(bits);
    uint32_t sign = dmcu_width_sign(bits);

    result &= mask;
    dmcu_set_flag(cpu, DMCU_PSW_Z, result == 0);
    dmcu_set_flag(cpu, DMCU_PSW_N, result & sign);
    dmcu_set_flag(cpu, DMCU_PSW_C, false);
    dmcu_set_flag(cpu, DMCU_PSW_V, false);
}

static uint32_t alu_add(DMCUCpu *cpu, uint32_t left, uint32_t right,
                        uint32_t carry, int bits, bool chained_z)
{
    uint32_t mask = dmcu_width_mask(bits);
    uint32_t sign = dmcu_width_sign(bits);
    uint32_t total = left + right + carry;
    uint32_t result = total & mask;
    bool old_z = dmcu_flag(cpu, DMCU_PSW_Z);

    dmcu_set_flag(cpu, DMCU_PSW_C, total > mask);
    dmcu_set_flag(cpu, DMCU_PSW_V,
                  (~(left ^ right) & (left ^ result) & sign) != 0);
    dmcu_set_flag(cpu, DMCU_PSW_N, result & sign);
    dmcu_set_flag(cpu, DMCU_PSW_Z, chained_z ? old_z && result == 0 : result == 0);
    return result;
}

static uint32_t alu_sub(DMCUCpu *cpu, uint32_t left, uint32_t right,
                        uint32_t borrow, int bits, bool chained_z)
{
    uint32_t mask = dmcu_width_mask(bits);
    uint32_t sign = dmcu_width_sign(bits);
    uint32_t subtrahend = right + borrow;
    uint32_t result = (left - subtrahend) & mask;
    bool old_z = dmcu_flag(cpu, DMCU_PSW_Z);

    dmcu_set_flag(cpu, DMCU_PSW_C, left < subtrahend); /* C 为借位 */
    dmcu_set_flag(cpu, DMCU_PSW_V,
                  ((left ^ right) & (left ^ result) & sign) != 0);
    dmcu_set_flag(cpu, DMCU_PSW_N, result & sign);
    dmcu_set_flag(cpu, DMCU_PSW_Z, chained_z ? old_z && result == 0 : result == 0);
    return result;
}

uint32_t dmcu_alu_op(DMCUCpu *cpu, unsigned operation,
                     uint32_t left, uint32_t right, int bits)
{
    uint32_t mask = dmcu_width_mask(bits);
    uint32_t result;

    left &= mask;
    right &= mask;
    switch (operation) {
    case 0: return alu_add(cpu, left, right, 0, bits, false);
    case 1: return alu_add(cpu, left, right, dmcu_flag(cpu, DMCU_PSW_C) ? 1 : 0,
                           bits, true);
    case 2: return alu_sub(cpu, left, right, 0, bits, false);
    case 3: return alu_sub(cpu, left, right, dmcu_flag(cpu, DMCU_PSW_C) ? 1 : 0,
                           bits, true);
    case 4: result = left & right; break;
    case 5: result = left | right; break;
    case 6: return alu_sub(cpu, left, right, 0, bits, false); /* CMP 不回写 */
    case 7: result = left ^ right; break;
    default: return 0;
    }
    dmcu_logic_flags(cpu, result, bits);
    return result & mask;
}

bool dmcu_condition(const DMCUCpu *cpu, unsigned code)
{
    bool z = (cpu->psw & DMCU_PSW_Z) != 0;
    bool c = (cpu->psw & DMCU_PSW_C) != 0;
    bool n = (cpu->psw & DMCU_PSW_N) != 0;
    bool v = (cpu->psw & DMCU_PSW_V) != 0;

    switch (code) {
    case 0x0: return true;
    case 0x1: return z;
    case 0x2: return !z;
    case 0x3: return v;
    case 0x4: return !v;
    case 0x5: return c;
    case 0x6: return !c;
    case 0x7: return n;
    case 0x8: return !n;
    case 0x9: return n != v;
    case 0xa: return !(z || c);
    case 0xb: return !((n != v) || z);
    case 0xc: return z || c;
    case 0xd: return (n != v) || z;
    case 0xe: return n == v;
    default: return false;
    }
}
