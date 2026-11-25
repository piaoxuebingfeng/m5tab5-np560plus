/*
 * np560_mp3_hle_tab5.c - Tab5 port of QEMU's ROM 01/02 MP3 HLE service.
 *
 * Semantics are kept in lockstep with
 * qemu-dmcu/hw/dmcu/np560_mp3_hle.c (gate marker, workspace offsets, status
 * codes, validation order, priming and gain). QEMU-specific AddressSpace,
 * GLib and clock plumbing is replaced with the flat physical bus of
 * dmcu_core. version 6 (the marker in the shipped resident.bin) has six
 * gates: 0 open, 1 decode, 2 pull PCM, 3 close, 4 input space, 5 copy PCM.
 */
#include "np560_mp3_hle_tab5.h"
#include "np560_mp3_codec.h"

#include <stdlib.h>
#include <string.h>

/* Same workspace as QEMU np560_mp3_hle.c. */
enum {
    WORK_BYTES = 0x6300, PENDING = 8, PCM_VALID = 14, STATUS = 16,
    DETAIL = 18, MAGIC = 20, HDR0 = 0x48, HDR1 = 0x4a, LENGTH = 0x4c,
    BODY_BYTES = 0x50, CRC = 0x54, FRAME = 0x100, SCRATCH = 0x3300,
    PCM_LEFT = 0x5700, PCM_RIGHT = 0x5b80,
    RECLAIM_END = 0x60, RECLAIM_PENDING = 0x62, STAGED = 0x64,
    FINAL_INPUT = 0x66, RING_FORMAT_CHANGE = 0x58,
};

#define MP3_MAX_SESSIONS 8u

typedef struct Mp3Tab5Session {
    uint32_t workspace;
    bool used;
    NP560MP3Codec *codec;
    NP560MP3Frame frame;
    bool pending;
    unsigned next_pcm_offset, priming_remaining;
    uint64_t frames, granules;
    unsigned startup_history_frames, startup_history_available;
} Mp3Tab5Session;

struct NP560Mp3HleTab5 {
    Np560Bus *bus;
    uint32_t base;
    unsigned version, gate_count;
    Mp3Tab5Session sessions[MP3_MAX_SESSIONS];
    uint64_t calls[6];
    uint64_t granules;
    uint64_t failures;
    uint64_t input_space_failures, pcm_copy_failures;
    uint64_t total;
};

/* ---- physical bus helpers (QEMU read_word/write_word replacement) ---- */

static uint16_t rd(NP560Mp3HleTab5 *h, uint32_t address)
{
    return dmcu_phys_read16(h->bus, address);
}

static void wr(NP560Mp3HleTab5 *h, uint32_t address, uint16_t value)
{
    dmcu_phys_write16(h->bus, address, value);
}

/* QEMU writable_span(): the service only touches ordinary RAM. On Tab5 the
 * data plane is one flat 16MB allocation with two device windows. */
static bool writable_span(uint32_t address, uint32_t size)
{
    if (!size || address > DMCU_PHYS_SIZE - size) {
        return false;
    }
    /* SFR/timer MMIO 0xF100..0xF3FF and ST7529 byte window 0xF00000..3. */
    if (address < 0xf400u && address + size > 0xf100u) {
        return false;
    }
    if (address < 0xf00004u && address + size > 0xf00000u) {
        return false;
    }
    return true;
}

static bool gate_matches(NP560Mp3HleTab5 *h, uint32_t pc, unsigned service)
{
    uint16_t words[8];
    unsigned i;

    words[0] = 0x0f00;             /* TRAP #0 */
    words[1] = 0xc620;             /* RETS far */
    words[2] = 0x504d;             /* "PM" */
    words[3] = 0x484c;             /* "LH" */
    words[4] = (uint16_t)h->version;
    words[5] = (uint16_t)service;
    words[6] = words[7] = 0;
    for (i = 0; i < 8; i++) {
        if (dmcu_code_read16(h->bus, pc + 2u * i) != words[i]) {
            return false;
        }
    }
    return true;
}

static Mp3Tab5Session *session_find(NP560Mp3HleTab5 *h, uint32_t workspace)
{
    unsigned i;
    for (i = 0; i < MP3_MAX_SESSIONS; i++) {
        if (h->sessions[i].used && h->sessions[i].workspace == workspace) {
            return &h->sessions[i];
        }
    }
    return NULL;
}

