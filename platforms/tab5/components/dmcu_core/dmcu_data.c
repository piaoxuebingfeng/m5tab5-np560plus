/*
 * dmcu_data.c - 数据访问原语：data_read/write、指针 AGU、push/pop、位访问。
 *
 * 移植自 helper.c：
 *   data_read/data_write          6139/6157（含 F2E0 GPR 别名）
 *   pointer_modifier              6561（X0/X2/Y2 循环缓冲长度）
 *   pointer_postincrement         6583（环形 AGU，base 对齐 2 的幂）
 *   pointer_read/pointer_write    6701/6711（FP/SP/Y 指针走内部端口）
 *   push/pop                      6723
 *   bit_read/bit_write            ~6900（低外设位地址/DPRAM/70..7F 别名）
 */
#include "dmcu_exec.h"

uint32_t dmcu_data_read(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, int bits)
{
    uint32_t logical = addr & MASK16;
    uint32_t physical = dmcu_data_physical_address(cpu, addr);

    /* 直接内存操作数经 F2E0..F2FE 暴露 R0..R15。寄存器间接 [Rn] 的
     * pointer_read 即使解析到同一物理地址也绕过该别名。 */
    if (bits == 16 && logical >= 0xf2e0 && logical <= 0xf2fe &&
        !(logical & 1) && physical == logical) {
        return cpu->gpr[(physical - 0xf2e0) >> 1] & MASK16;
    }
    return dmcu_translated_data_read(bus, cpu, addr, bits);
}

void dmcu_data_write(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr,
                     uint32_t value, int bits)
{
    uint32_t logical = addr & MASK16;
    uint32_t physical = dmcu_data_physical_address(cpu, addr);

    if (bits == 16 && logical >= 0xf2e0 && logical <= 0xf2fe &&
        !(logical & 1) && physical == logical) {
        cpu->gpr[(physical - 0xf2e0) >> 1] = value & MASK16;
        return;
    }
    dmcu_translated_data_write(bus, cpu, addr, value, bits);
}

/* 循环缓冲长度：X0/X2/Y2 的 modifier SFR；其余映射尚未核实。 */
static uint32_t pointer_modifier(const DMCUCpu *cpu, unsigned reg)
{
    switch (reg) {
    case 8:  return cpu->sfr[0x13] & MASK16; /* DMCU_SFR_XM0 */
    case 10: return cpu->sfr[0x17] & MASK16; /* DMCU_SFR_XM2 */
    case 14: return cpu->sfr[0x1d] & MASK16; /* DMCU_SFR_YM2 */
    default: return 0;
    }
}

/* 供 memory MAC（exec_mac）使用（helper.c:6571 pointer_circular_valid）。 */
uint32_t dmcu_pointer_modifier(const DMCUCpu *cpu, unsigned reg)
{
    return pointer_modifier(cpu, reg);
}

bool dmcu_pointer_circular_valid(const DMCUCpu *cpu, unsigned reg,
                                 uint32_t addr)
{
    uint32_t length = pointer_modifier(cpu, reg), span = 1;

    while (span < length) {
        span <<= 1;
    }
    return length != 0xffffu &&
           (!length || (addr & (span - 1)) < length);
}

/*
 * CN1916884A fig.10：长度以字节计，base 对齐到最小包含的 2 的幂。
 * 原厂音频用 length 0600 @ base 1000，因此 15ff++ 回绕到 1000。
 * 解析发生在操作数/寄存器写之前；地址保持局部直到指令提交。
 */
uint32_t dmcu_pointer_postincrement(DMCUCpu *cpu, unsigned reg,
                                    uint32_t addr, int32_t increment)
{
    uint32_t length = pointer_modifier(cpu, reg);
    uint32_t span = 1, base;
    int32_t offset;

    addr &= MASK16;
    if (!length) {
        return (addr + increment) & MASK16;
    }
    while (span < length) {
        span <<= 1;
    }
    base = addr & ~(span - 1);
    offset = ((int32_t)(addr - base) + increment) % (int32_t)length;
    if (offset < 0) {
        offset += length;
    }
    return (base + offset) & MASK16;
}

