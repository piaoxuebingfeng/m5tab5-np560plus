/*
 * dmcu_clock.c - DMCU 时钟域。
 *
 * 从 qemu-dmcu/target/dmcu/clock.c 移植：系统频率选择、外部数据等待、
 * 已文档化的指令成本表。原文件的指令时钟实验模式（instruction-clock）
 * 属于 QEMU icount 集成，不移植；re-host 使用简化的周期累加器。
 */
#include "dmcu_clock.h"
#include "dmcu_exec.h"

/* clock.c dmcu_system_clock_hz 原样移植。 */
uint32_t dmcu_system_clock_hz(const DMCUCpu *cpu)
{
    unsigned pll = cpu->sfr[DMCU_SFR_PLL];
    unsigned select = cpu->sfr[DMCU_SFR_CLKCON] & 0x1f;

    if (select & 0x10) {
        return 32768;
    }
    if (pll & 0x8000) {
        /* 原厂频率换算代码约束下的 4MHz × N / D。 */
        unsigned multiplier = ((pll & 0x1fff) >> 5) + 1;
        unsigned divisor = (pll & 0x1f) + 1;

        return (4000000u * multiplier / divisor) >> (select & 7);
    }
    /* CN1916884A fig.22 已公开的非 PLL CLKSEL 表。 */
    return (select & 8 ? 16384000u : 24576000u) >> (select & 7);
}

/*
 * CN1916884A pp.15,132,138-139：边界为下包含；EMCR 的 WS 作用于该边界
 * 之上区域。不排序畸形边界、不虚构复位配置。
 */
__attribute__((unused))
static int dmcu_external_waits(const DMCUCpu *cpu, uint32_t address)
{
    uint32_t high = address >> 16;
    uint32_t b1 = cpu->sfr[4] >> 8;
    uint32_t b2 = cpu->sfr[5] >> 8;
    uint32_t b3 = cpu->sfr[6] >> 8;

    if (b1 < 0x21u || b1 > b2 || b2 > b3) {
        return -1;
    }
    if (high >= b3) {
        return cpu->sfr[6] & 3;
    }
    if (high >= b2) {
        return cpu->sfr[5] & 3;
    }
    if (high >= b1) {
        return cpu->sfr[4] & 3;
    }
    return (cpu->sfr[DMCU_SFR_SOCR] >> 6) & 3;
}

/* clock.c dmcu_documented_cycles 原样移植（目视核对表，未知成本返回 0）。 */
static unsigned dmcu_documented_cycles(uint16_t word, bool external)
{
    unsigned op = word >> 8;
    unsigned arg = word & 0xff;
    unsigned ab = external ? 2 : 1;

    /* ADD/ADDB/ADDC/ADDCB, pp.20-23. */
    if ((op & 15) <= 1 && (op >> 4) <= 11) {
        return op >= 0x60 ? ab : 1;
    }
    /* AND/ANDB, pp.24-25. */
    if ((op & 15) == 4 && (op >> 4) <= 11) {
        return op >= 0x60 ? ab : 1;
    }
    /* OR/ORB, pp.49-50. */
    if ((op & 15) == 5 && (op >> 4) <= 11) {
        return op >= 0x60 ? ab : 1;
    }
    /* SUB/SUBB/SUBC/SUBCB 与 XOR/XORB, pp.51-56. */
    if (((op & 15) == 2 || (op & 15) == 3 || (op & 15) == 7) &&
        (op >> 4) <= 11) {
        return op >= 0x60 ? ab : 1;
    }
    /* CMP/CMPB, pp.95-96. */
    if ((op & 15) == 6 && (op >> 4) <= 9) {
        return op >= 0x60 ? ab : 1;
    }
    switch (op) {
    case 0xac:
        return arg == 0 ? 3 : 0;
    case 0xbc:
        return arg == 0 ? 4 : 0;
    case 0xad:
        return 3;
    case 0xd0:
        return 4;
    case 0xc6:
        return arg == 0 ? 2 : 0;
    case 0x0c: case 0x2c: case 0x4c:
        return 1;
    case 0x1c: case 0x5c: case 0x3c:
    case 0x0d: case 0x4d: case 0x2d:
    case 0x1d: case 0x5d: case 0x3d:
        return 1;
    case 0x4e: case 0x4f: case 0x5f:
        return 1;
    case 0x6e: case 0x6f: case 0x7e: case 0x7f:
    case 0x8e: case 0x8f: case 0x9e: case 0x9f:
        return 2;
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x1a: case 0x1b: case 0x28: case 0x29:
    case 0x2a: case 0x2b: case 0x38: case 0x39:
    case 0x3a: case 0x3b: case 0x48: case 0x49:
    case 0x58: case 0x59: case 0x5a: case 0x5b:
        return 1;
    case 0x68: case 0x69: case 0x6a: case 0x6b:
    case 0x78: case 0x79: case 0x88: case 0x89:
    case 0xa8: case 0xa9:
        return ab;
    case 0x8a: case 0x8b:
        return arg < 16 ? ab : 0;
    case 0x0e: case 0x18: case 0x19:
        return 1;
    case 0x98: case 0x99: case 0xb8: case 0xb9:
        return 2;
    case 0xc0:
        return arg < 16 ? 1 : 0;
    case 0xc1:
        return 1;
    case 0xc2: case 0xc4:
        return arg < 16 ? 4 : 0;
    case 0xc3:
        return arg < 16 ? 1 : 0;
    case 0x1f: case 0x2e:
        return 1;
    case 0xc7:
        return arg == 0 ? 1 : 0;
    case 0x3f:
        return (arg <= 3 || (arg >= 0x10 && arg <= 0x13)) ? 1 : 0;
    case 0xbf:
        return arg == 0 ? ab : 0;
    case 0xcf:
        return arg < 16 ? 1 : 0;
    case 0xae:
        return ab;
    case 0xaf:
        return arg < 16 ? ab : 0;
    default:
        return 0;
    }
}

unsigned dmcu_decode_cycles(uint16_t word, uint32_t pc)
{
    return pc >= 0x010000u ?
        dmcu_documented_cycles(word, pc >= 0x210000u) : 0;
}

void dmcu_clock_charge(DMCUCpu *cpu, uint64_t cycles)
{
    cpu->cycles += cycles;
}