static Mp3Tab5Session *session_claim(NP560Mp3HleTab5 *h, uint32_t workspace)
{
    Mp3Tab5Session *session = session_find(h, workspace);
    unsigned i;

    if (session) {
        return session;
    }
    for (i = 0; i < MP3_MAX_SESSIONS; i++) {
        session = &h->sessions[i];
        if (!session->used) {
            NP560MP3Codec *codec = np560_mp3_codec_new();

            if (!codec) {
                return NULL;
            }
            memset(session, 0, sizeof(*session));
            session->codec = codec;
            session->workspace = workspace;
            session->used = true;
            np560_mp3_codec_reset(session->codec);
            return session;
        }
    }
    return NULL;
}

static void session_release(NP560Mp3HleTab5 *h, uint32_t workspace)
{
    Mp3Tab5Session *session = session_find(h, workspace);

    if (session) {
        np560_mp3_codec_free(session->codec);
        memset(session, 0, sizeof(*session));
    }
}

NP560Mp3HleTab5 *np560_mp3_hle_tab5_create(Np560Bus *bus, uint32_t base)
{
    NP560Mp3HleTab5 *h;
    unsigned i, version;

    if (!bus || (base & 1) || base < 0x10000 || base > 0x30000 - 64 ||
        ((base >> 16) != ((base + 63) >> 16))) {
        return NULL;
    }
    version = dmcu_code_read16(bus, base + 8);
    if (version < 1 || version > 8) {
        return NULL;
    }
    h = calloc(1, sizeof(*h));
    if (!h) {
        return NULL;
    }
    h->bus = bus;
    h->base = base;
    h->version = version;
    h->gate_count = version >= 7 ? 9 : version >= 4 ? 6 : version >= 2 ? 5 : 4;
    if ((base >> 16) != ((base + h->gate_count * 16 - 1) >> 16)) {
        free(h);
        return NULL;
    }
    for (i = 0; i < h->gate_count; i++) {
        if (!gate_matches(h, base + 16u * i, i)) {
            free(h);
            return NULL;
        }
    }
    return h;
}

void np560_mp3_hle_tab5_destroy(NP560Mp3HleTab5 *h)
{
    unsigned i;
    if (!h) {
        return;
    }
    for (i = 0; i < MP3_MAX_SESSIONS; i++) {
        np560_mp3_codec_free(h->sessions[i].codec);
    }
    free(h);
}

uint64_t np560_mp3_hle_tab5_call_count(const NP560Mp3HleTab5 *h)
{
    return h ? h->total : 0;
}

/* ---- service 4: reconstructed free-word query (no codec involvement) ---- */

