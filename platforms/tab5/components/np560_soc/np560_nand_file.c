/*
 * np560_nand_file.c - NAND 控制器的 stdio 文件介质 + RAM 写回缓存。
 *
 * system.nand 镜像 = NP560N1 checkpoint 容器：64B 头 + data 平面
 * （pages×2048 连续）+ OOB 平面（pages×64 连续），两个平面分离、
 * 不交错——与 QEMU nand-save 导出及离线工具 CheckpointFat16 一致。
 * 注意：两种布局（平面分离 vs 2112B 交错）总大小相同，此前误按交错
 * 布局寻址，导致全部读数据错位 → FAT:Init error（2026-11-24 修复）。
 *
 * 页级语义（介质职责）：
 *   read_page    - 顺序读 2112 字节。
 *   program_page - NAND 编程是 AND 语义（1→0，np560.c:8669-8682）：
 *                  读出原页与掩码 AND 后写回。
 *   erase_block  - 整块（64 页）置 FF。
 *
 * RAM 写回缓存（ESP32 真机必需）：原生固件引导会发数万次块擦除命令
 * （QEMU journal 对"擦已空块"按 no-op 跳过，见 np560_nand_journal.c
 * 注释）。文件介质上每次擦除 = 一次 TF 卡同步 I/O（块擦 ~1s、页编程
 * ~0.7s），引导被拖到分钟级且 CPU 长停顿。设计：
 *   - 擦除路径零 I/O：erased 位图 + 覆盖层，擦除只设位（瞬时）；
 *   - 读：覆盖层 > 擦除位图（返回 FF）> 文件；
 *   - 编程：覆盖层 AND 掩码（块已擦则基线为 FF），标脏后台刷盘；
 *   - 纯擦除块不持久化：固件下次引导会重擦全部块，掉电自愈
 *     （journal 语义同为延迟落盘）。
 */
#include "np560_nand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

/* io_trace() 在 ESP_PLATFORM 守卫之外使用日志/计时（引导期 I/O 诊断）。
 * host 侧没有 ESP-IDF 头，这里给出空实现，保证源在两端都能编译。 */
#ifndef ESP_PLATFORM
#define ESP_LOGI(tag, fmt, ...) ((void)0)
static inline int64_t host_io_trace_us(void) { return 0; }
#define esp_timer_get_time() host_io_trace_us()
#endif

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
/* 写回槽：引导期 dirty=0（实测），无需 1024 个。缩到 256 个释放
 * ~1.6MB 连续 PSRAM，供 OOB 稀疏索引（~3.9MB）分配。 */
#define NP560_WB_SLOTS 256u     /* 256 页 × 2112B ≈ 528KB PSRAM */

/* 读失败采样日志：分支码区分失败来源（I/O 错 vs 缓存逻辑错）。 */
#define NF_FAIL_LOG(branch, pg)                                            \
    do {                                                                   \
        m->stat_read_fails++;                                              \
        if (m->stat_read_fails <= 8 || m->stat_read_fails % 128 == 0) {    \
            FILE *ff = (FILE *)m->file;                                    \
            ESP_LOGE("nand_file",                                          \
                     "read fail page=%u branch=%s (#%u) ferror=%d",        \
                     (unsigned)(pg), branch,                               \
                     (unsigned)m->stat_read_fails,                         \
                     ff ? ferror(ff) : -1);                                \
        }                                                                  \
    } while (0)
#endif /* ESP_PLATFORM */

/* 写回缓存槽（编程过的页覆盖层）。 */
typedef struct {
    bool used;
    bool dirty;
    uint32_t page;
    uint8_t data[NP560_NAND_PAGE_TOTAL];
} Np560NandWbSlot;

#define NP560_ERASED_BITMAP_BYTES (NP560_NAND_BLOCKS / 8u)
#define NP560_PROG_BITMAP_BYTES   (NP560_NAND_PAGES / 8u)

/* NP560N1 checkpoint 容器头大小（data 平面从该偏移开始）。 */
#define NP560_CHK_HEADER 64u

/* SD I/O 追踪：挂死时最后一条追踪行即卡住的页。每 256 次 I/O 打一行。 */
#define NP560_IO_TRACE_PERIOD 256u

static void io_trace(char op, uint32_t page)
{
    static uint32_t count;

    count++;
    if (count % NP560_IO_TRACE_PERIOD == 0) {
        ESP_LOGI("nand_file", "nandio #%u: %c page=%u t=%llums",
                 (unsigned)count, op, (unsigned)page,
                 (unsigned long long)(esp_timer_get_time() / 1000));
    }
}

/* data 平面内页 N 的偏移（2048B 连续）。 */
static long file_data_offset(const Np560NandFileMedium *m, uint32_t page)
{
    return (long)(NP560_CHK_HEADER + (uint64_t)page * NP560_NAND_PAGE_DATA);
}

/* OOB 平面内页 N 的偏移（64B 连续，紧跟整个 data 平面之后）。 */
static long file_oob_offset(const Np560NandFileMedium *m, uint32_t page)
{
    return (long)(NP560_CHK_HEADER +
                  (uint64_t)m->pages * NP560_NAND_PAGE_DATA +
                  (uint64_t)page * NP560_NAND_PAGE_OOB);
}

