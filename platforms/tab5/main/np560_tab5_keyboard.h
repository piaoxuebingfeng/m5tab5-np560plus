/*
 * np560_tab5_keyboard.h - Tab5 物理键盘（I2C STM32 @0x6D）→ NP560 矩阵桥。
 */
#ifndef NP560_TAB5_KEYBOARD_H
#define NP560_TAB5_KEYBOARD_H

struct Np560Periph;

#ifdef __cplusplus
extern "C" {
#endif

/* 启动键盘桥任务：轮询模式初始化键盘模块（未插上则周期重试），
 * 按键事件经映射表写入 periph->keypad 矩阵状态。 */
void np560_tab5_keyboard_start(Np560Periph *periph);

#ifdef __cplusplus
}
#endif

#endif /* NP560_TAB5_KEYBOARD_H */