static void input_space(NP560Mp3HleTab5 *h, DMCUCpu *cpu)
{
    uint32_t owner, workspace;
    uint16_t segment, offset, read, writer, bank;
    unsigned status = 2, result = 0, available;
    bool publish_status = false, root_busy = false;

    if (!writable_span(0x3342, 6)) {
        goto done;
    }
    segment = rd(h, 0x3342);
    offset = rd(h, 0x3344);
    if (segment == 0xffff) {
        status = 7;
        root_busy = true;
        goto done;
    }
    if (!(segment | offset)) {
        status = 8;
        goto failed;
    }
    if ((segment != 0x51 && segment != 0x52) || (offset & 1) ||
        (segment == 0x51 && offset < 0x4a46) ||
        (segment == 0x52 && offset > 0x3a40 - 96) ||
        (offset & 0x7fff) > 0x8000 - 96) {
        goto failed;
    }
    owner = ((uint32_t)segment << 16) | offset;
    if (!writable_span(owner, 96) ||
        rd(h, owner + 82) != 0x504d ||
        !rd(h, owner + 80) || rd(h, owner + 80) > 255) {
        goto failed;
    }
    if (rd(h, owner + 46)) {
        status = 7;
        goto failed;
    }
    publish_status = true;
    if (rd(h, owner + 44) != 0x534d) {
        status = 8;
        goto failed;
    }
    bank = rd(h, owner + 8) & 0x1ff;
    offset = rd(h, owner + 16);
    workspace = ((uint32_t)bank << 15) | offset;
    if ((offset & 1) || offset > 0x8000 - WORK_BYTES ||
        !writable_span(workspace, WORK_BYTES) ||
        rd(h, workspace + 0x46) != 0x5247 ||
        !writable_span(0x3340, 2)) {
        goto failed;
    }
    read = rd(h, workspace + 0x40);
    writer = rd(h, 0x3340);
    if (read < 0x1000 || read >= 0x1600 ||
        writer < 0x1000 || writer >= 0x1600 || (writer & 1)) {
        goto failed;
    }
    available = (writer + 0x600 - read) % 0x600;
    if (available > 1532) {
        goto failed;
    }
    if (h->version >= 3) {
        unsigned reclaim = rd(h, workspace + RECLAIM_PENDING);
        unsigned staged = rd(h, workspace + STAGED);
        unsigned end = rd(h, workspace + RECLAIM_END);
        unsigned length = rd(h, workspace + LENGTH);
        unsigned pending = rd(h, workspace + PENDING);

        if (reclaim > 1 || staged > 1 || (reclaim && staged) ||
            ((reclaim || staged) &&
             (!length || length > available ||
              end != 0x1000 + (read - 0x1000 + length) % 0x600)) ||
            (reclaim && pending)) {
            goto failed;
        }
        if (reclaim) {
            wr(h, workspace + 0x40, (uint16_t)end);
            wr(h, workspace + RECLAIM_PENDING, 0);
        }
    }
    result = 766 - (available + 1) / 2;
    status = 0;
failed:
    if (publish_status) {
        wr(h, owner + 52, (uint16_t)status);
    }
    if (status) {
        wr(h, 0x3346, 1);
        h->input_space_failures++;
    }
done:
    cpu->gpr[0] = result;
    cpu->gpr[2] = status;
    (void)root_busy;
}

/* ---- decoded PCM publication ---- */

static void publish_pcm(NP560Mp3HleTab5 *h, Mp3Tab5Session *session,
                        uint32_t address, unsigned offset)
{
    unsigned i, channel;

    for (channel = 0; channel < 2; channel++) {
        for (i = 0; i < 576; i++) {
            int16_t sample = channel < session->frame.channels ?
                session->frame.pcm[(offset + i) * session->frame.channels + channel] : 0;
            wr(h, address + (channel ? PCM_RIGHT : PCM_LEFT) + 2u * i,
               (uint16_t)sample);
        }
    }
    wr(h, address + PENDING, session->pending ? 1 : 0);
    wr(h, address + PCM_VALID, 576);
    h->granules++;
}

static const uint16_t mp3_volume_gain[8] = {
    0, 0x2027, 0x287a, 0x32f5, 0x4027, 0x50c3, 0x65ad, 0x7fff
};

static int16_t mp3_output_gain(int16_t sample, unsigned level)
{
    int32_t rounded = (int32_t)sample * mp3_volume_gain[level] + 16384;
    return rounded >= 0 ? (int16_t)(rounded / 32768)
                        : (int16_t)-((32767 - rounded) / 32768);
}

/* ---- service 5: software PCM delivery to the four guest-owned buffers ---- */

