/*
 * host/runner.c - DMCU 解释器主机测试。
 *
 * 分八个测试程序验证移植语义（均与 QEMU helper.c 对照）：
 *   A 基础:  MOV/ALU/字节车道/DS 分段/X RAM 别名/NOP
 *   B 移位:  SL/SR/ROL + C/V 标志 + SB 缓冲
 *   C 位域:  BST/BCLD/BOR + 位测试跳转 JB
 *   D 控制流: 条件近跳转 JE / CALL+RETS / SCALL+RETF
 *   E 重复:  单指令 REPS / 块 REPB
 *   F 杂项:  MUL 有符号/PUSH-POP SFR/LINK-UNLINK/NEG/CRIT/RESET
 *   G 算术:  CMPD/UDIV/DIV/NORM（RB10 计数）
 *   H 中断:  软件 RQ/向量/RETI/优先级嵌套/TRAP
 *   I 定时器: T0/T1/T2 周期模式/时基/CLKCON 门控
 *   J 音频:  DAC 请求时钟/ICR16-17/样本锁存
 *   K LCD:   ST7529 命令口/2B3P-3B3P/窗口回绕/读锁存/RMW/EXT/总线字节路由
 *   N 介质:  NAND 文件介质(AND/擦除)/code 平面 vma03 装载语义
 *   L 键盘:  F160 矩阵/F164 驱动/F340 事件/legacy 位/总线 word 路由
 *   M 存储:  NAND F300 命令 ABI/AND 编程/OOB 流/COPY_BACK/ECC 引擎/SOCR 视图
 *
 * 用法：make && ./dmcu_runner
 * 返回 0 表示全部断言通过。
 */
#include <stdio.h>
#include <string.h>
#include "dmcu_bus.h"
#include "dmcu_exec.h"
#include "dmcu_irq.h"
#include "dmcu_clock.h"
#include "np560_periph.h"
#include "np560_mp3_hle_tab5.h"

#define PROGRAM_BASE 0x200000u

static int failures;

static void check(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) {
        printf("  ok   %-26s = 0x%x\n", name, got);
    } else {
        printf("  FAIL %-26s = 0x%x (want 0x%x)\n", name, got, want);
        failures++;
    }
}

/* 载入程序（段 0x20 起）并跑至程序尾（仅以 20 段 PC 判定）或停机。 */
static void run(Np560Bus *bus, DMCUCpu *cpu, const uint16_t *program,
                size_t words, unsigned max_steps)
{
    uint32_t end = (uint32_t)(words * 2);
    unsigned step;

    memset(cpu, 0, sizeof(*cpu));
    cpu->gpr[DMCU_SP] = 0x1000;
    cpu->gpr[DMCU_FP] = 0x1000;
    np560_bus_load(bus, PROGRAM_BASE,
                   (const uint8_t *)program, words * 2);
    cpu->pc = dmcu_code_address(0x20, 0x0000);
    for (step = 0; step < max_steps; step++) {
        if (cpu->halted) {
            break;
        }
        dmcu_exec_step(bus, cpu);
        if (!cpu->halted &&
            dmcu_code_segment(cpu->pc) == 0x20 &&
            dmcu_code_pc(cpu->pc) >= end) {
            break;
        }
    }
    if (cpu->halted && cpu->unsupported_count) {
        printf("  [halted at 20:%04x word=%04x]\n",
               cpu->fault_pc & 0xffff, cpu->fault_word);
    }
}

/* 在 run() 之前设置寄存器的等价入口（先构造再运行）。 */
static void run2(Np560Bus *bus, DMCUCpu *cpu, const uint16_t *program,
                 size_t words, unsigned max_steps,
                 const unsigned *regs, const uint32_t *vals, unsigned n)
{
    unsigned i;

    memset(cpu, 0, sizeof(*cpu));
    cpu->gpr[DMCU_SP] = 0x1000;
    cpu->gpr[DMCU_FP] = 0x1000;
    for (i = 0; i < n; i++) {
        cpu->gpr[regs[i]] = vals[i];
    }
    np560_bus_load(bus, PROGRAM_BASE,
                   (const uint8_t *)program, words * 2);
    cpu->pc = dmcu_code_address(0x20, 0x0000);
    {
        uint32_t end = (uint32_t)(words * 2);
        unsigned step;

        for (step = 0; step < max_steps; step++) {
            if (cpu->halted) {
                break;
            }
            dmcu_exec_step(bus, cpu);
            if (!cpu->halted &&
                dmcu_code_segment(cpu->pc) == 0x20 &&
                dmcu_code_pc(cpu->pc) >= end) {
                break;
            }
        }
    }
    if (cpu->halted && cpu->unsupported_count) {
        printf("  [halted at 20:%04x word=%04x]\n",
               cpu->fault_pc & 0xffff, cpu->fault_word);
    }
}

/* ---- A: 基础（MOV/ALU/车道/分段） ---- */

static const uint16_t program_a[] = {
    0x4801,             /* 0  MOV  R0,#1                */
    0x4822,             /* 1  MOV  R2,#2                */
    0x1002,             /* 2  ADD  R0,R2     -> R0=3    */
    0x4843,             /* 3  MOV  R4,#3                */
    0x0a42,             /* 4  MOV  [R4],R2   -> X[3]=2  */
    0x2814,             /* 5  MOV  R1,[R4]   -> R1=2    */
    0x1004,             /* 6  ADD  R0,R4     -> R0=6    */
    0x2974,             /* 7  MOVB R7,[R4]   -> RB7=2   */
    0xc700,             /* 8  NOP                       */
    0x6810, 0x0002,     /* 9  MOV  DS0,#2               */
    0x4845,             /* 10 MOV  R4,#5                */
    0x0a40,             /* 11 MOV  [R4],R0   -> b2[5]=6 */
    0x6810, 0x0000,     /* 12 MOV  DS0,#0               */
    0x2814,             /* 13 MOV  R1,[R4]   -> R1=0    */
    0x6810, 0x0002,     /* 14 MOV  DS0,#2               */
    0x2814,             /* 15 MOV  R1,[R4]   -> R1=6    */
    /* 对齐字节的车道规则（helper.c:1834 偶车道=高字节；np560.c:4044
     * 逻辑 386C→物理 B86D 实证）：平坦字节地址 = 车道地址 ^ 1。 */
    0x4844,             /* 16 MOV  R4,#4                */
    0x0b45,             /* 17 MOVB [R4],RB5(R2 低字节=2) -> b2 bytes[5]=2 */
    0x2834,             /* 18 MOV  R3,[R4] -> R3=0x0200 */
    0x4845,             /* 19 MOV  R4,#5                */
    0x2954,             /* 20 MOVB R5,[R4] -> bytes[4]=0 */
    0x0b46,             /* 21 MOVB [R4],RB6(R3 高字节=2) -> bytes[4]=2 */
    0x4844,             /* 22 MOV  R4,#4                */
    0x2834,             /* 23 MOV  R3,[R4] -> R3=0x0202 */
};

static void test_a(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[A] MOV/ALU/字节车道/DS 分段\n");
    run(bus, &cpu, program_a,
        sizeof(program_a) / sizeof(program_a[0]), 64);
    check("A R0", dmcu_reg_read(&cpu, 0, 16), 6);
    check("A R1", dmcu_reg_read(&cpu, 1, 16), 6);
    check("A DS0", cpu.sfr[DMCU_SFR_DS0], 2);
    /* 字@5（bank2，word 11 写 6）后被 word 17 的字节写（bytes[5]=2）
     * 覆盖高字节。 */
    check("A bank2 word@5",
          bus->phys[0x10005] | (bus->phys[0x10006] << 8), 2);
    /* 对齐字节车道：偶车道=高字节（MOVB 写 [4] 落 bytes[5]。 */
    check("A lane4 byte->MSB", bus->phys[0x10005], 2);
    check("A RB5 lane5 read", dmcu_reg_read(&cpu, 5, 8), 0);
    check("A lane5 byte->LSB", bus->phys[0x10004], 2);
    check("A word@4 after lane5", dmcu_reg_read(&cpu, 3, 16), 0x0202);
    check("A no faults", cpu.unsupported_count, 0);
    np560_bus_destroy(bus);
}

/* ---- B: 移位族（大常量由宿主预置寄存器） ---- */

static const uint16_t program_b[] = {
    0x4c04,             /* 0  SL   R0,#4  -> 0x1230, C=1, V=1 */
    0x4d14,             /* 1  SR   R1,#4  -> 0x0876, C=0, SB=0x5000 */
    0xca24,             /* 2  ROL  R2,#4  -> 0x001F, C=1 */
    0xc700,             /* 3  NOP                       */
};

static const unsigned preset_b_regs[] = { 0, 1, 2 };
static const uint32_t preset_b_vals[] = { 0xf123, 0x8765, 0xf001 };

static void test_b(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[B] 移位 SL/SR/ROL + SB\n");
    run2(bus, &cpu, program_b, sizeof(program_b) / sizeof(program_b[0]),
         64, preset_b_regs, preset_b_vals, 3);
    check("B R0 SL4", dmcu_reg_read(&cpu, 0, 16), 0x1230);
    check("B R1 SR4", dmcu_reg_read(&cpu, 1, 16), 0x0876);
    check("B R2 ROL4", dmcu_reg_read(&cpu, 2, 16), 0x001f);
    check("B C", (cpu.psw & DMCU_PSW_C) != 0, 1);
    check("B V cleared by ROL", (cpu.psw & DMCU_PSW_V) != 0, 0);
    check("B SB", cpu.shift_buffer, 0x5000);
    check("B no faults", cpu.unsupported_count, 0);
    np560_bus_destroy(bus);
}

/* ---- C: 位域族 ---- */

