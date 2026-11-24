/*
 * np560_ecc.h - NP560+ 硬件 ECC 引擎（F20A/F20C/F20E）。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560.c:7630-7712。V3.80 的 51:C1A4 复位后向
 * F20E 连喂 256 个 16 位字再读 F20A/F20C；51:6CB8 证明这是配对奇偶
 * SEC-DED 码：新算值 XOR 存值后，F20A bits 3:0 选坏位、bits 15:4 选坏字，
 * F20C 是互补奇偶半部。三个地址是直接观测的 SoC 寄存器，与 F300 NAND
 * 命令 ABI 相互独立。
 *
 * 双视图：banked 物理 F20A 与 IGNDS unbanked 页 0x01000A0A 是同一外设
 * （np560.c:20856-20869）；banked 视图由总线 word 路由投递，unbanked 视图
 * 由总线 unbanked SFR 页分支换算后投递同一函数。
 */
#ifndef NP560_ECC_H
#define NP560_ECC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP560_ECC_BASE 0x0000f20au
#define NP560_ECC_SIZE 0x06u

typedef struct Np560Ecc {
    uint16_t parity;
    uint16_t nparity;
    uint16_t word_index;
    uint32_t reset_writes;
    uint32_t feed_writes;
} Np560Ecc;

/* 复位（np560.c:7637）：全零。 */
void np560_ecc_reset(Np560Ecc *ecc);

/* 寄存器访问（偏移 0/2/4 相对 F20A）。 */
uint16_t np560_ecc_mmio_read(Np560Ecc *ecc, uint32_t offset);
void np560_ecc_mmio_write(Np560Ecc *ecc, uint32_t offset, uint16_t value);

#ifdef __cplusplus
}
#endif

#endif /* NP560_ECC_H */