#ifdef ESP_PLATFORM
/* ---- OOB 稀疏索引：把引导期数万次 SD 随机读压成一次性顺序扫描 ----
 * QEMU oracle 证实固件启动对 OOB 平面做多遍扫描（off=0 len=64 整页 OOB），
 * 每次块预取在 TF 卡上 ≈4ms；14.7 万次读累计 150s+。镜像 262144 页中
 * OOB 非全 FF 的仅 ~53481，压缩存储只需 ~3.4MB。
 *
 * 存储格式（为 PSRAM 碎片优化，不要求单个 3.4MB 连续块）：
 *   present: 262144-bit 位图，标记哪些页 OOB 非全 FF；
 *   rank:    每 256 页的累计非 FF 页数（u16），用于 O(1) 定位；
 *   chunks:  64 个数据块（每 4096 页一块），块内只紧存非 FF 页的 64B，
 *            按需分配并倍增扩容——实测 PSRAM 空闲 3.53MB / 最大连续
 *            块 3.4MB，单个 3.9MB 数组分配必失败。
 * 构建时顺序读 16MB OOB 平面（一次线性扫描），之后只读页走 RAM 查询。
 * 已编程页（prog_bitmap）不走本索引——其内容可能已不同于镜像基线。 */
#define OOB_SPARSE_CHUNKS 64u
#define OOB_CHUNK_PAGES   4096u
/* QEMU oracle：把 OOB 低 32 字节全置 FF 后固件仍正常进桌面（每日一句）。
 * 即引导期只读 OOB[32..63]（ECC/FTL tag 区），低半区从不参与决策。
 * 只索引高 32 字节，索引数据从 3.4MB 降到 1.7MB——实测 PSRAM 空闲
 * 3.53MB 但最大连续块仅 3.4MB，全量索引因碎片分配失败。 */
#define OOB_SPARSE_HALF    32u

typedef struct {
    uint8_t present[NP560_NAND_PAGES / 8];
    uint16_t rank[NP560_NAND_PAGES / 256];
    uint32_t chunk_base[OOB_SPARSE_CHUNKS];
    uint32_t chunk_counts[OOB_SPARSE_CHUNKS];
    uint32_t chunk_caps[OOB_SPARSE_CHUNKS];
    uint8_t *chunks[OOB_SPARSE_CHUNKS];
    uint32_t total;
} OobSparse;

static const uint8_t oob_ff[NP560_NAND_PAGE_OOB] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

/* 前向声明：构建后立即用查询路径自检（要求两者索引语义完全一致）。 */
static bool oob_sparse_lookup(const Np560NandFileMedium *m, uint32_t page,
                              uint8_t *out);

/* 构建期抽样：每个 chunk 取一个非 FF 页，构建完成后回读比对。
 * 这样 FAT 阶段之前就能在日志里看到索引是否正确，不再靠间接症状猜。 */
#define OOB_SAMPLE_MAX OOB_SPARSE_CHUNKS
typedef struct {
    uint32_t page;
    uint8_t high[OOB_SPARSE_HALF];
} OobSample;

static void oob_sparse_free(OobSparse *s)
{
    if (!s) {
        return;
    }
    for (uint32_t i = 0; i < OOB_SPARSE_CHUNKS; i++) {
        if (s->chunks[i]) {
            heap_caps_free(s->chunks[i]);
        }
    }
    heap_caps_free(s);
}