uint32_t dmcu_pointer_read(Np560Bus *bus, DMCUCpu *cpu, unsigned reg,
                           uint32_t addr, int bits)
{
    /* CN1916884A figs.2/5/9：Y0..Y2 无论 DS0/DS1 都访问内部 Y RAM。 */
    if (dmcu_is_stack_pointer(reg) || dmcu_is_y_pointer(reg)) {
        return dmcu_internal_data_read(bus, addr, bits);
    }
    return dmcu_translated_data_read(bus, cpu, addr, bits);
}

void dmcu_pointer_write(Np560Bus *bus, DMCUCpu *cpu, unsigned reg,
                        uint32_t addr, uint32_t value, int bits)
{
    if (dmcu_is_stack_pointer(reg) || dmcu_is_y_pointer(reg)) {
        dmcu_internal_data_write(bus, addr, value, bits);
        return;
    }
    dmcu_translated_data_write(bus, cpu, addr, value, bits);
}

void dmcu_push(Np560Bus *bus, DMCUCpu *cpu, uint32_t value)
{
    cpu->gpr[DMCU_SP] = (cpu->gpr[DMCU_SP] - 2) & MASK16;
    dmcu_internal_data_write(bus, cpu->gpr[DMCU_SP], value, 16);
}

uint32_t dmcu_pop(Np560Bus *bus, DMCUCpu *cpu)
{
    uint32_t value = dmcu_internal_data_read(bus, cpu->gpr[DMCU_SP], 16);

    cpu->gpr[DMCU_SP] = (cpu->gpr[DMCU_SP] + 2) & MASK16;
    return value;
}

/*
 * 位访问（helper.c bit_read/bit_write）。低外设位地址指向直接 SFR；
 * 70..7F 是 F0..FF 的 GPR 位别名；高位地址选择 DPRAM F000..（fig.2）。
 */
static uint32_t bit_read(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, unsigned bitno)
{
    unsigned reg = addr & 0xff;

    if (reg >= 0x80) {
        return (dmcu_internal_data_read(bus, 0xf000 + 2 * (reg - 0x80), 16) >>
                (bitno & 15)) & 1;
    }
    if (reg >= 0x70 && reg < 0x80) {
        reg |= 0x80;
    }
    return (dmcu_sfr_read(cpu, reg, 16) >> (bitno & 15)) & 1;
}

static void bit_write(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr,
                      unsigned bitno, bool value)
{
    unsigned reg = addr & 0xff;
    uint32_t current;
    uint32_t mask = 1u << (bitno & 15);

    if (reg >= 0x80) {
        uint32_t address = 0xf000 + 2 * (reg - 0x80);

        current = dmcu_internal_data_read(bus, address, 16);
        dmcu_internal_data_write(bus, address,
                                 value ? current | mask : current & ~mask, 16);
        return;
    }
    if (reg >= 0x70 && reg < 0x80) {
        reg |= 0x80;
    }
    current = dmcu_sfr_read(cpu, reg, 16);
    dmcu_sfr_write(cpu, reg, value ? current | mask : current & ~mask, 16);
}

/* 供后续 exec_bit 族移植使用（当前未导出到头文件）。 */
uint32_t dmcu_bit_read(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, unsigned bitno);
void dmcu_bit_write(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, unsigned bitno, bool value);
uint32_t dmcu_bit_read(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, unsigned bitno)
{
    return bit_read(bus, cpu, addr, bitno);
}
void dmcu_bit_write(Np560Bus *bus, DMCUCpu *cpu, uint32_t addr, unsigned bitno, bool value)
{
    bit_write(bus, cpu, addr, bitno, value);
}
