/*
 * np560_ecc.c - NP560+ 硬件 ECC 引擎（F20A/F20C/F20E）。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560.c:7630-7712：置位的数据位把其二进制
 * 位置贡献给 Parity、互补位置贡献给 NParity；单个位翻转因此在 Parity
 * 综合征里留下精确的 (word << 4) | bit 地址，并使每个配对综合征位相异
 * （51:6E42..51:6ED2 实证）。12 位字计数为 16 位码全宽；NP560 的
 * 512 字节扇区只用到索引 0..255。
 */
#include "np560_ecc.h"

void np560_ecc_reset(Np560Ecc *ecc)
{
    ecc->parity = 0;
    ecc->nparity = 0;
    ecc->word_index = 0;
    ecc->reset_writes = 0;
    ecc->feed_writes = 0;
}

static void ecc_clear(Np560Ecc *ecc)
{
    ecc->parity = 0;
    ecc->nparity = 0;
    ecc->word_index = 0;
}

static void ecc_feed(Np560Ecc *ecc, uint16_t value)
{
    unsigned bit;

    for (bit = 0; bit < 16; bit++) {
        if (value & (1u << bit)) {
            uint16_t position = (uint16_t)((ecc->word_index << 4) | bit);

            ecc->parity ^= position;
            ecc->nparity ^= (uint16_t)~position;
        }
    }
    ecc->word_index = (ecc->word_index + 1) & 0x0fff;
    ecc->feed_writes++;
}

uint16_t np560_ecc_mmio_read(Np560Ecc *ecc, uint32_t offset)
{
    switch (offset) {
    case 0x00: return ecc->parity;
    case 0x02: return ecc->nparity;
    default:   return 0;
    }
}

void np560_ecc_mmio_write(Np560Ecc *ecc, uint32_t offset, uint16_t value)
{
    switch (offset) {
    case 0x00:
        /* V3.80 写零复位两个奇偶半部与索引。 */
        ecc_clear(ecc);
        ecc->reset_writes++;
        break;
    case 0x04:
        ecc_feed(ecc, value);
        break;
    default:
        break;
    }
}
