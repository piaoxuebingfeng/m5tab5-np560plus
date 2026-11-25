/*
 * np560_emu.cpp - NP560+ 模拟器 Tab5 宿主实现。
 *
 * 显示：ST7529 可见面 240x120（5 级灰度浓度）-> RGB565 -> LVGL9 canvas
 *       （5x 最近邻放大到 1200x600 居中；np560_lcd_render 含 COMSCN/
 *        模块接线/部分显示语义。仅当源帧内容变化时才放大+invalidate，
 *        静止界面零渲染开销——避免拖慢固件启动）
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
#include "bsp/m5stack_tab5.h"
#include "lvgl.h"
#include "dmcu_bus.h"
#include "dmcu_exec.h"
#include "dmcu_clock.h"
#include "dmcu_irq.h"
#include "np560_soc.h"
#include "np560_periph.h"
#include "np560_tab5_keyboard.h"
#include "np560_sd.h"

static const char *TAG = "np560";

#define LCD_SCALE     5u
#define CANVAS_W      (NP560_LCD_WIDTH * LCD_SCALE)   /* 1200 */
#define CANVAS_H      (NP560_LCD_HEIGHT * LCD_SCALE)  /* 600 */

static Np560Bus *s_bus;
static DMCUCpu s_cpu;
static Np560Periph s_periph;
static lv_obj_t *s_canvas;
static lv_image_dsc_t s_canvas_img;
/* RGB565 canvas（放大后 1200x600 = 1.4MB）放 PSRAM；面板源帧 240x120
 * 与上一帧副本放 PSRAM 用于变化检测；行展开缓冲放内部 SRAM。 */
static uint16_t *s_canvas_buf;
static uint16_t *s_frame;
static uint16_t *s_prev_frame;
static DRAM_ATTR uint16_t s_row[CANVAS_W];  /* 一行放大像素，内部 SRAM */

/* P0 渲染统计（每 5 秒报告并清零）：验证脏行增量刷新是否真的减少
 * 放大/LVGL 重绘面积，同时确认没有拖慢解释器。 */
static uint64_t s_render_calls;
static uint64_t s_render_changed;
static uint64_t s_render_dirty_rows;
static uint64_t s_render_dirty_runs;
static uint32_t s_render_max_dirty_rows;
static uint64_t s_render_us_total;
static uint64_t s_render_us_max;