static const uint16_t program_c[] = {
    0x4814,             /* 0  MOV  R1,#4                */
    0xe370,             /* 1  BST  R0.3（位地址 70=R0 别名） */
    0xf370,             /* 2  BCLD R0.3                 */
    0x1600,             /* 3  CMP  R0,R0  -> Z=1, C=0   */
    0x6c71, 0x0122,     /* 4  BOR  C,R1.2 -> C=1        */
    0x9a71, 0x0201,     /* 6  JB   R1.2,+1 -> 跳过 i8   */
    0x4801,             /* 8  MOV  R0,#1（被跳过）      */
    0x4855,             /* 9  MOV  R5,#5                */
};

static void test_c(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[C] 位域 BST/BCLD/BOR/JB\n");
    run(bus, &cpu, program_c, sizeof(program_c) / sizeof(program_c[0]), 64);
    /* R0.3 置位后清零 -> R0=0；BOR C,R1.2(R1=4) -> C=1。 */
    check("C R0", dmcu_reg_read(&cpu, 0, 16), 0);
    check("C PSW.C", (cpu.psw & DMCU_PSW_C) != 0, 1);
    check("C R5 (JB taken)", dmcu_reg_read(&cpu, 5, 16), 5);
    check("C R0 not set", dmcu_reg_read(&cpu, 0, 16), 0);
    check("C no faults", cpu.unsupported_count, 0);
    np560_bus_destroy(bus);
}

/* ---- D: 控制流族 ---- */

static const uint16_t program_d[] = {
    0x4801,             /* 0  MOV  R0,#1                */
    0x1600,             /* 1  CMP  R0,R0   -> Z=1        */
    0xd101,             /* 2  JE   +1     -> 跳过 i3     */
    0x4831,             /* 3  MOV  R3,#1（被跳过）      */
    0xcd03,             /* 4  CALL +3     -> 20:i8 子程序 */
    0x4811,             /* 5  MOV  R1,#1（RETS 回到这里）*/
    0x4877,             /* 6  MOV  R7,#7                */
    0xd102,             /* 7  JE   +2     -> 跳过子程序 */
    0x4866,             /* 8  MOV  R6,#6（子程序体）    */
    0xc600,             /* 9  RETS -> 回 20:i5           */
    0x4888,             /* 10 MOV  R8,#8                */
};

static const uint16_t program_d_main[] = {
    0xbd21, 0x0000,     /* 0  SCALL 21:0000             */
    0x4811,             /* 2  MOV  R1,#1（RETF 回到这里）*/
    0xc700,             /* 3  NOP                       */
};

static const uint16_t program_d_sub[] = {
    0x4822,             /* 21:0000  MOV  R2,#2         */
    0xc620,             /* 21:0002  RETF               */
};

static void test_d(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[D] 控制流 JE/CALL/RETS/SCALL/RETF\n");
    run(bus, &cpu, program_d, sizeof(program_d) / sizeof(program_d[0]), 64);
    check("D R3 (JE skip)", dmcu_reg_read(&cpu, 3, 16), 0);
    check("D R1 (after RETS)", dmcu_reg_read(&cpu, 1, 16), 1);
    check("D R6 (call body)", dmcu_reg_read(&cpu, 6, 16), 6);
    check("D R7", dmcu_reg_read(&cpu, 7, 16), 7);
    check("D R8", dmcu_reg_read(&cpu, 8, 16), 8);
    check("D SP restored", cpu.gpr[DMCU_SP], 0x1000);
    check("D no faults", cpu.unsupported_count, 0);

    /* 远调用：主程序 20:0000，子程序单独装入 21:0000。 */
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_bus_load(bus, PROGRAM_BASE,
                   (const uint8_t *)program_d_main,
                   sizeof(program_d_main));
    np560_bus_load(bus, 0x210000u,
                   (const uint8_t *)program_d_sub,
                   sizeof(program_d_sub));
    cpu.pc = dmcu_code_address(0x20, 0x0000);
    {
        unsigned step;

        for (step = 0; step < 64; step++) {
            if (cpu.halted) {
                break;
            }
            dmcu_exec_step(bus, &cpu);
            if (!cpu.halted &&
                dmcu_code_segment(cpu.pc) == 0x20 &&
                dmcu_code_pc(cpu.pc) >= sizeof(program_d_main)) {
                break;
            }
        }
    }
    check("D R2 (far body)", dmcu_reg_read(&cpu, 2, 16), 2);
    check("D R1 (far return)", dmcu_reg_read(&cpu, 1, 16), 1);
    check("D far SP restored", cpu.gpr[DMCU_SP], 0x1000);
    check("D far no faults", cpu.unsupported_count, 0);
    np560_bus_destroy(bus);
}

/* ---- E: 重复引擎 ---- */

static const uint16_t program_e[] = {
    0xbf00, 0x0002,     /* 0  REPS 下一条 ×3             */
    0x5041,             /* 2  ADD  R4,#1（重复 3 次）    */
    0xae01, 0x000e,     /* 3  REPB 块[5..6] ×2，end=20:000e */
    0x5051,             /* 5  ADD  R5,#1                 */
    0x5061,             /* 6  ADD  R6,#1                 */
    0x4877,             /* 7  MOV  R7,#7                 */
};

static void test_e(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[E] 重复引擎 REPS/REPB\n");
    run(bus, &cpu, program_e, sizeof(program_e) / sizeof(program_e[0]), 64);
    check("E R4 (REPS×3)", dmcu_reg_read(&cpu, 4, 16), 3);
    check("E R5 (REPB×2)", dmcu_reg_read(&cpu, 5, 16), 2);
    check("E R6 (REPB×2)", dmcu_reg_read(&cpu, 6, 16), 2);
    check("E R7", dmcu_reg_read(&cpu, 7, 16), 7);
    check("E no faults", cpu.unsupported_count, 0);
    np560_bus_destroy(bus);
}

/* ---- F: 杂项 MUL/PUSH-POP SFR/LINK/NEG/CRIT/RESET ---- */

static const uint16_t program_f[] = {
    0xc045,             /* 0  MUL  R4,R5 -> R6:R7=FFFFFFF6 */
    0x6810, 0x0002,     /* 1  MOV  DS0,#2                */
    0x1f10,             /* 3  PUSH DS0                   */
    0x6810, 0x0000,     /* 4  MOV  DS0,#0                */
    0x1f90,             /* 6  POP  DS0 -> 2              */
    0x2e04,             /* 7  LINK #4                 */
    0x2f00,             /* 8  UNLINK                  */
    0x4805,             /* 9  MOV  R0,#5               */
    0x3e00,             /* 10 NEG  R0  -> FFFB, C=1    */
    0x3f01,             /* 11 CRIT #2（其后 2 条受保护） */
    0x4822,             /* 12 MOV  R2,#2               */
    0x4833,             /* 13 MOV  R3,#3               */
    0xc700,             /* 14 NOP                     */
};

static const unsigned preset_f_regs[] = { 4, 5 };
static const uint32_t preset_f_vals[] = { 5, 0xfffe };

static const uint16_t program_f_reset[] = {
    0x3fff,             /* 0  RESET                      */
};

static void test_f(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[F] MUL/PUSH-POP SFR/LINK/NEG/CRIT\n");
    run2(bus, &cpu, program_f, sizeof(program_f) / sizeof(program_f[0]),
         64, preset_f_regs, preset_f_vals, 2);
    check("F R6 (hi)", dmcu_reg_read(&cpu, 6, 16), 0xffff);
    check("F R7 (lo)", dmcu_reg_read(&cpu, 7, 16), 0xfff6);
    check("F N", (cpu.psw & DMCU_PSW_N) != 0, 1);
    check("F DS0 popped", cpu.sfr[DMCU_SFR_DS0], 2);
    check("F SP restored", cpu.gpr[DMCU_SP], 0x1000);
    check("F FP restored", cpu.gpr[DMCU_FP], 0x1000);
    check("F NEG R0", dmcu_reg_read(&cpu, 0, 16), 0xfffb);
    check("F NEG C", (cpu.psw & DMCU_PSW_C) != 0, 1);
    check("F CRIT expired", cpu.critical_count, 0);
    check("F no faults", cpu.unsupported_count, 0);

    printf("[F] RESET 0x3fff\n");
    run(bus, &cpu, program_f_reset,
        sizeof(program_f_reset) / sizeof(program_f_reset[0]), 8);
    check("F reset_requested", cpu.reset_requested, 1);
    check("F halted", cpu.halted, 1);
    np560_bus_destroy(bus);
}

/* ---- G: 算术族 CMPD/DIV/NORM ---- */

static const uint16_t program_g[] = {
    0x6810, 0x0005,     /* 0  MOV  DS0,#5               */
    0x6e10, 0x0003,     /* 2  CMPD DS0,#3 -> DS0=4, C=0 */
    0xc300,             /* 4  UDIV R0: 100/7 -> q=14,r=2  */
    0x0887,             /* 5  MOV  R8,R7（保存无符号商）  */
    0x0875,             /* 6  MOV  R7,R5（R5 预置 -100）  */
    0xc200,             /* 7  DIV  R0: -100/7 -> q=-14,r=-2 */
    0x1e04,             /* 8  NORM R4（kind0 有符号）     */
    0x0895,             /* 9  MOV  R9,R5（取 RB10=8）     */
    0x1e20,             /* 10 NORM R0（kind2 无符号，R0=7）*/
    0x0835,             /* 11 MOV  R3,R5（取 RB10=13）    */
    0xc700,             /* 12 NOP                       */
};

static const unsigned preset_g_regs[] = { 0, 4, 5, 7 };
static const uint32_t preset_g_vals[] = { 7, 0x0040, 0xff9c, 100 };

