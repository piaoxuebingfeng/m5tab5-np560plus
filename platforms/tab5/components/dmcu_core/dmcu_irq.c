/*
 * dmcu_irq.c - ICR 仲裁 / 中断嵌套 / 软件陷阱 / DTU 传输引擎。
 *
 * 移植自 qemu-dmcu/target/dmcu/{cpu.c,dtu.c}，剥离全部 QEMU 侧诊断
 * 观察器（frame watch / refill trace / midword 记录等），保留架构语义。
 * 每处标注原函数与行号。
 */
#include "dmcu_irq.h"

/* ---- 优先级仲裁（cpu.c:34/39/75） ---- */

unsigned dmcu_irq_priority(unsigned icr)
{
    /* CN1916884A 图 7B/12/16：ED 来源的空 COUNT 通道以优先级 3 请求
     * CPU 服务，与 PRI 位（通道号）无关。 */
    return icr & DMCU_ICR_ED ? 3 : (icr & DMCU_ICR_PRI) >> 4;
}

int dmcu_irq_select(const DMCUCpu *cpu)
{
    unsigned psr = dmcu_sfr_read((DMCUCpu *)cpu, DMCU_SFR_PSW, 16);
    unsigned threshold = (psr & DMCU_PSR_CPRI) >> 12;
    int selected = -1;
    unsigned selected_priority = 0;
    unsigned source;

    if (!(cpu->sfr[DMCU_SFR_SOCR] & DMCU_SOCR_IE)) {
        return -1;
    }
    for (source = 0; source < DMCU_ICR_COUNT; source++) {
        unsigned icr = cpu->sfr[DMCU_ICR_FIRST + source];
        unsigned channel = (icr & DMCU_ICR_PRI) >> 4;
        unsigned priority = dmcu_irq_priority(icr);

        if ((icr & (DMCU_ICR_EN | DMCU_ICR_RQ)) !=
                (DMCU_ICR_EN | DMCU_ICR_RQ) ||
            ((icr & DMCU_ICR_ED) &&
             (cpu->sfr[DMCU_DTUC_FIRST + channel] & DMCU_DTUC_COUNT)) ||
            priority < threshold ||
            ((psr & DMCU_PSR_CIRQ) && priority == threshold)) {
            continue;
        }
        /* 同优先级并列时按升序向量号（规范只定义选最高优先级）。 */
        if (selected < 0 || priority > selected_priority) {
            selected = (int)source;
            selected_priority = priority;
        }
    }
    return selected;
}

bool dmcu_irq_or_dtu_pending(const DMCUCpu *cpu)
{
    unsigned psr = dmcu_sfr_read((DMCUCpu *)cpu, DMCU_SFR_PSW, 16);
    unsigned threshold = (psr & DMCU_PSR_CPRI) >> 12;
    bool enabled = cpu->sfr[DMCU_SFR_SOCR] & DMCU_SOCR_IE;
    unsigned source;

    for (source = 0; source < DMCU_ICR_COUNT; source++) {
        unsigned icr = cpu->sfr[DMCU_ICR_FIRST + source];
        unsigned priority;

        if ((icr & (DMCU_ICR_EN | DMCU_ICR_RQ)) !=
                (DMCU_ICR_EN | DMCU_ICR_RQ)) {
            continue;
        }
        if ((icr & DMCU_ICR_ED) &&
            (cpu->sfr[DMCU_DTUC_FIRST + ((icr & DMCU_ICR_PRI) >> 4)] &
             DMCU_DTUC_COUNT)) {
            if (!(cpu->dtu_fault_sources & (1u << source))) {
                return true;
            }
            continue;
        }
        priority = dmcu_irq_priority(icr);
        if (enabled && priority >= threshold &&
            !((psr & DMCU_PSR_CIRQ) && priority == threshold)) {
            return true;
        }
    }
    return false;
}

/* ---- 外设请求入口（cpu.c:122 dmcu_irq_raise） ---- */

void dmcu_irq_raise(Np560Bus *bus, DMCUCpu *cpu, unsigned source)
{
    if (source >= DMCU_ICR_COUNT) {
        return;
    }
    cpu->sfr[DMCU_ICR_FIRST + source] |= DMCU_ICR_RQ;
    /* 已启用的 DTU 传输在外设事件中即时搬运（cpu.c:126-132），不等待
     * 指令边界合并连续采样请求。bus 为 NULL（无宿主总线的裸 CPU 视图，
     * 如纯外设单测）时跳过搬运。 */
    if (bus) {
        dmcu_dtu_run(bus, cpu);
    }
    cpu->irq_state_changed = true;
}

/* ---- DTU 传输引擎（dtu.c:77/100/118 原样移植） ---- */