static void copy_pcm(NP560Mp3HleTab5 *h, DMCUCpu *cpu)
{
    uint32_t offset = cpu->gpr[3], phase = cpu->gpr[0];
    uint32_t owner, workspace, bank, work_offset;
    uint16_t buffers[4], pcm[1152];
    unsigned status = 2, i, j;

    if (cpu->ignore_ds_count || (offset & 1) || offset > 0x8000 - 96 ||
        phase > 1) {
        goto done;
    }
    owner = ((cpu->sfr[DMCU_SFR_DS0] & 0x1ff) << 15) | offset;
    if (owner < 0x514a46 || owner > 0x523a40 - 96 ||
        !writable_span(owner, 96) ||
        rd(h, owner + 82) != 0x504d ||
        !rd(h, owner + 80) || rd(h, owner + 80) > 255 ||
        rd(h, owner + 44) != 0x534d ||
        rd(h, owner + 46) != 1 ||
        rd(h, owner + 48) != (phase ^ 1)) {
        goto done;
    }
    bank = rd(h, owner + 8);
    work_offset = rd(h, owner + 16);
    workspace = (bank << 15) | work_offset;
    if (bank > 0x1ff || (work_offset & 1) ||
        work_offset > 0x8000 - WORK_BYTES ||
        !writable_span(workspace, WORK_BYTES) ||
        !session_find(h, workspace) ||
        rd(h, workspace + MAGIC) != 0x4d33 ||
        rd(h, workspace + PCM_VALID) != 576) {
        goto done;
    }
    for (i = 0; i < 4; i++) {
        buffers[i] = rd(h, owner + 20 + 4u * i);
        if (rd(h, owner + 18 + 4u * i) ||
            (buffers[i] & 1) || buffers[i] < 0x1600 ||
            buffers[i] > 0x8000 - 1152 ||
            !writable_span(buffers[i], 1152)) {
            goto done;
        }
        for (j = 0; j < i; j++) {
            if (buffers[i] < buffers[j] + 1152 &&
                buffers[j] < buffers[i] + 1152) {
                goto done;
            }
        }
    }
    for (i = 0; i < 1152; i++) {
        pcm[i] = rd(h, workspace + PCM_LEFT + 2u * i);
    }
    if (rd(h, 0xf014) & 0x0080) {
        memset(pcm, 0, sizeof(pcm));
    } else {
        unsigned level = rd(h, 0x4a64) >> 8; /* logical byte 4A64 = current */
        if (level < 8) {
            for (i = 0; i < 1152; i++) {
                pcm[i] = (uint16_t)mp3_output_gain((int16_t)pcm[i], level);
            }
        }
    }
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 576; j++) {
            wr(h, buffers[i * 2u + phase] + 2u * j,
               pcm[i * 576u + j]);
        }
    }
    status = 0;
done:
    cpu->gpr[0] = status ? 0 : 576;
    cpu->gpr[2] = status;
    h->pcm_copy_failures += status != 0;
}

/* ---- services 0..3 ---- */