static void oob_sparse_build(Np560NandFileMedium *m)
{
    FILE *f = (FILE *)m->file;
    OobSparse *s;
    /* 一次 fread 读整个 chunk（4096 页 × 64B = 256KB）：4KB 小批量
     * 在 FATFS 上要 4096 次命令，扫描 16MB 就花 10s；256KB 批量
     * 只需 64 次，线性读速度提升一个数量级。 */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(
        (size_t)OOB_CHUNK_PAGES * NP560_NAND_PAGE_OOB, MALLOC_CAP_SPIRAM);
    static OobSample samples[OOB_SAMPLE_MAX];
    uint32_t sample_count = 0;
    uint64_t t0;

    if (!f || m->pages != NP560_NAND_PAGES) {
        return;
    }
    if (!buf) {
        ESP_LOGW("nand_file", "OOB scan buffer alloc failed; "
                 "fallback to block prefetch");
        return;
    }
    s = (OobSparse *)heap_caps_malloc(sizeof(OobSparse), MALLOC_CAP_SPIRAM);
    if (!s) {
        heap_caps_free(buf);
        ESP_LOGW("nand_file", "OOB sparse header alloc failed; "
                 "fallback to block prefetch");
        return;
    }
    memset(s, 0, sizeof(*s));
    t0 = esp_timer_get_time();
    if (fseek(f, file_oob_offset(m, 0), SEEK_SET) != 0) {
        heap_caps_free(buf);
        oob_sparse_free(s);
        return;
    }
    for (uint32_t page = 0; page < m->pages; page += OOB_CHUNK_PAGES) {
        uint32_t chunk = page / OOB_CHUNK_PAGES;

        s->chunk_base[chunk] = s->total;
        if (fread(buf, NP560_NAND_PAGE_OOB, OOB_CHUNK_PAGES, f) !=
            OOB_CHUNK_PAGES) {
            ESP_LOGW("nand_file", "OOB sparse scan short read at page=%u",
                     (unsigned)page);
            heap_caps_free(buf);
            oob_sparse_free(s);
            return;
        }
        for (uint32_t i = 0; i < OOB_CHUNK_PAGES; i++) {
            const uint8_t *oob = buf + (size_t)i * NP560_NAND_PAGE_OOB;
            uint32_t p = page + i;

            /* rank[段] = 进入该 256 页段"之前"的非 FF 累计数。查询时
             * 以它为 base 再加 [段起点, page) 的 present popcount。
             * 必须在段起点、无条件记录：写在段末尾会把段内每页索引
             * 整体偏移，FTL tag 读到别的页的数据 → FAT init 崩溃。 */
            if ((p & 255u) == 0u) {
                s->rank[p / 256] = (uint16_t)s->total;
            }
            if (memcmp(oob + OOB_SPARSE_HALF, oob_ff,
                       NP560_NAND_PAGE_OOB - OOB_SPARSE_HALF) == 0) {
                continue; /* 高半区全 FF：查询时返回 FF 即可 */
            }
            if (s->chunk_counts[chunk] >= s->chunk_caps[chunk]) {
                uint32_t newcap = s->chunk_caps[chunk] ?
                                  s->chunk_caps[chunk] * 2 : 128;
                uint8_t *newbuf = (uint8_t *)heap_caps_malloc(
                    (size_t)newcap * OOB_SPARSE_HALF, MALLOC_CAP_SPIRAM);

                if (!newbuf) {
                    ESP_LOGW("nand_file", "OOB sparse chunk=%u alloc %u failed "
                             "(free=%u largest=%u); fallback to block prefetch",
                             (unsigned)chunk, (unsigned)newcap,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                             (unsigned)heap_caps_get_largest_free_block(
                                 MALLOC_CAP_SPIRAM));
                    heap_caps_free(buf);
                    oob_sparse_free(s);
                    return;
                }
                if (s->chunks[chunk]) {
                    memcpy(newbuf, s->chunks[chunk],
                           (size_t)s->chunk_counts[chunk] * OOB_SPARSE_HALF);
                    heap_caps_free(s->chunks[chunk]);
                }
                s->chunks[chunk] = newbuf;
                s->chunk_caps[chunk] = newcap;
            }
            s->present[p / 8] |= (uint8_t)(1u << (p % 8));
            memcpy(s->chunks[chunk] +
                       (size_t)s->chunk_counts[chunk] * OOB_SPARSE_HALF,
                   oob + OOB_SPARSE_HALF, OOB_SPARSE_HALF);
            s->chunk_counts[chunk]++;
            s->total++;
            /* 每 chunk 采一个非 FF 页（chunk 内第一个即够，覆盖全地址域）。 */
            if (i == 0 && sample_count < OOB_SAMPLE_MAX) {
                samples[sample_count].page = p;
                memcpy(samples[sample_count].high, oob + OOB_SPARSE_HALF,
                       OOB_SPARSE_HALF);
                sample_count++;
            }
        }
    }
    heap_caps_free(buf);
    m->oob_sparse = s;
    ESP_LOGI("nand_file", "OOB sparse index ready: %u pages (%uKB data, "
             "high %uB only), scan %llums",
             (unsigned)s->total,
             (unsigned)(s->total * OOB_SPARSE_HALF / 1024),
             (unsigned)OOB_SPARSE_HALF,
             (unsigned long long)((esp_timer_get_time() - t0) / 1000));
    /* 自检：用查询路径回读样本，高半区必须逐字节一致。全 FF 页样本
     * （少数 chunk 首块恰为 FF）只能用全 FF 结果比对。 */
    {
        uint8_t out[NP560_NAND_PAGE_OOB];
        uint32_t pass = 0;

        for (uint32_t i = 0; i < sample_count; i++) {
            bool all_ff = true;

            for (uint32_t j = 0; j < OOB_SPARSE_HALF; j++) {
                if (samples[i].high[j] != 0xff) {
                    all_ff = false;
                    break;
                }
            }
            oob_sparse_lookup(m, samples[i].page, out);
            if (memcmp(out + OOB_SPARSE_HALF, samples[i].high,
                       OOB_SPARSE_HALF) == 0) {
                pass++;
            } else {
                ESP_LOGE("nand_file", "OOB index self-check FAIL page=%u "
                         "want=%02x%02x got=%02x%02x",
                         (unsigned)samples[i].page,
                         samples[i].high[0], samples[i].high[1],
                         out[OOB_SPARSE_HALF], out[OOB_SPARSE_HALF + 1]);
            }
            (void)all_ff;
        }
        ESP_LOGI("nand_file", "OOB index self-check: %u/%u passed",
                 (unsigned)pass, (unsigned)sample_count);
    }
}

static bool oob_sparse_lookup(const Np560NandFileMedium *m, uint32_t page,
                              uint8_t *out)
{
    const OobSparse *s = (const OobSparse *)m->oob_sparse;
    uint32_t k, start, w0, w1, chunk, local;

    /* 低半区从不参与固件决策（QEMU 掩码实验证实），统一返回 FF。 */
    memset(out, 0xff, NP560_NAND_PAGE_OOB);
    if ((s->present[page / 8] & (1u << (page % 8))) == 0) {
        return true; /* 基线 OOB 高半区也全 FF */
    }
    start = (page / 256) * 256;
    k = s->rank[page / 256];
    w0 = start / 32;
    w1 = (page - 1) / 32;
    for (uint32_t w = w0; w <= w1; w++) {
        uint32_t bits = ((const uint32_t *)s->present)[w];

        if (w == w0) {
            bits &= ~((1u << (start % 32)) - 1u);
        }
        if (w == w1) {
            unsigned endbit = (page - 1) % 32;

            bits &= (endbit == 31) ? 0xffffffffu : ((1u << (endbit + 1)) - 1u);
        }
        k += __builtin_popcount(bits);
    }
    chunk = page / OOB_CHUNK_PAGES;
    local = k - s->chunk_base[chunk];
    if (local < s->chunk_counts[chunk] && s->chunks[chunk]) {
        memcpy(out + OOB_SPARSE_HALF,
               s->chunks[chunk] + (size_t)local * OOB_SPARSE_HALF,
               OOB_SPARSE_HALF);
    }
    return true;
}
#endif /* ESP_PLATFORM */

