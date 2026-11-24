/*
 * dmcu_cpu.h - DMCU CPU 解释器核心状态与寻址辅助。
 *
 * 从 QEMU target/dmcu/cpu.h 提取的架构状态。为 re-host 移植裁剪了全部
 * QEMU 侧诊断观察器字段（menu watch / NPE snapshot / trace ring 等），
 * 只保留架构状态。所有语义以 qemu-dmcu/target/dmcu/helper.c 为准。
 */
#ifndef DMCU_CPU_H
#define DMCU_CPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MASK16 0xffffu
#define MASK24 0xffffffu

/* 内部 PSW 位序（QEMU env->psw；硬件 PSR SFR 的位序是 Z,V,C,N）。 */
#define DMCU_PSW_Z (1u << 0)
#define DMCU_PSW_C (1u << 1)
#define DMCU_PSW_N (1u << 2)
#define DMCU_PSW_V (1u << 3)

#define DMCU_FP 11
#define DMCU_SP 15

/* 直接 SFR 寄存器编号（cpu.h）。 */
#define DMCU_SFR_SOCR   0x00
#define DMCU_SFR_PSW    0x01
#define DMCU_SFR_SB     0x02
#define DMCU_SFR_DS0    0x10
#define DMCU_SFR_DS1    0x11
#define DMCU_SFR_T0     0x25u   /* Tx/TxP/TxC 三寄存器组（cpu.h:351-361） */
#define DMCU_SFR_T0P    0x26u
#define DMCU_SFR_T0C    0x27u
#define DMCU_SFR_T2     0x2bu
#define DMCU_SFR_T2P    0x2cu
#define DMCU_SFR_T2C    0x2du
#define DMCU_SFR_CLKCON 0x2e
#define DMCU_SFR_TBC    0x2fu
#define DMCU_SFR_PLL    0xe9u
#define DMCU_SFR_AUDIO_PLL 0xeau
#define DMCU_SFR_DAC0   0x70u
#define DMCU_SFR_DAC1   0x71u
#define DMCU_SFR_DACC   0x72u

/* SOCR 位（cpu.h:326-331）。 */
#define DMCU_SOCR_IE   0x8000u
#define DMCU_SOCR_UP   0x4000u
#define DMCU_SOCR_NSEG 0x0020u
#define DMCU_SOCR_FR   0x0008u

/* 定时器控制位（np560_timer.c:12-14）。 */
#define DMCU_TX_TON             0x8000u  /* TxC 启停 */
#define DMCU_TX_PERIODIC_CONFIG 0x0080u  /* 原厂唯一使用的周期模式 */
#define DMCU_TB_TBON            0x8000u  /* TBC 启停 */

/* 执行结果（cpu.h DMCU_EXEC_*）。 */typedef enum {
    DMCU_EXEC_EXIT = 0,   /* 返回主循环仲裁（IRQ 状态变化/关机等） */
    DMCU_EXEC_NEXT = 1,   /* 顺序后继，继续执行 */
    DMCU_EXEC_LOOKUP = 2, /* 非顺序 PC（分支/调用/映射切换） */
} dmcu_exec_result_t;

/* 地址空间上限。 */
#define DMCU_PHYS_SIZE   0x1000000u  /* 16MB 物理（code NOR @0x200000 3MB） */
#define DMCU_X_RAM_SIZE  0x8000u      /* 内部 X RAM 32KB @ 00:0000..00:7FFF */
#define DMCU_INTERNAL_SIZE 0x10000u   /* 内部端口空间 64KB（X/Y/DPRAM） */
#define DMCU_Y_RAM_BASE  0x8000u      /* Y RAM 自 00:8000（经内部端口访问） */
#define DMCU_PROGRAM_BASE 0x200000u   /* 程序空间 20:0000 起的 NOR 窗口 */
#define DMCU_PROGRAM_SIZE 0x300000u   /* 数据可见 NOR 3MB（20:0000..4F:FFFF） */

/*
 * CPU 架构状态。寄存器均为 16 位（gpr/sfr），pc 为 32 位 CS:PC。
 */
typedef struct DMCUIrqContext {
    uint32_t frame_base;
    uint32_t frame_size;
    bool from_icr;
    bool repeat_saved;
    uint32_t repeat_pc, repeat_end, repeat_count;
    bool repeat_block;
    uint32_t single_repeat_pc, single_repeat_count;
} DMCUIrqContext;

#define DMCU_IRQ_MAX_DEPTH 8

