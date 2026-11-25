/*
 * np560_tab5_touch.h - Tab5 触摸屏 -> NP560 机身按键热区桥。
 *
 * 在 LVGL 全屏 canvas 之上放透明按钮，对齐 Web buildSkinKeyboard() 上半部分
 * （裁切 y=86..780）的机身按键热区。按下/释放复用 np560_keypad_set_key() +
 * np560_periph_request_key_wake/release()，与物理键盘桥完全同效。
 */
#ifndef NP560_TAB5_TOUCH_H
#define NP560_TAB5_TOUCH_H

struct Np560Periph;

#ifdef __cplusplus
extern "C" {
#endif

/* 创建透明触摸热区按钮（必须在 LVGL display lock 内、canvas 之后调用）。 */
void np560_tab5_touch_create(Np560Periph *periph);

#ifdef __cplusplus
}
#endif

#endif /* NP560_TAB5_TOUCH_H */