static bool file_read_raw(void *user, uint32_t page, uint8_t *out)
{
    Np560NandFileMedium *m = (Np560NandFileMedium *)user;
    FILE *f = (FILE *)m->file;

    if (!f || page >= m->pages) {
        return false;
    }
    io_trace('r', page);
    if (fseek(f, file_data_offset(m, page), SEEK_SET) != 0 ||
        fread(out, 1, NP560_NAND_PAGE_DATA, f) != NP560_NAND_PAGE_DATA ||
        fseek(f, file_oob_offset(m, page), SEEK_SET) != 0 ||
        fread(out + NP560_NAND_PAGE_DATA, 1, NP560_NAND_PAGE_OOB, f) !=
            NP560_NAND_PAGE_OOB) {
        return false;
    }
    return true;
}

static bool file_write_raw(void *user, uint32_t page, const uint8_t *bytes)
{
    Np560NandFileMedium *m = (Np560NandFileMedium *)user;
    FILE *f = (FILE *)m->file;

    if (!f || page >= m->pages) {
        return false;
    }
    io_trace('w', page);
    if (fseek(f, file_data_offset(m, page), SEEK_SET) != 0 ||
        fwrite(bytes, 1, NP560_NAND_PAGE_DATA, f) != NP560_NAND_PAGE_DATA ||
        fseek(f, file_oob_offset(m, page), SEEK_SET) != 0 ||
        fwrite(bytes + NP560_NAND_PAGE_DATA, 1, NP560_NAND_PAGE_OOB, f) !=
            NP560_NAND_PAGE_OOB) {
        return false;
    }
    /* 文件内容已变：OOB 块缓存里对应块的基线过期，令其失效。 */
    m->oob_prefetch_block = 0xffffffffu;
    return true;
}

static bool block_erased(const Np560NandFileMedium *m, uint32_t page)
{
    const uint8_t *bm = (const uint8_t *)m->erased_bitmap;
    uint32_t block = page / NP560_NAND_PAGES_PER_BLOCK;

    return bm != NULL && ((bm[block / 8] >> (block % 8)) & 1u);
}

/* 页级编程位图：该页内容已（或将要）持久化到文件。擦除时整块清除。 */
static bool page_programmed(const Np560NandFileMedium *m, uint32_t page)
{
    const uint8_t *bm = (const uint8_t *)m->prog_bitmap;

    return bm != NULL && ((bm[page / 8] >> (page % 8)) & 1u);
}

static void prog_bitmap_set(Np560NandFileMedium *m, uint32_t page, bool set)
{
    if (!m->prog_bitmap) {
        return;
    }
    uint8_t *bm = (uint8_t *)m->prog_bitmap;

    if (set) {
        bm[page / 8] |= (uint8_t)(1u << (page % 8));
    } else {
        bm[page / 8] &= (uint8_t)~(1u << (page % 8));
    }
}

#ifdef ESP_PLATFORM

/* 只查不建。持锁调用。 */
static Np560NandWbSlot *wb_find(Np560NandFileMedium *m, uint32_t page)
{
    Np560NandWbSlot *slots = (Np560NandWbSlot *)m->slots;

    for (uint32_t i = 0; i < NP560_WB_SLOTS; i++) {
        if (slots[i].used && slots[i].page == page) {
            return &slots[i];
        }
    }
    return NULL;
}

/* 预取取槽：优先未用槽；槽满则轮转覆盖干净槽（顺序块扫描场景下
 * 被覆盖的页内容仍在文件里，安全）。持锁调用。 */
static Np560NandWbSlot *prefetch_claim(Np560NandFileMedium *m)
{
    Np560NandWbSlot *slots = (Np560NandWbSlot *)m->slots;
    uint32_t i;

    for (i = 0; i < NP560_WB_SLOTS; i++) {
        if (!slots[i].used) {
            return &slots[i];
        }
    }
    for (i = 0; i < NP560_WB_SLOTS; i++) {
        uint32_t idx = (m->prefetch_cursor + i) % NP560_WB_SLOTS;

        if (!slots[idx].dirty) {
            m->prefetch_cursor = (idx + 1) % NP560_WB_SLOTS;
            return &slots[idx];
        }
    }
    return NULL;
}

