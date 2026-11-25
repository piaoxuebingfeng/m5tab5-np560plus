/*
 * np560_lcd.h - ST7529 LCD 控制器模型（NP560+ Tab5 侧宿主）。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560.c:9290-9836（np560_st7529_*），剥离
 * 诊断观察器（frame/status trace、record_frame 的栈回溯采样）。
 * 语义纪律：以 QEMU 实现为唯一权威，不按"看起来合理"补行为。
 *
 * 关键事实（源码注释摘录）：
 *  - GRAM 85 列 x 160 行，每列 3 个像素（SEG 组），5 位灰度级别。
 *  - 可见面 240x120：NP560 玻璃跨 ST7529 两个 80-COM bank，物理行 0..59
 *    接第二 bank 正序，60..119 接第一 bank 逆序（np560_st7529_visible_level）。
 *    V3.80 编程 COMSCN=001 → 可见行 0..119 读 GRAM 行 139..20。
 *  - 复位选择 2B3P 与零/零窗口；原厂驱动显式编程 3B3P 与窗口。
 *  - MMIO：物理 0x00F00000，4 字节；offset&2==0 → 命令/STREAD 状态，
 *    offset&2!=0 → 数据口；仅字节访问有效（valid.min=max=1）。
 *  - 读 DDRAM 有总线保持器：RAMRD/RMWIN 后第一次读是哑读（返回锁存旧值）。
 */
#ifndef NP560_LCD_H
#define NP560_LCD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* hw/dmcu/np560.c NP560_ST7529_* 与 target/dmcu/cpu.h DMCU_LCD_*。 */
#define NP560_ST7529_RAM_COLUMNS       85u
#define NP560_ST7529_RAM_LINES         160u
#define NP560_ST7529_PIXELS_PER_COLUMN 3u
#define NP560_ST7529_RAM_WIDTH \
    (NP560_ST7529_RAM_COLUMNS * NP560_ST7529_PIXELS_PER_COLUMN) /* 255 */

#define NP560_LCD_WIDTH    240u   /* 可见面板宽（cpu.h DMCU_LCD_WIDTH） */
#define NP560_LCD_HEIGHT   120u   /* 可见面板高 */
#define NP560_LCD_VISIBLE_LINE 20u

/* 自定义玻璃状态电极（annunciator）：QEMU np560_get_lcd_annunciators 读取
 * GRAM line 19 / 140 的 240-bit 电平，导出为 2 平面 × 30 字节。Web 前端在
 * 主画面上方额外渲染 12 行图标，故完整源帧为 240x132。 */
#define NP560_LCD_ANNUNCIATOR_HEIGHT 12u
#define NP560_LCD_ANNUNCIATOR_PLANES 2u
#define NP560_LCD_ANNUNCIATOR_BYTES_PER_PLANE 30u
#define NP560_LCD_ANNUNCIATOR_BYTES \
    (NP560_LCD_ANNUNCIATOR_PLANES * NP560_LCD_ANNUNCIATOR_BYTES_PER_PLANE)
#define NP560_LCD_FRAME_HEIGHT \
    (NP560_LCD_ANNUNCIATOR_HEIGHT + NP560_LCD_HEIGHT) /* 132 */

/* ST7529 MMIO 物理基址（np560.c:34）。 */
#define NP560_ST7529_BASE 0x00f00000u
#define NP560_ST7529_MMIO_SIZE 4u

typedef struct Np560Lcd {
    /* GRAM：每行 255 个 5 位灰度级别（0..31）。 */
    uint8_t gram[NP560_ST7529_RAM_LINES][NP560_ST7529_RAM_WIDTH];

    uint8_t command;
    uint8_t parameters[16];
    uint8_t parameter_index;
    uint8_t parameter_count;
    uint8_t gray_pwm[2][16];
    bool gray_pwm_programmed[2]; /* 复位调色板值未定义，仅记录是否编程。 */

    uint8_t column_start;
    uint8_t column_end;
    uint8_t line_start;
    uint8_t line_end;
    uint8_t column;
    uint8_t line;
    uint8_t pixel;
    uint16_t write_latch; /* 未完成的 2B3P/3B3P 总线组。 */
    uint8_t read_pixel;
    uint8_t read_latch;   /* DDRAM 总线保持器：第一次 MPU 读是哑读。 */

    uint8_t scan[3];        /* DATSDR 参数：LI/CI/CLR + C/L + 灰度模式。 */
    uint8_t com_scan;       /* COMSCN CD2/CD1/CD0。 */
    uint8_t display_lines;
    uint8_t partial_start;
    uint8_t partial_end;
    uint8_t area_scroll[4];
    uint8_t scroll_start;
    bool partial_display;
    bool extended;
    bool display_on;
    bool inverse;
    bool sleeping;
    bool ram_write;
    bool ram_read;
    bool read_modify_write;

    /* 状态计数器（非诊断 trace，属于架构状态）。 */
    uint64_t command_writes;
    uint64_t data_writes;
    uint64_t completed_frames;
} Np560Lcd;

/* 复位（np560_st7529_reset）：2B3P、零/零窗口、全屏滚动、SLPIN。 */
void np560_lcd_reset(Np560Lcd *lcd);

/* 命令口写（offset&2==0）。 */
void np560_lcd_command(Np560Lcd *lcd, uint8_t command);

/* 数据口写（offset&2!=0）。 */
void np560_lcd_data_write(Np560Lcd *lcd, uint8_t value);

/* 数据口读（offset&2!=0）：总线保持器语义，RAMRD 首读为哑读。 */
uint8_t np560_lcd_data_read(Np560Lcd *lcd);

/* 状态读 STREAD（offset&2==0，np560.c:9809）。 */
uint8_t np560_lcd_status_read(const Np560Lcd *lcd);

/* 可见面级别（np560_st7529_visible_level）：返回光学浓度 0..31（0=无
 * 驱动=亮），显示关/睡眠/部分显示未驱动电极返回 0。x<240, y<120。 */
uint8_t np560_lcd_visible_level(const Np560Lcd *lcd, unsigned x, unsigned y);

/* 渲染可见面 240x120 为 RGB565。光学浓度 0..31 取反后映射亮度：
 * 浓度 0（无驱动）→白 0xFFFF，浓度 31（满驱动）→黑 0x0000，与 QEMU Web
 * 前端方向一致。out_stride 为输出缓冲每行像素数。 */
void np560_lcd_render(const Np560Lcd *lcd, uint16_t *out, unsigned out_stride);

/* 导出两平面 240-bit 状态电极电平（与 QEMU np560_get_lcd_annunciators 逐位
 * 一致）：plane0 = GRAM line 19，plane1 = line 140，bit 按 x 的小端序打包。
 * 显示关/睡眠时全零。纯硬件状态，不合成任何图标语义或电池数值。 */
void np560_lcd_annunciators(const Np560Lcd *lcd,
                             uint8_t planes[NP560_LCD_ANNUNCIATOR_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* NP560_LCD_H */
