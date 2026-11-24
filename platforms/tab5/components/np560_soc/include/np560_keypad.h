/*
 * np560_keypad.h - NP560+ 键盘矩阵模型（F160/F164/F340）。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560.c:8983-9288：
 *   F160 (0xF160, 6 字节, word 访问)  offset0 = 矩阵列输入
 *                                     offset4 = 行驱动寄存器（回读）
 *   F340 (0xF340, 8 字节, word 访问)  offset0 = 事件锁存（读清）
 *                                     offset2/4 = 16 位 legacy 键状态
 *                                     offset6 = 事件码低 16 位
 *
 * 矩阵几何：64 位状态，bit = column*8 + row（V3.80 扫描器实证）。
 * 50:5416 提供八个 F164 行选通（低有效），50:5426 提供八个 F160 列掩码。
 * 驱动空闲值 0xFFFF；扫描器 OR C0FC 抬行选通、AND ~row 清一位后读 F160。
 * F160 是混合 GPIO 输入：bit12 = 外部电源/充电输入（22:F40C 实证）。
 */
#ifndef NP560_KEYPAD_H
#define NP560_KEYPAD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F160 矩阵区域（np560.c:76-79）。 */
#define NP560_KEY_MATRIX_BASE 0x0000f160u
#define NP560_KEY_MATRIX_SIZE 0x06u
#define NP560_KEYBOARD_BASE   0x0000f340u
#define NP560_KEYBOARD_SIZE   0x08u

typedef struct Np560Keypad {
    /* 64 位物理矩阵状态，bit = column*8 + row。宿主注入。 */
    uint64_t matrix;
    /* F160 offset4 行驱动寄存器（guest 写入）。 */
    uint16_t matrix_drive;
    /* legacy 16 位键状态（F340 offset2/4；QMP/web 兼容层）。 */
    uint32_t key_state;
    /* 事件码（F340 offset0/6；np560_keyboard_event 别名表输入）。 */
    uint32_t key_code;
    /* 外部电源/充电输入（F160 bit12）。 */
    bool external_power;
    /* 诊断计数（只增，观察用）。 */
    uint32_t matrix_reads;
    uint32_t matrix_active_reads;
} Np560Keypad;

/* 复位（np560.c:8983）：驱动 0xFFFF，状态清零。 */
void np560_keypad_reset(Np560Keypad *kp);

/* 注入矩阵触点：index 0..63（bit = column*8 + row）。 */
void np560_keypad_set_key(Np560Keypad *kp, unsigned index, bool pressed);

/* 注入 legacy 键状态位（0..15），按 legacy_matrix_indexes 展开到矩阵。
 * 对应 QEMU web_key_state 兼容路径（np560.c:9118-9127）。 */
void np560_keypad_set_legacy_key(Np560Keypad *kp, unsigned bit, bool pressed);

/* 注入事件码（原始码，读取时经导航别名表映射）。 */
void np560_keypad_set_event_code(Np560Keypad *kp, uint32_t code);

/* F160 offset0 矩阵输入合成（np560_key_matrix_input，np560.c:9089）。
 * 返回 16 位输入：bit0..7 列（低有效），bit12 = 外部电源。 */
uint16_t np560_keypad_matrix_input(const Np560Keypad *kp, uint16_t drive);

/* 行选通掩码表（np560.c:9084）：row index → F164 驱动位。 */
uint16_t np560_keypad_row_mask(unsigned row);

/* word MMIO 分发（对应 np560_key_matrix_read/write + np560_keyboard_read/
 * write）。physical 为 16 位事务的物理字地址；返回 0 表示未命中区域。 */
uint16_t np560_keypad_mmio_read(Np560Keypad *kp, uint32_t physical);
void np560_keypad_mmio_write(Np560Keypad *kp, uint32_t physical,
                             uint16_t value);

#ifdef __cplusplus
}
#endif

#endif /* NP560_KEYPAD_H */