/* 取槽：命中返回；未命中复用未用/干净槽，必要时先刷脏槽。持锁调用。 */
static Np560NandWbSlot *wb_slot(Np560NandFileMedium *m, uint32_t page,
                                bool load_from_file)
{
    Np560NandWbSlot *slots = (Np560NandWbSlot *)m->slots;
    Np560NandWbSlot *victim = NULL;
    uint32_t i;

    for (i = 0; i < NP560_WB_SLOTS; i++) {
        if (slots[i].used && slots[i].page == page) {
            return &slots[i];
        }
    }
    /* 两段式：先找未用槽（不牺牲任何缓存页），再找干净槽。 */
    for (i = 0; i < NP560_WB_SLOTS && !victim; i++) {
        if (!slots[i].used) {
            victim = &slots[i];
        }
    }
    for (i = 0; i < NP560_WB_SLOTS && !victim; i++) {
        if (!slots[i].dirty) {
            victim = &slots[i];
        }
    }
    if (!victim) {
        victim = &slots[0];
        for (uint32_t i = 1; i < NP560_WB_SLOTS; i++) {
            if (slots[i].page < victim->page) {
                victim = &slots[i];
            }
        }
        if (file_write_raw(m, victim->page, victim->data)) {
            m->stat_flushed++;
            prog_bitmap_set(m, victim->page, true);
        }
        m->stat_dirty--;
        victim->dirty = false;
    }
    victim->used = true;
    victim->dirty = false;
    victim->page = page;
    if (load_from_file) {
        if (!file_read_raw(m, page, victim->data)) {
            memset(victim->data, 0xff, sizeof(victim->data));
        }
    } else {
        /* 块已擦除：基线即 FF（编程是 AND，与 FF AND = 掩码本身）。 */
        memset(victim->data, 0xff, sizeof(victim->data));
    }
    return victim;
}

static void wb_flush_task(void *arg)
{
    Np560NandFileMedium *m = (Np560NandFileMedium *)arg;
    Np560NandWbSlot *slots = (Np560NandWbSlot *)m->slots;

    esp_task_wdt_add(NULL);
    for (;;) {
        Np560NandWbSlot *dirty = NULL;
        bool quit;

        xSemaphoreTake((SemaphoreHandle_t)m->lock, portMAX_DELAY);
        quit = m->flush_quit;
        for (uint32_t i = 0; i < NP560_WB_SLOTS && !dirty; i++) {
            if (slots[i].used && slots[i].dirty) {
                dirty = &slots[i];
            }
        }
        if (dirty) {
            if (file_write_raw(m, dirty->page, dirty->data)) {
                dirty->dirty = false;
                m->stat_flushed++;
                m->stat_dirty--;
                prog_bitmap_set(m, dirty->page, true);
            }
        }
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
        esp_task_wdt_reset();
        if (quit) {
            esp_task_wdt_delete(NULL);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(dirty ? 15 : 200));
    }
}

#endif /* ESP_PLATFORM */

static bool file_read_page(void *user, uint32_t page, uint8_t *out)
{
    Np560NandFileMedium *m = (Np560NandFileMedium *)user;
    bool ok;

#ifdef ESP_PLATFORM
    Np560NandWbSlot *slots = (Np560NandWbSlot *)m->slots;

    xSemaphoreTake((SemaphoreHandle_t)m->lock, portMAX_DELAY);
    for (uint32_t i = 0; i < NP560_WB_SLOTS; i++) {
        if (slots[i].used && slots[i].page == page) {
            memcpy(out, slots[i].data, NP560_NAND_PAGE_TOTAL);
            xSemaphoreGive((SemaphoreHandle_t)m->lock);
            m->stat_reads++;
            return true;
        }
    }
    if (page_programmed(m, page)) {
        /* 该页曾编程且已刷盘逐出：内容在文件里（擦除位图不适用）。 */
        ok = file_read_raw(m, page, out);
        if (!ok) {
            NF_FAIL_LOG("progfile", page);
        }
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
        m->stat_reads++;
        return ok;
    }
    if (block_erased(m, page)) {
        memset(out, 0xff, NP560_NAND_PAGE_TOTAL);
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
        m->stat_reads++;
        return true;
    }
    /* 块级读预取：一次随机访问 + 顺序流读整块 64 页进缓存。SD 随机
     * 单页读 ~40ms（fseek+首扇区延迟），顺序读 2112B ≈ 1.4ms——把
     * 固件的页扫描从 64×40ms 压到 ~100ms。 */
    {
        /* 两次大 fread（data 平面 128KB + OOB 平面 4KB，块内各自连续）：
         * FATFS 对连续扇区发多扇区读命令，64 次小 fread 每次仍要
         * 5 个单扇区命令（~40ms/页）。 */
        uint32_t b0 = page - page % NP560_NAND_PAGES_PER_BLOCK;
        FILE *f = (FILE *)m->file;
        size_t data_bytes = (size_t)NP560_NAND_PAGES_PER_BLOCK *
                            NP560_NAND_PAGE_DATA;
        size_t oob_bytes = (size_t)NP560_NAND_PAGES_PER_BLOCK *
                           NP560_NAND_PAGE_OOB;
        uint8_t *block_buf = (uint8_t *)m->block_buf;
        bool all_ok;

        io_trace('B', b0);
        all_ok = f && block_buf &&
                      fseek(f, file_data_offset(m, b0), SEEK_SET) == 0 &&
                      fread(block_buf, 1, data_bytes, f) == data_bytes &&
                      fseek(f, file_oob_offset(m, b0), SEEK_SET) == 0 &&
                      fread(block_buf + data_bytes, 1, oob_bytes, f) ==
                          oob_bytes;

        if (all_ok) {
            for (uint32_t i = 0; i < NP560_NAND_PAGES_PER_BLOCK; i++) {
                Np560NandWbSlot *slot;

                if (wb_find(m, b0 + i)) {
                    /* 已有槽（权威内容，可能比文件新）：跳过，不覆盖。 */
                    continue;
                }
                /* 直填：走 wb_slot 会反复复用同一干净槽，覆盖刚预取的页。 */
                slot = prefetch_claim(m);
                if (!slot) {
                    break;
                }
                slot->used = true;
                slot->dirty = false;
                slot->page = b0 + i;
                memcpy(slot->data,
                       block_buf + (size_t)i * NP560_NAND_PAGE_DATA,
                       NP560_NAND_PAGE_DATA);
                memcpy(slot->data + NP560_NAND_PAGE_DATA,
                       block_buf + data_bytes + (size_t)i * NP560_NAND_PAGE_OOB,
                       NP560_NAND_PAGE_OOB);
            }
        } else {
            /* 预取失败退化为单页直读。 */
            m->stat_prefetch_fail++;
            if (m->stat_prefetch_fail <= 4 ||
                m->stat_prefetch_fail % 128 == 0) {
                ESP_LOGW("nand_file",
                         "prefetch fail block=%u page=%u (#%u)",
                         (unsigned)b0 / NP560_NAND_PAGES_PER_BLOCK,
                         (unsigned)page,
                         (unsigned)m->stat_prefetch_fail);
            }
            ok = file_read_raw(m, page, out);
            if (!ok) {
                NF_FAIL_LOG("raw", page);
            }
            xSemaphoreGive((SemaphoreHandle_t)m->lock);
            m->stat_reads++;
            return ok;
        }
    }
    for (uint32_t i = 0; i < NP560_WB_SLOTS; i++) {
        if (slots[i].used && slots[i].page == page) {
            memcpy(out, slots[i].data, NP560_NAND_PAGE_TOTAL);
            xSemaphoreGive((SemaphoreHandle_t)m->lock);
            m->stat_reads++;
            return true;
        }
    }
    xSemaphoreGive((SemaphoreHandle_t)m->lock);
    NF_FAIL_LOG("miss", page);
    m->stat_reads++;
    return false;
#else
    ok = file_read_raw(user, page, out);
    m->stat_reads++;
    return ok;
#endif
}

