/*
 * np560_emu.cpp - NP560+ 模拟器 Tab5 宿主实现。
 *
 * 显示：ST7529 可见面 240x120 + 顶部 12 行状态图标 = 240x132 源帧
 *       -> RGB565（Web 精确绿灰配色）-> 2.471x 最近邻贴入 1280x720 全屏
 *       LVGL canvas 的皮肤屏幕洞 (356,212,593x326)。皮肤 1280x708 raw 固化
 *       编译进 Flash，启动时 memcpy 到 canvas（0,6）；之后每帧只覆盖洞内画面
 *       矩形，源帧无变化则整帧跳过。整幅 invalidate，不碰 rotation/flush，
 *       保持 DISPLAY_FLUENCY_OPTIMIZATION.md 的三条安全不变量。
 *       np560_lcd_render 含 COMSCN/模块接线/部分显示语义；图标位来自
 *       GRAM line19/140，与 QEMU Web 一致。
 * 输入：Tab5 物理键盘桥（I2C 0x6D）-> NP560 矩阵；TODO 触摸皮肤
 * 存储：TF 卡 /sd/noahnp560/（用户约定：不放根目录）：
 *       NP560.PRG -> data 平面 0x30000（vma03）、resident.bin -> code ROM
 *       0x0、system.nand.gz -> 首次解压为 system.nand（NAND 介质）
 * 音频：TODO esp_codec_dev (ES8388 I2S) + MP3 HLE
 */
#include "np560_emu.h"
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "bsp/m5stack_tab5.h"
#include "lvgl.h"
#include "dmcu_bus.h"
#include "dmcu_exec.h"
#include "dmcu_clock.h"
#include "dmcu_irq.h"
#include "np560_soc.h"
#include "np560_periph.h"
#include "np560_tab5_keyboard.h"
#include "np560_tab5_touch.h"
#include "np560_sd.h"
#include "np560_mp3_hle_tab5.h"

static const char *TAG = "np560";

/* 全屏合成（第③步）：LVGL 逻辑屏 1280x720，皮肤固化 1280x708 @ (0,6)。
 * 洞内画面按宽 593/240 = 2.471x 贴入，与 Web .lcd-window object-fit 语义一致。
 * 这些常量来自 tools/build_skin_raw.py 的静态坐标核对（已修正 CROP_TOP=86）。 */
#define CANVAS_W      1280u
#define CANVAS_H      720u
#define SKIN_W        1280u
#define SKIN_H        708u
#define SKIN_Y        6u              /* (720-708)/2 */
/* 洞坐标来自真实透明连通域（alpha<200 flood fill）实测，非文档估值：
 * 源图 x=337..931 / y=256..643，中心 634（略偏右是 Web 原图事实）。
 * 落屏洞 x=343.99..950.30 / y=179.33..574.35；画面按宽适配 2.5263x。 */
/* 画面矩形相对皮肤填色块(0xb654, 落屏 x=338..954/y=169..572)的位置：
 * (340,210) -> left=2/right=8/top=41/bot=29（按用户真机观感左移3、下移6）。
 * 完全居中的值是 (343,204) -> 5/5/35/35；洞(x=344..950/y=180..575)是皮肤
 * PNG 固有、固件无法移动，仅作参考不作基准。 */
#define FRAME_X       340u
#define FRAME_Y       210u
#define FRAME_W       606u            /* 240 * 2.526 */
#define FRAME_H       333u            /* 132 * 2.526 */

/* 固化皮肤放在独立 data 分区（np560skin, subtype 0x40），用 esp_partition_read
 * 直接读，不经过 .drom->PSRAM XIP 映射——否则 1.73MB 会吃掉 resident.bin
 * 装载所需的连续 PSRAM（实测 largest 6.55MB->4.72MB 导致 fread 失败）。 */
#define SKIN_PARTITION_NAME "np560skin"

static Np560Bus *s_bus;
static NP560Mp3HleTab5 *s_mp3_hle;
static DMCUCpu s_cpu;
static Np560Periph s_periph;
static lv_obj_t *s_canvas;
static lv_image_dsc_t s_canvas_img;
/* RGB565 全屏 canvas（1280x720 = 1.76MB）放 PSRAM；源帧 240x132
 * （顶部 12 行状态图标 + 240x120 主画面）与上一帧副本放 PSRAM 用于变化检测。
 * 皮肤 raw 常驻 Flash（1.73MB），只在启动时铺一次，之后画布洞外区域不再改动。 */
