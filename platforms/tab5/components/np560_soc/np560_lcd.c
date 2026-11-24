/*
 * np560_lcd.c - ST7529 LCD 控制器模型实现。
 *
 * 逐函数对照 qemu-dmcu/hw/dmcu/np560.c（行号为该文件）：
 *   np560_st7529_reset            9290
 *   np560_st7529_parameter_count  9304
 *   np560_st7529_apply_parameters 9343
 *   np560_st7529_command          9427
 *   np560_st7529_advance_group    9501
 *   np560_st7529_output_level     9536
 *   np560_st7529_panel_darkness   9570
 *   np560_st7529_visible_level    9576
 *   np560_st7529_pixel_x          9657
 *   np560_st7529_store_pixel      9666（剥离 status_write 诊断 trace）
 *   np560_st7529_load_pixel       9697
 *   np560_st7529_data_write       9706（剥离 record_frame 观察器）
 *   np560_st7529_data_read        9766
 *   np560_st7529_read/status      9809
 *
 * 未建模（QEMU 亦未建模）：灰度 PWM/FRC 模拟曲线、ANASET、EEPROM（EPCTIN/EPINT）。
 */
#include "np560_lcd.h"
#include <string.h>

/* 两数取小（QEMU 用 glib MIN）。 */
static unsigned lcd_min(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

void np560_lcd_reset(Np560Lcd *lcd)
{
    memset(lcd, 0, sizeof(*lcd));
    /* ST7529 v1.8 pp34-35：复位选择 2B3P 与零/零窗口。
     * 原厂驱动显式编程 3B3P 模式与窗口。 */
    lcd->scan[2] = 1;
    lcd->area_scroll[3] = 3; /* 复位选择全屏滚动模式。 */
    lcd->display_lines = 4;
    lcd->sleeping = true;
}

/* ST7529 v1.8 pp36-38,50：只有 EXT IN/OUT 是共享命令。
 * 特别地，EXT=0 的 0x20 是 PWRCTRL（1 字节），EXT=1 的 0x20 是
 * 完整 16 项奇帧灰度表。 */
static uint8_t st7529_parameter_count(uint8_t command, bool extended)
{
    if (extended) {
        switch (command) {
        case 0x20: /* Gray 1 Set */
        case 0x21: /* Gray 2 Set */
            return 16;
        case 0x32: /* ANASET；模拟行为未建模。 */
            return 3;
        case 0xcd: /* EPCTIN；EEPROM 行为未建模。 */
            return 1;
        default:
            return 0;
        }
    }
    switch (command) {
    case 0x15: /* CASET */
    case 0x75: /* LASET */
    case 0x81: /* VOLCTRL */
    case 0xa8: /* PTLIN */
        return 2;
    case 0xbc: /* DATSDR */
    case 0xca: /* DISCTRL */
        return 3;
    case 0x20: /* PWRCTRL */
    case 0xab: /* SCSTART */
    case 0xbb: /* COMSCN */
    case 0x07: /* EPINT */
        return 1;
    case 0xaa: /* ASCSET */
        return 4;
    default:
        return 0;
    }
}

static void st7529_apply_parameters(Np560Lcd *lcd)
{
    if (lcd->extended) {
        if (lcd->command == 0x20 || lcd->command == 0x21) {
            unsigned bank = lcd->command - 0x20;
            unsigned i;

            /* PB1..PB16 各含一个 5 位 PWM 值。保留编程的寄存器，
             * 不发明未文档化的 32 码到 16 级 FRC 选择或模拟传递曲线。 */
            for (i = 0; i < 16; i++) {
                lcd->gray_pwm[bank][i] = lcd->parameters[i] & 0x1f;
            }
            lcd->gray_pwm_programmed[bank] = true;
        }
        return;
    }
    switch (lcd->command) {
    case 0x15: /* 列地址选择三个 SEG 像素的组。 */
        lcd->column_start = lcd_min(lcd->parameters[0],
                                    NP560_ST7529_RAM_COLUMNS - 1);
        lcd->column_end = lcd_min(lcd->parameters[1],
                                  NP560_ST7529_RAM_COLUMNS - 1);
        if (lcd->column_end < lcd->column_start) {
            lcd->column_end = lcd->column_start;
        }
        break;
    case 0x75:
        lcd->line_start = lcd_min(lcd->parameters[0],
                                  NP560_ST7529_RAM_LINES - 1);
        lcd->line_end = lcd_min(lcd->parameters[1],
                                NP560_ST7529_RAM_LINES - 1);
        if (lcd->line_end < lcd->line_start) {
            lcd->line_end = lcd->line_start;
        }
        break;
    case 0xbc:
        memcpy(lcd->scan, lcd->parameters, sizeof(lcd->scan));
        /* 不支持的灰度模式：QEMU 记 LOG_UNIMP 后按原样保存；
         * 数据通路对 (scan[2]&7) 不是 1/2 的情况直接丢弃写。 */
        break;
    case 0xbb:
        lcd->com_scan = lcd->parameters[0] & 7;
        break;
    case 0xca:
        lcd->display_lines = lcd_min(
            ((lcd->parameters[1] & 0x3f) + 1u) * 4u,
            NP560_ST7529_RAM_LINES);
        break;
    case 0xa8: {
        unsigned start = lcd->parameters[0] & 0x3f;
        unsigned end = lcd->parameters[1] & 0x3f;

        /* 7.5 节：越界部分显示设置为无操作。地址是四行 RAM 块。 */
        if (start >= NP560_ST7529_RAM_LINES / 4 ||
            end >= NP560_ST7529_RAM_LINES / 4) {
            break;
        }
        if (start > end) {
            /* 反向部分范围：无操作（QEMU 记 LOG_UNIMP）。 */
            break;
        }
        lcd->partial_start = start;
        lcd->partial_end = end;
        lcd->partial_display = true;
        break;
    }
    case 0xaa:
        memcpy(lcd->area_scroll, lcd->parameters, sizeof(lcd->area_scroll));
        break;
    case 0xab:
        lcd->scroll_start = lcd->parameters[0] & 0x3f;
        break;
    default:
        break;
    }
}

void np560_lcd_command(Np560Lcd *lcd, uint8_t command)
{
    lcd->command = command;
    lcd->command_writes++;
    lcd->parameter_index = 0;
    lcd->parameter_count = st7529_parameter_count(command, lcd->extended);
    lcd->ram_write = false;
    lcd->ram_read = false;
    lcd->read_modify_write = false;
    /* 任何命令都终止 RAM 传输，包括未完成的像素组。后续 RAMWR/RAMRD
     * 从字节零开始，而不是旧的相位。 */
    lcd->pixel = 0;
    lcd->read_pixel = 0;
    lcd->write_latch = 0;

    if (command == 0x30 || command == 0x31) {
        lcd->extended = command == 0x31;
        return;
    }
    if (lcd->extended) {
        /* EXT=1 bank 不执行普通 RAM/显示命令。 */
        return;
    }

    switch (command) {
    case 0x5c: /* RAMWR */
        lcd->column = lcd->column_start;
        lcd->line = lcd->line_start;
        lcd->pixel = 0;
        lcd->ram_write = true;
        break;
    case 0x5d: /* RAMRD */
        lcd->column = lcd->column_start;
        lcd->line = lcd->line_start;
        lcd->pixel = 0;
        lcd->ram_read = true;
        break;
    case 0xe0: /* RMWIN，EXT=0（ST7529 v1.8 pp45-46，例 p63）。 */
        if (!lcd->extended) {
            lcd->column = lcd->column_start;
            lcd->line = lcd->line_start;
            lcd->read_modify_write = true;
            lcd->ram_read = true;
            lcd->ram_write = true;
        }
        break;
    case 0xee: /* RMWOUT；同任何命令，取消上述模式。 */
        break;
    case 0x94: /* SLPOUT */
        lcd->sleeping = false;
        break;
    case 0x95: /* SLPIN */
        lcd->sleeping = true;
        break;
    case 0xa6: /* DISNOR */
        lcd->inverse = false;
        break;
    case 0xa7: /* DISINV */
        lcd->inverse = true;
        break;
    case 0xae: /* DISOFF */
        lcd->display_on = false;
        break;
    case 0xaf: /* DISON */
        lcd->display_on = true;
        break;
    case 0xa9: /* PTLOUT */
        lcd->partial_display = false;
        break;
    default:
        break;
    }
}

static bool st7529_advance_group(Np560Lcd *lcd)
{
    bool wrapped = false;

    /* DATSDR C/L=0 先扫列组；C/L=1 先扫行。 */
    if (!(lcd->scan[0] & 0x04)) {
        if (lcd->column < lcd->column_end) {
            lcd->column++;
        } else {
            lcd->column = lcd->column_start;
            if (lcd->line < lcd->line_end) {
                lcd->line++;
            } else {
                lcd->line = lcd->line_start;
                wrapped = true;
            }
        }
    } else if (lcd->line < lcd->line_end) {
        lcd->line++;
    } else {
        lcd->line = lcd->line_start;
        if (lcd->column < lcd->column_end) {
            lcd->column++;
        } else {
            lcd->column = lcd->column_start;
            wrapped = true;
        }
    }
    if (wrapped) {
        lcd->completed_frames++;
    }
    return wrapped;
}

/* 未驱动的部分显示电极返回 -1，不是灰度零。 */
static int st7529_output_level(const Np560Lcd *lcd,
                               unsigned segment, unsigned line)
{
    /*
     * ST7529 v1.8 7.3.2/7.3.3 节与 RAM 映射：LI/CI 反转 DDRAM 到 COM/SEG
     * 的对应关系，不反转升序的 MPU 计数器。CI 反转全部 85 个列组，保留
     * 每组三个像素。CLR 控制组内总线排列。先应用于控制器的完整 RAM
     * 几何，再裁剪到 NP560 面板，包括光栅外的状态电极。
     */
    if (lcd->scan[0] & 0x02) {
        unsigned pixel = segment % NP560_ST7529_PIXELS_PER_COLUMN;
        unsigned column = segment / NP560_ST7529_PIXELS_PER_COLUMN;

        segment = (NP560_ST7529_RAM_COLUMNS - 1 - column) *
            NP560_ST7529_PIXELS_PER_COLUMN + pixel;
    }
    if (lcd->scan[0] & 0x01) {
        line = NP560_ST7529_RAM_LINES - 1 - line;
    }
    if (lcd->partial_display &&
        (line / 4 < lcd->partial_start || line / 4 > lcd->partial_end)) {
        return -1;
    }
    return lcd->gram[line][segment] & 0x1f;
}

/* 导出光学浓度而非控制器 DISINV 后的代码。NP560 正性玻璃无驱动为亮。
 * 原厂 14:69B4 选 A7，14:6B94..6BBC 以 F0（RAM 码 30）发送原生墨色 15，
 * 该墨必须保持深色。本转换不修改 GRAM 或 DISNOR/DISINV 状态。
 * 精确的模拟 PWM/对比度曲线未建模。 */
static uint8_t st7529_panel_darkness(const Np560Lcd *lcd, uint8_t level)
{
    return lcd->inverse ? level : (uint8_t)(0x1f - level);
}

static uint8_t st7529_visible_level(const Np560Lcd *lcd,
                                    unsigned x, unsigned y)
{
    unsigned panel_y;
    int level;

    /*
     * NP560+ 玻璃跨 ST7529 的两个 80-COM bank：物理行 0..59 接第二
     * bank 正序引脚，60..119 接第一 bank 逆序引脚。先建模该固定模块
     * 接线，再应用 COMSCN。CD1 反转控制器前半，CD0 反转后半，CD2
     * 交换两半。
     *
     * V3.80 编程 COMSCN=001。结合模块接线，可见行 0..119 读 GRAM 行
     * 139..20。该全局自底向上顺序也可在 18:6454 字体位块传送器中独立
     * 验证：每个后续字形行减 0x3c 帧缓冲字。把玻璃当作两个正常接线
     * 的 bank 会导致只有下部文字正立、上部文字垂直镜像。
     */
    panel_y = y < NP560_LCD_HEIGHT / 2 ?
        y + NP560_LCD_HEIGHT / 2 : NP560_LCD_HEIGHT - 1 - y;
    if (lcd->com_scan & 4) {
        panel_y = (panel_y + NP560_LCD_HEIGHT / 2) % NP560_LCD_HEIGHT;
    }
    if (panel_y < NP560_LCD_HEIGHT / 2) {
        if (lcd->com_scan & 2) {
            panel_y = NP560_LCD_HEIGHT / 2 - 1 - panel_y;
        }
    } else if (lcd->com_scan & 1) {
        panel_y = NP560_LCD_HEIGHT + NP560_LCD_HEIGHT / 2 - 1 - panel_y;
    }
    level = st7529_output_level(lcd, x, panel_y + NP560_LCD_VISIBLE_LINE);

    if (!lcd->display_on || lcd->sleeping || level < 0) {
        return 0;
    }
    return st7529_panel_darkness(lcd, (uint8_t)level);
}

uint8_t np560_lcd_visible_level(const Np560Lcd *lcd, unsigned x, unsigned y)
{
    return st7529_visible_level(lcd, x, y);
}

static unsigned st7529_pixel_x(const Np560Lcd *lcd, unsigned pixel)
{
    if (lcd->scan[1] & 1) { /* CLR 反转每组内的 P1/P2/P3。 */
        pixel = NP560_ST7529_PIXELS_PER_COLUMN - 1 - pixel;
    }
    return lcd->column * NP560_ST7529_PIXELS_PER_COLUMN + pixel;
}

static void st7529_store_pixel(Np560Lcd *lcd, unsigned pixel, uint8_t level)
{
    unsigned x = st7529_pixel_x(lcd, pixel);

    if (lcd->line < NP560_ST7529_RAM_LINES &&
        x < NP560_ST7529_RAM_WIDTH) {
        lcd->gram[lcd->line][x] = level;
        /* QEMU 侧 line>=140 的 status_write 诊断 trace 不移植。 */
    }
}

static uint8_t st7529_load_pixel(const Np560Lcd *lcd, unsigned pixel)
{
    unsigned x = st7529_pixel_x(lcd, pixel);

    return lcd->line < NP560_ST7529_RAM_LINES &&
           x < NP560_ST7529_RAM_WIDTH ? lcd->gram[lcd->line][x] & 0x1f : 0;
}

void np560_lcd_data_write(Np560Lcd *lcd, uint8_t value)
{
    bool wrapped = false;

    lcd->data_writes++;
    if (lcd->parameter_index < lcd->parameter_count) {
        lcd->parameters[lcd->parameter_index++] = value;
        if (lcd->parameter_index == lcd->parameter_count) {
            st7529_apply_parameters(lcd);
        }
        return;
    }
    if (!lcd->ram_write) {
        return;
    }
    switch (lcd->scan[2] & 7) {
    case 1:
        /* ST7529 v1.8 p16：8 位 2B3P 在第二字节后提交。
         * 第一字节：P0[4:0],P1[4:2]。第二字节：P1[1:0],X,P2[4:0]。
         * 第二字节的 D5 被忽略，不属于任何像素。 */
        if (!lcd->pixel) {
            lcd->write_latch = value;
            lcd->pixel = 1;
            return;
        }
        st7529_store_pixel(lcd, 0, lcd->write_latch >> 3);
        st7529_store_pixel(lcd, 1,
                           ((lcd->write_latch & 7) << 2) | (value >> 6));
        st7529_store_pixel(lcd, 2, value & 0x1f);
        lcd->pixel = 0;
        wrapped = st7529_advance_group(lcd);
        break;
    case 2:
        /* p17：3B3P 同样整组提交，在第三次写时。只写一两个字节后跟
         * 命令必须保持 GRAM 完好。 */
        if (lcd->pixel < 2) {
            lcd->write_latch = (uint16_t)((lcd->write_latch << 8) | value);
            lcd->pixel++;
            return;
        }
        st7529_store_pixel(lcd, 0, lcd->write_latch >> 11);
        st7529_store_pixel(lcd, 1, (lcd->write_latch >> 3) & 0x1f);
        st7529_store_pixel(lcd, 2, value >> 3);
        lcd->pixel = 0;
        wrapped = st7529_advance_group(lcd);
        break;
    default:
        return;
    }
    if (lcd->read_modify_write) {
        /* 每次完成的写前进到下一组。它的下一次读序列以哑周期开始，
         * 与写相位无关。 */
        lcd->read_pixel = 0;
    }
    (void)wrapped;
    /* QEMU 侧 wrapped 时的 record_frame 诊断采样不移植；
     * completed_frames 计数已在 advance_group 中维护。 */
}

uint8_t np560_lcd_data_read(Np560Lcd *lcd)
{
    uint8_t value;
    uint8_t previous = lcd->read_latch;

    if (!lcd->ram_read) {
        return 0;
    }
    switch (lcd->scan[2] & 7) {
    case 1:
        if (!lcd->read_pixel) {
            value = (uint8_t)((st7529_load_pixel(lcd, 0) << 3) |
                              (st7529_load_pixel(lcd, 1) >> 2));
            lcd->read_pixel = 1;
        } else {
            /* 未用的总线位读零；不携带 GRAM 状态。 */
            value = (uint8_t)(((st7529_load_pixel(lcd, 1) & 3) << 6) |
                              st7529_load_pixel(lcd, 2));
            lcd->read_pixel = 0;
            if (!lcd->read_modify_write) {
                st7529_advance_group(lcd);
            }
        }
        break;
    case 2:
        value = (uint8_t)(st7529_load_pixel(lcd, lcd->read_pixel) << 3);
        if (++lcd->read_pixel == NP560_ST7529_PIXELS_PER_COLUMN) {
            lcd->read_pixel = 0;
            if (!lcd->read_modify_write) {
                st7529_advance_group(lcd);
            }
        }
        break;
    default:
        return 0;
    }
    /* 7.2 节/图 7.2.1：读填充总线保持器并返回前一个值。RAMRD/RMWIN 后
     * 第一个结果是哑读，内容未定义。A0=0 状态不使用该流水线。 */
    lcd->read_latch = value;
    return previous;
}

uint8_t np560_lcd_status_read(const Np560Lcd *lcd)
{
    /* STREAD（A0=0），ST7529 v1.8 p49。EEPROM 访问未实现且保持在外；
     * 不要把 display-on 与 SCM0 混淆。 */
    return (uint8_t)(((lcd->area_scroll[3] & 3) << 6) |
                     (lcd->read_modify_write ? 0x20 : 0) |
                     ((lcd->scan[0] & 4) << 2) |
                     (lcd->display_on ? 8 : 0) |
                     (lcd->inverse ? 0 : 2) |
                     (lcd->partial_display ? 1 : 0));
}

void np560_lcd_render(const Np560Lcd *lcd, uint16_t *out, unsigned out_stride)
{
    unsigned x, y;

    for (y = 0; y < NP560_LCD_HEIGHT; y++) {
        for (x = 0; x < NP560_LCD_WIDTH && x < out_stride; x++) {
            unsigned darkness = st7529_visible_level(lcd, x, y);
            /* 光学浓度 0..31（0=无驱动=亮/白，31=满驱动=暗/黑）转亮度：
             * 浓度越大越暗，故取 31-darkness。与 QEMU Web app.js 一致
             * （value=0→off 浅色，value=31→on 深色）；写成 darkness 会
             * 整幅反色（白底黑字变黑底白字）。 */
            unsigned bright = (31u - darkness) * 255u / 31u;
            uint16_t r = (uint16_t)(bright >> 3);
            uint16_t g = (uint16_t)(bright >> 2);
            uint16_t b = (uint16_t)(bright >> 3);

            out[(size_t)y * out_stride + x] =
                (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}