static bool file_program_page(void *user, uint32_t page, const uint8_t *mask)
{
    Np560NandFileMedium *m = (Np560NandFileMedium *)user;

#ifdef ESP_PLATFORM
    Np560NandWbSlot *slot;

    xSemaphoreTake((SemaphoreHandle_t)m->lock, portMAX_DELAY);
    /* 块已擦除（或被覆盖层记录为 FF）：基线 FF；否则从文件读基线。 */
    slot = wb_slot(m, page, !block_erased(m, page));
    if (!slot) {
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
        return false;
    }
    prog_bitmap_set(m, page, true);
    /* AND 编程：掩码 0 位把原位写为 0（1→0 单向）。 */
    for (unsigned i = 0; i < NP560_NAND_PAGE_TOTAL; i++) {
        slot->data[i] &= mask[i];
    }
    if (!slot->dirty) {
        slot->dirty = true;
        m->stat_dirty++;
    }
    m->stat_programs++;
    xSemaphoreGive((SemaphoreHandle_t)m->lock);
    return true;
#else
    uint8_t page_bytes[NP560_NAND_PAGE_TOTAL];

    if (!file_read_raw(user, page, page_bytes)) {
        return false;
    }
    for (unsigned i = 0; i < NP560_NAND_PAGE_TOTAL; i++) {
        page_bytes[i] &= mask[i];
    }
    m->stat_programs++;
    return file_write_raw(user, page, page_bytes);
#endif
}

static bool file_erase_block(void *user, uint32_t block)
{
    Np560NandFileMedium *m = (Np560NandFileMedium *)user;
    uint32_t page = block * NP560_NAND_PAGES_PER_BLOCK;

    if (page + NP560_NAND_PAGES_PER_BLOCK > m->pages) {
        return false;
    }
#ifdef ESP_PLATFORM
    Np560NandWbSlot *slots = (Np560NandWbSlot *)m->slots;

    xSemaphoreTake((SemaphoreHandle_t)m->lock, portMAX_DELAY);
    /* 擦除路径零 I/O：覆盖层内该块的槽丢弃（位图接管 FF 语义），
     * 位图置位。纯擦除块不持久化——固件引导会重擦全部块，掉电自愈。 */
    for (uint32_t i = 0; i < NP560_WB_SLOTS; i++) {
        if (slots[i].used && slots[i].page >= page &&
            slots[i].page < page + NP560_NAND_PAGES_PER_BLOCK) {
            if (slots[i].dirty) {
                m->stat_dirty--;
            }
            slots[i].used = false;
            slots[i].dirty = false;
        }
    }
    ((uint8_t *)m->erased_bitmap)[block / 8] |=
        (uint8_t)(1u << (block % 8));
    {
        uint32_t b0 = block * NP560_NAND_PAGES_PER_BLOCK;

        for (uint32_t p = b0; p < b0 + NP560_NAND_PAGES_PER_BLOCK; p++) {
            prog_bitmap_set(m, p, false);
        }
    }
    m->stat_erases++;
    xSemaphoreGive((SemaphoreHandle_t)m->lock);
    return true;
#else
    uint8_t erased[NP560_NAND_PAGE_TOTAL];

    memset(erased, 0xff, sizeof(erased));
    for (uint32_t i = 0; i < NP560_NAND_PAGES_PER_BLOCK; i++) {
        if (!file_write_raw(user, page + i, erased)) {
            return false;
        }
    }
    m->stat_erases++;
    return true;
#endif
}