/* PSRAM 大缓冲分配（启动早期调用）。失败返回 false。 */
static bool alloc_framebuffers()
{
    s_canvas_buf = (uint16_t *)heap_caps_malloc(
        CANVAS_W * CANVAS_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_frame = (uint16_t *)heap_caps_malloc(
        NP560_LCD_WIDTH * NP560_LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_prev_frame = (uint16_t *)heap_caps_malloc(
        NP560_LCD_WIDTH * NP560_LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
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
    memset(s_frame, 0, NP560_LCD_WIDTH * NP560_LCD_HEIGHT * sizeof(uint16_t));
    memset(s_prev_frame, 0, NP560_LCD_WIDTH * NP560_LCD_HEIGHT * sizeof(uint16_t));
    return true;
}

/* ST7529 可见面 -> RGB565 源帧 -> 5x 最近邻放大到 canvas。
 * P0：逐源行 diff，只放大连续变化的源行段，并只失效这些行在 canvas 中
 * 对应的区域。原来任何一处变化都会放大全幅并 invalidate 整块 1200x600；
 * 现在光标/状态栏等局部更新只需处理几行，5x 放大倍率与方向均不变。
 * 静止帧仍然零放大、零 invalidate。 */
static void render_frame()
{
    const uint64_t t0 = esp_timer_get_time();
    np560_lcd_render(&s_periph.lcd, s_frame, NP560_LCD_WIDTH);
    s_render_calls++;

    unsigned run_start = 0;
    unsigned run_len = 0;
    unsigned dirty_rows = 0;

    /* 放大一段连续脏源行，并只失效该段对应的 canvas 条带。
     * 行先在内部 SRAM 的 s_row 展开 240→1200，再 memcpy 5 行到 PSRAM。 */
    auto flush_run = [&]() {
        for (unsigned y = run_start; y < run_start + run_len; y++) {
            const uint16_t *src = s_frame + (size_t)y * NP560_LCD_WIDTH;
            for (unsigned x = 0; x < NP560_LCD_WIDTH; x++) {
                uint16_t c = src[x];
                uint16_t *d = s_row + x * LCD_SCALE;
                for (unsigned k = 0; k < LCD_SCALE; k++) {
                    d[k] = c;
                }
            }
            uint16_t *dst = s_canvas_buf + (size_t)y * LCD_SCALE * CANVAS_W;
            for (unsigned k = 0; k < LCD_SCALE; k++) {
                memcpy(dst + (size_t)k * CANVAS_W, s_row, CANVAS_W * sizeof(uint16_t));
            }
        }
        lv_area_t area;
        area.x1 = 0;
        area.y1 = (lv_coord_t)(run_start * LCD_SCALE);
        area.x2 = (lv_coord_t)(CANVAS_W - 1);
        area.y2 = (lv_coord_t)((run_start + run_len) * LCD_SCALE - 1);
        lv_obj_invalidate_area(s_canvas, &area);
        s_render_dirty_runs++;
    };

    for (unsigned y = 0; y < NP560_LCD_HEIGHT; y++) {
        uint16_t *src = s_frame + (size_t)y * NP560_LCD_WIDTH;
        uint16_t *prev = s_prev_frame + (size_t)y * NP560_LCD_WIDTH;
        if (memcmp(src, prev, NP560_LCD_WIDTH * sizeof(uint16_t)) == 0) {
            if (run_len) {
                flush_run();
                run_len = 0;
            }
            continue;
        }
        if (!run_len) {
            run_start = y;
        }
        run_len++;
        dirty_rows++;
        memcpy(prev, src, NP560_LCD_WIDTH * sizeof(uint16_t));
    }
    if (run_len) {
        flush_run();
    }

    if (dirty_rows) {
        s_render_changed++;
        s_render_dirty_rows += dirty_rows;
        if (dirty_rows > s_render_max_dirty_rows) {
            s_render_max_dirty_rows = dirty_rows;
        }
    }
    const uint64_t elapsed = esp_timer_get_time() - t0;
    s_render_us_total += elapsed;
    if (elapsed > s_render_us_max) {
        s_render_us_max = elapsed;
    }
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
                     "irq_pending=%d in_irq=%d unsup=%u",
                     (unsigned)((s_cpu.pc >> 16) & 0xff),
                     (unsigned)(s_cpu.pc & 0xffff), ips,
                     (unsigned long long)total_insns,
                     (int)dmcu_irq_or_dtu_pending(&s_cpu),
                     (int)s_cpu.in_interrupt,
                     (unsigned)s_cpu.unsupported_count);
            /* P0 显示统计：changed/calls 为实际发生变化的渲染帧比例，
             * dirty_avg 是平均每帧变化的源行数（满屏为 120），runs 为合并后
             * 的连续脏段数；us_avg/us_max 是 render_frame 自身耗时。 */
            ESP_LOGI(TAG,
                     "render: changed=%llu/%llu dirty_avg=%u max=%u "
                     "runs=%llu us_avg=%llu us_max=%llu",
                     (unsigned long long)s_render_changed,
                     (unsigned long long)s_render_calls,
                     (unsigned)(s_render_changed
                                    ? s_render_dirty_rows / s_render_changed
                                    : 0),
                     (unsigned)s_render_max_dirty_rows,
                     (unsigned long long)s_render_dirty_runs,
                     (unsigned long long)(s_render_calls
                                              ? s_render_us_total / s_render_calls
                                              : 0),
                     (unsigned long long)s_render_us_max);
            s_render_calls = 0;
            s_render_changed = 0;
            s_render_dirty_rows = 0;
            s_render_dirty_runs = 0;
            s_render_max_dirty_rows = 0;
            s_render_us_total = 0;
            s_render_us_max = 0;
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
        if (firmware_ok) {
            np560_sd_attach_nand(&s_periph.nand);
        }
    }
    /* 放大 canvas(1.4MB) 必须在固件装载之后分配：装载 resident.bin 需要
     * 连续 PSRAM，提前占用会让 fread 失败（实测读 resident.bin failed）。 */
    if (!alloc_framebuffers()) {
        return;
    }
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

    /* 全屏 LVGL canvas：HAL 已启动 LVGL（hal_esp32.cpp display init）。 */
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
    /* canvas 缓冲本身已是放大后的 1200x600，居中于 1280x720 屏幕。 */
    lv_obj_center(s_canvas);
    memset(s_canvas_buf, 0, CANVAS_W * CANVAS_H * sizeof(uint16_t));
    render_frame();
    bsp_display_unlock();

    /* 优先级 10 + 核 1：诊断发现 exec 相位 10000 条耗时 7.8s（rest 仅
     * 8ms），疑似被同核高负载任务持续抢占。提优先级并隔离到核 1 验证。 */
    xTaskCreatePinnedToCore(emulator_task, "np560_emu", 8192, NULL,
                            10, NULL, 1);
}
