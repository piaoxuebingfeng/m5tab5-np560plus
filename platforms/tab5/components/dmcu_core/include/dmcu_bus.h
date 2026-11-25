/*
 * dmcu_bus.h - NP560+ 总线/存储模型。
 *
 * 三个地址空间（对应 QEMU 的三个 TLB 视图）：
 *  1. 代码空间：扁平物理 CS:PC 取指（NOR @0x200000，3MB）
 *  2. 翻译数据空间：16 位逻辑地址经 DS0/DS1 分段 → 24 位物理总线
 *     （物理 0x0000..0x7FFF 为内部 X RAM 视图，0xF100+ 为 SFR/MMIO）
 *  3. 内部端口空间：FP/SP/Y 指针绕过总线的 64KB 内部 SRAM
 *     （X RAM 0..0x7FFF、Y RAM 0x8000..0xBFFF、DPRAM 0xF000..0xF0FF）
 */
#ifndef DMCU_BUS_H
#define DMCU_BUS_H

#include "dmcu_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Np560Bus {
    uint8_t *internal_data;  /* 64KB 内部端口空间（X/Y/DPRAM） */
    uint8_t *phys;           /* 16MB 物理总线（ESP32-P4 上分配于 PSRAM） */

    /* MMIO/SFR 计时设备钩子（16 位事务，物理 0xF100..0xF3FF）。
     * 字节钩子用于 ST7529 LCD 区间 0xF00000..0xF00003（QEMU valid.min=
     * max=1：仅字节事务有效，入参为平坦字节地址，offset = 地址 & 3）。 */
    uint16_t (*mmio_read)(void *user, uint32_t physical);
    void (*mmio_write)(void *user, uint32_t physical, uint16_t value);
    void *mmio_user;
    uint8_t (*mmio_read8)(void *user, uint32_t flat_byte);
    void (*mmio_write8)(void *user, uint32_t flat_byte, uint8_t value);
    void *mmio8_user;

    /* 远调用 HLE 钩子（对应 QEMU np560_overlay_service /
     * np560_rom_service 契约）。overlay=true 为任意目标的覆盖服务；
     * overlay=false 为 mask ROM 窗口（target < 0x200000）服务。
     * 返回 true 表示已服务且已设置 cpu->pc。 */
    bool (*hle_service)(void *user, DMCUCpu *cpu, uint32_t target,
                        uint32_t start, uint32_t next, bool overlay);
    void *hle_user;

    /* ROM 01/02 每 PC HLE 门（对应 QEMU helper.c:22854 的
     * cpu->rom_service 调用）。命中时解释器跳过 TRAP#0，直接执行门后
     * 的 RETS-far。返回 false 则维持原 TRAP/缺 ROM 语义。 */
    bool (*rom_service)(void *user, DMCUCpu *cpu, uint32_t pc);
    void *rom_service_user;

    /*
     * code 平面（QEMU s->code 独立于 data 总线，np560.c:20993）。为 NULL
     * 时取指回落 phys（旧单平面行为，回归测试兼容）。装载 resident.bin
     * 后按 QEMU firmware-vma03 + soc-rom 布局解析（np560.c:21028-21348）：
     *   0x0000..0x7FFF          X RAM（code-internal-x-alias，优先级 2）
     *   0x8000..0x2FFFF         resident.bin ROM；其中 data 平面已装 ROM
     *                           镜像/overlay veneer 的地址 phys 优先
     *   0x30000..0x42FFFF       NP560.PRG（code_program_alias，优先级 4；
     *                           ROM 2MB 在 0x1FFFFF 结束，被 PRG 覆盖完）
     *   0x430000..0xFFFFFF      missing → 0x0f00（vma03 模式无 NOR 映射）
     */
    uint8_t *code_rom;
    uint32_t code_rom_size;
} Np560Bus;

Np560Bus *np560_bus_create(void);
void np560_bus_destroy(Np560Bus *bus);
/* 将镜像装入物理空间（如 NP560.PRG 装到 0x200000）。 */
void np560_bus_load(Np560Bus *bus, uint32_t addr, const uint8_t *data, size_t len);
/* 装载 SoC ROM（resident.bin）并启用 code 平面取指语义（内部拷贝）。 */
bool np560_bus_load_code_rom(Np560Bus *bus, const uint8_t *rom, uint32_t size);
/* 同上但转移所有权：rom 必须是 malloc 得来且调用后不得再 free。
 * 设备端 PSRAM 紧张时避免 2MB 二次拷贝。 */
void np560_bus_take_code_rom(Np560Bus *bus, uint8_t *rom, uint32_t size);

/* 代码取指（扁平物理，无分段）——helper.c 的 cpu_lduw_code 语义。 */
uint16_t dmcu_code_read16(Np560Bus *bus, uint32_t address);

/* 翻译数据空间访问（经 DS0/DS1；字节序遵循 16 位总线的车道规则）。
 * 对应 helper.c translated_data_read/write 的核心路径（剥离观察器）。 */
uint32_t dmcu_translated_data_read(Np560Bus *bus, DMCUCpu *cpu,
                                   uint32_t addr, int bits);
void dmcu_translated_data_write(Np560Bus *bus, DMCUCpu *cpu,
                                uint32_t addr, uint32_t value, int bits);

/* 内部端口访问（FP/SP/Y 指针绕过总线；X/Y/DPRAM）。
 * 对应 helper.c internal_data_read/internal_data_write。 */
uint32_t dmcu_internal_data_read(Np560Bus *bus, uint32_t addr, int bits);
void dmcu_internal_data_write(Np560Bus *bus, uint32_t addr, uint32_t value, int bits);

/* DS0/DS1 逻辑→物理地址翻译（helper.c data_physical_address）。 */
uint32_t dmcu_data_physical_address(const DMCUCpu *cpu, uint32_t addr);

/* 物理总线原始访问（DTU 使用；16 位小端字 + 车道交换字节）。
 * read8/write8 的入参是"逻辑车道字节地址"（与 QEMU DTU address^(byte?1:0)
 * 前的地址一致），内部按偶=高字节规则解析。 */
uint16_t dmcu_phys_read16(Np560Bus *bus, uint32_t physical);
void dmcu_phys_write16(Np560Bus *bus, uint32_t physical, uint16_t value);
uint8_t dmcu_phys_read8(Np560Bus *bus, uint32_t logical_byte);
void dmcu_phys_write8(Np560Bus *bus, uint32_t logical_byte, uint8_t value);

/* slot 字节访问（不做 ^1 车道交换）：字节槽 S = LE 字 S&~1 的第 S&1 字节。
 * 供 NAND DMA 等设备级字节流使用。 */
uint8_t dmcu_phys_slot_read8(Np560Bus *bus, uint32_t slot);
void dmcu_phys_slot_write8(Np560Bus *bus, uint32_t slot, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* DMCU_BUS_H */