static void test_g(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[G] CMPD/UDIV/DIV/NORM\n");
    run2(bus, &cpu, program_g, sizeof(program_g) / sizeof(program_g[0]),
         64, preset_g_regs, preset_g_vals, 4);
    /* CMPD：与 #3 比较（5-3 无借位），随后按 delta 自减。 */
    check("G DS0 (CMPD-1)", cpu.sfr[DMCU_SFR_DS0], 4);
    check("G udiv quot (R8)", dmcu_reg_read(&cpu, 8, 16), 14);
    check("G div quot (R7)", dmcu_reg_read(&cpu, 7, 16), 0xfff2);
    check("G div rem (R6)", dmcu_reg_read(&cpu, 6, 16), 0xfffe);
    /* NORM 在 DIV 之后执行并清 C/N/V，此处只验证 V 清零。 */
    check("G div V", (cpu.psw & DMCU_PSW_V) != 0, 0);
    /* kind0 有符号对正数减一；kind2 无符号为纯前导零计数。 */
    check("G norm signed (R9>>8)", dmcu_reg_read(&cpu, 9, 16) >> 8, 8);
    check("G norm unsigned (R3>>8)", dmcu_reg_read(&cpu, 3, 16) >> 8, 13);
    check("G no faults", cpu.unsupported_count, 0);
    np560_bus_destroy(bus);
}

/* ---- H: 中断 ICR/嵌套/TRAP ---- */

/* H1: 软件 RQ -> 向量 01:0040 -> RETI 返回。 */
static const uint16_t program_h1[] = {
    0x6800, 0x8000,     /* 0  SOCR = IE                  */
    0x6850, 0x0003,     /* 2  ICR0 = EN|RQ -> 进入 ISR0   */
    0x4811,             /* 4  MOV  R1,#1（RETI 回来后）   */
    0xc700,             /* 5  NOP                        */
};

static const uint16_t program_h1_isr0[] = {   /* 01:0040 */
    0x48a5,             /* MOV  R10,#5               */
    0xc610,             /* RETI                       */
};

/* H2: ISR0 内请求更高优先级 source16（向量 01:00c0）-> 嵌套 ISR1。
 * 注意 ISR1 的向量 0xc0 与 ISR0 代码区不得重叠；ICR16 的 PRI=1 使其
 * 优先级高于 ISR0 的 CPRI=0（CIRQ 只阻塞同级）。 */
static const uint16_t program_h2[] = {
    0x6800, 0x8000,     /* 0  SOCR = IE                  */
    0x6850, 0x0003,     /* 2  ICR0 = EN|RQ PRI=0         */
    0x4811,             /* 4  MOV  R1,#1                 */
    0x4822,             /* 5  MOV  R2,#2                 */
    0xc700,             /* 6  NOP                        */
};

static const uint16_t program_h2_isr0[] = {   /* 01:0040 */
    0x48a5,             /* MOV  R10,#5               */
    0x6860, 0x0013,     /* ICR16 = EN|RQ PRI=1 -> 抢占 */
    0x48b6,             /* MOV  R11,#6（ISR1 返回后）*/
    0xc610,             /* RETI                       */
};

static const uint16_t program_h2_isr1[] = {   /* 01:00c0 */
    0x48c7,             /* MOV  R12,#7               */
    0xc610,             /* RETI                       */
};

/* H3: TRAP #1 -> 01:0004（NSEG=0 时 CS=1）。 */
static const uint16_t program_h3[] = {
    0x4801,             /* 0  MOV  R0,#1（置 Z=0）        */
    0x0f01,             /* 1  TRAP #1 -> 01:0004         */
    0x4833,             /* 2  MOV  R3,#3（RETI 回来后）   */
    0xc700,             /* 3  NOP                        */
};

static const uint16_t program_h3_isr[] = {    /* 01:0004 */
    0x48d4,             /* MOV  R13,#4               */
    0xc610,             /* RETI                       */
};

/* 载入多段程序并运行（直到主程序结束或停机）。 */
static void run_multi(Np560Bus *bus, DMCUCpu *cpu,
                      const uint16_t *main_prog, size_t main_words,
                      const uint32_t *extra_addr,
                      const uint16_t *const *extra_prog,
                      const size_t *extra_words, unsigned n_extra,
                      unsigned max_steps)
{
    uint32_t end = (uint32_t)(main_words * 2);
    unsigned step, i;

    memset(cpu, 0, sizeof(*cpu));
    cpu->gpr[DMCU_SP] = 0x1000;
    cpu->gpr[DMCU_FP] = 0x1000;
    np560_bus_load(bus, PROGRAM_BASE,
                   (const uint8_t *)main_prog, main_words * 2);
    for (i = 0; i < n_extra; i++) {
        np560_bus_load(bus, extra_addr[i],
                       (const uint8_t *)extra_prog[i], extra_words[i] * 2);
    }
    cpu->pc = dmcu_code_address(0x20, 0x0000);
    for (step = 0; step < max_steps; step++) {
        if (cpu->halted) {
            break;
        }
        dmcu_exec_step(bus, cpu);
        if (!cpu->halted &&
            dmcu_code_segment(cpu->pc) == 0x20 &&
            dmcu_code_pc(cpu->pc) >= end) {
            break;
        }
    }
    if (cpu->halted && cpu->unsupported_count) {
        printf("  [halted at %02x:%04x word=%04x]\n",
               dmcu_code_segment(cpu->fault_pc), cpu->fault_pc & 0xffff,
               cpu->fault_word);
    }
}

static void test_h(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    printf("[H1] 软件 RQ -> 向量 -> RETI\n");
    {
        const uint32_t addr[] = { 0x010040u };
        const uint16_t *prog[] = { program_h1_isr0 };
        const size_t words[] = {
            sizeof(program_h1_isr0) / sizeof(program_h1_isr0[0])
        };

        run_multi(bus, &cpu, program_h1, 6, addr, prog, words, 1, 64);
    }
    check("H1 R10 (ISR body)", dmcu_reg_read(&cpu, 10, 16), 5);
    check("H1 R1 (after RETI)", dmcu_reg_read(&cpu, 1, 16), 1);
    check("H1 SP restored", cpu.gpr[DMCU_SP], 0x1000);
    check("H1 not in interrupt", cpu.in_interrupt, 0);
    check("H1 entries", cpu.irq_entries, 1);
    check("H1 PSR CIRQ cleared",
          (cpu.sfr[DMCU_SFR_PSW] & DMCU_PSR_CIRQ) != 0, 0);
    check("H1 no faults", cpu.unsupported_count, 0);

    printf("[H2] 优先级嵌套 ISR0 <- ISR1\n");
    {
        const uint32_t addr[] = { 0x010040u, 0x0100c0u };
        const uint16_t *prog[] = { program_h2_isr0, program_h2_isr1 };
        const size_t words[] = {
            sizeof(program_h2_isr0) / sizeof(program_h2_isr0[0]),
            sizeof(program_h2_isr1) / sizeof(program_h2_isr1[0]),
        };

        run_multi(bus, &cpu, program_h2, 7, addr, prog, words, 2, 64);
    }
    check("H2 R10 (ISR0 pre)", dmcu_reg_read(&cpu, 10, 16), 5);
    check("H2 R12 (ISR1 body)", dmcu_reg_read(&cpu, 12, 16), 7);
    check("H2 R11 (ISR0 post)", dmcu_reg_read(&cpu, 11, 16), 6);
    check("H2 R1 (main)", dmcu_reg_read(&cpu, 1, 16), 1);
    check("H2 SP restored", cpu.gpr[DMCU_SP], 0x1000);
    check("H2 depth max", cpu.irq_max_depth, 2);
    check("H2 entries", cpu.irq_entries, 2);
    check("H2 not in interrupt", cpu.in_interrupt, 0);
    check("H2 no faults", cpu.unsupported_count, 0);

    printf("[H3] TRAP #1 -> 01:0004\n");
    {
        const uint32_t addr[] = { 0x010004u };
        const uint16_t *prog[] = { program_h3_isr };
        const size_t words[] = {
            sizeof(program_h3_isr) / sizeof(program_h3_isr[0])
        };

        run_multi(bus, &cpu, program_h3, 4, addr, prog, words, 1, 64);
    }
    check("H3 R13 (trap body)", dmcu_reg_read(&cpu, 13, 16), 4);
    check("H3 R3 (after RETI)", dmcu_reg_read(&cpu, 3, 16), 3);
    check("H3 SP restored", cpu.gpr[DMCU_SP], 0x1000);
    check("H3 not in interrupt", cpu.in_interrupt, 0);
    check("H3 no faults", cpu.unsupported_count, 0);
    np560_bus_destroy(bus);
}

/* ---- I: 定时器（周期模式 + 时基 + CLKCON 门控） ---- */

