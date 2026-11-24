/*
 * dmcu_regs.c - 寄存器与 SFR 访问原语。
 *
 * 移植自 helper.c reg_read/reg_write (1558)/sfr_read (1597)/sfr_write (1795)。
 * 字节寄存器 RBn 是字寄存器的高低字节（RB0/RB2/... 为高字节；RB1/RB3/... 为
 * 低字节）；SE 桶形移位指数架构别名到 RB10（R5 高字节）。
 */
#include "dmcu_exec.h"
#include "dmcu_irq.h"

/* 计时 SFR 区间（cpu.h:368 dmcu_sfr_has_timing 原样移植）。 */
bool dmcu_sfr_has_timing(unsigned reg)
{
    return (reg >= DMCU_DTUC_FIRST && reg <= DMCU_SFR_TBC) ||
           (reg >= DMCU_ICR_FIRST && reg < DMCU_ICR_FIRST + DMCU_ICR_COUNT) ||
           (reg >= DMCU_SFR_DAC0 && reg <= DMCU_SFR_DACC) ||
           reg == DMCU_SFR_PLL || reg == DMCU_SFR_AUDIO_PLL;
}

uint32_t dmcu_reg_read(DMCUCpu *cpu, unsigned reg, int bits)
{
    reg &= 15;
    if (bits == 8) {
        uint32_t word = cpu->gpr[reg >> 1] & MASK16;

        /* RB0/RB2/... 为高字节；RB1/RB3/... 为低字节。 */
        return reg & 1 ? word & 0xff : word >> 8;
    }
    return cpu->gpr[reg] & MASK16;
}

void dmcu_reg_write(DMCUCpu *cpu, unsigned reg, uint32_t value, int bits)
{
    reg &= 15;
    if (bits == 8) {
        unsigned word_reg = reg >> 1;

        if (reg & 1) {
            cpu->gpr[word_reg] = (cpu->gpr[word_reg] & 0xff00) | (value & 0xff);
        } else {
            cpu->gpr[word_reg] = (cpu->gpr[word_reg] & 0x00ff) |
                                 ((value & 0xff) << 8);
        }
        /* SE 桶形移位指数架构暴露为 RB10（R5/ARX 高字节）。 */
        if (reg == 10) {
            cpu->se = value & 0xff;
        }
    } else {
        cpu->gpr[reg] = value & MASK16;
        if (reg == 5) {
            cpu->se = (value >> 8) & 0xff;
        }
    }
}

uint32_t dmcu_sfr_read(DMCUCpu *cpu, unsigned reg, int bits)
{
    uint32_t value;

    reg &= 0xff;
    if (reg >= 0xf0) {
        return dmcu_reg_read(cpu, reg - 0xf0, bits);
    }
    if (reg == DMCU_SFR_SB) {
        /* SB 是架构移位缓冲：普通移位丢弃的位存放于此。 */
        return cpu->shift_buffer & (bits == 8 ? 0xff : MASK16);
    }
    if (reg == DMCU_SFR_PSW) {
        /* 硬件 PSR 位序为 Z,V,C,N（位 0..3）。 */
        value = cpu->sfr[reg] & ~0xfu;
        value |= cpu->psw & DMCU_PSW_Z ? 0x1 : 0;
        value |= cpu->psw & DMCU_PSW_V ? 0x2 : 0;
        value |= cpu->psw & DMCU_PSW_C ? 0x4 : 0;
        value |= cpu->psw & DMCU_PSW_N ? 0x8 : 0;
        return value & (bits == 8 ? 0xff : 0xffff);
    }
    /* 计时设备读（helper.c:1623-1631）：计数器 T0/T1/T2 回读上计数。 */
    value = cpu->sfr[reg];
    if (dmcu_sfr_has_timing(reg) && cpu->sfr_device_read) {
        value = cpu->sfr_device_read(cpu->sfr_device_opaque, reg,
                                     (uint16_t)value);
    }
    return value & (bits == 8 ? 0xff : MASK16);
}

void dmcu_sfr_write(DMCUCpu *cpu, unsigned reg, uint32_t value, int bits)
{
    reg &= 0xff;
    if (reg >= 0xf0) {
        dmcu_reg_write(cpu, reg - 0xf0, value, bits);
        return;
    }
    /* CN1916884A p16/fig.7A：CLKCON 写需要 SOCR.UP，被拒时不改任何
     * 状态（helper.c:1654-1659；所有写路径共用此门）。 */
    if (reg == DMCU_SFR_CLKCON &&
        !(cpu->sfr[DMCU_SFR_SOCR] & DMCU_SOCR_UP)) {
        return;
    }
    if (reg == DMCU_SFR_SB) {
        cpu->shift_buffer = value & (bits == 8 ? 0xff : MASK16);
        cpu->sfr[reg] = cpu->shift_buffer;
        return;
    }
    if (reg == DMCU_SFR_PSW) {
        value &= bits == 8 ? 0xff : MASK16;
        cpu->psw = (value & 0x1 ? DMCU_PSW_Z : 0) |
                   (value & 0x2 ? DMCU_PSW_V : 0) |
                   (value & 0x4 ? DMCU_PSW_C : 0) |
                   (value & 0x8 ? DMCU_PSW_N : 0);
        cpu->sfr[reg] = value;
        /* PSR 写改变 CPRI/CIRQ 仲裁阈值，指令边界重新仲裁。 */
        cpu->irq_state_changed = true;
        return;
    }
    /* DS0/DS1 (0x10/0x11) 写入后由 data_physical_address 即时生效。 */
    {
        uint32_t old_value = cpu->sfr[reg];

        cpu->sfr[reg] = value & (bits == 8 ? 0xff : MASK16);
        /* 计时设备写分发（helper.c:1744-1751）：定时器/音频挂在此钩子上。 */
        if (dmcu_sfr_has_timing(reg) && cpu->sfr_device_write) {
            cpu->sfr_device_write(cpu->sfr_device_opaque, reg,
                                  (uint16_t)old_value, (uint16_t)cpu->sfr[reg]);
        }
    }
    /* ICR/SOCR/PSW 写改变中断资格，完成当前指令后需重新仲裁
     * （helper.c sfr_write "Only IE changes CPU eligibility" 段）。 */
    if ((reg >= DMCU_ICR_FIRST && reg < DMCU_ICR_FIRST + DMCU_ICR_COUNT) ||
        reg == DMCU_SFR_SOCR || reg == DMCU_SFR_PSW) {
        cpu->irq_state_changed = true;
    }
}
