/*
 * np560_tab5_keyboard.cpp - Tab5 物理键盘 → NP560 键盘矩阵桥。
 *
 * Tab5 键盘模块（SKU A164）：STM32F030 @I2C 0x6D，SDA=0/SCL=1，INT=50。
 * 普通模式事件：pressed = raw&0x80，row = (raw>>4)&7，col = raw&0x0F。
 * 键位 id = row*14 + col（M5Tab5-Keyboard-UserDemo ui_app.cpp:234 实证）。
 *
 * NP560+ 矩阵：64 位状态 bit = column*8 + row（V3.80 扫描器 2E:5F28）。
 * 布局数据源：np560-plus-emulator scripts/web/app.js matrixIndexes
 * （4F:29B2 三张键表 + 恢复的开关位置实证）。
 *
 * 映射原则：只映射有证据的物理触点；NP560 上不存在的 Tab5 键
 * （CTRL/ALT/符号排等）暂不映射（0xFF），后续真机验证时可调。
 */
#include "np560_tab5_keyboard.h"

#include <esp_log.h>

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "m5_tab5_keyboard.h"
#include "np560_periph.h"

static const char *TAG = "np560_kb";

/* Tab5 key_id → NP560 矩阵 index（col*8+row）。0xFF = 不映射。 */
static const uint8_t s_tab5_to_np560[70] = {
    /* Row 0: ESC 1 2 3 4 5 6 7 8 9 0 - + DEL */
    28,   /*  ESC -> Escape/Backspace 触点 (0x84 别名)  */
    2, 3, 0, 8, 16, 24, 32, 40, 48, 56,   /* 1..0（与 QWERTY 上排共触点） */
    20,   /*  '-' -> 逗号触点（最近标点）              */
    0xff, /*  '+'（NP560 无对应触点）                  */
    28,   /*  DEL -> Escape/Backspace 触点             */

    /* Row 1: ` ! @ # $ % ^ & * ( ) [ ] \（NP560 无这些键） */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

    /* Row 2: TAB q w e r t y u i o p ; ' BS */
    37,   /*  TAB -> Tab 触点（与 PageDown/下 同）     */
    2, 3, 0, 8, 16, 24, 32, 40, 48, 56,   /* q w e r t y u i o p      */
    0xff, /*  ';'                                     */
    28,   /*  ''' -> 撇号触点（web: "'" -> 28）        */
    28,   /*  BS -> Escape/Backspace 触点             */

    /* Row 3: SYM Aa a s d f g h j k l UP _ ENT */
    26,   /*  SYM -> SHIFT/符号模式触点（M26）         */
    26,   /*  Aa  -> SHIFT/符号模式触点               */
    10, 18, 1, 9, 17, 25, 33, 41, 49,     /* a s d f g h j k l        */
    61,   /*  UP -> 上翻页盘触点（M61）               */
    0xff, /*  '_'                                     */
    53,   /*  ENT -> Enter 触点（M53）                */

    /* Row 4: CTRL ALT z x c v b n m . LT DN RT SPC */
    0xff, /*  CTRL                                    */
    0xff, /*  ALT                                     */
    34, 6, 14, 22, 30, 38, 46,            /* z x c v b n m            */
    54,   /*  '.' -> 句点触点（与空格同，web 实证）    */
    45,   /*  LT -> 左方向触点（M45）                 */
    37,   /*  DN -> 下方向触点（M37）                 */
    29,   /*  RT -> 右方向触点（M29）                 */
    54,   /*  SPC -> 空格触点（M54）                  */
};

static Np560Periph *s_periph;
static m5::M5Tab5Keyboard s_kb;
static bool s_kb_ready;

static void on_key_event(m5_tab5_key_event_t event, void *arg)
{
    Np560Periph *periph = (Np560Periph *)arg;
    uint16_t key_id = (uint16_t)event.row * 14u + event.col;
    uint8_t matrix;

    if (key_id >= 70) {
        return;
    }
    matrix = s_tab5_to_np560[key_id];
    if (matrix == 0xff) {
        ESP_LOGI(TAG, "KB unmapped key_id=%u row=%u col=%u",
                 key_id, event.row, event.col);
        return;
    }
    np560_keypad_set_key(&periph->keypad, matrix, event.pressed);
    ESP_LOGI(TAG, "KB key_id=%u row=%u col=%u pressed=%d -> matrix=%u mask=%016llx",
             key_id, event.row, event.col, (int)event.pressed, matrix,
             (unsigned long long)periph->keypad.matrix);
}

/* 键盘模块探活/重试任务：成功后由组件轮询任务驱动回调。 */
static void keyboard_bridge_task(void *arg)
{
    while (!s_kb_ready) {
        m5_tab5_kb_err_t err = s_kb.begin(
            I2C_NUM_1, M5_TAB5_KB_DEFAULT_ADDR, M5_TAB5_KB_DEFAULT_SDA,
            M5_TAB5_KB_DEFAULT_SCL, M5_TAB5_KB_I2C_FREQ_400K,
            M5_TAB5_KB_INT_MODE_POLLING);

        if (err == M5_TAB5_KB_OK) {
            s_kb.setKeyCallback(on_key_event, s_periph);
            s_kb_ready = true;
            ESP_LOGI(TAG, "Tab5 keyboard ready (polling mode), mapping "
                          "70-key -> NP560 matrix");
            break;
        }
        ESP_LOGW(TAG, "keyboard not detected (%d), retry in 5 s", err);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelete(NULL);
}

extern "C" void np560_tab5_keyboard_start(Np560Periph *periph)
{
    s_periph = periph;
    xTaskCreate(keyboard_bridge_task, "np560_kb", 4096, NULL, 5, NULL);
}