static void test_i(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;
    Np560Periph periph;
    uint32_t cpu_hz;
    unsigned step;

    printf("[I] 定时器 T1 周期模式 / 时基 / CLKCON 门控\n");
    run(bus, &cpu, program_e, sizeof(program_e) / sizeof(program_e[0]), 4);
    /* program_e 是 REPS 测试；这里只需要一个初始化过的 CPU。 */
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_periph_init(&periph, &cpu, bus);
    cpu_hz = dmcu_system_clock_hz(&cpu);

    /* CLKCON 写需要 SOCR.UP（helper.c:1654）：默认 UP=0 必须拒绝。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_CLKCON, 0x1234, 16);
    check("I CLKCON gated", cpu.sfr[DMCU_SFR_CLKCON], 0);
    dmcu_sfr_write(&cpu, DMCU_SFR_SOCR, DMCU_SOCR_UP, 16);
    dmcu_sfr_write(&cpu, DMCU_SFR_CLKCON, 0x1234, 16);
    check("I CLKCON written", cpu.sfr[DMCU_SFR_CLKCON], 0x1234);
    /* 经设备钩子写回 0 → 定时器频率恢复 24.576MHz 基线。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_CLKCON, 0, 16);
    check("I T1 freq baseline", dmcu_system_clock_hz(&cpu),
          24576000u);

    /* T1：T1P=9（limit=10），T1C=0x8080（周期模式启动）。
     * 节拍频率 = 系统时钟：周期 = limit 个 CPU 周期。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_T0 + 4, 9, 16);       /* T1P */
    dmcu_sfr_write(&cpu, DMCU_SFR_T0 + 5, 0x8080, 16);  /* T1C */
    check("I T1 running", periph.timers.channel[1].running, 1);
    check("I T1 limit", periph.timers.channel[1].limit, 10);
    check("I T1 count", dmcu_sfr_read(&cpu, DMCU_SFR_T0 + 3, 16), 0);

    /* 推进 10 个周期：恰好一次到期 → ICR1（源 1）RQ 锁存。 */
    np560_periph_advance(&periph, 10);
    check("I T1 expired", periph.timers.channel[1].expirations, 1);
    check("I ICR2 RQ (T1)", (cpu.sfr[DMCU_ICR_FIRST + 2] & DMCU_ICR_RQ) != 0,
          1);
    /* EN=0：RQ 锁存但 IE 屏蔽投递（等指令边界仲裁）。 */
    check("I T1 reload", periph.timers.channel[1].down_count, 10);
    /* 计数器回读：上计数视图 = limit - down。 */
    check("I T1 counter read",
          dmcu_sfr_read(&cpu, DMCU_SFR_T0 + 3, 16), 0);

    /* 非周期控制模式不支持：T0C=0x8040 → 记录并拒绝启动。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_T0 + 2, 0x8040, 16);
    check("I T0 mode unsupported",
          periph.timers.channel[0].unsupported_configs, 1);
    check("I T0 not running", periph.timers.channel[0].running, 0);

    /* 时基：TBC = TBON|select0 → 32768Hz；推进 1 秒 → 32768 次触发。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_TBC, 0x8000, 16);
    check("I TB running", periph.timers.timebase.running, 1);
    check("I TB freq", periph.timers.timebase.frequency_hz, 32768);
    np560_periph_advance(&periph, cpu_hz);   /* 1 秒 */
    check("I TB expirations", periph.timers.timebase.expirations, 32768);
    check("I ICR0 RQ", (cpu.sfr[DMCU_ICR_FIRST + 0] & DMCU_ICR_RQ) != 0, 1);

    /* ICR2 EN|RQ + SOCR.IE 后指令边界仲裁可接受源 2（T1）。 */
    cpu.sfr[DMCU_ICR_FIRST + 2] |= DMCU_ICR_EN;
    cpu.sfr[DMCU_SFR_SOCR] |= DMCU_SOCR_IE;
    cpu.irq_state_changed = true;
    {
        /* 以 NOP 程序驱动一轮指令边界仲裁。 */
        static const uint16_t nop_prog[] = { 0xc700, 0xc700 };
        np560_bus_load(bus, PROGRAM_BASE, (const uint8_t *)nop_prog,
                       sizeof(nop_prog));
        cpu.pc = dmcu_code_address(0x20, 0);
        for (step = 0; step < 8 && !cpu.in_interrupt; step++) {
            dmcu_exec_step(bus, &cpu);
        }
        check("I ISR entered via T1", cpu.in_interrupt, 1);
        check("I ICR2 RQ cleared", (cpu.sfr[DMCU_ICR_FIRST + 2] &
                                    DMCU_ICR_RQ), 0);
    }
    np560_bus_destroy(bus);
}

/* ---- J: 音频 DAC（请求时钟 + ICR16/17） ---- */

static void test_j(void)
{
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;
    Np560Periph periph;
    uint32_t cpu_hz;

    printf("[J] 音频 DAC 请求时钟 / ICR16-17\n");
    run(bus, &cpu, program_e, sizeof(program_e) / sizeof(program_e[0]), 4);
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_periph_init(&periph, &cpu, bus);
    cpu_hz = dmcu_system_clock_hz(&cpu);

    /* 无效模式：mode=0 → 不运行。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_DACC, 0x0001, 16);   /* EN，mode=0 */
    check("J idle not running", periph.audio.request_running, 0);

    /* PLL 0x80A0 + mode 8（DACC=0x0009）：hz = 4M*6/(1*1024) ≈ 23437。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_AUDIO_PLL, 0x80a0, 16);
    dmcu_sfr_write(&cpu, DMCU_SFR_DACC, 0x0009, 16);   /* mode 8 + EN */
    check("J running", periph.audio.request_running, 1);
    check("J request hz", periph.audio.request_hz,
          4000000u * 6u / 1024u);

    /* 写一个 DAC 样本；推进 1 秒 → 23437 次请求 → ICR16/17 RQ。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_DAC0, 0x1234, 16);
    dmcu_sfr_write(&cpu, DMCU_SFR_DAC1, 0x2468, 16);
    np560_periph_advance(&periph, cpu_hz);
    check("J request ticks", periph.audio.request_ticks,
          4000000u * 6u / 1024u);
    check("J ICR16 RQ", (cpu.sfr[DMCU_ICR_FIRST + 16] & DMCU_ICR_RQ) != 0, 1);
    check("J ICR17 RQ", (cpu.sfr[DMCU_ICR_FIRST + 17] & DMCU_ICR_RQ) != 0, 1);
    check("J DAC0 latched", periph.audio.sample[0], 0x1234);
    check("J DAC1 latched", periph.audio.sample[1], 0x2468);

    /* DACC bit15（停止）：enabled=false → 请求时钟停止。 */
    dmcu_sfr_write(&cpu, DMCU_SFR_DACC, 0x8009, 16);
    check("J stopped", periph.audio.request_running, 0);
    np560_periph_advance(&periph, cpu_hz);
    check("J no extra ticks", periph.audio.request_ticks,
          4000000u * 6u / 1024u);
    /* 禁用时 DAC 锁存值仍保持（np560_audio_sync 语义）。 */
    check("J DAC hold", periph.audio.sample[0], 0x1234);
    np560_bus_destroy(bus);
}

/* ---- K: ST7529 LCD（命令口/像素格式/窗口回绕/读锁存/RMW/EXT/总线） ---- */

