/*
 * np560_nand.c - NP560+ NAND 控制器模型（F300 命令 ABI）。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560.c:8114-8931。guest 可见行为：
 *   - 寄存器 F300..F321，word 访问；写 COMMAND 立即执行。
 *   - 编程 AND 语义（1→0）；擦除整块置 FF；OOB 流可跨 4 页。
 *   - DMA 缓冲区为归一化远指针（物理字地址），字节流按 slot 寻址；
 *     length==1 时列/缓冲区做 ^1 车道修正（控制器标量探测 A0 行为）。
 *   - READ_ID 返回 Samsung EC:DC（0xDCEC），几何 0x95（ID_EXT 0x1095）。
 *
 * 诊断 trace（page3f/CAFE/FTL/caller 控制流）不移植——属 QEMU 调试设施，
 * 无 guest 可见状态。journal 为宿主持久化层，由介质实现替代。
 */
#include "np560_nand.h"
#include "dmcu_bus.h"
#include "esp_log.h"

#include <string.h>

static uint32_t nand_page(const Np560Nand *nand)
{
    return ((uint32_t)nand->page_hi << 16) | nand->page_lo;
}

static uint32_t nand_copy_destination(const Np560Nand *nand)
{
    return ((uint32_t)nand->buffer_hi << 16) | nand->buffer_lo;
}

/* 归一化 32 位远指针（np560.c:8124-8132）。 */
static uint32_t nand_buffer(const Np560Nand *nand)
{
    return (((uint32_t)nand->buffer_hi << 16) | nand->buffer_lo) &
           0x00ffffffu;
}

/* 标量探测保留列 A0 行为（np560.c:8135-8143）。 */
static uint32_t nand_dma_column(const Np560Nand *nand)
{
    return nand->length == 1 ? nand->column ^ 1u : nand->column;
}

static uint32_t nand_dma_buffer(const Np560Nand *nand)
{
    return nand_buffer(nand) ^ (nand->length == 1 ? 1u : 0u);
}

static void nand_set_failure(Np560Nand *nand, uint16_t error)
{
    nand->status = 0;
    nand->error = error;
}

static bool nand_validate_transfer(Np560Nand *nand, uint32_t page)
{
    uint32_t column = nand_dma_column(nand);
    uint32_t oob_offset;

    if (page >= NP560_NAND_PAGES) {
        nand_set_failure(nand, NP560_NAND_ERROR_PAGE);
        return false;
    }
    if (column >= NP560_NAND_PAGE_DATA) {
        uint32_t pages;

        oob_offset = column - NP560_NAND_PAGE_DATA;
        pages = (oob_offset + nand->length + NP560_NAND_PAGE_OOB - 1) /
                NP560_NAND_PAGE_OOB;

        if (oob_offset > NP560_NAND_OOB_STREAM_MAX ||
            nand->length > NP560_NAND_OOB_STREAM_MAX - oob_offset ||
            page > NP560_NAND_PAGES - pages) {
            nand_set_failure(nand, NP560_NAND_ERROR_RANGE);
            return false;
        }
        return true;
    }
    if (column > NP560_NAND_PAGE_TOTAL ||
        nand->length > NP560_NAND_PAGE_TOTAL - column) {
        nand_set_failure(nand, NP560_NAND_ERROR_RANGE);
        return false;
    }
    return true;
}

static bool nand_is_oob_stream(const Np560Nand *nand)
{
    return nand_dma_column(nand) >= NP560_NAND_PAGE_DATA;
}

/* 经介质读一页完整 2112 字节（data+oob）。 */
static bool medium_read_page(Np560Nand *nand, uint32_t page, uint8_t *out)
{
    if (!nand->medium.read_page ||
        !nand->medium.read_page(nand->medium.user, page, out)) {
        static unsigned rd_fail;

        rd_fail++;
        if (rd_fail <= 8 || rd_fail % 64 == 0) {
            ESP_LOGE("np560_nand", "medium read fail page=%u (#%u)",
                     (unsigned)page, rd_fail);
        }
        nand_set_failure(nand, NP560_NAND_ERROR_BACKING);
        return false;
    }
    return true;
}

/* OOB 跨页流读取（np560.c:8204-8221 语义）。介质支持 read_oob 时
 * 每目标页只取 64 字节 OOB（文件介质上 = 1 扇区 I/O），否则回退整页读。 */
