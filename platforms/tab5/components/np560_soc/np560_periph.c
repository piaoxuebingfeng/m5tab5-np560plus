/*
 * np560_periph.c - NP560+ 板级外设组合层。
 *
 * SFR 设备分发移植自 hw/dmcu/np560.c:7804-7826（np560_sfr_device_*）。
 * LCD 字节 MMIO 分发对应 np560_st7529_read/write（np560.c:9809-9836）：
 * 物理 0xF00000..0xF00003，offset&2==0 → 命令/STREAD 状态，offset&2!=0 →
 * 数据口。入参为平坦字节地址（车道 ^1 后）。
 */
#include "np560_periph.h"
#include <string.h>

/* 节点②诊断：SFR 读写热点计数（np560_periph_diag_take 取走）。 */
static uint32_t s_diag_reads[512];
static uint32_t s_diag_writes[512];

/* ST7529 命令/状态口（offset&2==0，np560.c:9809/9826）。 */
static uint8_t np560_lcd_port_read(void *opaque, uint32_t flat_byte)
{
    Np560Periph *s = opaque;

    if (flat_byte & 2) {
        return np560_lcd_data_read(&s->lcd);
    }
    return np560_lcd_status_read(&s->lcd);
}

static void np560_lcd_port_write(void *opaque, uint32_t flat_byte,
                                 uint8_t value)
{
    Np560Periph *s = opaque;

    if (flat_byte & 2) {
        np560_lcd_data_write(&s->lcd, value);
    } else {
        np560_lcd_command(&s->lcd, value);
    }
}

/* 总线字节 MMIO 钩子：LCD 区间由 dmcu_bus.c 识别后投递。 */
static uint8_t np560_bus_mmio_read8(void *opaque, uint32_t flat_byte)
{
    return np560_lcd_port_read(opaque, flat_byte);
}

static void np560_bus_mmio_write8(void *opaque, uint32_t flat_byte,
                                  uint8_t value)
{
    np560_lcd_port_write(opaque, flat_byte, value);
}

/*
 * 总线 word MMIO 钩子（0xF100..0xF3FF + IGNDS unbanked 页换算视图）：
 *   F100        SOCR（system_control，np560.c:9023-9077，SFR 直写）
 *   F160..F165  键盘矩阵 / F340..F347 事件锁存
 *   F20A/C/E    硬件 ECC 引擎
 *   F300..F321  NAND 控制器
 * 其余区域读 0 / 写丢弃。
 */
#define NP560_MMIO_WORD_LOW  0xf100u
#define NP560_MMIO_WORD_HIGH 0xf400u

static uint16_t np560_bus_mmio_read(void *opaque, uint32_t physical)
{
    Np560Periph *s = opaque;

    if (physical < NP560_MMIO_WORD_LOW || physical >= NP560_MMIO_WORD_HIGH) {
        return 0;
    }
    s_diag_reads[0x100u + ((physical >> 1) & 0xffu)]++;
    if (physical == 0xf100u) {
        return s->cpu->sfr[DMCU_SFR_SOCR];
    }
    if (physical >= NP560_KEY_MATRIX_BASE &&
        physical < NP560_KEY_MATRIX_BASE + NP560_KEY_MATRIX_SIZE) {
        return np560_keypad_mmio_read(&s->keypad, physical);
    }
    if (physical >= NP560_KEYBOARD_BASE &&
        physical < NP560_KEYBOARD_BASE + NP560_KEYBOARD_SIZE) {
        return np560_keypad_mmio_read(&s->keypad, physical);
    }
    if (physical >= NP560_ECC_BASE &&
        physical < NP560_ECC_BASE + NP560_ECC_SIZE) {
        return np560_ecc_mmio_read(&s->ecc, physical - NP560_ECC_BASE);
    }
    if (physical >= NP560_NAND_CTRL_BASE &&
        physical < NP560_NAND_CTRL_BASE + NP560_NAND_CTRL_SIZE) {
        return np560_nand_mmio_read(&s->nand, physical - NP560_NAND_CTRL_BASE);
    }
    return 0;
}