static void mp3_session_service(NP560Mp3HleTab5 *h, DMCUCpu *cpu,
                                uint32_t address, unsigned service)
{
    unsigned status = 0, detail = 0, result = 0, i;
    Mp3Tab5Session *session;

    h->calls[service]++;
    if (cpu->ignore_ds_count || (cpu->gpr[3] & 1) ||
        cpu->gpr[3] > 0x8000 - WORK_BYTES ||
        !writable_span(address, WORK_BYTES)) {
        status = 2;
        goto complete;
    }
    session = session_find(h, address);
    if (service == 0) {
        if (!session) {
            session = session_claim(h, address);
            if (!session) {
                status = 2;
                goto complete;
            }
        }
        np560_mp3_codec_reset(session->codec);
        memset(&session->frame, 0, sizeof(session->frame));
        session->pending = false;
        session->next_pcm_offset = session->priming_remaining = 0;
        session->frames = session->granules = 0;
        session->startup_history_frames = session->startup_history_available = 0;
        for (i = 0; i < 1152; i++) {
            wr(h, address + PCM_LEFT + 2u * i, 0);
        }
        wr(h, address + PENDING, 0);
        wr(h, address + PCM_VALID, 0);
        wr(h, address + MAGIC, 0x4d33);
    } else if (service == 3) {
        session_release(h, address);
        wr(h, address + MAGIC, 0);
        wr(h, address + PENDING, 0);
        wr(h, address + PCM_VALID, 0);
    } else if (!session || rd(h, address + MAGIC) != 0x4d33) {
        status = 2;
    } else if (service == 2) {
        if (!session->pending) {
            status = 1;
        } else if (session->priming_remaining) {
            session->priming_remaining--;
        } else {
            unsigned pcm_offset = session->next_pcm_offset;
            session->next_pcm_offset += 576;
            session->pending = session->next_pcm_offset <
                               session->frame.samples_per_channel;
            if (pcm_offset == 0) {
                wr(h, address + RING_FORMAT_CHANGE, 1);
            }
            publish_pcm(h, session, address, pcm_offset);
            session->granules++;
            result = 576;
        }
    } else {
        uint8_t input[1532];
        uint16_t h0 = rd(h, address + HDR0);
        uint16_t h1 = rd(h, address + HDR1);
        unsigned header = (h0 & 1) ? 4 : 6;
        unsigned body = rd(h, address + BODY_BYTES);
        unsigned length = rd(h, address + LENGTH);
        unsigned final = h->version >= 5 ? rd(h, address + FINAL_INPUT) : 0;
        NP560MP3Status decoded;

        wr(h, address + PCM_VALID, 0);
        if (body > sizeof(input) - header || length != header + body ||
            final > 1) {
            status = 2;
            goto complete;
        }
        input[0] = (uint8_t)(h0 >> 8);
        input[1] = (uint8_t)h0;
        input[2] = (uint8_t)(h1 >> 8);
        input[3] = (uint8_t)h1;
        if (header == 6) {
            uint16_t crc = rd(h, address + CRC);
            input[4] = (uint8_t)(crc >> 8);
            input[5] = (uint8_t)crc;
        }
        for (i = 0; i < body; i++) {
            uint16_t word = rd(h, address + SCRATCH + (i & ~1u));
            input[header + i] = (uint8_t)((i & 1) ? word : word >> 8);
        }
        if (rd(h, address + FRAME + 2) !=
            ((input[3] >> 6) == 3 ? 1 : 2)) {
            status = 7;
            detail = 0x800 + NP560_MP3_DECODE_ERROR;
            goto complete;
        }
        if (final) {
            if (!writable_span(0xf012, 2) ||
                !(rd(h, 0xf012) & 0x80)) {
                status = 2;
                goto complete;
            }
            decoded = np560_mp3_codec_decode_final(session->codec, input,
                                                   length, &session->frame);
        } else {
            decoded = np560_mp3_codec_decode(session->codec, input,
                                             length, &session->frame);
        }
        if (decoded != NP560_MP3_OK ||
            session->frame.consumed != length ||
            session->frame.channels != rd(h, address + FRAME + 2)) {
            status = 7;
            detail = 0x800 + (decoded ? decoded : NP560_MP3_DECODE_ERROR);
            wr(h, address + PCM_VALID, 0);
        } else {
            unsigned prime = 1;
            if (h->version >= 6 && session->frames == 0 &&
                writable_span(0xf006, 2) && writable_span(0xf014, 2) &&
                writable_span(0x4a60, 2) && writable_span(0x512b10, 2) &&
                !(rd(h, 0xf006) & 0x8000) &&
                (rd(h, 0xf014) & 0x400) &&
                rd(h, 0x4a60) == session->frame.hz) {
                unsigned configured = rd(h, 0x512b10);
                if (configured >= 1 && configured <= 3) {
                    prime = configured;
                }
            }
            session->pending = session->frame.samples_per_channel == 1152;
            session->next_pcm_offset = 576;
            if (prime > 1) {
                session->next_pcm_offset = 0;
                session->priming_remaining = prime - 2;
                session->pending = true;
                wr(h, address + PENDING, 1);
                wr(h, address + PCM_VALID, 0);
            } else {
                publish_pcm(h, session, address, 0);
                result = 576;
                session->granules++;
            }
            session->frames++;
        }
    }
complete:
    wr(h, address + STATUS, (uint16_t)status);
    wr(h, address + DETAIL, (uint16_t)detail);
    cpu->gpr[0] = result;
    cpu->gpr[2] = status;
    h->failures += status > 1;
}

bool np560_mp3_hle_tab5_service(void *user, DMCUCpu *cpu, uint32_t pc)
{
    NP560Mp3HleTab5 *h = user;
    unsigned service;

    if (!h || !cpu || pc < h->base ||
        pc >= h->base + 16u * h->gate_count || (pc - h->base) % 16) {
        return false;
    }
    service = (pc - h->base) / 16u;
    if (!gate_matches(h, pc, service)) {
        return false;
    }
    h->total++;
    if (service == 4) {
        input_space(h, cpu);
        return true;
    }
    if (service == 5) {
        copy_pcm(h, cpu);
        return true;
    }
    if (service < 4) {
        uint32_t address = ((cpu->sfr[DMCU_SFR_DS0] & 0x1ff) << 15) |
                           (cpu->gpr[3] & 0xffff);
        mp3_session_service(h, cpu, address, service);
        return true;
    }
    /* v6 has no mode/EQ gates; anything else is a real ROM TRAP. */
    h->total--;
    return false;
}
