/*
 * np560_timers.c - NP560+ 三路周期定时器 + 32.768kHz 时基。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560_timer.c；ptimer 语义改用 CPU 周期
 * 累加器实现（每次推进 delta 周期，按 frequency_hz/cpu_hz 分数节拍）。
 * 触发时机、锁存、装载语义与 ptimer 对齐：
 *   - PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT：仅在减到 0 时触发；
 *   - set_limit(limit, 1)：重装值 = limit，周期性；
 *   - 计数器读 = limit - count（上计数视图）。
 */
#include "np560_timers.h"
#include "dmcu_exec.h"
#include "dmcu_irq.h"
#include "dmcu_clock.h"
#include <string.h>

/* 上计数视图（np560_timer.c:54 np560_periodic_timer_counter）。 */
static uint32_t periodic_timer_counter(const Np560PeriodicTimer *s)
{
    return (uint32_t)(s->limit - s->down_count) & 0xffffu;
}

static void periodic_timer_set_count(Np560PeriodicTimer *s, uint32_t count)
{
    s->down_count = count;
}

/* ptimer_set_limit(limit, reload)：装载 limit。 */
static void periodic_timer_set_limit(Np560PeriodicTimer *s, uint32_t limit)
{
    s->limit = limit;
}

void np560_timers_reset(Np560Timers *timers)
{
    unsigned i;

    for (i = 0; i < NP560_PERIODIC_TIMERS; i++) {
        Np560PeriodicTimer *s = &timers->channel[i];

        s->running = false;
        s->expirations = 0;
        s->unsupported_configs = 0;
        s->frequency_hz = dmcu_system_clock_hz(timers->cpu);
        s->limit = 1;
        s->down_count = 1;
        s->acc_num = 0;
    }
    timers->timebase.running = false;
    timers->timebase.expirations = 0;
    timers->timebase.unsupported_configs = 0;
    timers->timebase.frequency_hz = 32768;
    timers->timebase.limit = 1;
    timers->timebase.down_count = 1;
    timers->timebase.acc_num = 0;
}

void np560_timers_init(Np560Timers *timers, DMCUCpu *cpu)
{
    unsigned i;

    timers->cpu = cpu;
    for (i = 0; i < NP560_PERIODIC_TIMERS; i++) {
        timers->channel[i].cpu = cpu;
        timers->channel[i].index = i;
    }
    timers->timebase.cpu = cpu;
    timers->timebase.index = 3;
    np560_timers_reset(timers);
}

/* 时基写（np560_timer.c:20 np560_timebase_write）。 */
static void timebase_write(Np560Timers *timers, uint16_t old_value,
                           uint16_t value)
{
    Np560PeriodicTimer *s = &timers->timebase;
    unsigned select = value & 15;
    bool enabled = (value & DMCU_TB_TBON) && select != 15;

    if (!((old_value ^ value) & (DMCU_TB_TBON | 15))) {
        return;
    }
    s->down_count = s->limit = 1;
    s->acc_num = 0;
    s->frequency_hz = select != 15 ? 32768u >> select : 0;
    if (enabled) {
        /* 启用/选择字变更时重启完整周期。 */
        s->down_count = 1;
        s->running = true;
    } else {
        s->running = false;
        if (value & DMCU_TB_TBON) {
            s->unsupported_configs++;
        }
    }
}

