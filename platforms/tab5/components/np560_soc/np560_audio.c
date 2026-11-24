/*
 * np560_audio.c - NP560+ 双通道 DAC 寄存器输出模型（架构子集）。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560_audio.c。QEMU 侧呈现/诊断（快照、
 * 控制事件环、延迟统计）裁剪；保留：样本锁存、控制/PLL 写、请求
 * 时钟公式、ICR 源 17/16 锁存请求、PCM 呈现环。
 */
#include "np560_audio.h"
#include "dmcu_exec.h"
#include "dmcu_irq.h"
#include "dmcu_clock.h"
#include <string.h>

static bool audio_enabled(const Np560Audio *s)
{
    return (s->control & 0x8001u) == 1u;
}

/* np560_audio_request_config（np560_audio.c:86）：速率公式原样移植。 */
static void audio_request_config(Np560Audio *s)
{
    unsigned mode = s->control & 0xe;
    unsigned multiplier = ((s->pll & 0x1fffu) >> 5) + 1;
    unsigned divisor = (s->pll & 0x1fu) + 1;
    unsigned decimation = mode & 2 ? 1536u : 1024u;
    bool supported = (mode == 8 || mode == 10) &&
                     (s->pll & 0xc000u) == 0x8000u;
    uint32_t hz = supported ?
        4000000u * multiplier / (divisor * decimation) : 0;
    bool running = supported && audio_enabled(s);

    /* 不得用名义固件速率替代，也不得绑定 host 48kHz 呈现速率。 */
    if (running != s->request_running || hz != s->request_hz) {
        s->request_hz = hz;
        s->request_running = running;
        s->acc_num = 0;
    }
}

/* np560_audio_sync（np560_audio.c:138-144）：把保持值渲染进呈现环。
 * 目标帧数按 NP560_AUDIO_RATE 折算；落后时丢弃旧帧（不再呈现）。 */
static void audio_render_frames(Np560Audio *s, uint64_t frames)
{
    uint64_t first;
    uint16_t a = audio_enabled(s) ? s->sample[0] : 0;
    uint16_t b = audio_enabled(s) ? s->sample[1] : 0;
    uint64_t target = s->next_frame + frames;

    first = (target > NP560_AUDIO_FRAMES) ?
            (target - NP560_AUDIO_FRAMES) : 0;
    if (first < s->next_frame) {
        first = s->next_frame;
    }
    for (; first < target; first++) {
        size_t offset = (size_t)(first % NP560_AUDIO_FRAMES) * 4u;

        s->pcm[offset] = (uint8_t)(a & 0xffu);
        s->pcm[offset + 1] = (uint8_t)(a >> 8);
        s->pcm[offset + 2] = (uint8_t)(b & 0xffu);
        s->pcm[offset + 3] = (uint8_t)(b >> 8);
    }
    s->next_frame = target;
}

void np560_audio_reset(Np560Audio *audio, DMCUCpu *cpu)
{
    audio->cpu = cpu;
    audio->sample[0] = cpu->sfr[DMCU_SFR_DAC0] & MASK16;
    audio->sample[1] = cpu->sfr[DMCU_SFR_DAC1] & MASK16;
    audio->control = cpu->sfr[DMCU_SFR_DACC] & MASK16;
    audio->pll = cpu->sfr[DMCU_SFR_AUDIO_PLL] & MASK16;
    audio->request_hz = 0;
    audio->request_running = false;
    audio->acc_num = 0;
    audio->next_frame = 0;
    memset(audio->pcm, 0, sizeof(audio->pcm));
    audio_request_config(audio);
}

void np560_audio_init(Np560Audio *audio, DMCUCpu *cpu)
{
    memset(audio, 0, sizeof(*audio));
    np560_audio_reset(audio, cpu);
}

/* np560_audio_write（np560_audio.c:178）：单一设备边界。 */
void np560_audio_write(Np560Audio *audio, unsigned reg, uint16_t value)
{
    if (reg == DMCU_SFR_DAC0 || reg == DMCU_SFR_DAC1) {
        unsigned ch = reg - DMCU_SFR_DAC0;

        audio->sample[ch] = value;
        audio->sample_writes[ch]++;
    } else if (reg == DMCU_SFR_DACC) {
        audio->control = value;
        audio->control_writes++;
        audio_request_config(audio);
    } else if (reg == DMCU_SFR_AUDIO_PLL) {
        audio->pll = value;
        audio_request_config(audio);
    }
}

/* 请求 tick（np560_audio.c:40）：先渲染后锁存，双通道同属一个
 * 采样时钟事件；对每通道各置一次锁存 RQ。 */
static void audio_request_tick(Np560Audio *s)
{
    s->request_ticks++;
    /* REG61 -> DTU0/F1E0，REG60 -> DTU1/F1E2：源 17/16。 */
    dmcu_irq_raise(NULL, s->cpu, 17);
    dmcu_irq_raise(NULL, s->cpu, 16);
}

void np560_audio_advance(Np560Audio *audio, uint64_t delta_cpu_cycles)
{
    uint32_t cpu_hz = dmcu_system_clock_hz(audio->cpu);
    uint64_t frames;

    if (!delta_cpu_cycles) {
        return;
    }
    if (audio->request_running && audio->request_hz && cpu_hz) {
        audio->acc_num += delta_cpu_cycles * audio->request_hz;
        while (audio->acc_num >= cpu_hz) {
            audio->acc_num -= cpu_hz;
            audio_request_tick(audio);
        }
    }
    /* 请求间隙也持续渲染保持值（48kHz 呈现环，QEMU sync 语义）。 */
    if (cpu_hz) {
        frames = delta_cpu_cycles * NP560_AUDIO_RATE / cpu_hz;
        if (frames) {
            audio_render_frames(audio, frames);
        }
    }
}
