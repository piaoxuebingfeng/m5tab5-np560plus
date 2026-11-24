/*
 * np560_audio.h - NP560+ 双通道 DAC 寄存器输出模型。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560_audio.c（架构子集）。虚拟时间改为
 * CPU 周期累加器；PCM 呈现环保留（host 音频线程消费）。
 *
 * 语义要点（np560_audio.c 注释原样保留）：
 *  - 原厂 24:97AC/9A1E 将 REG61 路由到 DTU0/F1E0、REG60 到 DTU1/F1E2；
 *    请求节拍对每个通道置一次锁存 RQ（ICR 源 17/16），不拷贝缓冲、
 *    不调用 guest 回调。EN/IE 与完成归 ICR/DTU 管。
 *  - 请求速率公式覆盖原厂五个专用配置至 0.04% 以内：
 *    hz = 4MHz * mult / (div * decimation)，decimation = mode&2 ? 1536 : 1024，
 *    supported = (mode==8||mode==10) && (pll&0xc000)==0x8000。
 *  - enabled = (control & 0x8001) == 1（08:4702/475E 用 REG72 bit0/bit15
 *    启停）；禁用时 DAC 值保持，呈现为 0。
 */
#ifndef NP560_AUDIO_H
#define NP560_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "dmcu_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP560_AUDIO_RATE 48000
#define NP560_AUDIO_FRAMES 9600  /* 200ms 呈现环 */

typedef struct Np560Audio {
    DMCUCpu *cpu;
    uint16_t sample[2];      /* DAC0/DAC1 锁存值 */
    uint16_t control;        /* DACC */
    uint16_t pll;            /* AUDIO_PLL */
    uint64_t sample_writes[2];
    uint64_t control_writes;

    /* 请求时钟（CPU 周期累加器；对应 QEMU 虚拟 ns 定时器）。 */
    uint32_t request_hz;
    bool request_running;
    uint64_t request_ticks;
    uint64_t acc_num;        /* acc_num += delta * request_hz；>= cpu_hz 时 tick */

    /* PCM 呈现环（小端双声道 u16 交错；host I2S 线程消费）。 */
    uint64_t next_frame;
    uint8_t pcm[NP560_AUDIO_FRAMES * 4];
} Np560Audio;

void np560_audio_init(Np560Audio *audio, DMCUCpu *cpu);
void np560_audio_reset(Np560Audio *audio, DMCUCpu *cpu);
void np560_audio_write(Np560Audio *audio, unsigned reg, uint16_t value);

/* 时间推进：请求节拍到期时 raise ICR 源 17/16 并渲染保持值。 */
void np560_audio_advance(Np560Audio *audio, uint64_t delta_cpu_cycles);

/* 取当前双声道保持值（host 音频输出线程用）。 */
static inline void np560_audio_current_samples(const Np560Audio *audio,
                                               uint16_t *a, uint16_t *b)
{
    bool enabled = (audio->control & 0x8001u) == 1u;

    *a = enabled ? audio->sample[0] : 0;
    *b = enabled ? audio->sample[1] : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* NP560_AUDIO_H */
