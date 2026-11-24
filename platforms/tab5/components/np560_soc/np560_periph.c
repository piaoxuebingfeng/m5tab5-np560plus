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
#include "dmcu_exec.h"   /* dmcu_sfr_read/write：low SFR bank fallback */

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

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

/* F2D0 ADC 电量比较器窗口（QEMU NP560_ADC_BASE=0xf2d0，size=0x06）：
 *   F2D0/F2D1  ADC 控制/比较结果
 *   F2D2/F2D3  CPU PLL  (DMCU_SFR_PLL)
 *   F2D4/F2D5  Audio PLL (DMCU_SFR_AUDIO_PLL)
 * 固件采样器 4F:C336 用 DS1 写逻辑 72D0，落到物理 F2D0。QEMU 默认
 * adc_level=8（device.json），此前 Tab5 无此分支→恒读 0→电量等级 0，
 * 触发原厂 "电压过低,请及时充电" 事件（PRG 31:6B30）。 */
#define NP560_ADC_BASE       0xf2d0u
#define NP560_ADC_SIZE       0x06u

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
    if (physical >= NP560_ADC_BASE && physical < NP560_ADC_BASE + NP560_ADC_SIZE) {
        return np560_adc_read_reg(s, physical - NP560_ADC_BASE, 2);
    }
    /* low SFR bank fallback（np560.c:8946 np560_low_sfr_read）：
     * 物理 F100 + 2*reg 是 direct SFR 的 banked 别名。原厂在 DS0=1 下
     * 写逻辑 715E/71A0 启动 64Hz 时基，经 bank 映射落到 F15E(TBC)/
     * F1A0(TBICR)。QEMU 的 low_sfr_banked_alias 覆盖这一区；此前 Tab5
     * 把它当未映射地址读 0，导致 TBICR.EN 永为 0、时基不启动、idle
     * loop 无 64Hz 中断可跳出，键盘矩阵永远不被扫描。 */
    if (physical >= 0xf100u && physical < 0xf200u) {
        return dmcu_sfr_read(s->cpu, (physical - 0xf100u) >> 1, 16);
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
    if (physical >= NP560_ADC_BASE && physical < NP560_ADC_BASE + NP560_ADC_SIZE) {
        np560_adc_write_reg(s, physical - NP560_ADC_BASE, value, 2);
        return;
    }
    /* low SFR bank fallback（np560.c:8954 np560_low_sfr_write）：
     * 走 dmcu_sfr_write 以触发 sfr_device_write hook（TBC->timebase_write
     * 启动 64Hz 时基；TBICR->ICR[0].EN）。详见 read 侧注释。 */
    if (physical >= 0xf100u && physical < 0xf200u) {
        dmcu_sfr_write(s->cpu, (physical - 0xf100u) >> 1, value, 16);
        return;
    }
}

/* ADC 电量比较器（对齐 QEMU np560_adc_value/read/write，np560.c:7715）：
 * 采样器写 (threshold<<2)|2 到 F2D0、读回 bit7 作比较结果。
 * level=8 时 threshold 0..7 全部断言 → 采样器返回满电等级 8。 */
uint16_t np560_adc_read_reg(Np560Periph *s, unsigned offset, unsigned size)
{
    unsigned threshold;
    uint16_t value;

    if (offset >= 2) {
        unsigned reg = offset < 4 ? DMCU_SFR_PLL : DMCU_SFR_AUDIO_PLL;

        value = s->cpu->sfr[reg];
        return size == 1 ? (value >> ((offset & 1) * 8)) & 0xff : value;
    }
    threshold = (s->adc_control >> 2) & 7;
    value = s->adc_control & 0x007fu;
    if (threshold < MIN(s->adc_level, 8u)) {
        value |= 0x0080u;
    }
    s->adc_reads++;
    return size == 1 ? (value >> ((offset & 1) * 8)) & 0xff : value;
}

void np560_adc_write_reg(Np560Periph *s, unsigned offset, uint16_t value,
                         unsigned size)
{
    if (offset >= 2) {
        unsigned reg = offset < 4 ? DMCU_SFR_PLL : DMCU_SFR_AUDIO_PLL;

        if (size == 1) {
            unsigned shift = (offset & 1) * 8;
            uint16_t old = s->cpu->sfr[reg];

            value = (old & ~(0xffu << shift)) | ((value & 0xffu) << shift);
        }
        s->cpu->sfr[reg] = value;
        return;
    }
    if (size == 1) {
        unsigned shift = (offset & 1) * 8;

        value = (s->adc_control & ~(0xffu << shift)) |
                ((value & 0xffu) << shift);
    }
    s->adc_control = value & 0x007fu;
    s->adc_writes++;
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
    periph->key_wake_ticks = 0;
    periph->stat_key_wake_irq = 0;
    periph->key_debounce_request = 0;
    periph->stat_debounce_fast = 0;
    /* 对齐 QEMU device.json 的 adc_level=8（满电），避免开机误报低电。 */
    periph->adc_control = 0;
    periph->adc_level = 8;
    periph->adc_reads = 0;
    periph->adc_writes = 0;
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

void np560_periph_request_key_wake(Np560Periph *periph)
{
    /* 按下：请求在指令边界预置去抖状态（跳过 0490 累加空转）。注入侧按
     * RQ 已清才注入下一 tick，部分 tick 可能被 CRIT 窗口吞掉，保留少量
     * 余量。预置后不再需要 16 tick 走满 8 次 ISR。 */
    periph->key_debounce_request = 1;
    periph->key_wake_ticks = 6;
}

void np560_periph_request_key_release(Np560Periph *periph)
{
    /* 释放：请求预置 + 补无键扫描，让 guest 扫描器重新武装 2A94=1、
     * 清 2A8C=0（QEMU oracle 实证释放后 50ms 内 2A8C→0/2A94→1）。
     * 不补这一步时，下一次短按走连发路径而不响应。 */
    periph->key_debounce_request = 2;
    periph->key_wake_ticks = 4;
}

int np560_periph_apply_fast_debounce(Np560Periph *periph, Np560Bus *bus)
{
    uint8_t req = periph->key_debounce_request;

    if (!req || !bus) {
        return 0;
    }
    periph->key_debounce_request = 0;
    periph->stat_debounce_fast++;

    /* 0490（内部 X RAM 物理字 0x0490）：时基 ISR 每进入 +1、满 4 调一次
     * 27:E528 扫描。预置为 4 让下一次 ISR 立即触发扫描，跳过 3 次空转。
     * 内部 X RAM 走 internal_data，字节序小端（phys_write16 同款）。 */
    if (bus->internal_data) {
        bus->internal_data[0x490] = 4;
        bus->internal_data[0x491] = 0;
    }

    /* 2A8C（外部 RAM 字节，Guest DS0=0xA2 → 物理 0x512A8C）：去抖计数。
     * QEMU oracle：按下后第 1 次扫描 2A8C(字节)=1，第 2 次=2 时生成事件。
     * 预置为 2 让本次扫描即满足去抖门；配合 2A94 保持 1（armed），guest
     * 扫描器仍原生走事件管线（28D6/2A8A/candidate++），不会漏事件。
     * 逻辑字节地址按 ^1 高字节车道 → 物理 0x512A8D。 */
    if (bus->phys) {
        bus->phys[0x512a8du] = 2;   /* 高字节 = guest 字节 2A8C 的值 */
    }

    /* 2A94（armed 锁存，物理 0x512A94 字）不写：由 guest 扫描器在生成
     * 事件时清 0、释放无键扫描时置 1。直写会破坏事件生成时序。 */
    return req;
}
