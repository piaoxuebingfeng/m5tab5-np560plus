/*
 * host/bench.c - DMCU 解释器吞吐基准（IPS）。
 *
 * 目的：量化 exec_step 纯执行吞吐，用于验证取指/分发优化收益。
 * 采用与真机一致的取指路径：code_rom 非空 + 程序位于 PRG 区
 * 0x30000（dmcu_code_read16 的 0x30000..0x430000 分支 → phys_read16）。
 *
 * 用法：make bench && ./dmcu_bench [总指令数]
 * 输出：步数、耗时、IPS（每秒指令数）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dmcu_bus.h"
#include "dmcu_exec.h"

#define PRG_BASE 0x30000u
#define BENCH_STEPS_DEFAULT 20000000u

/* 代表性混合：MOV 立即数/寄存器、ALU、移位、内存读写、NOP。
 * 全部取自 host/runner.c 已验证语义的编码，避免基准引入未支持指令。 */
static const uint16_t bench_program[] = {
    0x4801,             /* MOV  R0,#1                */
    0x4822,             /* MOV  R2,#2                */
    0x1002,             /* ADD  R0,R2                */
    0x4843,             /* MOV  R4,#3                */
    0x0a42,             /* MOV  [R4],R2   -> X RAM   */
    0x2814,             /* MOV  R1,[R4]              */
    0x4c04,             /* SL   R0,#4                */
    0x4d14,             /* SR   R1,#4                */
    0xca24,             /* ROL  R2,#4                */
    0xc700,             /* NOP                       */
    0x4805,             /* MOV  R0,#5                */
    0x1012,             /* ADD  R0,R2                */
    0x0802,             /* MOV  R0,R2                */
    0x2904,             /* MOV  R2,[R4]              */
    0x0b42,             /* MOVB [R4],R2              */
    0xc700,             /* NOP                       */
};
#define BENCH_WORDS (sizeof(bench_program) / sizeof(bench_program[0]))
#define BENCH_BYTES (BENCH_WORDS * 2u)

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    uint64_t total = BENCH_STEPS_DEFAULT;
    uint64_t steps;
    uint32_t end = PRG_BASE + BENCH_BYTES;
    double t0, t1, sec;
    Np560Bus *bus = np560_bus_create();
    DMCUCpu cpu;

    if (!bus) {
        fprintf(stderr, "bus create failed\n");
        return 1;
    }
    if (argc > 1) {
        unsigned long v = strtoul(argv[1], NULL, 0);

        if (v) {
            total = v;
        }
    }
    /* 模拟真机：code_rom 非空（resident.bin 2MB），程序装 PRG 区。 */
    {
        uint8_t *rom = (uint8_t *)calloc(2 * 1024 * 1024, 1);

        if (!rom) {
            fprintf(stderr, "rom alloc failed\n");
            np560_bus_destroy(bus);
            return 1;
        }
        np560_bus_take_code_rom(bus, rom, 2 * 1024 * 1024);
    }
    memset(&cpu, 0, sizeof(cpu));
    cpu.gpr[DMCU_SP] = 0x1000;
    cpu.gpr[DMCU_FP] = 0x1000;
    np560_bus_load(bus, PRG_BASE, (const uint8_t *)bench_program, BENCH_BYTES);
    cpu.pc = PRG_BASE;

    /* 预热：排除首次 cache/分支预测冷启动。 */
    for (steps = 0; steps < 1000000; steps++) {
        if (cpu.pc >= end || cpu.pc < PRG_BASE) {
            cpu.pc = PRG_BASE;
        }
        dmcu_exec_step(bus, &cpu);
    }
    if (cpu.halted || cpu.unsupported_count) {
        fprintf(stderr, "bench program faulted: halted=%u unsup=%u\n",
                cpu.halted, cpu.unsupported_count);
        np560_bus_destroy(bus);
        return 1;
    }

    t0 = now_sec();
    for (steps = 0; steps < total; steps++) {
        if (cpu.pc >= end || cpu.pc < PRG_BASE) {
            cpu.pc = PRG_BASE;
        }
        dmcu_exec_step(bus, &cpu);
    }
    t1 = now_sec();
    sec = t1 - t0;
    if (cpu.halted || cpu.unsupported_count) {
        fprintf(stderr, "bench program faulted during run\n");
        np560_bus_destroy(bus);
        return 1;
    }
    printf("steps=%llu sec=%.6f IPS=%.0f unsup=%u\n",
           (unsigned long long)total, sec,
           sec > 0 ? (double)total / sec : 0.0,
           cpu.unsupported_count);
    np560_bus_destroy(bus);
    return 0;
}