static bool nand_read_oob_stream(Np560Nand *nand, uint32_t page, uint8_t *out)
{
    uint8_t page_bytes[NP560_NAND_PAGE_TOTAL];
    uint8_t oob_bytes[NP560_NAND_PAGE_OOB];
    uint32_t copied = 0;
    uint32_t offset = nand_dma_column(nand) - NP560_NAND_PAGE_DATA;

    while (copied < nand->length) {
        uint32_t target_page = page + offset / NP560_NAND_PAGE_OOB;
        uint32_t target_column = offset % NP560_NAND_PAGE_OOB;
        uint32_t chunk = NP560_NAND_PAGE_OOB - target_column;
        const uint8_t *oob;

        if (chunk > nand->length - copied) {
            chunk = nand->length - copied;
        }
        if (nand->medium.read_oob) {
            if (!nand->medium.read_oob(nand->medium.user, target_page,
                                       oob_bytes)) {
                static unsigned rd_oob_fail;

                rd_oob_fail++;
                if (rd_oob_fail <= 8 || rd_oob_fail % 64 == 0) {
                    ESP_LOGE("np560_nand",
                             "medium oob fail page=%u (#%u)",
                             (unsigned)target_page, rd_oob_fail);
                }
                nand_set_failure(nand, NP560_NAND_ERROR_BACKING);
                return false;
            }
            oob = oob_bytes;
        } else {
            if (!medium_read_page(nand, target_page, page_bytes)) {
                return false;
            }
            oob = page_bytes + NP560_NAND_PAGE_DATA;
        }
        memcpy(out + copied, oob + target_column, chunk);
        copied += chunk;
        offset += chunk;
    }
    return true;
}

/* OOB 跨页流编程：掩码 AND 进各页 OOB 平面。 */
static bool nand_program_oob_stream(Np560Nand *nand, uint32_t page,
                                    const uint8_t *bytes)
{
    uint8_t page_bytes[NP560_NAND_PAGE_TOTAL];
    uint32_t copied = 0;
    uint32_t offset = nand_dma_column(nand) - NP560_NAND_PAGE_DATA;

    while (copied < nand->length) {
        uint32_t target_page = page + offset / NP560_NAND_PAGE_OOB;
        uint32_t target_column = offset % NP560_NAND_PAGE_OOB;
        uint32_t chunk = NP560_NAND_PAGE_OOB - target_column;
        uint8_t *oob;
        uint32_t i;

        if (chunk > nand->length - copied) {
            chunk = nand->length - copied;
        }
        if (!medium_read_page(nand, target_page, page_bytes)) {
            return false;
        }
        oob = page_bytes + NP560_NAND_PAGE_DATA;
        for (i = 0; i < chunk; i++) {
            oob[target_column + i] &= bytes[copied + i];
        }
        if (!nand->medium.program_page(nand->medium.user, target_page,
                                       page_bytes)) {
            nand_set_failure(nand, NP560_NAND_ERROR_BACKING);
            return false;
        }
        copied += chunk;
        offset += chunk;
    }
    return true;
}

/* DMA：向 guest slot 流写 length 字节（np560.c:8531-8539）。 */
static bool nand_dma_out(Np560Nand *nand, const uint8_t *bytes)
{
    uint32_t slot = nand_dma_buffer(nand);
    uint32_t i;

    for (i = 0; i < nand->length; i++) {
        dmcu_phys_slot_write8(nand->bus, slot + i, bytes[i]);
    }
    return true;
}

/* DMA：从 guest slot 流读 length 字节。 */
static void nand_dma_in(Np560Nand *nand, uint8_t *bytes)
{
    uint32_t slot = nand_dma_buffer(nand);
    uint32_t i;

    for (i = 0; i < nand->length; i++) {
        bytes[i] = dmcu_phys_slot_read8(nand->bus, slot + i);
    }
}