static uint16_t *s_canvas_buf;
static uint16_t *s_frame;
static uint16_t *s_prev_frame;

/* PSRAM 大缓冲分配（启动早期调用）。失败返回 false。 */
static bool alloc_framebuffers()
{
    s_canvas_buf = (uint16_t *)heap_caps_malloc(
        CANVAS_W * CANVAS_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_frame = (uint16_t *)heap_caps_malloc(
        NP560_LCD_WIDTH * NP560_LCD_FRAME_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_prev_frame = (uint16_t *)heap_caps_malloc(
        NP560_LCD_WIDTH * NP560_LCD_FRAME_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_canvas_buf || !s_frame || !s_prev_frame) {
        ESP_LOGE(TAG, "framebuffer alloc failed (psram?)");
        free(s_canvas_buf);
        free(s_frame);
        free(s_prev_frame);
        s_canvas_buf = NULL;
        s_frame = NULL;
        s_prev_frame = NULL;
        return false;
    }
    memset(s_canvas_buf, 0, CANVAS_W * CANVAS_H * sizeof(uint16_t));
    memset(s_frame, 0,
           NP560_LCD_WIDTH * NP560_LCD_FRAME_HEIGHT * sizeof(uint16_t));
    memset(s_prev_frame, 0,
           NP560_LCD_WIDTH * NP560_LCD_FRAME_HEIGHT * sizeof(uint16_t));
    return true;
}

/* 启动时从独立 Flash 分区读取固化皮肤铺到全屏 canvas（0,6）。用分区首部的
 * 16B 头校验尺寸（magic 'NP560SKN' + w/h），再逐行读；整幅只读一次，之后
 * 皮肤区不再改动。必须先在 alloc_framebuffers 之后执行（resident 已装载）。 */
static void paint_skin_background()
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 (esp_partition_subtype_t)0x40, SKIN_PARTITION_NAME);
    if (!part) {
        ESP_LOGE(TAG, "skin partition '%s' not found", SKIN_PARTITION_NAME);
        return;
    }
    uint8_t hdr[16];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK ||
        memcmp(hdr, "NP560SKN", 8) != 0) {
        ESP_LOGE(TAG, "skin partition header invalid");
        return;
    }
    uint32_t sw = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24);
    uint32_t sh = hdr[12] | (hdr[13] << 8) | (hdr[14] << 16) | (hdr[15] << 24);
    if (sw != SKIN_W || sh != SKIN_H) {
        ESP_LOGE(TAG, "skin size mismatch: %ux%u (want %ux%u)",
                 (unsigned)sw, (unsigned)sh, SKIN_W, SKIN_H);
        return;
    }
    for (unsigned y = 0; y < SKIN_H; y++) {
        if (esp_partition_read(part, 16 + (size_t)y * SKIN_W * sizeof(uint16_t),
                               s_canvas_buf + (size_t)(SKIN_Y + y) * CANVAS_W,
                               SKIN_W * sizeof(uint16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "skin read row %u failed", y);
            return;
        }
    }
    ESP_LOGI(TAG, "skin painted: %ux%u from partition '%s' (%u KB)",
             SKIN_W, SKIN_H, SKIN_PARTITION_NAME,
             (unsigned)(SKIN_W * SKIN_H * 2u / 1024u));
}

/* 状态图标点阵：逐点复刻 QEMU Web app.js 的 statusArtwork（8 行 × 9 列）。
 * bit 是 plane1 的位号，x 是 240 宽源帧起点。只按 guest 导出的电极位决定
 * 激活/淡显，不合成图标语义；形状/位置取自实机照片，与 Web 保持一致。 */
typedef struct {
    unsigned bit;
    unsigned x;
    const char *rows[8];
} AnnunciatorArtwork;