static bool dtu_request(const DMCUCpu *cpu, unsigned source)
{
    unsigned icr = cpu->sfr[DMCU_ICR_FIRST + source];
    unsigned channel = (icr & DMCU_ICR_PRI) >> 4;

    return (icr & (DMCU_ICR_EN | DMCU_ICR_RQ | DMCU_ICR_ED)) ==
           (DMCU_ICR_EN | DMCU_ICR_RQ | DMCU_ICR_ED) &&
           (cpu->sfr[DMCU_DTUC_FIRST + channel] & DMCU_DTUC_COUNT) &&
           !(cpu->dtu_fault_sources & (1u << source));
}

/*
 * DTU 地址翻译（dtu.c:100 dmcu_dtu_address）。QEMU 的 unbanked 低页
 * （0x01000000）与 SFR 页（0x01000800）是 24 位数据存储的别名区域
 * （hw/dmcu/np560.c:20832-20843），在扁平物理模型中折叠为 pointer 本身。
 */
static uint32_t dtu_address(DMCUCpu *cpu, uint16_t pointer, bool banked,
                            bool byte)
{
    uint32_t address = pointer;

    if (banked) {
        unsigned ds = cpu->sfr[pointer & 0x8000 ? DMCU_SFR_DS1 :
                                               DMCU_SFR_DS0] & 0x1ff;
        address = (ds << 15) | (pointer & 0x7fff);
    }
    /* 数据总线按高字节在先编号，即使对内部 RAM 也一样（dtu.c:114）。 */
    return address ^ (byte ? 1u : 0u);
}

bool dmcu_dtu_run(Np560Bus *bus, DMCUCpu *cpu)
{
    unsigned source;
    bool handled = false;

    /* 一次遍历各处理一次当前锁存的来源；并列时升序向量号。IE/CPRI/
     * ISR 状态只限制 CPU 进入服务，不阻挡已致能的 DTU 搬运。 */
    for (source = 0; source < DMCU_ICR_COUNT; source++) {
        unsigned icr, channel, control, count, increment, size;
        uint16_t src, dst;
        uint32_t pointers, src_address, dst_address;

        if (!dtu_request(cpu, source)) {
            continue;
        }
        handled = true;
        icr = cpu->sfr[DMCU_ICR_FIRST + source];
        channel = (icr & DMCU_ICR_PRI) >> 4;
        control = cpu->sfr[DMCU_DTUC_FIRST + channel];
        count = control & DMCU_DTUC_COUNT;
        increment = (control & DMCU_DTUC_INC) >> 14;
        size = control & DMCU_DTUC_WBT ? 1 : 2;
        /* 指针表是 unbanked 内部存储，不受前台 DS/IGNDS 影响。
         * 扁平模型中 unbanked SFR 页别名即 0xF000+（dtu.c:150）。 */
        pointers = 0xf000u + 0xf0u + channel * 4u;
        src = dmcu_phys_read16(bus, pointers);
        dst = dmcu_phys_read16(bus, pointers + 2);
        if (increment == 3 || (size == 2 && ((src | dst) & 1))) {
            /* 不支持的配置（INC=3 / 字传输奇地址）：标记故障源。 */
            cpu->dtu_fault_sources |= 1u << source;
            continue;
        }
        src_address = dtu_address(cpu, src, control & DMCU_DTUC_SDS,
                                  size == 1);
        dst_address = dtu_address(cpu, dst, control & DMCU_DTUC_DDS,
                                  size == 1);
        cpu->sfr[DMCU_ICR_FIRST + source] &= ~DMCU_ICR_RQ;
        if (size == 1) {
            dmcu_phys_write8(bus, dst_address,
                             dmcu_phys_read8(bus, src_address));
        } else {
            dmcu_phys_write16(bus, dst_address,
                              dmcu_phys_read16(bus, src_address));
        }
        if (increment == 1) {
            dmcu_phys_write16(bus, pointers + 2, (uint16_t)(dst + size));
        } else if (increment == 2) {
            dmcu_phys_write16(bus, pointers, (uint16_t)(src + size));
        }
        cpu->sfr[DMCU_DTUC_FIRST + channel] =
            (control & ~DMCU_DTUC_COUNT) | (count - 1);
        if (count == 1) {
            /* 完成请求锁存直到 CPU 受理；目标侧副作用引发的独立请求
             * 仍然存活（dtu.c:203-206）。 */
            cpu->sfr[DMCU_ICR_FIRST + source] |= DMCU_ICR_RQ;
        }
    }
    return handled;
}

/* ---- 嵌套帧与重复影子（cpu.c:161/205/216/242） ---- */

/* 中断帧压栈（cpu.c:161 dmcu_interrupt_push）：16 位字经内部端口。 */
static void irq_push(Np560Bus *bus, DMCUCpu *cpu, uint32_t value)
{
    dmcu_push(bus, cpu, value);
}