static void nand_execute(Np560Nand *nand)
{
    uint8_t page_bytes[NP560_NAND_PAGE_TOTAL];
    uint8_t stream[NP560_NAND_OOB_STREAM_MAX];
    uint32_t page = nand_page(nand);
    uint32_t column = nand_dma_column(nand);

    nand->status = 0;
    nand->error = NP560_NAND_ERROR_NONE;
    switch (nand->command) {
    case NP560_NAND_CMD_READ_PAGE:
        if (!nand_validate_transfer(nand, page)) {
            return;
        }
        if (nand_is_oob_stream(nand)) {
            if (!nand_read_oob_stream(nand, page, stream)) {
                return;
            }
            nand_dma_out(nand, stream);
        } else {
            if (!medium_read_page(nand, page, page_bytes)) {
                return;
            }
            nand_dma_out(nand, page_bytes + column);
        }
        nand->page_reads++;
        nand->status = 1;
        return;

    case NP560_NAND_CMD_PROGRAM_PAGE:
        if (!nand_validate_transfer(nand, page)) {
            return;
        }
        if (nand_is_oob_stream(nand)) {
            nand_dma_in(nand, stream);
            if (!nand_program_oob_stream(nand, page, stream)) {
                return;
            }
            nand->page_programs++;
            nand->status = 1;
            return;
        }
        if (!medium_read_page(nand, page, page_bytes)) {
            return;
        }
        nand_dma_in(nand, page_bytes + column);
        /* NAND 编程只能把 1 变 0（np560.c:8669-8682）。 */
        if (!nand->medium.program_page ||
            !nand->medium.program_page(nand->medium.user, page,
                                       page_bytes)) {
            nand_set_failure(nand, NP560_NAND_ERROR_BACKING);
            return;
        }
        nand->page_programs++;
        nand->status = 1;
        return;

    case NP560_NAND_CMD_ERASE_BLOCK:
        if (page >= NP560_NAND_PAGES) {
            nand_set_failure(nand, NP560_NAND_ERROR_PAGE);
            return;
        }
        page -= page % NP560_NAND_PAGES_PER_BLOCK;
        if (!nand->medium.erase_block ||
            !nand->medium.erase_block(nand->medium.user,
                                      page / NP560_NAND_PAGES_PER_BLOCK)) {
            nand_set_failure(nand, NP560_NAND_ERROR_BACKING);
            return;
        }
        nand->block_erases++;
        nand->status = 1;
        return;

    case NP560_NAND_CMD_READ_ID:
        nand->id_reads++;
        nand->status = 1;
        return;

    case NP560_NAND_CMD_COPY_BACK: {
        uint32_t destination = nand_copy_destination(nand);

        if (!nand_validate_transfer(nand, page)) {
            return;
        }
        if (destination >= NP560_NAND_PAGES) {
            nand_set_failure(nand, NP560_NAND_ERROR_PAGE);
            return;
        }
        if (!medium_read_page(nand, page, page_bytes)) {
            return;
        }
        /* Copy-back 仍是编程：仅 1→0（np560.c:8756-8768）。 */
        if (!nand->medium.program_page ||
            !nand->medium.program_page(nand->medium.user, destination,
                                       page_bytes)) {
            nand_set_failure(nand, NP560_NAND_ERROR_BACKING);
            return;
        }
        nand->page_reads++;
        nand->page_programs++;
        nand->page_copies++;
        nand->status = 1;
        return;
    }
    default:
        nand_set_failure(nand, NP560_NAND_ERROR_COMMAND);
        return;
    }
}

void np560_nand_reset(Np560Nand *nand)
{
    nand->command = NP560_NAND_CMD_NONE;
    nand->status = 0;
    nand->page_hi = 0;
    nand->page_lo = 0;
    nand->column = 0;
    nand->buffer_hi = 0;
    nand->buffer_lo = 0;
    nand->length = 0;
    nand->flags = 0;
    nand->ds0 = 0;
    nand->ds1 = 0;
    nand->error = NP560_NAND_ERROR_NONE;
    nand->id_reads = 0;
    nand->page_reads = 0;
    nand->page_programs = 0;
    nand->block_erases = 0;
    nand->page_copies = 0;
}

uint16_t np560_nand_mmio_read(Np560Nand *nand, uint32_t offset)
{
    switch (offset) {
    case NP560_NAND_REG_COMMAND:        return nand->command;
    case NP560_NAND_REG_STATUS:         return nand->status;
    case NP560_NAND_REG_PAGE_HI:        return nand->page_hi;
    case NP560_NAND_REG_PAGE_LO:        return nand->page_lo;
    case NP560_NAND_REG_COLUMN:         return nand->column;
    case NP560_NAND_REG_BUFFER_HI:      return nand->buffer_hi;
    case NP560_NAND_REG_BUFFER_LO:      return nand->buffer_lo;
    case NP560_NAND_REG_LENGTH:         return nand->length;
    case NP560_NAND_REG_FLAGS:          return nand->flags;
    case NP560_NAND_REG_DS0:            return nand->ds0;
    case NP560_NAND_REG_DS1:            return nand->ds1;
    case NP560_NAND_REG_ID:             return 0xdcec; /* Samsung EC:DC。 */
    case NP560_NAND_REG_PAGE_SIZE:      return NP560_NAND_PAGE_TOTAL;
    case NP560_NAND_REG_PAGES_PER_BLOCK: return NP560_NAND_PAGES_PER_BLOCK;
    case NP560_NAND_REG_BLOCKS:         return NP560_NAND_BLOCKS;
    case NP560_NAND_REG_ERROR:          return nand->error;
    /* BB1E 在其高字低字节返回几何字节。 */
    case NP560_NAND_REG_ID_EXT:         return 0x1095; /* 几何 0x95。 */
    default:                            return 0;
    }
}