typedef struct DMCUCpu {
    uint32_t pc;              /* 字节寻址 CS:PC */
    uint32_t gpr[16];         /* R0..R15（16 位） */
    uint32_t sfr[256];        /* 直接/SFR 寄存器组（16 位） */
    uint32_t psw;             /* Z,C,N,V 在内部位序中 */
    uint8_t  se;              /* 桶形移位指数（RB10 架构别名） */
    uint16_t shift_buffer;    /* SB 移位缓冲（DMCU_SFR_SB） */

    /* 重复引擎（REPB/REP；helper.c 尾部回绕语义）。 */
    uint32_t repeat_pc, repeat_end, repeat_count;
    bool repeat_block;
    uint32_t single_repeat_pc, single_repeat_count;

    /* IGNDS / CRIT 窗口计数。 */
    uint32_t ignore_ds_count;
    uint32_t critical_count;
    bool irq_state_changed;

    /* 中断簿记（dmcu_irq.c；cpu.c interrupt_* 架构子集）。 */
    bool in_interrupt;
    bool interrupt_from_icr;
    uint32_t irq_frame_base, irq_frame_size;
    bool irq_repeat_saved;
    uint32_t irq_repeat_pc, irq_repeat_end, irq_repeat_count;
    bool irq_repeat_block;
    uint32_t irq_single_repeat_pc, irq_single_repeat_count;
    unsigned irq_depth;
    DMCUIrqContext irq_contexts[DMCU_IRQ_MAX_DEPTH];
    unsigned irq_entries, irq_max_depth;
    uint32_t dtu_fault_sources;

    /* 虚拟周期计数（时钟域推进结果）。 */
    uint64_t cycles;

    /* HLE 边界：重建 resident ROM 窗口（空区间表示未映射）。
     * target 落在此窗口外且 < DMCU_PROGRAM_BASE 的远调用必须由宿主
     * 的 HLE 钩子服务（对应 QEMU np560_rom_service 契约）。 */
    uint32_t resident_rom_base, resident_rom_limit;
    uint32_t auxiliary_code_base, auxiliary_code_limit;
    bool hle_rom_enabled;

    /* 停机诊断（对应 QEMU unsupported() + EXCP_HLT）。 */
    bool halted;
    bool reset_requested;    /* 0x3fff RESET 在系统边界处理 */
    uint32_t fault_pc;
    uint16_t fault_word;
    uint32_t unsupported_count;

    /* 计时 SFR 设备钩子（cpu.h:4700-4702）：定时器/音频/ICR 区间的
     * 寄存器写经此分发到板级外设（对应 board 的 np560_sfr_device_*）。
     * has_timing 区间内的读（计数器回读）经 sfr_device_read。 */
    uint16_t (*sfr_device_read)(void *opaque, unsigned reg, uint16_t value);
    void (*sfr_device_write)(void *opaque, unsigned reg,
                             uint16_t old_value, uint16_t value);
    void *sfr_device_opaque;
} DMCUCpu;

/* 计时 SFR 区间判定（cpu.h:368；实现在 dmcu_regs.c，避免头文件循环包含） */
bool dmcu_sfr_has_timing(unsigned reg);

/* ---- 段式代码寻址（cpu.h 尾部 inline 原样移植） ---- */

static inline uint32_t dmcu_code_segment(uint32_t pc)
{
    return (pc >> 16) & 0xff;
}

static inline uint32_t dmcu_code_pc(uint32_t pc)
{
    return pc & 0xffff;
}

static inline uint32_t dmcu_code_address(uint32_t segment, uint32_t pc)
{
    return ((segment & 0xff) << 16) | (pc & 0xffff);
}

/* Fig. 3：CS 拼接 16 位 PC；PC 进位不改变 CS。 */
static inline uint32_t dmcu_code_advance(uint32_t address, unsigned bytes)
{
    return dmcu_code_address(dmcu_code_segment(address),
                             dmcu_code_pc(address) + bytes);
}

/* ---- 指令长度（helper.c dmcu_insn_size；translate.c dmcu_is_32bit） ---- */

/* 返回指令长度（2 或 4 字节）。 */
static inline uint32_t dmcu_insn_size(uint16_t word)
{
    uint8_t op = word >> 8;
    uint8_t arg = word;
    uint8_t hi = op >> 4;
    uint8_t lo = op & 15;

    if (lo <= 7 && hi >= 6 && hi <= 11) {
        return 4;
    }
    switch (op) {
    case 0x68: case 0x69: case 0x6a: case 0x6b:
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:
    case 0x78: case 0x79: case 0x7c: case 0x7d:
    case 0x7e: case 0x7f:
    case 0x88: case 0x89: case 0x8a: case 0x8b:
    case 0x8c: case 0x8d: case 0x8e: case 0x8f:
    case 0x98: case 0x99: case 0x9a: case 0x9b:
    case 0x9c: case 0x9d: case 0x9e: case 0x9f:
    case 0xa8: case 0xa9: case 0xac: case 0xad:
    case 0xae: case 0xaf:
    case 0xb8: case 0xb9: case 0xbc: case 0xbd:
    case 0xbe:
        return 4;
    case 0xbf:
        return arg == 0 ? 4 : 2;
    default:
        return 2;
    }
}

/* ---- 通用小工具（helper.c width_mask 158 / width_sign 1512 / sext16 1522） ---- */

static inline uint32_t dmcu_width_mask(int bits)
{
    return bits >= 32 ? 0xffffffffu : (1u << bits) - 1;
}

static inline uint32_t dmcu_width_sign(int bits)
{
    return bits >= 32 ? 0x80000000u : 1u << (bits - 1);
}

static inline int32_t dmcu_sext8(uint32_t value)
{
    return (int8_t)value;
}

static inline int32_t dmcu_sext16(uint32_t value)
{
    return (int16_t)value;
}

static inline bool dmcu_is_stack_pointer(unsigned reg)
{
    return reg == DMCU_FP || reg == DMCU_SP;
}

static inline bool dmcu_is_y_pointer(unsigned reg)
{
    return reg >= 12 && reg <= 14;
}

/* 条件码表（helper.c condition()，供控制流族移植使用）。 */
bool dmcu_condition(const DMCUCpu *cpu, unsigned code);

#ifdef __cplusplus
}
#endif

#endif /* DMCU_CPU_H */