static const AnnunciatorArtwork s_ann_art[] = {
    { 133, 92, { "000010000", "000111000", "001111100", "011111110",
                 "000111000", "000111000", "000111000", "000111000" } },
    { 150, 110, { "000100000", "001100000", "011111110", "111111110",
                  "011111110", "001100000", "000100000", "000000000" } },
    { 163, 128, { "000001000", "000001100", "011111110", "011111111",
                  "011111110", "000001100", "000001000", "000000000" } },
    { 179, 146, { "000111000", "000111000", "000111000", "000111000",
                  "011111110", "001111100", "000111000", "000010000" } },
    { 196, 164, { "000000000", "000000000", "000010000", "000111000",
                  "001111100", "011111110", "000000000", "000000000" } },
    { 211, 182, { "000000000", "000000000", "011111110", "001111100",
                  "000111000", "000010000", "000000000", "000000000" } },
};

/* Web 精确配色常量（app.js colors() + drawAnnunciators()）：
 *   off = #b7c8a1 -> 0xb654（洞内熄灭色/图标背景）
 *   on  = #243126 -> 0x2184（激活图标/文字最深色）
 *   ghost = off*0.86 + on*0.14 = rgb(162,179,144) -> 0xa592
 * 统一后主画面底色与洞内留白一致，无绿色/白色色差。 */
#define LCD_OFF_RGB565   0xb654u
#define LCD_ON_RGB565    0x2184u
#define ANN_GHOST_RGB565 0xa592u

static bool ann_bit(const uint8_t *planes, unsigned bit)
{
    return (planes[NP560_LCD_ANNUNCIATOR_BYTES_PER_PLANE + (bit >> 3)] &
            (uint8_t)(1u << (bit & 7u))) != 0;
}

static void ann_fill(uint16_t *frame, unsigned x, unsigned y,
                     unsigned w, unsigned h, uint16_t color)
{
    for (unsigned yy = y; yy < y + h && yy < NP560_LCD_ANNUNCIATOR_HEIGHT; yy++) {
        for (unsigned xx = x; xx < x + w && xx < NP560_LCD_WIDTH; xx++) {
            frame[yy * NP560_LCD_WIDTH + xx] = color;
        }
    }
}

/* 图标配色与 Web 完全一致：背景 off、激活 on、未激活 ghost；
 * 主画面也走同一组 off/on 插值（np560_lcd_render 的 web_gray5 表）。 */
static void draw_annunciators(uint16_t *frame)
{
    uint8_t planes[NP560_LCD_ANNUNCIATOR_BYTES];
    const uint16_t bg = LCD_OFF_RGB565;
    const uint16_t active = LCD_ON_RGB565;
    const uint16_t ghost = ANN_GHOST_RGB565;

    np560_lcd_annunciators(&s_periph.lcd, planes);
    ann_fill(frame, 0, 0, NP560_LCD_WIDTH, NP560_LCD_ANNUNCIATOR_HEIGHT, bg);

    for (unsigned i = 0; i < sizeof(s_ann_art) / sizeof(s_ann_art[0]); i++) {
        uint16_t color = ann_bit(planes, s_ann_art[i].bit) ? active : ghost;
        for (unsigned row = 0; row < 8; row++) {
            const char *bits = s_ann_art[i].rows[row];
            for (unsigned col = 0; col < 9; col++) {
                if (bits[col] == '1') {
                    ann_fill(frame, s_ann_art[i].x + col, row + 2, 1, 1, color);
                }
            }
        }
    }

    /* 电池：轮廓由 plane1 的 bit228/235 决定，4 组电量格独立叠加。 */
    if (ann_bit(planes, 228) || ann_bit(planes, 235)) {
        ann_fill(frame, 207, 2, 18, 1, active);
        ann_fill(frame, 207, 9, 18, 1, active);
        ann_fill(frame, 206, 3, 1, 6, active);
        ann_fill(frame, 225, 3, 1, 6, active);
        ann_fill(frame, 226, 5, 2, 3, active);
        /* 单 bit 组用重复位号，语义仍等价于 Web 的 some() 判断。 */
        static const unsigned level_bits[4][2] = {
            { 224, 230 }, { 234, 234 }, { 232, 236 }, { 225, 238 },
        };
        for (unsigned level = 0; level < 4; level++) {
            if (ann_bit(planes, level_bits[level][0]) ||
                ann_bit(planes, level_bits[level][1])) {
                ann_fill(frame, 208 + level * 4, 4, 3, 4, active);
            }
        }
    }
}