/* 周期定时器写（np560_timer.c:73 np560_periodic_timer_write）。 */
static void periodic_timer_write(Np560Timers *t, Np560PeriodicTimer *s,
                                 unsigned reg, uint16_t old_value,
                                 uint16_t value)
{
    (void)t;
    unsigned counter_reg = DMCU_SFR_T0 + 3 * s->index;
    unsigned control = s->cpu->sfr[counter_reg + 2];
    uint32_t counter = periodic_timer_counter(s);
    uint32_t limit = s->cpu->sfr[counter_reg + 1] + 1u;
    bool supported = (control & ~DMCU_TX_TON) == DMCU_TX_PERIODIC_CONFIG;
    bool enabled = (control & DMCU_TX_TON) && supported;

    if (reg == DMCU_SFR_PLL || reg == DMCU_SFR_CLKCON) {
        s->frequency_hz = dmcu_system_clock_hz(s->cpu);
    }
    if (reg == counter_reg + 1) {
        periodic_timer_set_limit(s, limit);
        /* 保持上计数：新比较值若已落后于当前计数，16 位计数器会在
         * 到达比较前回绕（np560_timer.c:88-94）。 */
        periodic_timer_set_count(s,
            limit > counter ? limit - counter : 0x10000u + limit - counter);
    }
    if (reg == counter_reg) {
        periodic_timer_set_count(s,
            limit > value ? limit - value : 0x10000u + limit - value);
    }
    if (reg == counter_reg + 2) {
        if (enabled && !s->running) {
            /* 原厂周期设置先写 P 再写 8080，期望一个完整首周期。 */
            periodic_timer_set_limit(s, limit);
            s->down_count = limit;
            s->acc_num = 0;
            s->running = true;
        } else if (!enabled && s->running) {
            s->running = false;
        }
        if ((control & DMCU_TX_TON) && !supported && old_value != value) {
            s->unsupported_configs++;
        }
        s->running = enabled;
    }
}

uint16_t np560_timers_read(Np560Timers *timers, unsigned reg, uint16_t value)
{
    unsigned i;

    /* 计数地址遵循 Tx/TxP/TxC 三寄存器布局；计数回读即时取当前值。 */
    for (i = 0; i < NP560_PERIODIC_TIMERS; i++) {
        if (reg == DMCU_SFR_T0 + 3 * i) {
            return (uint16_t)periodic_timer_counter(&timers->channel[i]);
        }
    }
    return value;
}

void np560_timers_write(Np560Timers *timers, unsigned reg,
                        uint16_t old_value, uint16_t value)
{
    unsigned i;

    if (reg == DMCU_SFR_PLL || reg == DMCU_SFR_CLKCON) {
        for (i = 0; i < NP560_PERIODIC_TIMERS; i++) {
            periodic_timer_write(timers, &timers->channel[i], reg,
                                 old_value, value);
        }
    } else if (reg == DMCU_SFR_TBC) {
        timebase_write(timers, old_value, value);
    } else if (reg >= DMCU_SFR_T0 && reg <= DMCU_SFR_T2C) {
        periodic_timer_write(timers,
                             &timers->channel[(reg - DMCU_SFR_T0) / 3],
                             reg, old_value, value);
    }
}

/* 周期 tick（np560_timer.c:136）：到期触发，RQ 锁存由 ICR 完成。 */
static void periodic_timer_tick(Np560PeriodicTimer *s)
{
    s->expirations++;
    dmcu_irq_raise(NULL, s->cpu, 1 + s->index);
}

/* 时基 tick（np560_timer.c:146）：RQ 即使 TBICR.EN/SOCR.IE 清零也产生。 */
static void timebase_tick(Np560PeriodicTimer *s)
{
    s->expirations++;
    dmcu_irq_raise(NULL, s->cpu, 0);
}

void np560_timers_advance(Np560Timers *timers, uint64_t delta_cpu_cycles)
{
    unsigned i;
    uint32_t cpu_hz = dmcu_system_clock_hz(timers->cpu);

    if (!delta_cpu_cycles) {
        return;
    }
    for (i = 0; i <= NP560_PERIODIC_TIMERS; i++) {
        Np560PeriodicTimer *s = i < NP560_PERIODIC_TIMERS ?
                                &timers->channel[i] : &timers->timebase;

        if (!s->running || !s->frequency_hz) {
            continue;
        }
        s->acc_num += delta_cpu_cycles * s->frequency_hz;
        while (s->acc_num >= cpu_hz) {
            s->acc_num -= cpu_hz;
            /* 下计数到 0 触发并重装（TRIGGER_ONLY_ON_DECREMENT）。 */
            if (s->down_count > 0) {
                s->down_count--;
            }
            if (s->down_count == 0) {
                s->down_count = s->limit;
                if (s->index == 3) {
                    timebase_tick(s);
                } else {
                    periodic_timer_tick(s);
                }
            }
        }
    }
}
