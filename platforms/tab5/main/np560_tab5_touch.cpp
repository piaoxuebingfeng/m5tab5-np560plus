/*
 * np560_tab5_touch.cpp - Tab5 触摸屏 -> NP560 机身按键热区桥。
 *
 * 数据源：QEMU Web scripts/web/app.js:619-698 buildSkinKeyboard() 上半部分
 * （裁切 y=86..780）的机身实体按键热区，1254 源坐标系 + matrixIndexes
 * （app.js:63-92）。这些是原厂机身开关，与图下方虚拟键盘无关。
 *
 * 坐标换算：皮肤 raw 1280x708 由 1254x694 缩放，贴在 1280x720 屏幕 y=6：
 *   screen_x = src_x * 1280 / 1254
 *   screen_y = (src_y - 86) * 1280 / 1254 + 6
 *
 * 实现：每个热区一个透明 LVGL 按钮（叠在 canvas 之上），按下/释放走与物理
 * 键盘桥 np560_tab5_keyboard.cpp 相同的注入路径：
 *   np560_keypad_set_key() + request_key_wake/release()。
 * 不写 canvas buffer、不碰 rotation/flush，三条安全不变量保持不变。
 */
#include "np560_tab5_touch.h"

#include <esp_log.h>

#include <cstdint>

#include "lvgl.h"
#include "np560_keypad.h"
#include "np560_periph.h"

static const char *TAG = "np560_touch";

/* MP3 触摸入口临时禁用：进入 MP3 后按返回仍会触发 ROM TRAP 自陷，
 * 修复前先关掉触摸入口（物理 MP3 键不受影响）。见 TODO.md。 */
#define NP560_TOUCH_MP3_ENABLED 0

/* Web 源热区：src_x/src_y/src_w/src_h（1254 坐标系）+ NP560 矩阵索引。 */
struct TouchZone {
    uint16_t sx, sy, sw, sh;
    uint8_t matrix;
    const char *label;
};

static const TouchZone s_zones[] = {
    /* 左侧：F1..F4 = MP3 文件/曲目/设置/退出(0x8E=返回)，M60/52/44/4 */
    {87, 198, 125, 37, 60, "F1-A"},
    {87, 258, 125, 37, 52, "F2-B"},
    {87, 318, 125, 37, 44, "F3-C"},
    {87, 378, 125, 37,  4, "F4-D/exit"},
    /* 左侧翻页盘（不是右侧光标键）：M11/19/27 */
    {117, 465, 64, 40, 11, "page-up"},
    {117, 505, 64, 50, 19, "page-enter"},
    {117, 555, 64, 40, 27, "page-down"},
    /* 左侧设置 / 返回（Escape 与 F4 同为 M4/0x8E，原厂同触点） */
    {88, 634, 126, 38, 12, "setup"},
    {88, 696, 126, 38,  4, "back"},
    /* 右侧快捷列：同步学习/多科助考/我的硬盘/MP3 = M59/51/43/35 */
    {1038, 198, 129, 37, 59, "sync-learn"},
    {1038, 258, 129, 37, 51, "multi-exam"},
    {1038, 318, 129, 37, 43, "my-disk"},
#if NP560_TOUCH_MP3_ENABLED
    {1038, 377, 129, 37, 35, "mp3"},
#endif
    /* 右侧光标键：上/左/确认/右/下 = M61/45/53/29/37 */
    {1080, 461, 58, 49, 61, "up"},
    {1039, 501, 50, 60, 45, "left"},
    {1080, 501, 61, 60, 53, "enter"},
    {1132, 501, 43, 60, 29, "right"},
    {1080, 555, 58, 42, 37, "down"},
    /* 右侧发音/空格 = M54 */
    {1039, 634, 129, 37, 54, "speak/space"},
};

static Np560Periph *s_periph;

static void touch_event_cb(lv_event_t *event)
{
    const TouchZone *zone = (const TouchZone *)lv_event_get_user_data(event);
    if (!zone || !s_periph) return;

    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        np560_keypad_set_key(&s_periph->keypad, zone->matrix, true);
        np560_periph_request_key_wake(s_periph);
        ESP_LOGI(TAG, "TOUCH %-12s M%02u pressed", zone->label, zone->matrix);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        np560_keypad_set_key(&s_periph->keypad, zone->matrix, false);
        np560_periph_request_key_release(s_periph);
        ESP_LOGI(TAG, "TOUCH %-12s M%02u released", zone->label, zone->matrix);
    }
}

void np560_tab5_touch_create(Np560Periph *periph)
{
    s_periph = periph;
    for (unsigned i = 0; i < sizeof(s_zones) / sizeof(s_zones[0]); i++) {
        const TouchZone *zone = &s_zones[i];
        int x = (int)((uint32_t)zone->sx * 1280u / 1254u);
        int y = (int)(((int)zone->sy - 86) * 1280 / 1254) + 6;
        int w = (int)((uint32_t)zone->sw * 1280u / 1254u);
        int h = (int)((uint32_t)zone->sh * 1280u / 1254u);

        lv_obj_t *btn = lv_btn_create(lv_scr_act());
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, w, h);
        /* 完全透明：不改皮肤、不改画面，只接收触摸 */
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_outline_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, touch_event_cb, LV_EVENT_PRESSED, (void *)zone);
        lv_obj_add_event_cb(btn, touch_event_cb, LV_EVENT_RELEASED, (void *)zone);
        lv_obj_add_event_cb(btn, touch_event_cb, LV_EVENT_PRESS_LOST, (void *)zone);
    }
    ESP_LOGI(TAG, "touch zones ready: %u (Web upper-shell hotspots)",
             (unsigned)(sizeof(s_zones) / sizeof(s_zones[0])));
}