/* ST7529 -> 240x132 源帧（顶部 12 行图标 + 120 行主画面）-> 2.471x 最近邻
 * 贴入全屏 canvas 的洞内矩形 (356,212,593x326)。皮肤在启动时已铺好，洞内
 * 上下 30.85px 留白仍是皮肤预填的熄灭色 0xb654，与 Web 的 object-fit 一致。
 * 关键：源帧未变时整帧跳过；变化时只覆写洞内矩形（皮肤其余部分不动），
 * 但 invalidate 仍是整幅——不碰 rotation/flush，保持三条安全不变量。 */
static void render_frame()
{
    const size_t frame_px = NP560_LCD_WIDTH * NP560_LCD_FRAME_HEIGHT;
    np560_lcd_render(&s_periph.lcd,
                     s_frame + NP560_LCD_ANNUNCIATOR_HEIGHT * NP560_LCD_WIDTH,
                     NP560_LCD_WIDTH);
    draw_annunciators(s_frame);
    if (memcmp(s_frame, s_prev_frame, frame_px * sizeof(uint16_t)) == 0) {
        return; /* 无变化：跳过放大与 LVGL 重绘 */
    }
    memcpy(s_prev_frame, s_frame, frame_px * sizeof(uint16_t));

    /* 非整数倍最近邻：dst_x 映射到 src_x = dst_x*240/593（Web 同为
     * 非整数倍，2323 交替的轻微不均是既有视觉基线）。 */
    for (unsigned dy = 0; dy < FRAME_H; dy++) {
        unsigned sy = (unsigned)((uint64_t)dy * NP560_LCD_FRAME_HEIGHT / FRAME_H);
        const uint16_t *src = s_frame + (size_t)sy * NP560_LCD_WIDTH;
        uint16_t *dst = s_canvas_buf + (size_t)(FRAME_Y + dy) * CANVAS_W + FRAME_X;
        for (unsigned dx = 0; dx < FRAME_W; dx++) {
            unsigned sx = (unsigned)((uint64_t)dx * NP560_LCD_WIDTH / FRAME_W);
            dst[dx] = src[sx];
        }
    }
    lv_obj_invalidate(s_canvas);
}

