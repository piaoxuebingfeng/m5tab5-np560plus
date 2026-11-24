/*
 * dmcu_bus.c - NP560+ 总线/存储模型实现。
 *
 * 物理映射（依据 hw/dmcu/np560.c）：
 *   0x00000000..0x00007FFF  内部 X RAM（数据总线 bank 0/1 视图，与内部端口共享后端）
 *   0x00010000..            内部程序 ROM 窗口（resident 镜像装载点）
 *   0x0000F100..0x0000F3FF  SFR/MMIO 计时设备（定时器/键盘/ADC/系统控制）
 *   0x00200000..0x004FFFFF  程序/数据 NOR（20:0000..4F:FFFF）
 *
 * 内部端口空间（64KB，绕过总线）：
 *   0x0000..0x7FFF  X RAM；0x8000..0xBFFF  Y RAM；0xF000..0xF0FF  DPRAM
 *
 * 字节车道规则（helper.c data_backing_address）：16 位字按小端序列化，
 * 字节访问时偶地址选高字节、奇地址选低字节——对内部端口同样适用。
 */
#include "dmcu_bus.h"
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_attr.h"
#else
#include "esp_attr.h"   /* host shim：IRAM_ATTR 为空 */
#endif

/* 内部 X/Y/DPRAM（64KB）是每条 guest 指令数据读写的最热目标。
 * ESP32-P4 上普通 calloc 会落到 PSRAM，每次访存跨 PSRAM 总线；
 * 优先放内部 SRAM（DIRAM 空闲 ~158KB），失败再回退普通分配。 */