/* 只读页尾 OOB（64 字节）：FTL 全盘扫描的读请求 95% 只需要 OOB，
 * 每次只读 1 个扇区（512B）而非整块 135KB——引导提速关键路径。
 * 覆盖顺序：写回槽 > 擦除位图（FF）> 文件 OOB 扇区。 */
static bool file_read_oob(void *user, uint32_t page, uint8_t *out)
{
    Np560NandFileMedium *m = (Np560NandFileMedium *)user;
    bool ok;

    if (page >= m->pages) {
        return false;
    }
#ifdef ESP_PLATFORM
    Np560NandWbSlot *slots = (Np560NandWbSlot *)m->slots;

    xSemaphoreTake((SemaphoreHandle_t)m->lock, portMAX_DELAY);
    for (uint32_t i = 0; i < NP560_WB_SLOTS; i++) {
        if (slots[i].used && slots[i].page == page) {
            memcpy(out, slots[i].data + NP560_NAND_PAGE_DATA,
                   NP560_NAND_PAGE_OOB);
            xSemaphoreGive((SemaphoreHandle_t)m->lock);
            m->stat_reads++;
            return true;
        }
    }
    /* 覆盖顺序与 file_read_page 一致：编程位图优先于擦除位图——
     * 擦块后编程的页（FTL 元数据常态）槽被逐出时，必须读文件里的
     * 编程内容而不是擦除位图的 FF，否则固件读回自己写的元数据变
     * FF → FAT init 报错。编程已刷盘与从未编程都是读文件 OOB 扇区。 */
    if (block_erased(m, page) && !page_programmed(m, page)) {
        memset(out, 0xff, NP560_NAND_PAGE_OOB);
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
        m->stat_reads++;
        return true;
    }
    /* OOB 稀疏索引：开机时已顺序扫描整个 OOB 平面，未编程页的基线
     * OOB 从 RAM 二分查询返回，完全绕开 TF 卡随机 I/O。已编程页的
     * 内容可能已不同于镜像基线，仍走下面的文件/块预取路径。 */
    if (m->oob_sparse && !page_programmed(m, page)) {
        oob_sparse_lookup(m, page, out);
        m->stat_oob_sparse_hits++;
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
        m->stat_reads++;
        return true;
    }
    {
        FILE *f = (FILE *)m->file;
        uint32_t block = page / NP560_NAND_PAGES_PER_BLOCK;
        uint32_t in_block = page % NP560_NAND_PAGES_PER_BLOCK;
        uint8_t *oob_buf = (uint8_t *)m->oob_block_buf;

        /* OOB 块预取：块内 64 页 OOB 在文件里连续（4KB）。首次访问某块
         * 一次 fread 读整块 OOB，之后块内其余页从 RAM 拷贝。FTL 扫描按
         * 块顺序进行，命中率接近 100%——TF 卡随机读次数除以 64。 */
        if (oob_buf && m->oob_prefetch_block == block) {
            memcpy(out, oob_buf + (size_t)in_block * NP560_NAND_PAGE_OOB,
                   NP560_NAND_PAGE_OOB);
            m->stat_oob_hits++;
            xSemaphoreGive((SemaphoreHandle_t)m->lock);
            m->stat_reads++;
            return true;
        }
        if (oob_buf) {
            long block_off = file_oob_offset(m, block * NP560_NAND_PAGES_PER_BLOCK);
            size_t block_bytes = (size_t)NP560_NAND_PAGES_PER_BLOCK *
                                 NP560_NAND_PAGE_OOB;
            io_trace('O', block * NP560_NAND_PAGES_PER_BLOCK);
            ok = f && fseek(f, block_off, SEEK_SET) == 0 &&
                 fread(oob_buf, 1, block_bytes, f) == block_bytes;
            if (ok) {
                m->oob_prefetch_block = block;
                m->stat_oob_prefetch++;
                memcpy(out, oob_buf + (size_t)in_block * NP560_NAND_PAGE_OOB,
                       NP560_NAND_PAGE_OOB);
                xSemaphoreGive((SemaphoreHandle_t)m->lock);
                m->stat_reads++;
                return true;
            }
            m->stat_prefetch_fail++;
        }
        /* 预取失败（或 host 无缓存）：退化为单页读。 */
        {
            long offset = file_oob_offset(m, page);

            io_trace('o', page);
            ok = f && fseek(f, offset, SEEK_SET) == 0 &&
                 fread(out, 1, NP560_NAND_PAGE_OOB, f) == NP560_NAND_PAGE_OOB;
            if (!ok) {
                NF_FAIL_LOG("oobfile", page);
            }
            xSemaphoreGive((SemaphoreHandle_t)m->lock);
            m->stat_reads++;
            return ok;
        }
    }
#else
    FILE *f = (FILE *)m->file;
    long offset = (long)(NP560_CHK_HEADER +
                         (uint64_t)m->pages * NP560_NAND_PAGE_DATA +
                         (uint64_t)page * NP560_NAND_PAGE_OOB);

    if (!f || fseek(f, offset, SEEK_SET) != 0 ||
        fread(out, 1, NP560_NAND_PAGE_OOB, f) != NP560_NAND_PAGE_OOB) {
        return false;
    }
    m->stat_reads++;
    return true;
#endif
}