void np560_nand_mmio_write(Np560Nand *nand, uint32_t offset, uint16_t value)
{
    switch (offset) {
    case NP560_NAND_REG_COMMAND:
        nand->command = value;
        nand_execute(nand);
        break;
    case NP560_NAND_REG_PAGE_HI:   nand->page_hi = value; break;
    case NP560_NAND_REG_PAGE_LO:   nand->page_lo = value; break;
    case NP560_NAND_REG_COLUMN:    nand->column = value; break;
    case NP560_NAND_REG_BUFFER_HI: nand->buffer_hi = value; break;
    case NP560_NAND_REG_BUFFER_LO: nand->buffer_lo = value; break;
    case NP560_NAND_REG_LENGTH:    nand->length = value; break;
    case NP560_NAND_REG_FLAGS:     nand->flags = value; break;
    case NP560_NAND_REG_DS0:       nand->ds0 = value; break;
    case NP560_NAND_REG_DS1:       nand->ds1 = value; break;
    default:
        break;
    }
}

/* ---- host 内存介质 ---- */

void np560_nand_ram_medium_init(Np560NandRamMedium *m, uint8_t *data,
                                uint8_t *oob, uint32_t pages)
{
    m->data = data;
    m->oob = oob;
    m->pages = pages;
}

static bool ram_read_page(void *user, uint32_t page, uint8_t *out)
{
    Np560NandRamMedium *m = user;

    if (page >= m->pages) {
        return false;
    }
    memcpy(out, m->data + (size_t)page * NP560_NAND_PAGE_DATA,
           NP560_NAND_PAGE_DATA);
    memcpy(out + NP560_NAND_PAGE_DATA,
           m->oob + (size_t)page * NP560_NAND_PAGE_OOB,
           NP560_NAND_PAGE_OOB);
    return true;
}

static bool ram_program_page(void *user, uint32_t page, const uint8_t *mask)
{
    Np560NandRamMedium *m = user;
    uint32_t i;

    if (page >= m->pages) {
        return false;
    }
    for (i = 0; i < NP560_NAND_PAGE_DATA; i++) {
        m->data[(size_t)page * NP560_NAND_PAGE_DATA + i] &= mask[i];
    }
    for (i = 0; i < NP560_NAND_PAGE_OOB; i++) {
        m->oob[(size_t)page * NP560_NAND_PAGE_OOB + i] &=
            mask[NP560_NAND_PAGE_DATA + i];
    }
    return true;
}

static bool ram_erase_block(void *user, uint32_t block)
{
    Np560NandRamMedium *m = user;
    uint32_t page = block * NP560_NAND_PAGES_PER_BLOCK;

    if (page >= m->pages) {
        return false;
    }
    memset(m->data + (size_t)page * NP560_NAND_PAGE_DATA, 0xff,
           NP560_NAND_PAGES_PER_BLOCK * NP560_NAND_PAGE_DATA);
    memset(m->oob + (size_t)page * NP560_NAND_PAGE_OOB, 0xff,
           NP560_NAND_PAGES_PER_BLOCK * NP560_NAND_PAGE_OOB);
    return true;
}

static bool ram_read_oob(void *user, uint32_t page, uint8_t *out)
{
    Np560NandRamMedium *m = user;

    if (page >= m->pages) {
        return false;
    }
    memcpy(out, m->oob + (size_t)page * NP560_NAND_PAGE_OOB,
           NP560_NAND_PAGE_OOB);
    return true;
}

void np560_nand_ram_medium_attach(Np560Nand *nand, Np560NandRamMedium *m)
{
    nand->medium.user = m;
    nand->medium.read_page = ram_read_page;
    nand->medium.read_oob = ram_read_oob;
    nand->medium.program_page = ram_program_page;
    nand->medium.erase_block = ram_erase_block;
}