/* 保存上一层上下文（cpu.c:205 dmcu_interrupt_suspend）。 */
static void irq_suspend(DMCUCpu *cpu)
{
    DMCUIrqContext *ctx = &cpu->irq_contexts[cpu->irq_depth++];

    ctx->frame_base = cpu->irq_frame_base;
    ctx->frame_size = cpu->irq_frame_size;
    ctx->from_icr = cpu->interrupt_from_icr;
    ctx->repeat_saved = cpu->irq_repeat_saved;
    ctx->repeat_pc = cpu->irq_repeat_pc;
    ctx->repeat_end = cpu->irq_repeat_end;
    ctx->repeat_count = cpu->irq_repeat_count;
    ctx->repeat_block = cpu->irq_repeat_block;
    ctx->single_repeat_pc = cpu->irq_single_repeat_pc;
    ctx->single_repeat_count = cpu->irq_single_repeat_count;
}

/* 结束当前层（cpu.c:216 dmcu_interrupt_complete）：恢复父层或退出中断。 */
static void irq_complete(DMCUCpu *cpu)
{
    if (cpu->irq_depth) {
        DMCUIrqContext *ctx = &cpu->irq_contexts[--cpu->irq_depth];

        cpu->irq_frame_base = ctx->frame_base;
        cpu->irq_frame_size = ctx->frame_size;
        cpu->interrupt_from_icr = ctx->from_icr;
        cpu->irq_repeat_saved = ctx->repeat_saved;
        cpu->irq_repeat_pc = ctx->repeat_pc;
        cpu->irq_repeat_end = ctx->repeat_end;
        cpu->irq_repeat_count = ctx->repeat_count;
        cpu->irq_repeat_block = ctx->repeat_block;
        cpu->irq_single_repeat_pc = ctx->single_repeat_pc;
        cpu->irq_single_repeat_count = ctx->single_repeat_count;
    } else {
        cpu->in_interrupt = false;
        cpu->interrupt_from_icr = false;
        cpu->irq_repeat_saved = false;
    }
    cpu->irq_state_changed = true;
}

/* 共享帧入口（cpu.c:242 dmcu_interrupt_enter）。source<0 为同步 TRAP，
 * 不受理 ICR、不改优先级结构。 */
static void irq_enter(Np560Bus *bus, DMCUCpu *cpu, uint32_t return_pc,
                      uint32_t target, int source)
{
    uint16_t interrupted_psw = dmcu_sfr_read(cpu, DMCU_SFR_PSW, 16);
    uint16_t socr = dmcu_sfr_read(cpu, DMCU_SFR_SOCR, 16);
    unsigned frame_size = (socr & DMCU_SOCR_NSEG) ? 4u : 6u;

    /* 嵌套时独立保存父层的帧位置与重复影子（nesting.md 改动 2）。 */
    if (cpu->in_interrupt && cpu->irq_depth < DMCU_IRQ_MAX_DEPTH) {
        irq_suspend(cpu);
    }

    /* 帧顺序（cpu.c:282-286 / nesting.md）：PC、NSEG=0 时的 CS、PSW；
     * RETI 按相反次序弹出。 */
    irq_push(bus, cpu, dmcu_code_pc(return_pc));
    if (!(socr & DMCU_SOCR_NSEG)) {
        irq_push(bus, cpu, dmcu_code_segment(return_pc));
    }
    irq_push(bus, cpu, interrupted_psw);

    cpu->irq_frame_size = frame_size;
    cpu->irq_frame_base = cpu->gpr[DMCU_SP] & MASK16;

    /*
     * CN1916884A：中断挂起活动的重复源/计数上下文，服务例程返回后恢复；
     * ISR 必须在重复引擎停用的状态下执行（cpu.c:328-339）。嵌套的 ICR
     * 进入各自独立保存父层影子。
     */
    cpu->irq_repeat_saved = cpu->repeat_count != 0 ||
                            cpu->single_repeat_count != 0;
    if (cpu->irq_repeat_saved) {
        cpu->irq_repeat_pc = cpu->repeat_pc;
        cpu->irq_repeat_end = cpu->repeat_end;
        cpu->irq_repeat_count = cpu->repeat_count;
        cpu->irq_repeat_block = cpu->repeat_block;
        cpu->irq_single_repeat_pc = cpu->single_repeat_pc;
        cpu->irq_single_repeat_count = cpu->single_repeat_count;
    }
    cpu->repeat_pc = 0;
    cpu->repeat_end = 0;
    cpu->repeat_count = 0;
    cpu->repeat_block = false;
    cpu->single_repeat_pc = 0;
    cpu->single_repeat_count = 0;

    cpu->interrupt_from_icr = source >= 0;
    if (source >= 0) {
        unsigned icr = cpu->sfr[DMCU_ICR_FIRST + source];
        unsigned psr = (interrupted_psw & ~DMCU_PSR_CPRI) |
                       DMCU_PSR_CIRQ | (dmcu_irq_priority(icr) << 12);

        /* 受理时只清所选来源的 RQ（icr_vector_dispatch.md）。 */
        cpu->sfr[DMCU_ICR_FIRST + source] &= ~DMCU_ICR_RQ;
        dmcu_sfr_write(cpu, DMCU_SFR_PSW, psr, 16);
    }
    cpu->pc = target;
    cpu->in_interrupt = true;
    cpu->irq_entries++;
    if (cpu->irq_depth + 1 > cpu->irq_max_depth) {
        cpu->irq_max_depth = cpu->irq_depth + 1;
    }
    cpu->irq_state_changed = true;
}