static uint8_t *alloc_internal_data(void)
{
#ifdef ESP_PLATFORM
    uint8_t *p = (uint8_t *)heap_caps_malloc(DMCU_INTERNAL_SIZE,
                                             MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

    if (p) {
        memset(p, 0, DMCU_INTERNAL_SIZE);
        return p;
    }
#endif
    return (uint8_t *)calloc(1, DMCU_INTERNAL_SIZE);
}

Np560Bus *np560_bus_create(void)
{
    Np560Bus *bus = calloc(1, sizeof(Np560Bus));

    if (!bus) {
        return NULL;
    }
    bus->internal_data = alloc_internal_data();
    bus->phys = calloc(1, DMCU_PHYS_SIZE);
    if (!bus->internal_data || !bus->phys) {
        free(bus->internal_data);
        free(bus->phys);
        free(bus);
        return NULL;
    }
    return bus;
}

void np560_bus_destroy(Np560Bus *bus)
{
    if (!bus) {
        return;
    }
    free(bus->internal_data);
    free(bus->phys);
    free(bus);
}

void np560_bus_load(Np560Bus *bus, uint32_t addr, const uint8_t *data, size_t len)
{
    if (addr < DMCU_PHYS_SIZE && len <= DMCU_PHYS_SIZE - addr) {
        memcpy(bus->phys + addr, data, len);
    }
}

/* ST7529 MMIO 区间（np560.c:34）：仅字节访问有效（valid.min=max=1），
 * 16 位访问被 QEMU 拒绝（读 0 / 写丢弃）。 */
#define DMCU_LCD_MMIO_BASE 0x00f00000u
#define DMCU_LCD_MMIO_SIZE 4u

static bool is_lcd_mmio(uint32_t physical)
{
    return (physical & ~(uint32_t)(DMCU_LCD_MMIO_SIZE - 1u)) ==
           DMCU_LCD_MMIO_BASE;
}

/* IGNDS unbanked SFR 页（cpu.h:36）：banked F1xx..F3xx 设备的第二视图，
 * 物理 = 0x01000800 + (banked - 0xF000)（np560.c:20856-20912）。 */
#define DMCU_UNBANKED_SFR_PAGE_BASE 0x01000800u

static bool unbanked_sfr_word(uint32_t physical, uint32_t *banked)
{
    if (physical >= DMCU_UNBANKED_SFR_PAGE_BASE &&
        physical < DMCU_UNBANKED_SFR_PAGE_BASE + 0x800u) {
        *banked = physical - DMCU_UNBANKED_SFR_PAGE_BASE + 0xf000u;
        return true;
    }
    return false;
}

/* 物理总线 16 位访问（小端）。入参不裁剪：IGNDS unbanked SFR 页
 * （0x01000800+）位于 24 位字空间之上，先于兜底分支识别；最终 phys[]
 * 访问前裁剪到 24 位。 */
static IRAM_ATTR uint16_t phys_read16(Np560Bus *bus, uint32_t physical)
{
    uint32_t banked;

    if (unbanked_sfr_word(physical, &banked)) {
        return bus->mmio_read ? bus->mmio_read(bus->mmio_user, banked) : 0;
    }
    physical &= MASK24;
    if (physical < DMCU_X_RAM_SIZE) {
        uint8_t *p = bus->internal_data + physical;
        return (uint16_t)(p[0] | (p[1] << 8));
    }
    if (is_lcd_mmio(physical)) {
        return 0;   /* QEMU valid.min=max=1：16 位访问被拒绝。 */
    }
    if (physical >= 0xf100u && physical < 0xf400u) {
        if (bus->mmio_read) {
            return bus->mmio_read(bus->mmio_user, physical);
        }
        return 0;
    }
    return (uint16_t)(bus->phys[physical] |
                      (bus->phys[physical + 1] << 8));
}

static void phys_write16(Np560Bus *bus, uint32_t physical, uint16_t value)
{
    uint32_t banked;

    if (unbanked_sfr_word(physical, &banked)) {
        if (bus->mmio_write) {
            bus->mmio_write(bus->mmio_user, banked, value);
        }
        return;
    }
    physical &= MASK24;
    if (physical < DMCU_X_RAM_SIZE) {
        bus->internal_data[physical] = value & 0xff;
        bus->internal_data[physical + 1] = value >> 8;
        return;
    }
    if (is_lcd_mmio(physical)) {
        return;     /* QEMU valid.min=max=1：16 位写被丢弃。 */
    }
    if (physical >= 0xf100u && physical < 0xf400u) {
        if (bus->mmio_write) {
            bus->mmio_write(bus->mmio_user, physical, value);
        }
        return;
    }
    bus->phys[physical] = value & 0xff;
    bus->phys[physical + 1] = value >> 8;
}

/* 物理总线 16 位访问（小端）。 */
/* 物理总线 16 位访问（小端，入参保持原样，unbanked 页可见）。 */
uint16_t dmcu_phys_read16(Np560Bus *bus, uint32_t physical)
{
    return phys_read16(bus, physical);
}

void dmcu_phys_write16(Np560Bus *bus, uint32_t physical, uint16_t value)
{
    phys_write16(bus, physical, value);
}

/*
 * 字节访问：车道地址 L 的平坦字节地址 = L ^ 1（helper.c:1834 "even byte
 * address selects the high byte"；np560.c:4044 逻辑 386C → 物理 B86D 实证）。
 * 返回/写入平坦字节 bytes[F]：F 奇 = LE 字(F&~1) 的高字节，F 偶 = 低字节。
 * 命中 ST7529 MMIO 区间时按字节事务投递设备钩子。
 */
uint8_t dmcu_phys_read8(Np560Bus *bus, uint32_t logical_byte)
{
    uint32_t flat = (logical_byte & MASK24) ^ 1u;
    uint16_t word;

    if (is_lcd_mmio(flat)) {
        if (bus->mmio_read8) {
            return bus->mmio_read8(bus->mmio8_user, flat);
        }
        return 0;
    }
    word = phys_read16(bus, flat & ~1u);
    return (uint8_t)((flat & 1) ? (word >> 8) : (word & 0xff));
}

void dmcu_phys_write8(Np560Bus *bus, uint32_t logical_byte, uint8_t value)
{
    uint32_t flat = (logical_byte & MASK24) ^ 1u;
    uint16_t word;

    if (is_lcd_mmio(flat)) {
        if (bus->mmio_write8) {
            bus->mmio_write8(bus->mmio8_user, flat, value);
        }
        return;
    }
    word = phys_read16(bus, flat & ~1u);
    if (flat & 1) {
        word = (word & 0x00ff) | ((uint16_t)value << 8);
    } else {
        word = (word & 0xff00) | (value & 0xff);
    }
    phys_write16(bus, flat & ~1u, word);
}

/*
 * code 平面取指（QEMU s->code 独立总线；np560.c:20993/21028/21340）。
 * bus->code_rom 为 NULL 时保持单平面旧行为（回归兼容）。
 *
 * 优先级对应 QEMU add_subregion_overlap：
 *   X RAM alias(2) > PRG alias(4 仅 0x30000..0x42FFFF) > ROM(1) > missing。
 * ROM 区间 phys 优先（0 回落 code_rom）：实现 data→code overlay alias
 * 的近似——data 平面 ROM 镜像非 0（seed）或 loader 写入 veneer 时被取指
 * 观察到；data 为 0 时回落 ROM 本体。
 */
IRAM_ATTR uint16_t dmcu_code_read16(Np560Bus *bus, uint32_t address)
{
    uint32_t a = address & MASK24;

    if (!bus->code_rom) {
        return phys_read16(bus, a);
    }
    if (a < 0x8000u) {
        /* code-internal-x-alias：读 X RAM。 */
        uint8_t *p = bus->internal_data + a;

        return (uint16_t)(p[0] | (p[1] << 8));
    }
    if (a >= 0x30000u && a < 0x430000u) {
        /* PRG 区快路径：该区间与 unbanked SFR(0x1000800+)、LCD MMIO
         * (0xf00000)、SFR(0xf100..0xf400)、X RAM(<0x8000) 均不重叠，
         * phys_read16 的四层分支在此恒为假。直接读 phys[] 跳过它们。
         * a+1 <= 0x430001，远在 16MB phys 范围内。 */
        return (uint16_t)(bus->phys[a] | (bus->phys[a + 1] << 8));
    }
    if (a >= 0x200000u) {
        return 0x0f00u;                 /* vma03 模式 missing-soc-rom */
    }
    {
        uint16_t word = phys_read16(bus, a);

        if (word != 0) {
            return word;                /* overlay/ROM 镜像 veneer */
        }
    }
    if (a < bus->code_rom_size) {
        const uint8_t *p = bus->code_rom + a;

        return (uint16_t)(p[0] | (p[1] << 8));
    }
    return 0x0f00u;
}

bool np560_bus_load_code_rom(Np560Bus *bus, const uint8_t *rom, uint32_t size)
{
    uint8_t *copy = malloc(size);

    if (!copy) {
        return false;
    }
    memcpy(copy, rom, size);
    free(bus->code_rom);
    bus->code_rom = copy;
    bus->code_rom_size = size;
    return true;
}

void np560_bus_take_code_rom(Np560Bus *bus, uint8_t *rom, uint32_t size)
{
    free(bus->code_rom);
    bus->code_rom = rom;
    bus->code_rom_size = size;
}

/*
 * slot 字节访问：直接寻址平坦字节（字节槽 S = LE 字 S&~1 的第 S&1 字节）。
 * 供 NAND DMA 等设备级字节流使用，不做 ^1 车道交换。X RAM 与 LCD MMIO
 * 分支与 16 位路径一致。
 */
uint8_t dmcu_phys_slot_read8(Np560Bus *bus, uint32_t slot)
{
    slot &= MASK24;

    if (slot < DMCU_X_RAM_SIZE) {
        return bus->internal_data[slot];
    }
    if (is_lcd_mmio(slot)) {
        return bus->mmio_read8 ? bus->mmio_read8(bus->mmio8_user, slot) : 0;
    }
    return bus->phys[slot];
}

void dmcu_phys_slot_write8(Np560Bus *bus, uint32_t slot, uint8_t value)
{
    slot &= MASK24;

    if (slot < DMCU_X_RAM_SIZE) {
        bus->internal_data[slot] = value;
        return;
    }
    if (is_lcd_mmio(slot)) {
        if (bus->mmio_write8) {
            bus->mmio_write8(bus->mmio8_user, slot, value);
        }
        return;
    }
    bus->phys[slot] = value;
}

uint32_t dmcu_data_physical_address(const DMCUCpu *cpu, uint32_t addr)
{
    uint32_t logical = addr & MASK16;
    uint32_t segment;

    if (cpu->ignore_ds_count) {
        /* IGNDS：暴露未分段的 64KB 内部数据/SFR 窗口。 */
        return logical;
    }
    segment = cpu->sfr[(logical & 0x8000) ? DMCU_SFR_DS1 : DMCU_SFR_DS0] & 0x1ff;
    return ((segment << 15) | (logical & 0x7fff)) & MASK24;
}

/* 翻译数据空间：字节访问时平坦地址 = 车道地址 ^ 1（车道交换）。
 * 返回平坦字节 bytes[F]（F 奇 = LE 字高字节）。命中 ST7529 MMIO 区间时
 * 按字节事务直接投递设备钩子（对应 QEMU address_space byte access 到
 * io region），不做读改写。 */
uint32_t dmcu_translated_data_read(Np560Bus *bus, DMCUCpu *cpu,
                                   uint32_t addr, int bits)
{
    uint32_t physical = dmcu_data_physical_address(cpu, addr);

    if (bits == 8) {
        uint32_t flat = physical ^ 1u;
        uint16_t word;

        if (is_lcd_mmio(flat)) {
            if (bus->mmio_read8) {
                return bus->mmio_read8(bus->mmio8_user, flat) & 0xff;
            }
            return 0;
        }
        word = phys_read16(bus, flat & ~1u);
        return (uint8_t)((flat & 1) ? (word >> 8) : (word & 0xff));
    }
    if (is_lcd_mmio(physical)) {
        return 0;
    }
    return phys_read16(bus, physical);
}

void dmcu_translated_data_write(Np560Bus *bus, DMCUCpu *cpu,
                                uint32_t addr, uint32_t value, int bits)
{
    uint32_t physical = dmcu_data_physical_address(cpu, addr);

    if (bits == 8) {
        uint32_t flat = physical ^ 1u;
        uint16_t word;

        if (is_lcd_mmio(flat)) {
            if (bus->mmio_write8) {
                bus->mmio_write8(bus->mmio8_user, flat, value & 0xff);
            }
            return;
        }
        word = phys_read16(bus, flat & ~1u);
        if (flat & 1) {
            word = (word & 0x00ff) | ((uint16_t)(value & 0xff) << 8);
        } else {
            word = (word & 0xff00) | (value & 0xff);
        }
        phys_write16(bus, flat & ~1u, word);
        return;
    }
    if (is_lcd_mmio(physical)) {
        return;
    }
    phys_write16(bus, physical, value & MASK16);
}

uint32_t dmcu_internal_data_read(Np560Bus *bus, uint32_t addr, int bits)
{
    uint32_t backing = addr & MASK16;

    if (bits == 8) {
        backing ^= 1u;
        return bus->internal_data[backing];
    }
    {
        uint8_t *p = bus->internal_data + backing;
        return (uint32_t)(p[0] | (p[1] << 8));
    }
}

void dmcu_internal_data_write(Np560Bus *bus, uint32_t addr, uint32_t value, int bits)
{
    uint32_t backing = addr & MASK16;

    if (bits == 8) {
        backing ^= 1u;
        bus->internal_data[backing] = value & 0xff;
        return;
    }
    bus->internal_data[backing] = value & 0xff;
    bus->internal_data[backing + 1] = (value >> 8) & 0xff;
}