bool np560_nand_file_medium_open(Np560NandFileMedium *m, const char *path)
{
    FILE *f;
    struct stat st;
    const uint64_t expect =
        NP560_CHK_HEADER +
        (uint64_t)NP560_NAND_PAGES * NP560_NAND_PAGE_DATA +
        (uint64_t)NP560_NAND_PAGES * NP560_NAND_PAGE_OOB;
    /* 头部几何：ver/hdr/2048/64/64/4096/512MiB/16MiB（<6I2Q @8）。 */
    static const uint8_t magic[8] = { 'N', 'P', '5', '6', '0', 'N', '1', 0 };
    uint8_t hdr[64];

    memset(m, 0, sizeof(*m));
    f = fopen(path, "r+b");
    if (!f) {
        return false;
    }
    if (fstat(fileno(f), &st) != 0 || (uint64_t)st.st_size != expect ||
        fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, magic, sizeof(magic)) != 0 || hdr[8] != 1) {
        fclose(f);
        return false;
    }
    m->file = f;
    m->pages = NP560_NAND_PAGES;
#ifdef ESP_PLATFORM
    m->slots = heap_caps_malloc(sizeof(Np560NandWbSlot) * NP560_WB_SLOTS,
                                MALLOC_CAP_SPIRAM);
    m->erased_bitmap = heap_caps_malloc(NP560_ERASED_BITMAP_BYTES,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    m->prog_bitmap = heap_caps_malloc(NP560_PROG_BITMAP_BYTES,
                                      MALLOC_CAP_SPIRAM);
    m->block_buf = heap_caps_malloc(
        (size_t)NP560_NAND_PAGES_PER_BLOCK * NP560_NAND_PAGE_TOTAL,
        MALLOC_CAP_SPIRAM);
    m->oob_block_buf = heap_caps_malloc(
        (size_t)NP560_NAND_PAGES_PER_BLOCK * NP560_NAND_PAGE_OOB,
        MALLOC_CAP_SPIRAM);
    if (!m->slots || !m->erased_bitmap || !m->prog_bitmap || !m->block_buf ||
        !m->oob_block_buf) {
        free(m->slots);
        m->slots = NULL;
        free(m->prog_bitmap);
        m->prog_bitmap = NULL;
        free(m->block_buf);
        m->block_buf = NULL;
        free(m->oob_block_buf);
        m->oob_block_buf = NULL;
        fclose(f);
        m->file = NULL;
        return false;
    }
    memset(m->slots, 0, sizeof(Np560NandWbSlot) * NP560_WB_SLOTS);
    memset(m->erased_bitmap, 0, NP560_ERASED_BITMAP_BYTES);
    memset(m->prog_bitmap, 0, NP560_PROG_BITMAP_BYTES);
    m->oob_prefetch_block = 0xffffffffu;
    m->lock = (void *)xSemaphoreCreateMutex();
    m->flush_quit = false;
    if (xTaskCreate(wb_flush_task, "nand_wb", 3072, m, 2,
                    (TaskHandle_t *)&m->flush_task) != pdPASS) {
        m->flush_task = NULL;
    }
    /* 一次性顺序扫描 16MB OOB 平面建索引；此后固件引导期的数万次
     * OOB 随机读全部变成 RAM 二分查询（真机实测启动 150s+ → 数秒）。 */
    oob_sparse_build(m);
    ESP_LOGI("nand_file", "file medium open: %s, build %s %s", path,
             __DATE__, __TIME__);
#endif
    return true;
}

void np560_nand_file_medium_close(Np560NandFileMedium *m)
{
#ifdef ESP_PLATFORM
    Np560NandWbSlot *slots;
    FILE *f;

    if (!m->file) {
        return;
    }
    /* 停后台任务并把脏页全部落盘。 */
    if (m->lock) {
        xSemaphoreTake((SemaphoreHandle_t)m->lock, portMAX_DELAY);
    }
    m->flush_quit = true;
    if (m->lock) {
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
    }
    slots = (Np560NandWbSlot *)m->slots;
    f = (FILE *)m->file;
    if (slots) {
        for (uint32_t i = 0; i < NP560_WB_SLOTS; i++) {
            if (slots[i].used && slots[i].dirty) {
                file_write_raw(m, slots[i].page, slots[i].data);
                slots[i].dirty = false;
                m->stat_flushed++;
            }
        }
    }
    if (m->lock) {
        xSemaphoreTake((SemaphoreHandle_t)m->lock, portMAX_DELAY);
        xSemaphoreGive((SemaphoreHandle_t)m->lock);
        vSemaphoreDelete((SemaphoreHandle_t)m->lock);
        m->lock = NULL;
    }
    fclose(f);
    m->file = NULL;
    free(m->slots);
    m->slots = NULL;
    free(m->erased_bitmap);
    m->erased_bitmap = NULL;
    free(m->prog_bitmap);
    m->prog_bitmap = NULL;
    free(m->block_buf);
    m->block_buf = NULL;
    free(m->oob_block_buf);
    m->oob_block_buf = NULL;
    if (m->oob_sparse) {
        oob_sparse_free((OobSparse *)m->oob_sparse);
        m->oob_sparse = NULL;
    }
#else
    if (m->file) {
        fclose((FILE *)m->file);
        m->file = NULL;
    }
#endif
}

void np560_nand_file_medium_attach(Np560Nand *nand, Np560NandFileMedium *m)
{
    nand->medium.user = m;
    nand->medium.read_page = file_read_page;
    nand->medium.read_oob = file_read_oob;
    nand->medium.program_page = file_program_page;
    nand->medium.erase_block = file_erase_block;
}