static void test_k(void)
{
    Np560Lcd lcd;
    Np560Bus *bus;
    DMCUCpu cpu;
    Np560Periph periph;

    printf("[K] ST7529 LCD 命令口\n");
    np560_lcd_reset(&lcd);
    check("K reset sleeping", lcd.sleeping, 1);
    check("K reset 2B3P", lcd.scan[2], 1);
    check("K reset whole scroll", lcd.area_scroll[3], 3);
    check("K reset display_lines", lcd.display_lines, 4);
    check("K reset visible dark", np560_lcd_visible_level(&lcd, 0, 0), 0);

    /* SLPOUT + DISON。 */
    np560_lcd_command(&lcd, 0x94);
    np560_lcd_command(&lcd, 0xaf);
    check("K awake", lcd.sleeping, 0);
    check("K display on", lcd.display_on, 1);
    /* STREAD：SCM(area_scroll[3]&3)<<6 | ON<<3 | !INV<<1（复位选全屏滚动
     * → SCM=3 → 0xC0）。 */
    check("K STREAD", np560_lcd_status_read(&lcd), 0xca);

    /* DATSDR 3B3P（scan=0,0,2）；CASET 0..1；LASET 0..0；RAMWR。 */
    np560_lcd_command(&lcd, 0xbc);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 2);
    check("K DATSDR 3B3P", lcd.scan[2] & 7, 2);
    np560_lcd_command(&lcd, 0x15);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 1);
    np560_lcd_command(&lcd, 0x75);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_command(&lcd, 0x5c);
    check("K RAMWR at start", lcd.column, 0);

    /* 组 0：P0=0xab>>3=21, P1=(latch>>3)&0x1f=24, P2=0xf0>>3=30。 */
    np560_lcd_data_write(&lcd, 0xab);
    np560_lcd_data_write(&lcd, 0xc0);
    np560_lcd_data_write(&lcd, 0xf0);
    check("K 3B3P P0", lcd.gram[0][0], 21);
    check("K 3B3P P1", lcd.gram[0][1], 24);
    check("K 3B3P P2", lcd.gram[0][2], 30);
    /* 组 1 提交 → 列回绕到 0、行回绕 → 整窗回绕计数。 */
    np560_lcd_data_write(&lcd, 0x1f);
    np560_lcd_data_write(&lcd, 0x00);
    np560_lcd_data_write(&lcd, 0x00);
    check("K wrap col", lcd.column, 0);
    check("K wrap frames", (unsigned)(lcd.completed_frames & 0xff), 1);

    /* 未完成的组被任何命令终止且 GRAM 保持完好（3B3P 第三字节缺席）。 */
    np560_lcd_command(&lcd, 0x5c);
    np560_lcd_data_write(&lcd, 0xff);
    np560_lcd_command(&lcd, 0x00);
    check("K partial group intact", lcd.gram[0][0], 21);
    check("K latch cleared", lcd.write_latch, 0);

    /* 2B3P：DATSDR scan=0,0,1；CASET 0..0；LASET 1..1；RAMWR。 */
    np560_lcd_command(&lcd, 0xbc);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 1);
    np560_lcd_command(&lcd, 0x15);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_command(&lcd, 0x75);
    np560_lcd_data_write(&lcd, 1);
    np560_lcd_data_write(&lcd, 1);
    np560_lcd_command(&lcd, 0x5c);
    np560_lcd_data_write(&lcd, 0xf2);
    np560_lcd_data_write(&lcd, 0x3c);
    /* P0=0xf2>>3=30, P1=((0xf2&7)<<2)|(0x3c>>6)=8, P2=0x3c&0x1f=28。 */
    check("K 2B3P P0", lcd.gram[1][0], 30);
    check("K 2B3P P1", lcd.gram[1][1], 8);
    check("K 2B3P P2", lcd.gram[1][2], 28);
    check("K 2B3P wrap frames", (unsigned)(lcd.completed_frames & 0xff), 2);

    /* RAMRD 读锁存：首读哑读返回锁存旧值；读满一组回绕。 */
    np560_lcd_command(&lcd, 0x15);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_command(&lcd, 0x75);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_data_write(&lcd, 0);
    np560_lcd_command(&lcd, 0x5d);
    check("K RAMRD dummy", np560_lcd_data_read(&lcd), 0);
    check("K RAMRD v1", np560_lcd_data_read(&lcd), 174);  /* (21<<3)|(24>>2) */
    check("K RAMRD v2", np560_lcd_data_read(&lcd), 30);   /* ((24&3)<<6)|30 */
    check("K RAMRD wrap frames", (unsigned)(lcd.completed_frames & 0xff), 3);

    /* RMWIN：读不回绕；写提交并清读相位；RMWOUT 清模式。 */
    np560_lcd_command(&lcd, 0xe0);
    check("K RMW status", (np560_lcd_status_read(&lcd) & 0x20) != 0, 1);
    np560_lcd_data_read(&lcd);
    np560_lcd_data_read(&lcd);
    np560_lcd_data_read(&lcd);
    check("K RMW read no wrap", (unsigned)(lcd.completed_frames & 0xff), 3);
    np560_lcd_data_write(&lcd, 0x1f);
    np560_lcd_data_write(&lcd, 0xff);
    np560_lcd_data_write(&lcd, 0xff);
    check("K RMW write P0", lcd.gram[0][0], 3);
    check("K RMW write P1", lcd.gram[0][1], 31);
    check("K RMW write P2", lcd.gram[0][2], 31);
    np560_lcd_command(&lcd, 0xee);
    check("K RMWOUT", (np560_lcd_status_read(&lcd) & 0x20), 0);

    /* EXT=1 bank：Gray1 Set 16 参数；普通 RAM 命令不执行。 */
    np560_lcd_command(&lcd, 0x31);
    np560_lcd_command(&lcd, 0x20);
    {
        unsigned i;

        for (i = 0; i < 16; i++) {
            np560_lcd_data_write(&lcd, (uint8_t)(i + 1));
        }
    }
    check("K EXT gray bank", lcd.gray_pwm_programmed[0], 1);
    check("K EXT gray PB5", lcd.gray_pwm[0][4], 5);
    np560_lcd_command(&lcd, 0x5c);
    check("K EXT no RAM write", lcd.ram_write, 0);
    np560_lcd_command(&lcd, 0x30);
    check("K EXT out", lcd.extended, 0);

    /* COMSCN=001（V3.80）+ 模块双 bank 接线：可见行 0 读 GRAM 139。 */
    np560_lcd_command(&lcd, 0xbb);
    np560_lcd_data_write(&lcd, 1);
    lcd.gram[139][0] = 0;
    lcd.gram[139][1] = 31;
    check("K visible r0c0 dark", np560_lcd_visible_level(&lcd, 0, 0), 31);
    check("K visible r0c1 light", np560_lcd_visible_level(&lcd, 1, 0), 0);
    np560_lcd_command(&lcd, 0xae);
    check("K DISOFF blanks", np560_lcd_visible_level(&lcd, 0, 0), 0);
    np560_lcd_command(&lcd, 0xaf);
    check("K DISON restores", np560_lcd_visible_level(&lcd, 0, 0), 31);

    /* 渲染方向/配色：精确复刻 QEMU Web app.js drawFrame() 的绿灰插值。
     * 浓度 31=满驱动 → on=#243126 → 0x2184；浓度 0=无驱动 → off=#b7c8a1
     * → 0xb654（即皮肤洞内熄灭色）。防止误改回纯灰阶白/黑导致色差。 */
    {
        uint16_t px[4]; /* stride=2，只需 (0,0)/(1,0) 两列。 */

        np560_lcd_render(&lcd, px, 2);
        check("K render dark pixel on", px[0], 0x2184);
        check("K render light pixel off", px[1], 0xb654);
    }

    /* 状态电极导出：对齐 QEMU np560_get_lcd_annunciators。
     * plane0=GRAM line19、plane1=line140；level>0 才置位；bit 按 x 小端序；
     * 显示关/睡眠全零。用独立 LCD 避免前面的窗口/扫向设置干扰。 */
    {
        Np560Lcd ann;
        uint8_t planes[NP560_LCD_ANNUNCIATOR_BYTES];

        np560_lcd_reset(&ann);
        np560_lcd_annunciators(&ann, planes);
        check("K ann sleeping zero", planes[0] + planes[30], 0);

        ann.display_on = true;
        ann.sleeping = false;
        ann.gram[19][0] = 31;    /* plane0 bit0 */
        ann.gram[19][8] = 5;     /* plane0 byte1 bit0（level>0 即置位） */
        ann.gram[19][16] = 0;    /* plane0 byte2 bit0 保持清零 */
        ann.gram[140][133] = 31; /* plane1 bit133 → byte46 bit5 */
        ann.gram[140][228] = 31; /* plane1 bit228 → byte58 bit4 */
        np560_lcd_annunciators(&ann, planes);
        check("K ann plane0 bit0", (planes[0] & 1u) != 0, 1);
        check("K ann plane0 byte1", (planes[1] & 1u) != 0, 1);
        check("K ann plane0 zero level", (planes[2] & 1u) != 0, 0);
        check("K ann plane1 bit133",
              (planes[30 + (133 >> 3)] & (1u << (133 & 7))) != 0, 1);
        check("K ann plane1 bit228",
              (planes[30 + (228 >> 3)] & (1u << (228 & 7))) != 0, 1);
        check("K ann plane1 no spill", planes[0], 0x01); /* line19 仅 bit8+bit0 */

        ann.display_on = false;
        np560_lcd_annunciators(&ann, planes);
        check("K ann display off zero", planes[0] + planes[30 + 16], 0);
    }

    /* 总线路由：DS0=0x1E0（480）→ 物理 0xF00000；字节车道 ^1 后
     * 偶平坦字节=命令/状态口（offset&2==0），奇=数据口。 */
    bus = np560_bus_create();
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_periph_init(&periph, &cpu, bus);
    cpu.sfr[DMCU_SFR_DS0] = 0x1e0;
    dmcu_translated_data_write(bus, &cpu, 0x0000, 0x94, 8);
    check("K bus cmd SLPOUT", periph.lcd.command, 0x94);
    check("K bus sleeping", periph.lcd.sleeping, 0);
    /* 16 位访问被拒绝（QEMU valid.min=max=1）。 */
    dmcu_translated_data_write(bus, &cpu, 0x0000, 0x5c5c, 16);
    check("K bus 16bit ignored", periph.lcd.command, 0x94);
    /* 数据口：逻辑 2 → 物理 0xF00002 → 平坦 0xF00003（offset&2!=0）。 */
    dmcu_translated_data_write(bus, &cpu, 0x0002, 0xab, 8);
    check("K bus data count", (unsigned)(periph.lcd.data_writes & 0xff), 1);
    /* 状态口读：逻辑 0 → 平坦 0xF00001。 */
    check("K bus status read",
          dmcu_translated_data_read(bus, &cpu, 0x0000, 8) & 0x08, 0);
    np560_bus_destroy(bus);
}

/* ---- L: 键盘矩阵 F160/F340（np560.c:8983-9288） ---- */

