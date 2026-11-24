/*
 * np560_nand.h - NP560+ NAND 控制器模型（F300 命令 ABI）与可插拔介质层。
 *
 * 移植自 qemu-dmcu/hw/dmcu/np560.c:8114-8931：
 *   寄存器 F300..F321（word 访问），见 NP560_NAND_REG_*。
 *   命令：READ_PAGE(1) PROGRAM_PAGE(2) ERASE_BLOCK(3) READ_ID(4) COPY_BACK(5)。
 *   编程是 AND 语义（1→0），擦除整块置 FF。OOB 流可跨页（4 页 OOB 上限）。
 *   DMA 缓冲区是 32 位归一化远指针（物理字地址），字节流经总线 phys 空间；
 *   length==1 时列与缓冲区地址做 ^1 车道修正（标量探测保留 A0 行为）。
 *
 * 介质抽象：控制器不持有 544MB 阵列，经 Np560NandMedium 回调访问页数据
 * （host 用内存介质；ESP32 上接 microSD 分页介质）。QEMU 侧的 journal 是
 * 宿主持久化层（非 guest ABI），re-host 用介质自身持久化替代。
 */
#ifndef NP560_NAND_H
#define NP560_NAND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 控制器寄存器区（np560.c:62-63）。 */
#define NP560_NAND_CTRL_BASE 0x0000f300u
#define NP560_NAND_CTRL_SIZE 0x22u

/* 几何（np560.c:80-88）。 */
#define NP560_NAND_PAGE_DATA       2048u
#define NP560_NAND_PAGE_OOB        64u
#define NP560_NAND_PAGE_TOTAL      (NP560_NAND_PAGE_DATA + NP560_NAND_PAGE_OOB)
#define NP560_NAND_OOB_STREAM_MAX  (4u * NP560_NAND_PAGE_OOB)
#define NP560_NAND_PAGES_PER_BLOCK 64u
#define NP560_NAND_BLOCKS          4096u
#define NP560_NAND_PAGES           (NP560_NAND_PAGES_PER_BLOCK * NP560_NAND_BLOCKS)

/* 寄存器偏移（np560.c:2422-2438）。 */
#define NP560_NAND_REG_COMMAND        0x00
#define NP560_NAND_REG_STATUS         0x02
#define NP560_NAND_REG_PAGE_HI        0x04
#define NP560_NAND_REG_PAGE_LO        0x06
#define NP560_NAND_REG_COLUMN         0x08
#define NP560_NAND_REG_BUFFER_HI      0x0a
#define NP560_NAND_REG_BUFFER_LO      0x0c
#define NP560_NAND_REG_LENGTH         0x0e
#define NP560_NAND_REG_FLAGS          0x10
#define NP560_NAND_REG_DS0            0x12
#define NP560_NAND_REG_DS1            0x14
#define NP560_NAND_REG_ID             0x16
#define NP560_NAND_REG_PAGE_SIZE      0x18
#define NP560_NAND_REG_PAGES_PER_BLOCK 0x1a
#define NP560_NAND_REG_BLOCKS         0x1c
#define NP560_NAND_REG_ERROR          0x1e
#define NP560_NAND_REG_ID_EXT         0x20

/* 命令码（np560.c:2442-2447）。 */
#define NP560_NAND_CMD_NONE         0
#define NP560_NAND_CMD_READ_PAGE    1
#define NP560_NAND_CMD_PROGRAM_PAGE 2
#define NP560_NAND_CMD_ERASE_BLOCK  3
#define NP560_NAND_CMD_READ_ID      4
#define NP560_NAND_CMD_COPY_BACK    5

/* 错误码（np560.c:2451-2456）。 */
#define NP560_NAND_ERROR_NONE     0
#define NP560_NAND_ERROR_COMMAND  1
#define NP560_NAND_ERROR_PAGE     2
#define NP560_NAND_ERROR_RANGE    3
#define NP560_NAND_ERROR_DMA      4
#define NP560_NAND_ERROR_BACKING  5

/*
 * 可插拔介质：页粒度访问。program_page 传入 2112 字节掩码，介质按 AND
 * 语义合并（1→0）；erase_block 参数为块号（0..4095）。
 */
typedef struct Np560NandMedium {
    void *user;
    bool (*read_page)(void *user, uint32_t page, uint8_t *out);
    /* 可选：只读页尾 OOB（NP560_NAND_PAGE_OOB 字节）。NULL 时控制器
     * 回退 read_page。文件介质用它把 FTL 全盘扫描的每次 I/O 从
     * 整块 135KB 降到 1 扇区，是引导提速的关键路径。 */
    bool (*read_oob)(void *user, uint32_t page, uint8_t *out);
    bool (*program_page)(void *user, uint32_t page, const uint8_t *mask);
    bool (*erase_block)(void *user, uint32_t block);
} Np560NandMedium;