static void emulator_task(void *arg)
{
    /* 订阅 TWDT：任务阻塞（如 SD 卡 I/O 挂死）时打印回溯定位卡点。
     * 注意 idle 任务不受阻塞影响，此前挂死 TWDT 全程静默。 */
    esp_task_wdt_add(NULL);
    uint64_t last_report = esp_timer_get_time();
    uint64_t last_render = 0;
    uint64_t burst_insns = 0;
    uint64_t total_insns = 0;

    ESP_LOGI(TAG, "emulator task start, clock=%u Hz",
             (unsigned)dmcu_system_clock_hz(&s_cpu));

    while (!s_cpu.halted) {
        uint64_t loop_insns = 0;
        for (int i = 0; i < 10000; i++) {
            /* 键盘唤醒（确定性修复）：原厂去抖是纯软件按 ISR 进入次数计数——
             * 时基 ISR 每进入一次给 RAM 0490 +1，满 4 次才调一次键盘扫描，
             * 去抖门 31:BE70 需连续 2 次扫描，共需 8 次真实进入 ISR。
             *
             * ICR RQ 是锁存位：受理时（irq_enter）清零，未受理时重复 raise
             * 会合并成一次 ISR。因此只有当 RQ 已清（上一次 IRQ 确已被 CPU
             * 接受）才注入下一个 tick，每个 tick 保证对应一次真实 ISR。
             * 之前的实现在最内层 for 里每条指令无脑 raise，12 个 tick 在
             * 头 12 条指令内合并成 1 次 ISR，补 tick 完全失效。 */
            /* 路线②：在指令边界消费预置去抖请求，直写 guest RAM 的
             * 0490=4 / 2A8C=2，跳过"0490 累加到 4"的 3 次 ISR 空转；
             * 2A94（armed）保持 guest 原生管理，扫描器仍真实生成事件。
             * 与解释器串行，避免键盘 I2C 任务并发改 guest RAM。 */
            if (s_periph.key_debounce_request) {
                np560_periph_apply_fast_debounce(&s_periph, s_bus);
            }
            if (s_periph.key_wake_ticks &&
                !(s_cpu.sfr[DMCU_ICR_FIRST] & DMCU_ICR_RQ)) {
                s_periph.key_wake_ticks--;
                s_periph.stat_key_wake_irq++;
                dmcu_irq_raise(s_bus, &s_cpu, 0);
            }
            loop_insns++;
            dmcu_exec_step(s_bus, &s_cpu);
            if (s_cpu.halted) {
                break;
            }
        }
        total_insns += loop_insns;
        burst_insns += loop_insns;
        /* QEMU 主循环语义：EXEC_EXIT（CRIT/IGNDS 窗口到期、irq_state_changed
         * 仲裁边界）只是 TB 出口，主循环立即重入继续执行——此前在 EXIT 处
         * break 出 burst 再 vTaskDelay(1)，固件 ICR/PSW 自旋循环退化成
         * 每毫秒 1 条指令（~1000 ips），引导要数小时。 */
        /* 虚拟时钟按指令数推进（QEMU icount 语义：1 指令 = 1 cycle）。
         * 此前按墙钟充值：CPU 只有实时的 ~1.3%，虚拟时钟却全速走，
         * 固件所有等待型超时提前 ~80 倍到期 → FAT init 超时报错
         * （QEMU 能进桌面是因为 TCG 跑得接近实时，掩盖了该语义差异）。
         * I/O 阻塞期间无指令执行，虚拟时钟自然暂停——等待不会误超时。
         * 代价：RTC/秒表随 CPU 速度变慢，CPU 提速后自动恢复。 */
        {
            uint64_t delta = loop_insns;

            if (delta) {
                dmcu_clock_charge(&s_cpu, delta);
                np560_periph_advance(&s_periph, delta);
            }
        }
        /* 渲染节流到 ~20fps：PSRAM canvas 拷贝 + LVGL 重绘开销大，
         * 逐循环渲染曾占掉解释器大部分 CPU 时间（等效 >100fps）。 */
        {
            uint64_t now_us = esp_timer_get_time();
            if (now_us - last_render >= 50000) {
                bsp_display_lock(50);
                render_frame();
                bsp_display_unlock();
                last_render = now_us;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        esp_task_wdt_reset();

        /* 每 5s 报告一次聚合状态。逐 burst 打印会在启动扫描期间产生
         * 数万条串口日志，显著拖慢解释器；保留本摘要便于真机回归诊断。 */
        uint64_t now = esp_timer_get_time();
        if (now - last_report >= 5000000) {
            unsigned ips = (unsigned)(burst_insns / 5);
            ESP_LOGI(TAG,
                     "run: PC=%02x:%04x ips=%u total=%llu "
                     "irq_pending=%d in_irq=%d unsup=%u mp3hle=%llu",
                     (unsigned)((s_cpu.pc >> 16) & 0xff),
                     (unsigned)(s_cpu.pc & 0xffff), ips,
                     (unsigned long long)total_insns,
                     (int)dmcu_irq_or_dtu_pending(&s_cpu),
                     (int)s_cpu.in_interrupt,
                     (unsigned)s_cpu.unsupported_count,
                     (unsigned long long)np560_mp3_hle_tab5_call_count(s_mp3_hle));
            last_report = now;
            burst_insns = 0;
            /* NAND 介质统计：确认页编程/擦除不再阻塞 CPU。 */
            {
                Np560NandFileMedium *nf =
                    (Np560NandFileMedium *)s_periph.nand.medium.user;
                ESP_LOGI(TAG,
                         "nand: reads=%u prog=%u erase=%u dirty=%u flushed=%u pf=%u rf=%u op=%u oh=%u os=%u om=%u",
                         (unsigned)nf->stat_reads, (unsigned)nf->stat_programs,
                         (unsigned)nf->stat_erases, (unsigned)nf->stat_dirty,
                         (unsigned)nf->stat_flushed,
                         (unsigned)nf->stat_prefetch_fail,
                         (unsigned)nf->stat_read_fails,
                         (unsigned)nf->stat_oob_prefetch,
                         (unsigned)nf->stat_oob_hits,
                         (unsigned)nf->stat_oob_sparse_hits,
                         (unsigned)nf->stat_oob_sparse_misses);
            }
            /* 时基/中断诊断：定位 idle loop 不响应键盘的根因。
             * tb=运行频率 exp=tick次数 run=是否启动
             * tbicr=ICR[0](EN|RQ) socr=全局中断控制(bit15=IE)
             * icr1..3=周期定时器 ICR（诊断仲裁为何选中不了中断源）。 */
            {
                Np560Timers *tm = &s_periph.timers;
                ESP_LOGI(TAG,
                         "timebase: hz=%u exp=%llu run=%d acc=%llu "
                         "tbicr=%04x socr=%04x psr=%04x irqchg=%d",
                         (unsigned)tm->timebase.frequency_hz,
                         (unsigned long long)tm->timebase.expirations,
                         (int)tm->timebase.running,
                         (unsigned long long)tm->timebase.acc_num,
                         (unsigned)s_cpu.sfr[DMCU_ICR_FIRST + 0],
                         (unsigned)s_cpu.sfr[DMCU_SFR_SOCR],
                         (unsigned)s_cpu.sfr[DMCU_SFR_PSW],
                         (int)s_cpu.irq_state_changed);
            }
            /* 键盘诊断：观察 guest 是否真的在扫描 F160 矩阵。
             * mr=矩阵读次数 ma=读到有效按键次数 mask=当前物理键位。 */
            ESP_LOGI(TAG,
                     "keypad: mr=%u ma=%u mask=%016llx code=%04x wake=%u kwirq=%u fast=%u",
                     (unsigned)s_periph.keypad.matrix_reads,
                     (unsigned)s_periph.keypad.matrix_active_reads,
                     (unsigned long long)s_periph.keypad.matrix,
                     (unsigned)s_periph.keypad.key_code,
                     (unsigned)s_periph.key_wake_ticks,
                     (unsigned)s_periph.stat_key_wake_irq,
                     (unsigned)s_periph.stat_debounce_fast);
            /* ADC 电量比较器诊断：level=8 满电；rd/wr 为固件采样次数。
             * control 是最后一次写入的 threshold<<2|2，bit7 不应被固件写。 */
            ESP_LOGI(TAG, "adc: level=%u ctrl=%04x rd=%u wr=%u",
                     (unsigned)s_periph.adc_level,
                     (unsigned)s_periph.adc_control,
                     (unsigned)s_periph.adc_reads,
                     (unsigned)s_periph.adc_writes);
            /* 诊断：SFR/MMIO 写事件全列表（低频关键写不被热点淹没）
             * + 读热点 top4。SFR reg 原样、MMIO 前缀 M（physical>>1）。 */
            {
                static uint32_t reads[512], writes[512];
                np560_periph_diag_take(&s_periph, reads, writes);
                char rb[128], wb[160];
                size_t rn = 0, wn = 0;
                rb[0] = wb[0] = 0;
                for (int r = 0; r < 512; r++) {
                    if (writes[r]) {
                        wn += snprintf(wb + wn, sizeof(wb) - wn,
                                       "%s%s%02x(%lu)", wn ? " " : "",
                                       r >= 0x100 ? "M" : "", r & 0xff,
                                       (unsigned long)writes[r]);
                    }
                }
                for (int round = 0; round < 4; round++) {
                    int best = -1;
                    for (int r = 0; r < 512; r++) {
                        if (reads[r] > 1 && (best < 0 || reads[r] > reads[best])) {
                            best = r;
                        }
                    }
                    if (best >= 0) {
                        rn += snprintf(rb + rn, sizeof(rb) - rn,
                                       "%s%s%02x(%lu)", rn ? " " : "",
                                       best >= 0x100 ? "M" : "", best & 0xff,
                                       (unsigned long)reads[best]);
                        reads[best] = 0;
                    }
                }
                ESP_LOGI(TAG, "sfr W[%s] R[%s]", wb, rb);
            }
        }
    }
    ESP_LOGW(TAG, "CPU halted: %04x @ %02x:%04x",
             s_cpu.fault_word, (unsigned)((s_cpu.fault_pc >> 16) & 0xff),
             (unsigned)(s_cpu.fault_pc & 0xffff));
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

/* QEMU 复位语义（target/dmcu/cpu.c:479-501）：全零后 SP=0xfffe、
 * DS1=1（复位栈在 X RAM 高半区）、resident ROM 窗口 [0, 2MB)。 */
static void cpu_reset(const Np560Bus *bus)
{
    memset(&s_cpu, 0, sizeof(s_cpu));
    s_cpu.gpr[DMCU_SP] = 0xfffe;
    s_cpu.sfr[DMCU_SFR_DS1] = 1;
    s_cpu.resident_rom_base = 0;
    s_cpu.resident_rom_limit = bus->code_rom ? bus->code_rom_size : 0;
    /* 复位 PC：装载真实固件 = 0x30000（firmware-vma03，np560.c:22838）；
     * 未装载 = DMCU_PROGRAM_BASE 兜底（smoke test 自行覆盖 PC）。 */
    s_cpu.pc = bus->code_rom ? 0x00030000u : 0x00200000u;
}

static void smoke_test()
{
    /* host/runner 同款合成程序：验证板上解释器语义。 */
    const uint16_t prog[] = {
        0x4801, 0x4822, 0x1002, 0x4843, 0x0a42,
        0x2814, 0x1004, 0x2974, 0xc700,
        0x6810, 0x0002, 0x4845, 0x0a40,
        0x6810, 0x0000, 0x2814, 0x6810, 0x0002, 0x2814,
    };
    np560_bus_load(s_bus, 0x200000u,
                   (const uint8_t *)prog, sizeof(prog));
    s_cpu.pc = 0x200000u;
    uint64_t t0 = esp_timer_get_time();
    for (unsigned i = 0; i < sizeof(prog) / 2; i++) {
        dmcu_exec_step(s_bus, &s_cpu);
    }
    ESP_LOGI(TAG, "smoke test: R0=%u (want 6) faults=%u, %lluus for %u insns",
             (unsigned)s_cpu.gpr[0], (unsigned)s_cpu.unsupported_count,
             (unsigned long long)(esp_timer_get_time() - t0),
             (unsigned)(sizeof(prog) / 2));
}

void np560_emu_start()
{
    s_bus = np560_bus_create();
    cpu_reset(s_bus);
    /* 内存微基准：区分"PSRAM 慢"与"解释器逻辑慢"（bus 空间总是 16MB；
     * code_rom 此时未装载，只测 phys）。 */
    {
        volatile uint32_t sum = 0;
        uint64_t m0 = esp_timer_get_time();
        for (unsigned i = 0; i < 100000u; i++) {
            sum += s_bus->phys[(i * 977u) & 0xffffffu];
        }
        uint64_t m1 = esp_timer_get_time();
        ESP_LOGW(TAG, "mem bench: phys %uus/100k (sum=%u)",
                 (unsigned)(m1 - m0), (unsigned)sum);
        /* internal_data 落位验证：内部 SRAM 命中时此处应显著快于 phys。 */
        {
            volatile uint32_t isum = 0;
            uint64_t i0 = esp_timer_get_time();
            for (unsigned i = 0; i < 100000u; i++) {
                isum += s_bus->internal_data[(i * 977u) & 0xffffu];
            }
            uint64_t i1 = esp_timer_get_time();
            ESP_LOGW(TAG, "mem bench: internal %uus/100k (sum=%u, ptr=%p)",
                     (unsigned)(i1 - i0), (unsigned)isum,
                     (void *)s_bus->internal_data);
        }
    }
    /* 外设组合层：定时器/音频 SFR 钩子 + LCD/键盘/NAND/ECC MMIO 钩子。 */
    np560_periph_init(&s_periph, &s_cpu, s_bus);

    bool firmware_ok = false;
    if (np560_sd_mount()) {
        firmware_ok = np560_sd_load_firmware(s_bus);
    }
    /* QEMU 在每条指令前调用 rom_service 拦截 01/02 ROM 洞里的 MP3 门
     * (TRAP#0 + "MPLH")。6 个门的真正基址是 0x21E26（QEMU 配置的
     * rom_mp3_hle_base=138790）；0x21E66 只是第 4 号门，不能当基址。
     * Tab5 解释器原本只覆盖远调用 HLE，进 MP3 会在 01:0000 向量
     * 无限自陷，触摸/物理按键全部失去响应。 */
    if (firmware_ok) {
        s_mp3_hle = np560_mp3_hle_tab5_create(s_bus, 0x21e26u);
        if (s_mp3_hle) {
            s_bus->rom_service = np560_mp3_hle_tab5_service;
            s_bus->rom_service_user = s_mp3_hle;
            ESP_LOGI(TAG, "MP3 HLE gate ready @0x21e26");
        } else {
            ESP_LOGW(TAG, "MP3 HLE gate marker absent; TRAP path unchanged");
        }
    }
    /* 全屏 canvas(1.76MB) 必须在固件装载之后、NAND attach 之前分配：
     * ① 装载 resident.bin 需要连续 PSRAM，提前占用会让 fread 失败；
     * ② NAND OOB 稀疏索引另占 1.67MB，若先 attach 会让剩余连续块不足
     *    1.76MB 导致 canvas 分配失败。CPU 任务在函数末尾才创建，NAND
     *    延后到此处 attach 不影响启动。 */
    if (!alloc_framebuffers()) {
        return;
    }
    if (firmware_ok) {
        np560_sd_attach_nand(&s_periph.nand);
    }
    paint_skin_background();
    if (firmware_ok) {
        /* 固件装载后重设复位向量（cpu_reset 初次调用时 code_rom 尚未
         * 装载，PC 兜底在 0x200000）。不能重复整个 cpu_reset：其 memset
         * 会清掉 periph_init 挂上的 SFR 设备回调。 */
        s_cpu.pc = 0x00030000u;                     /* firmware-vma03 */
        s_cpu.resident_rom_limit = s_bus->code_rom_size;
        ESP_LOGI(TAG, "reset PC=0x30000, resident ROM window %uKB",
                 (unsigned)(s_bus->code_rom_size / 1024));
    }
    if (!firmware_ok) {
        ESP_LOGW(TAG, "no firmware from SD; running smoke test only");
        smoke_test();
        s_cpu.halted = true;
    }

    /* Tab5 物理键盘桥：I2C 事件 -> NP560 矩阵（未插键盘则后台周期重试）。 */
    np560_tab5_keyboard_start(&s_periph);

    /* 全屏 LVGL canvas：HAL 已启动 LVGL（hal_esp32.cpp display init）。
     * 皮肤已在 paint_skin_background() 铺入 buffer，此处只建对象、不 memset。 */
    memset(&s_canvas_img, 0, sizeof(s_canvas_img));
    s_canvas_img.header.cf = LV_COLOR_FORMAT_RGB565;
    s_canvas_img.header.w = CANVAS_W;
    s_canvas_img.header.h = CANVAS_H;
    s_canvas_img.header.stride = CANVAS_W * 2;
    s_canvas_img.data_size = CANVAS_W * CANVAS_H * 2;
    s_canvas_img.data = (const uint8_t *)s_canvas_buf;

    bsp_display_lock((uint32_t)-1 /* portMAX_DELAY */);
    s_canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CANVAS_W, CANVAS_H,
                         LV_COLOR_FORMAT_RGB565);
    /* canvas 缓冲即 1280x720 全屏，铺满逻辑屏（不再居中留黑边）。 */
    lv_obj_set_pos(s_canvas, 0, 0);
    render_frame();
    /* 透明触摸热区叠在 canvas 之上（Web 上半部分机身按键），不改画面。 */
    np560_tab5_touch_create(&s_periph);
    bsp_display_unlock();

    /* 优先级 10 + 核 1：诊断发现 exec 相位 10000 条耗时 7.8s（rest 仅
     * 8ms），疑似被同核高负载任务持续抢占。提优先级并隔离到核 1 验证。 */
    xTaskCreatePinnedToCore(emulator_task, "np560_emu", 8192, NULL,
                            10, NULL, 1);
}