static void np560_bus_mmio_write(void *opaque, uint32_t physical,
                                 uint16_t value)
{
    Np560Periph *s = opaque;

    if (physical < NP560_MMIO_WORD_LOW || physical >= NP560_MMIO_WORD_HIGH) {
        return;
    }
    s_diag_writes[0x100u + ((physical >> 1) & 0xffu)]++;
    if (physical == 0xf100u) {
        s->cpu->sfr[DMCU_SFR_SOCR] = value;
        return;
    }
    if (physical >= NP560_KEY_MATRIX_BASE &&
        physical < NP560_KEY_MATRIX_BASE + NP560_KEY_MATRIX_SIZE) {
        np560_keypad_mmio_write(&s->keypad, physical, value);
        return;
    }
    if (physical >= NP560_KEYBOARD_BASE &&
        physical < NP560_KEYBOARD_BASE + NP560_KEYBOARD_SIZE) {
        np560_keypad_mmio_write(&s->keypad, physical, value);
        return;
    }
    if (physical >= NP560_ECC_BASE &&
        physical < NP560_ECC_BASE + NP560_ECC_SIZE) {
        np560_ecc_mmio_write(&s->ecc, physical - NP560_ECC_BASE, value);
        return;
    }
    if (physical >= NP560_NAND_CTRL_BASE &&
        physical < NP560_NAND_CTRL_BASE + NP560_NAND_CTRL_SIZE) {
        np560_nand_mmio_write(&s->nand, physical - NP560_NAND_CTRL_BASE,
                              value);
        return;
    }
}

/* 板级 SFR 设备读（np560.c:7804）。 */
static uint16_t np560_sfr_device_read(void *opaque, unsigned reg,
                                      uint16_t value)
{
    Np560Periph *s = opaque;

    s_diag_reads[reg & 0xff]++;
    return np560_timers_read(&s->timers, reg, value);
}

/* 板级 SFR 设备写（np560.c:7813）：DAC/音频 PLL -> 音频，其余 -> 定时器。 */
static void np560_sfr_device_write(void *opaque, unsigned reg,
                                   uint16_t old_value, uint16_t value)
{
    Np560Periph *s = opaque;

    s_diag_writes[reg & 0xff]++;
    if ((reg >= DMCU_SFR_DAC0 && reg <= DMCU_SFR_DACC) ||
        reg == DMCU_SFR_AUDIO_PLL) {
        np560_audio_write(&s->audio, reg, value);
    } else {
        np560_timers_write(&s->timers, reg, old_value, value);
    }
}

/* 节点②诊断：取走 SFR 读写热点计数（同时清零），定位固件轮询目标。 */
void np560_periph_diag_take(Np560Periph *s, uint32_t *reads, uint32_t *writes)
{
    (void)s;
    memcpy(reads, s_diag_reads, sizeof(s_diag_reads));
    memcpy(writes, s_diag_writes, sizeof(s_diag_writes));
    memset(s_diag_reads, 0, sizeof(s_diag_reads));
    memset(s_diag_writes, 0, sizeof(s_diag_writes));
}

void np560_periph_init(Np560Periph *periph, DMCUCpu *cpu, Np560Bus *bus)
{
    periph->cpu = cpu;
    np560_timers_init(&periph->timers, cpu);
    np560_audio_init(&periph->audio, cpu);
    np560_lcd_reset(&periph->lcd);
    np560_keypad_reset(&periph->keypad);
    np560_ecc_reset(&periph->ecc);
    np560_nand_reset(&periph->nand);
    if (bus) {
        periph->nand.bus = bus;
    }
    cpu->sfr_device_read = np560_sfr_device_read;
    cpu->sfr_device_write = np560_sfr_device_write;
    cpu->sfr_device_opaque = periph;
    if (bus) {
        bus->mmio_read8 = np560_bus_mmio_read8;
        bus->mmio_write8 = np560_bus_mmio_write8;
        bus->mmio8_user = periph;
        bus->mmio_read = np560_bus_mmio_read;
        bus->mmio_write = np560_bus_mmio_write;
        bus->mmio_user = periph;
    }
}

void np560_periph_advance(Np560Periph *periph, uint64_t delta_cpu_cycles)
{
    np560_timers_advance(&periph->timers, delta_cpu_cycles);
    np560_audio_advance(&periph->audio, delta_cpu_cycles);
}
