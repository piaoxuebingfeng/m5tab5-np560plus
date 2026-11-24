/*
 * np560_keypad.c - NP560+ 键盘矩阵模型。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560.c:8983-9288。
 * 语义要点（全部有 QEMU 注释/原文证据）：
 *  - 矩阵状态 64 位，bit = column*8 + row（V3.80 扫描器 2E:5F28）。
 *  - 行选通低有效：扫描器 OR C0FC 抬行、AND ~row 清一位再读 F160；
 *    selected = (~drive) & 0xC0FC 恰为一行掩码时为有效扫描。
 *  - F160 bit0..7 = 列输入（低有效），同列多键按下时合并（无源矩阵）。
 *  - F160 bit12 = 外部电源/充电输入（22:F40C 低电清除功耗锁存）。
 *  - F340 offset0 事件锁存：读时经 np560_keyboard_event 别名表映射，
 *    非零事件读后清零；offset6 提供原始码低 16 位。
 *  - legacy 键状态位（web_key_state 0..15）经 legacy_matrix_indexes 展开。
 */
#include "np560_keypad.h"

#include <stddef.h>

/* 行选通掩码（np560.c:9084-9087）：50:5416 提供的八个 F164 行。 */
static const uint16_t np560_matrix_row_masks[8] = {
    0x0004, 0x0008, 0x0010, 0x0020,
    0x0040, 0x0080, 0x4000, 0x8000,
};

uint16_t np560_keypad_row_mask(unsigned row)
{
    return row < 8 ? np560_matrix_row_masks[row] : 0;
}

/* legacy 别名索引（np560.c:9118-9121）：与 web 矩阵恢复的开关位置一致
 * （左/右是 M45/M29）。 */
static const uint8_t legacy_matrix_indexes[16] = {
    61, 37, 45, 29, 53, 28, 28, 37,
    60, 52, 44, 4, 61, 37, 54, 55,
};

/* 事件别名表（np560_keyboard_event，np560.c:8998-9018）。 */
static uint16_t np560_keypad_event_alias(uint32_t raw)
{
    switch (raw) {
    case 0x100: /* up */
    case 0x10c: /* page up */
        return 0x0082;
    case 0x101: /* down */
    case 0x107: /* tab */
    case 0x10d: /* page down */
        return 0x0083;
    case 0x102: /* left */
    case 0x105: /* escape */
    case 0x106: /* backspace */
        return 0x0084;
    case 0x103: /* right */
    case 0x104: /* enter */
        return 0x0085;
    default:
        return raw & 0xffff;
    }
}

void np560_keypad_reset(Np560Keypad *kp)
{
    kp->matrix = 0;
    kp->matrix_drive = 0xffff;
    kp->key_state = 0;
    kp->key_code = 0;
    kp->external_power = false;
    kp->matrix_reads = 0;
    kp->matrix_active_reads = 0;
}

void np560_keypad_set_key(Np560Keypad *kp, unsigned index, bool pressed)
{
    if (index >= 64) {
        return;
    }
    if (pressed) {
        kp->matrix |= 1ull << index;
    } else {
        kp->matrix &= ~(1ull << index);
    }
}

void np560_keypad_set_legacy_key(Np560Keypad *kp, unsigned bit, bool pressed)
{
    if (bit >= 16) {
        return;
    }
    if (pressed) {
        kp->key_state |= 1u << bit;
    } else {
        kp->key_state &= ~(1u << bit);
    }
    np560_keypad_set_key(kp, legacy_matrix_indexes[bit], pressed);
}

void np560_keypad_set_event_code(Np560Keypad *kp, uint32_t code)
{
    kp->key_code = code;
}

/* F160 offset0 输入合成（np560.c:9089-9141）。 */
uint16_t np560_keypad_matrix_input(const Np560Keypad *kp, uint16_t drive)
{
    uint64_t matrix = kp->matrix;
    uint16_t input = 0x00ff | (kp->external_power ? 0x1000u : 0);
    unsigned index;

    for (index = 0; index < 64; index++) {
        uint16_t row = np560_matrix_row_masks[index & 7];
        uint16_t column = 1u << (index >> 3);

        /* 行选通低有效：驱动位为 0 表示该行被选中。 */
        if ((matrix & (1ull << index)) && !(drive & row)) {
            input &= ~column;
        }
    }
    return input;
}

uint16_t np560_keypad_mmio_read(Np560Keypad *kp, uint32_t physical)
{
    switch (physical) {
    case NP560_KEY_MATRIX_BASE + 0: /* F160 矩阵列输入。 */
    {
        uint16_t input = np560_keypad_matrix_input(kp, kp->matrix_drive);

        kp->matrix_reads++;
        if ((input & 0x00ff) != 0x00ff) {
            kp->matrix_active_reads++;
        }
        return input;
    }
    case NP560_KEY_MATRIX_BASE + 4: /* F164 驱动寄存器回读。 */
        return kp->matrix_drive;
    case NP560_KEYBOARD_BASE + 0: /* F340 事件锁存（读清，np560.c:9244）。 */
    {
        uint16_t event = np560_keypad_event_alias(kp->key_code);

        if (event) {
            kp->key_code = 0;
        }
        return event;
    }
    case NP560_KEYBOARD_BASE + 2: /* legacy 键状态低 16 位。 */
        return kp->key_state & 0xffff;
    case NP560_KEYBOARD_BASE + 4: /* legacy 键状态高 16 位。 */
        return kp->key_state >> 16;
    case NP560_KEYBOARD_BASE + 6: /* 原始事件码低 16 位。 */
        return kp->key_code & 0xffff;
    default:
        return 0;
    }
}

void np560_keypad_mmio_write(Np560Keypad *kp, uint32_t physical,
                             uint16_t value)
{
    if (physical == NP560_KEY_MATRIX_BASE + 4) { /* F164 驱动。 */
        kp->matrix_drive = value;
        return;
    }
    if (physical == NP560_KEYBOARD_BASE + 0 && value == 0) {
        /* F340 写 0 清事件码（np560.c:9266-9274）。 */
        kp->key_code = 0;
    }
}