/* ---- 软件陷阱（cpu.c:388 dmcu_software_trap） ---- */

void dmcu_software_trap(Np560Bus *bus, DMCUCpu *cpu, uint32_t return_pc,
                        unsigned trap)
{
    /* CN1916884A p124：PC=zero_extend(trap*4)，NSEG 模式 CS=当前段，
     * 否则 CS=1。不指定 RAM 向量选择、屏蔽或 PRI 更新。 */
    unsigned segment = cpu->sfr[DMCU_SFR_SOCR] & DMCU_SOCR_NSEG ?
                       dmcu_code_segment(return_pc) : 1;

    irq_enter(bus, cpu, return_pc,
              dmcu_code_address(segment, (trap & 0xff) * 4u), -1);
}

/* ---- RETI 簿记（helper.c:19879 Retire 段 + cpu.c:216） ---- */

void dmcu_irq_reti_retire(Np560Bus *bus, DMCUCpu *cpu, uint32_t frame_sp)
{
    /* 合成 RETI（guest 自构帧）不提前结束外层硬件帧：只结算帧位置
     * 匹配的那一层（helper.c:19912-19916）。 */
    if (cpu->in_interrupt && frame_sp == cpu->irq_frame_base) {
        /* 恢复被挂起的前台重复上下文（helper.c:19944-19952）。 */
        if (cpu->irq_repeat_saved) {
            cpu->repeat_pc = cpu->irq_repeat_pc;
            cpu->repeat_end = cpu->irq_repeat_end;
            cpu->repeat_count = cpu->irq_repeat_count;
            cpu->repeat_block = cpu->irq_repeat_block;
            cpu->single_repeat_pc = cpu->irq_single_repeat_pc;
            cpu->single_repeat_count = cpu->irq_single_repeat_count;
            cpu->irq_repeat_saved = false;
        }
        irq_complete(cpu);
    }
    (void)bus;
}

/* ---- 指令边界仲裁（cpu.c:404 dmcu_cpu_exec_interrupt） ---- */

void dmcu_irq_service(Np560Bus *bus, DMCUCpu *cpu)
{
    uint16_t socr;
    unsigned frame_size;
    uint16_t old_sp, old_fp;
    int source;
    unsigned vector, segment;

    /* DTU 传输不受 CPU 屏蔽限制；先把挂起的搬运跑掉。 */
    dmcu_dtu_run(bus, cpu);

    socr = dmcu_sfr_read(cpu, DMCU_SFR_SOCR, 16);
    /* CN1916884A p114：IGNDS 无 DS 序列同时屏蔽硬件中断；CRIT 窗口
     * 同理。保持 RQ 挂起，不清除或改写 SOCR.IE（cpu.c:420-427）。 */
    if (!(socr & DMCU_SOCR_IE) || cpu->critical_count ||
        cpu->ignore_ds_count) {
        return;
    }
    source = dmcu_irq_select(cpu);
    if (source < 0) {
        return;
    }
    old_sp = cpu->gpr[DMCU_SP] & MASK16;
    old_fp = cpu->gpr[DMCU_FP] & MASK16;
    frame_size = socr & DMCU_SOCR_NSEG ? 4u : 6u;
    /* 历史沿袭的异步 SP/FP 重叠延后（cpu.c:438-445）；不适用于同步
     * TRAP，也不冒充 STOVUN 栈异常。 */
    if (old_sp >= frame_size && old_fp &&
        old_fp < old_sp && old_fp + 2u > old_sp - frame_size) {
        return;
    }
    vector = DMCU_IRQ_VECTOR((unsigned)source);
    segment = socr & DMCU_SOCR_NSEG ? dmcu_code_segment(cpu->pc) :
              (socr & DMCU_SOCR_RAM ? 0u : 1u);
    irq_enter(bus, cpu, cpu->pc,
              dmcu_code_address(segment, vector), source);
}