static void test_l(void)
{
    Np560Keypad kp;
    Np560Bus *bus;
    DMCUCpu cpu;
    Np560Periph periph;

    printf("[L] 键盘矩阵 F160/F340\n");
    np560_keypad_reset(&kp);
    check("L idle drive", kp.matrix_drive, 0xffff);
    /* 无键 + 空闲驱动：0x00ff（bit12 外电断开）。 */
    check("L idle input",
          np560_keypad_matrix_input(&kp, kp.matrix_drive), 0x00ff);

    /* 按下 E（index 0 = col0,row0）：无行选通不导通。 */
    np560_keypad_set_key(&kp, 0, true);
    check("L idle no contact", np560_keypad_matrix_input(&kp, 0xffff),
          0x00ff);
    /* 扫描 row0（drive = 0xffff & ~row0）。 */
    kp.matrix_drive =
        (uint16_t)(0xffffu & ~np560_keypad_row_mask(0));
    check("L row0 E", np560_keypad_matrix_input(&kp, kp.matrix_drive),
          0x00fe);
    /* R（index 8 = col1,row0）同按：同列合并（无源矩阵）。 */
    np560_keypad_set_key(&kp, 8, true);
    check("L row0 E+R", np560_keypad_matrix_input(&kp, kp.matrix_drive),
          0x00fc);
    /* 扫描 row1：E/R 都不在 row1。 */
    kp.matrix_drive =
        (uint16_t)(0xffffu & ~np560_keypad_row_mask(1));
    check("L row1 empty", np560_keypad_matrix_input(&kp, kp.matrix_drive),
          0x00ff);

    /* F340 事件锁存：别名表 + 读清 + 写 0 清除。 */
    np560_keypad_set_event_code(&kp, 0x100); /* up */
    check("L F340 up event",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE), 0x82);
    check("L F340 read-clear",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE), 0);
    np560_keypad_set_event_code(&kp, 0x104); /* enter */
    check("L F340 raw code",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE + 6), 0x0104);
    check("L F340 enter",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE), 0x85);
    np560_keypad_set_event_code(&kp, 0x107); /* tab */
    np560_keypad_mmio_write(&kp, NP560_KEYBOARD_BASE, 0);
    check("L F340 write-clear",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE), 0);
    /* Escape 是返回键 M04(0x8E)，不与 Backspace/M28(0x84) 共用事件码。
     * 回归 QEMU commit 6fcc208992 / PR #2（词典返回失效）。 */
    np560_keypad_set_event_code(&kp, 0x105); /* escape */
    check("L F340 escape event",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE), 0x8e);

    /* legacy 位 4 = Enter → 矩阵 53（col6,row5）。 */
    np560_keypad_set_legacy_key(&kp, 4, true);
    check("L legacy state low",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE + 2), 0x0010);
    check("L legacy state high",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE + 4), 0);
    kp.matrix_drive =
        (uint16_t)(0xffffu & ~np560_keypad_row_mask(5));
    check("L legacy enter matrix",
          np560_keypad_matrix_input(&kp, kp.matrix_drive),
          0x00ff & ~(1u << 6));
    np560_keypad_set_legacy_key(&kp, 4, false);
    /* legacy 位 15 = StartMenu → 矩阵 55（col6,row7）。位 15 在低字。 */
    np560_keypad_set_legacy_key(&kp, 15, true);
    check("L legacy startmenu low",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE + 2), 0x8000);
    kp.matrix_drive =
        (uint16_t)(0xffffu & ~np560_keypad_row_mask(7));
    check("L legacy startmenu",
          np560_keypad_matrix_input(&kp, kp.matrix_drive),
          0x00ff & ~(1u << 6));
    np560_keypad_set_legacy_key(&kp, 15, false);
    /* legacy 位 5 = Escape → 返回触点 M04（col0,row4），不再指向 28。 */
    np560_keypad_set_legacy_key(&kp, 5, true);
    check("L legacy escape state",
          np560_keypad_mmio_read(&kp, NP560_KEYBOARD_BASE + 2), 0x0020);
    kp.matrix_drive =
        (uint16_t)(0xffffu & ~np560_keypad_row_mask(4));
    check("L legacy escape matrix",
          np560_keypad_matrix_input(&kp, kp.matrix_drive),
          0x00ff & ~(1u << 0));
    np560_keypad_set_legacy_key(&kp, 5, false);

    /* F160 bit12 = 外部电源/充电输入。 */
    kp.external_power = true;
    check("L ext power", np560_keypad_matrix_input(&kp, 0xffff), 0x10ff);
    kp.external_power = false;

    /* 总线 word 路由：0xF160 的逻辑地址 bit15 置位 → 走 DS1 段。
     * DS1=1 → 物理 = (1<<15)|0x7160 = 0xF160。 */
    bus = np560_bus_create();
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_periph_init(&periph, &cpu, bus);
    cpu.sfr[DMCU_SFR_DS1] = 1;
    check("L bus idle input",
          dmcu_translated_data_read(bus, &cpu, 0xf160, 16), 0x00ff);
    /* guest 写 F164 选 row0，回读一致。 */
    dmcu_translated_data_write(
        bus, &cpu, 0xf164, 0xffffu & ~np560_keypad_row_mask(0), 16);
    check("L bus drive readback",
          dmcu_translated_data_read(bus, &cpu, 0xf164, 16),
          0xffffu & ~np560_keypad_row_mask(0));
    /* 注入 E 后 guest 扫描应见 col0 导通。 */
    np560_keypad_set_key(&periph.keypad, 0, true);
    check("L bus E contact",
          dmcu_translated_data_read(bus, &cpu, 0xf160, 16), 0x00fe);
    np560_bus_destroy(bus);
}

/* ---- M: NAND 控制器 F300 + ECC 引擎 F20A（np560.c:8114-8931/7630-7712） ---- */

static void test_m(void)
{
    static uint8_t nand_data[256 * NP560_NAND_PAGE_DATA];
    static uint8_t nand_oob[256 * NP560_NAND_PAGE_OOB];
    Np560NandRamMedium rm;
    Np560Nand *nd;
    Np560Ecc *ec;
    Np560Bus *bus;
    DMCUCpu cpu;
    Np560Periph periph;
    uint32_t i;

    printf("[M] NAND 控制器 + ECC 引擎\n");
    bus = np560_bus_create();
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_periph_init(&periph, &cpu, bus);
    memset(nand_data, 0xff, sizeof(nand_data));
    memset(nand_oob, 0xff, sizeof(nand_oob));
    np560_nand_ram_medium_init(&rm, nand_data, nand_oob, 256);
    np560_nand_ram_medium_attach(&periph.nand, &rm);
    nd = &periph.nand;
    ec = &periph.ecc;

    /* 复位态。 */
    check("M reset status", np560_nand_mmio_read(nd, NP560_NAND_REG_STATUS), 0);
    check("M reset error", np560_nand_mmio_read(nd, NP560_NAND_REG_ERROR), 0);

    /* 种入页 2 数据 0x10..0x17 与字节 0x20。 */
    for (i = 0; i < 8; i++) {
        nand_data[2 * NP560_NAND_PAGE_DATA + 0x10 + i] = (uint8_t)(0xa0 + i);
    }
    nand_data[2 * NP560_NAND_PAGE_DATA + 0x20] = 0x5c;

    /* READ_ID：状态 1 + 器件 ID/几何。 */
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, NP560_NAND_CMD_READ_ID);
    check("M read_id status", np560_nand_mmio_read(nd, NP560_NAND_REG_STATUS), 1);
    check("M id", np560_nand_mmio_read(nd, NP560_NAND_REG_ID), 0xdcec);
    check("M id ext", np560_nand_mmio_read(nd, NP560_NAND_REG_ID_EXT), 0x1095);
    check("M page size",
          np560_nand_mmio_read(nd, NP560_NAND_REG_PAGE_SIZE), 2112);
    check("M pages/block",
          np560_nand_mmio_read(nd, NP560_NAND_REG_PAGES_PER_BLOCK), 64);
    check("M blocks", np560_nand_mmio_read(nd, NP560_NAND_REG_BLOCKS), 4096);

    /* READ_PAGE：页 2 列 0x10 长 8 → X RAM slot 0x3000。 */
    np560_nand_mmio_write(nd, NP560_NAND_REG_PAGE_LO, 2);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COLUMN, 0x10);
    np560_nand_mmio_write(nd, NP560_NAND_REG_LENGTH, 8);
    np560_nand_mmio_write(nd, NP560_NAND_REG_BUFFER_LO, 0x3000);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, NP560_NAND_CMD_READ_PAGE);
    check("M read status", np560_nand_mmio_read(nd, NP560_NAND_REG_STATUS), 1);
    for (i = 0; i < 8; i++) {
        if (bus->internal_data[0x3000 + i] != (uint8_t)(0xa0 + i)) {
            break;
        }
    }
    check("M dma out", i, 8);
    check("M page reads", nd->page_reads, 1);

    /* 字节车道：length==1 时列/缓冲 ^1（A0 标量行为）。 */
    np560_nand_mmio_write(nd, NP560_NAND_REG_COLUMN, 0x21);
    np560_nand_mmio_write(nd, NP560_NAND_REG_LENGTH, 1);
    np560_nand_mmio_write(nd, NP560_NAND_REG_BUFFER_LO, 0x3002);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, NP560_NAND_CMD_READ_PAGE);
    check("M lane status", np560_nand_mmio_read(nd, NP560_NAND_REG_STATUS), 1);
    check("M lane byte", bus->internal_data[0x3003], 0x5c);

    /* PROGRAM_PAGE：X RAM 0x3010..0x3017 → 页 2 列 0x40。 */
    for (i = 0; i < 8; i++) {
        bus->internal_data[0x3010 + i] = (uint8_t)(0x11 + i);
    }
    np560_nand_mmio_write(nd, NP560_NAND_REG_COLUMN, 0x40);
    np560_nand_mmio_write(nd, NP560_NAND_REG_LENGTH, 8);
    np560_nand_mmio_write(nd, NP560_NAND_REG_BUFFER_LO, 0x3010);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND,
                          NP560_NAND_CMD_PROGRAM_PAGE);
    check("M program status",
          np560_nand_mmio_read(nd, NP560_NAND_REG_STATUS), 1);
    check("M program data", nand_data[2 * NP560_NAND_PAGE_DATA + 0x40], 0x11);
    check("M program last",
          nand_data[2 * NP560_NAND_PAGE_DATA + 0x47], 0x18);

    /* AND 编程语义：length=2（无车道修正）编 0xF0,0x0F →
     * 0x11&0xF0=0x10、0x12&0x0F=0x02。 */
    bus->internal_data[0x3010] = 0xf0;
    bus->internal_data[0x3011] = 0x0f;
    np560_nand_mmio_write(nd, NP560_NAND_REG_LENGTH, 2);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND,
                          NP560_NAND_CMD_PROGRAM_PAGE);
    check("M and semantics",
          nand_data[2 * NP560_NAND_PAGE_DATA + 0x40], 0x10);
    check("M and semantics 2",
          nand_data[2 * NP560_NAND_PAGE_DATA + 0x41], 0x02);

    /* OOB 跨页流：列 2048 长 4 → 页 2 OOB 0..3，读回验证。 */
    bus->internal_data[0x3020] = 0x12;
    bus->internal_data[0x3021] = 0x34;
    bus->internal_data[0x3022] = 0x56;
    bus->internal_data[0x3023] = 0x78;
    np560_nand_mmio_write(nd, NP560_NAND_REG_COLUMN, 0x800);
    np560_nand_mmio_write(nd, NP560_NAND_REG_LENGTH, 4);
    np560_nand_mmio_write(nd, NP560_NAND_REG_BUFFER_LO, 0x3020);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND,
                          NP560_NAND_CMD_PROGRAM_PAGE);
    check("M oob program",
          nand_oob[2 * NP560_NAND_PAGE_OOB + 0], 0x12);
    check("M oob program 3",
          nand_oob[2 * NP560_NAND_PAGE_OOB + 3], 0x78);
    np560_nand_mmio_write(nd, NP560_NAND_REG_BUFFER_LO, 0x3030);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, NP560_NAND_CMD_READ_PAGE);
    check("M oob read back", bus->internal_data[0x3030], 0x12);
    check("M oob read back 2", bus->internal_data[0x3033], 0x78);

    /* ERASE_BLOCK：块 2（页 128..191）。 */
    np560_nand_mmio_write(nd, NP560_NAND_REG_PAGE_LO, 128);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND,
                          NP560_NAND_CMD_ERASE_BLOCK);
    check("M erase status",
          np560_nand_mmio_read(nd, NP560_NAND_REG_STATUS), 1);
    check("M erase data", nand_data[130 * NP560_NAND_PAGE_DATA + 5], 0xff);
    check("M erase oob", nand_oob[130 * NP560_NAND_PAGE_OOB + 5], 0xff);

    /* 错误路径：越界页 / 越界列 / 坏命令。 */
    np560_nand_mmio_write(nd, NP560_NAND_REG_PAGE_HI, 4);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, NP560_NAND_CMD_READ_PAGE);
    check("M page error",
          np560_nand_mmio_read(nd, NP560_NAND_REG_ERROR), NP560_NAND_ERROR_PAGE);
    np560_nand_mmio_write(nd, NP560_NAND_REG_PAGE_HI, 0);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COLUMN, 2000);
    np560_nand_mmio_write(nd, NP560_NAND_REG_LENGTH, 113);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, NP560_NAND_CMD_READ_PAGE);
    check("M range error",
          np560_nand_mmio_read(nd, NP560_NAND_REG_ERROR),
          NP560_NAND_ERROR_RANGE);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COLUMN, 0);
    np560_nand_mmio_write(nd, NP560_NAND_REG_LENGTH, 8);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, 9);
    check("M bad command",
          np560_nand_mmio_read(nd, NP560_NAND_REG_ERROR),
          NP560_NAND_ERROR_COMMAND);

    /* COPY_BACK：页 2 → 目的页 3（buffer 即目的页号）。 */
    np560_nand_mmio_write(nd, NP560_NAND_REG_PAGE_LO, 2);
    np560_nand_mmio_write(nd, NP560_NAND_REG_BUFFER_LO, 3);
    np560_nand_mmio_write(nd, NP560_NAND_REG_COMMAND, NP560_NAND_CMD_COPY_BACK);
    check("M copy status",
          np560_nand_mmio_read(nd, NP560_NAND_REG_STATUS), 1);
    check("M copy data", nand_data[3 * NP560_NAND_PAGE_DATA + 0x40], 0x10);
    check("M copy oob", nand_oob[3 * NP560_NAND_PAGE_OOB + 1], 0x34);
    check("M page copies", nd->page_copies, 1);

    /* ECC：复位 / 喂字 / 配对奇偶 / 写 F20A 清零。 */
    check("M ecc idle parity",
          np560_ecc_mmio_read(ec, 0x00), 0);
    np560_ecc_mmio_write(ec, 0x00, 0);
    np560_ecc_mmio_write(ec, 0x04, 0x0001); /* word0 bit0 → pos 0 */
    np560_ecc_mmio_write(ec, 0x04, 0x0002); /* word1 bit1 → pos 0x11 */
    check("M ecc parity", np560_ecc_mmio_read(ec, 0x00), 0x0011);
    check("M ecc nparity", np560_ecc_mmio_read(ec, 0x02), 0x0011);
    np560_ecc_mmio_write(ec, 0x00, 0);
    check("M ecc cleared", np560_ecc_mmio_read(ec, 0x00), 0);

    /* 双视图：banked F20A 与 unbanked 0x01000A0A 同一寄存器。 */
    np560_ecc_mmio_write(ec, 0x04, 0x0004); /* word0 bit2 → pos 2 */
    check("M ecc banked view", dmcu_phys_read16(bus, 0xf20a), 0x0002);
    check("M ecc unbanked view",
          dmcu_phys_read16(bus, 0x01000a0a), 0x0002);
    dmcu_phys_write16(bus, 0x01000a0e, 0x0008); /* word1 bit3 → pos 0x13 */
    check("M ecc feed unbanked",
          dmcu_phys_read16(bus, 0xf20a), 0x0002 ^ 0x0013);

    /* 总线 word 路由：物理 F316 → NAND ID；F100 → SOCR 视图。 */
    check("M bus nand id", dmcu_phys_read16(bus, 0xf316), 0xdcec);
    cpu.sfr[DMCU_SFR_DS1] = 1;
    /* 逻辑 0xF316（bit15 选 DS1）→ 物理 (1<<15)|0x7316 = 0xF316。 */
    check("M bus translated id",
          dmcu_translated_data_read(bus, &cpu, 0xf316, 16), 0xdcec);
    cpu.sfr[DMCU_SFR_SOCR] = 0x1234;
    check("M socr view", dmcu_phys_read16(bus, 0xf100), 0x1234);
    dmcu_phys_write16(bus, 0xf100, 0x4210);
    check("M socr write", cpu.sfr[DMCU_SFR_SOCR], 0x4210);

    np560_bus_destroy(bus);
}