/* host 内存介质：data/oob 各为 pages*page_data / pages*page_oob 字节。 */
typedef struct Np560NandRamMedium {
    uint8_t *data;
    uint8_t *oob;
    uint32_t pages;
} Np560NandRamMedium;

void np560_nand_ram_medium_init(Np560NandRamMedium *m, uint8_t *data,
                                uint8_t *oob, uint32_t pages);

/*
 * stdio 文件介质：整片 raw 镜像（data+OOB 交错，NP560_NAND_PAGE_TOTAL
 * 字节/页顺序排列，与 QEMU system.nand checkpoint 格式一致）。host 回归
 * 与 ESP32 的 microSD 介质共用。
 */
typedef struct Np560NandFileMedium {
    void *file;             /* 实际为 FILE *（避免 stdio 泄漏进头文件）。 */
    uint32_t pages;
    /* RAM 写回缓存（ESP_PLATFORM）：编程/擦除只改 RAM，后台任务刷盘。
     * 固件一次 NAND 页编程 = 一次 TF 卡同步 I/O（~0.7s/页），没有这层
     * 缓存引导期会被卡到分钟级。host 单线程直写。 */
    void *slots;            /* Np560NandWbSlot[]（PSRAM） */
    void *block_buf;        /* 64 页块预取缓冲 135KB（PSRAM） */
    void *erased_bitmap;    /* NP560_NAND_BLOCKS/8：引导期擦除位图（零 I/O） */
    void *prog_bitmap;      /* NP560_NAND_PAGES/8：已持久化编程页 */
    void *lock;             /* SemaphoreHandle_t（ESP）/ NULL（host） */
    volatile bool flush_quit;
    void *flush_task;
    /* OOB 块预取缓存（ESP_PLATFORM）：FTL 引导扫描 95% 只读 OOB，
     * 且按块顺序。OOB 平面内一块的 64 页（64×64B=4KB）物理连续，
     * 一次 fread 整块即可让块内后续 63 次读变成纯 RAM 拷贝——把
     * ~14.7 万次 TF 卡随机读压到 ~2300 次块读（引导提速主路径）。 */
    void *oob_block_buf;         /* 一块 OOB：64×64B = 4KB（PSRAM） */
    uint32_t oob_prefetch_block; /* 已缓存块号，0xffffffff = 无效 */
    uint32_t stat_oob_prefetch;  /* 实际块预取次数 */
    uint32_t stat_oob_hits;      /* 块缓存命中次数 */
    /* 诊断统计（页单位）。 */
    uint32_t stat_reads;
    uint32_t stat_programs;
    uint32_t stat_erases;
    uint32_t stat_flushed;
    uint32_t stat_dirty;
    uint32_t stat_prefetch_fail;
    uint32_t stat_read_fails;
    uint32_t prefetch_cursor;
} Np560NandFileMedium;

/* 打开 raw 镜像文件并接到控制器；失败返回 false（errno 保留）。 */
bool np560_nand_file_medium_open(Np560NandFileMedium *m, const char *path);
void np560_nand_file_medium_close(Np560NandFileMedium *m);

struct Np560Bus;

typedef struct Np560Nand {
    Np560NandMedium medium;
    /* DMA 使用的物理总线（slot 字节流读写）。 */
    struct Np560Bus *bus;
    uint16_t command;
    uint16_t status;
    uint16_t page_hi;
    uint16_t page_lo;
    uint16_t column;
    uint16_t buffer_hi;
    uint16_t buffer_lo;
    uint16_t length;
    uint16_t flags;
    uint16_t ds0;
    uint16_t ds1;
    uint16_t error;
    /* 诊断计数（np560.c:2478-2482）。 */
    uint32_t id_reads;
    uint32_t page_reads;
    uint32_t page_programs;
    uint32_t block_erases;
    uint32_t page_copies;
} Np560Nand;

/* 复位（np560.c:8781）：寄存器清零，介质由宿主负责装载。 */
void np560_nand_reset(Np560Nand *nand);

/* F300 区 word 访问（np560.c:8867/8894）。写 COMMAND 触发执行。 */
uint16_t np560_nand_mmio_read(Np560Nand *nand, uint32_t offset);
void np560_nand_mmio_write(Np560Nand *nand, uint32_t offset, uint16_t value);

/* 把 host 内存介质接到控制器（host 回归用）。 */
void np560_nand_ram_medium_attach(Np560Nand *nand, Np560NandRamMedium *m);

/* 把 stdio 文件介质接到控制器。 */
void np560_nand_file_medium_attach(Np560Nand *nand, Np560NandFileMedium *m);

#ifdef __cplusplus
}
#endif

#endif /* NP560_NAND_H */