/* ---- N: NAND 文件介质 + code 平面（vma03 装载语义） ---- */

#include <unistd.h>
#include <stdlib.h>

static void test_n(void)
{
    Np560Bus *bus;
    DMCUCpu cpu;
    Np560Nand *nd;
    Np560NandFileMedium fm;
    const char *path = "/tmp/np560_nand_test.bin";

    printf("[N] NAND 文件介质 / code 平面\n");

    /* 稀疏文件：写 64B N1 容器头（np560_nand_file_medium_open 校验
     * magic + ver=1），再 truncate 到整片镜像大小，不占实际磁盘。 */
    FILE *f = fopen(path, "wb");
    check("N create", f != NULL, 1);
    if (f) {
        static const uint8_t magic[8] = { 'N', 'P', '5', '6', '0', 'N', '1', 0 };
        uint8_t hdr[64];

        memset(hdr, 0, sizeof(hdr));
        memcpy(hdr, magic, sizeof(magic));
        hdr[8] = 1;  /* ver */
        fwrite(hdr, 1, sizeof(hdr), f);
        fseek(f, (long)(sizeof(hdr) + (uint64_t)NP560_NAND_PAGES *
                        NP560_NAND_PAGE_TOTAL) - 1, SEEK_SET);
        fputc(0, f);
        fclose(f);
    }

    bus = np560_bus_create();
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0xfffe;
    cpu.sfr[DMCU_SFR_DS1] = 1;

    Np560Nand nand;
    np560_nand_reset(&nand);
    nand.bus = bus;
    check("N medium open", np560_nand_file_medium_open(&fm, path), 1);
    np560_nand_file_medium_attach(&nand, &fm);

    /* 页 5 经控制器 ABI 往返：擦除块 0（sparse 镜像读 0 非 FF，必须先
     * 擦到擦除态）→ PROGRAM_PAGE 全页 0xab → READ_PAGE → AND 编程 0x0f
     * → 再擦除恢复 FF。DMA 缓冲在 X RAM 0x3000。 */
    {
        np560_nand_mmio_write(&nand, NP560_NAND_REG_PAGE_LO, 0);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COMMAND,
                              NP560_NAND_CMD_ERASE_BLOCK);
        check("N precondition erase", np560_nand_mmio_read(&nand,
              NP560_NAND_REG_STATUS), 1);
    }
    {
        uint32_t i;
        static uint8_t buf[NP560_NAND_PAGE_TOTAL];

        memset(buf, 0xab, sizeof(buf));
        for (i = 0; i < NP560_NAND_PAGE_TOTAL; i++) {
            bus->internal_data[0x3000 + i] = 0xab;
        }
        np560_nand_mmio_write(&nand, NP560_NAND_REG_PAGE_LO, 5);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COLUMN, 0);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_LENGTH,
                              NP560_NAND_PAGE_TOTAL);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_BUFFER_LO, 0x3000);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COMMAND,
                              NP560_NAND_CMD_PROGRAM_PAGE);
        check("N program status", np560_nand_mmio_read(&nand,
              NP560_NAND_REG_STATUS), 1);
        check("N page programs", nand.page_programs, 1);

        for (i = 0; i < 4; i++) {
            bus->internal_data[0x3000 + i] = 0;
        }
        np560_nand_mmio_write(&nand, NP560_NAND_REG_LENGTH, 4);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COMMAND,
                              NP560_NAND_CMD_READ_PAGE);
        check("N read status", np560_nand_mmio_read(&nand,
              NP560_NAND_REG_STATUS), 1);
        check("N read data", bus->internal_data[0x3000], 0xab);
        check("N read data3", bus->internal_data[0x3003], 0xab);

        /* AND 编程：0xab & 0x0f = 0x0a（1→0 单向）。 */
        for (i = 0; i < NP560_NAND_PAGE_TOTAL; i++) {
            bus->internal_data[0x3000 + i] = 0x0f;
        }
        np560_nand_mmio_write(&nand, NP560_NAND_REG_LENGTH,
                              NP560_NAND_PAGE_TOTAL);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COMMAND,
                              NP560_NAND_CMD_PROGRAM_PAGE);
        check("N program2 status", np560_nand_mmio_read(&nand,
              NP560_NAND_REG_STATUS), 1);
        for (i = 0; i < 4; i++) {
            bus->internal_data[0x3000 + i] = 0;
        }
        np560_nand_mmio_write(&nand, NP560_NAND_REG_LENGTH, 4);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COMMAND,
                              NP560_NAND_CMD_READ_PAGE);
        check("N read after and", bus->internal_data[0x3000], 0x0b);

        /* 块 0 擦除（ERASE_BLOCK，页寄存器=块首页 0）→ 页 5 恢复 FF。 */
        np560_nand_mmio_write(&nand, NP560_NAND_REG_PAGE_LO, 0);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COMMAND,
                              NP560_NAND_CMD_ERASE_BLOCK);
        check("N erase status", np560_nand_mmio_read(&nand,
              NP560_NAND_REG_STATUS), 1);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_PAGE_LO, 5);
        np560_nand_mmio_write(&nand, NP560_NAND_REG_COMMAND,
                              NP560_NAND_CMD_READ_PAGE);
        check("N read after erase", bus->internal_data[0x3000], 0xff);
        np560_nand_file_medium_close(&fm);
    }

    /* code 平面：装载 resident.bin 模拟 ROM（构造 0x8000 处 0x1234、
     * 0x45000 处 0x5678），PRG 装到 0x30000。 */
    {
        uint8_t *rom = calloc(1, 0x00200000u);
        check("N rom alloc", rom != NULL, 1);
        if (rom) {
            rom[0x8000] = 0x34; rom[0x8001] = 0x12;
            rom[0x45000] = 0x78; rom[0x45001] = 0x56;
            check("N load_code_rom", np560_bus_load_code_rom(bus, rom,
                                                             0x00200000u), 1);
            free(rom);
        }
        uint16_t prg_word[4] = {0x1111, 0x2222, 0x3333, 0x4444};
        np560_bus_load(bus, 0x30000u, (const uint8_t *)prg_word, 8);
        np560_bus_load(bus, 0x42ffeu, (const uint8_t *)&prg_word[3], 2);

        /* X RAM alias：code 0x10 读 internal_data[0x10]。 */
        bus->internal_data[0x10] = 0x99;
        bus->internal_data[0x11] = 0x07;
        check("N code xram", dmcu_code_read16(bus, 0x0010), 0x0799);
        /* ROM 区间：code 0x8000 = 0x1234（phys 该处为 0 → 回落 ROM）。 */
        check("N code rom", dmcu_code_read16(bus, 0x8000), 0x1234);
        /* overlay 优先：phys 同地址非 0 时取 phys（loader veneer）。 */
        bus->phys[0x8000] = 0x21; bus->phys[0x8001] = 0x43;
        check("N code overlay", dmcu_code_read16(bus, 0x8000), 0x4321);
        /* PRG 区间：0x30000..0x42FFFF 取 phys（含 0x200000）。 */
        check("N code prg", dmcu_code_read16(bus, 0x30000), 0x1111);
        check("N code prg mid", dmcu_code_read16(bus, 0x200000), 0x0);
        check("N code prg end", dmcu_code_read16(bus, 0x42ffe), 0x4444);
        /* PRG 结束后（ROM 2MB 已被 PRG 覆盖完）missing → 0x0f00。 */
        check("N code after prg", dmcu_code_read16(bus, 0x430008), 0x0f00);
        check("N code missing", dmcu_code_read16(bus, 0x600000), 0x0f00);
        /* ROM 区间内容为 0 的字原样返回 0（未 seed overlay 语义）。 */
        check("N code rom zero", dmcu_code_read16(bus, 0x1fffe), 0);
    }

    np560_bus_destroy(bus);
    /* unlink(path); */
}

/* ---- O: ADC 电量比较器 F2D0（QEMU np560.c:7715 np560_adc_value） ----
 * 低电误报根因回归：采样器 4F:C336 写 (threshold<<2)|2 到 F2D0、读回
 * bit7 作比较结果；level=8 时 threshold 0..7 全部断言 → 满电等级 8。
 * 同时验证 F2D2/F2D4 转发到 CPU/Audio PLL SFR。 */
static void test_o(void)
{
    Np560Bus *bus;
    DMCUCpu cpu;
    Np560Periph periph;
    unsigned threshold;

    printf("[O] ADC 电量比较器 F2D0\n");
    bus = np560_bus_create();
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_periph_init(&periph, &cpu, bus);
    cpu.sfr[DMCU_SFR_DS1] = 1;

    check("O adc default level", periph.adc_level, 8);
    /* 固件采样器逐 threshold 扫描：level=8 时每次读回 bit7=1。 */
    for (threshold = 0; threshold < 8; threshold++) {
        uint16_t written = (threshold << 2) | 2;

        dmcu_translated_data_write(bus, &cpu, 0xf2d0, written, 16);
        check("O adc bit7",
              dmcu_translated_data_read(bus, &cpu, 0xf2d0, 16) & 0x0080u,
              0x0080u);
    }
    check("O adc control masked", periph.adc_control & 0x0080u, 0);
    /* F2D2 -> CPU PLL (0xe9)，F2D4 -> Audio PLL (0xea)。 */
    dmcu_translated_data_write(bus, &cpu, 0xf2d2, 0x1234, 16);
    check("O adc cpu pll", cpu.sfr[DMCU_SFR_PLL], 0x1234);
    check("O adc cpu pll read",
          dmcu_translated_data_read(bus, &cpu, 0xf2d2, 16), 0x1234);
    dmcu_translated_data_write(bus, &cpu, 0xf2d4, 0x5678, 16);
    check("O adc audio pll", cpu.sfr[DMCU_SFR_AUDIO_PLL], 0x5678);
    /* 键盘唤醒：预置去抖（路线②）后补 tick 减少，重复请求重置。 */
    periph.key_wake_ticks = 3;
    np560_periph_request_key_wake(&periph);
    check("O key wake ticks", periph.key_wake_ticks, 6);
    check("O key wake request", periph.key_debounce_request, 1);
    np560_periph_request_key_wake(&periph);
    check("O key wake no accumulate", periph.key_wake_ticks, 6);
    /* 释放也要补 tick 重新武装去抖门（否则下一次短按无响应）。 */
    periph.key_wake_ticks = 20;
    np560_periph_request_key_release(&periph);
    check("O key release ticks", periph.key_wake_ticks, 4);
    check("O key release request", periph.key_debounce_request, 2);

    /* 路线②：应用预置去抖应直写 0490=4、2A8C(字节)>=2，且不碰 2A94。
     * 内部 X RAM 0490 = 字；外部 RAM 2A8C 物理 0x512A8C 按 ^1 高字节车道
     * 落在 phys[0x512A8D]；2A94 保持 idle 原值 1（armed）。 */
    {
        uint16_t w0490;
        bus->phys[0x512a8cu] = 0;
        bus->phys[0x512a8du] = 0;
        bus->internal_data[0x490] = 0;
        bus->internal_data[0x491] = 0;
        bus->phys[0x512a94u] = 1;   /* idle armed */
        np560_periph_apply_fast_debounce(&periph, bus);
        w0490 = (uint16_t)(bus->internal_data[0x490] |
                           (bus->internal_data[0x491] << 8));
        check("O fast 0490=4", w0490, 4);
        check("O fast 2A8C byte>=2", bus->phys[0x512a8du] >= 2, 1);
        check("O fast 2A94 armed kept", bus->phys[0x512a94u], 1);
        check("O fast request cleared", periph.key_debounce_request, 0);
        check("O fast stat", periph.stat_debounce_fast, 1);
        /* 无请求时应用应为空操作。 */
        check("O fast noop", np560_periph_apply_fast_debounce(&periph, bus), 0);
    }

    np560_bus_destroy(bus);
}

/* MP3 ROM 门：TRAP#0 必须被 per-PC HLE 拦截，而不是跳进 01:0000 自陷。 */
static void test_p(void)
{
    Np560Bus *bus;
    DMCUCpu cpu;
    NP560Mp3HleTab5 *hle;
    uint8_t *rom;
    unsigned i, service;
    const uint16_t marker[8] = {0x0f00, 0xc620, 0x504d, 0x484c,
                                6, 0, 0, 0};

    printf("[P] MP3 ROM gate per-PC HLE\n");
    bus = np560_bus_create();
    rom = (uint8_t *)calloc(0x30000, 1);
    for (service = 0; service < 6; service++) {
        for (i = 0; i < 8; i++) {
            uint16_t word = marker[i];
            uint32_t at = 0x21e26u + 16u * service + 2u * i;
            if (i == 5) {
                word = (uint16_t)service;
            }
            rom[at] = (uint8_t)word;
            rom[at + 1] = (uint8_t)(word >> 8);
        }
    }
    np560_bus_take_code_rom(bus, rom, 0x30000);

    hle = np560_mp3_hle_tab5_create(bus, 0x21e26u);
    check("P gate marker accepted", hle != NULL, 1);
    bus->rom_service = np560_mp3_hle_tab5_service;
    bus->rom_service_user = hle;

    memset(&cpu, 0, sizeof(cpu));
    cpu.pc = 0x21e26u;
    cpu.sfr[DMCU_SFR_DS0] = 0xa2u;      /* workspace = 0x510000 (0xa2<<15) */
    cpu.gpr[3] = 0;
    dmcu_exec_step(bus, &cpu);
    /* TRAP 被服务：顺序后继是门内的 RETS-far，没有进入 01:0000 向量。 */
    check("P gate skips TRAP", cpu.pc, 0x21e28u);
    check("P gate serviced once", (uint32_t)np560_mp3_hle_tab5_call_count(hle), 1);
    check("P service 0 status", cpu.gpr[2], 0);
    check("P service 0 magic",
          (uint32_t)dmcu_phys_read16(bus, 0x510000u + 20u), 0x4d33u);

    np560_mp3_hle_tab5_destroy(hle);
    np560_bus_destroy(bus);
}

int main(void)
{
    test_a();
    test_b();
    test_c();
    test_d();
    test_e();
    test_f();
    test_g();
    test_h();
    test_i();
    test_j();
    test_k();
    test_l();
    test_m();
    test_n();
    test_o();
    test_p();

    if (failures) {
        printf("FAILED: %d checks\n", failures);
        return 1;
    }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
