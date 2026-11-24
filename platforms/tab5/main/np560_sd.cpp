/*
 * np560_sd.cpp - NP560+ 模拟器的 microSD 存储层（ESP32 侧）。
 *
 * TF 卡约定（用户要求）：文件一律放 /sd/noahnp560/（不放根目录）。
 *   NP560.PRG      4MB    -> data 平面 phys 0x30000（firmware-vma03，
 *                            QEMU np560.c:22624 memcpy(data+VMA03, program)）
 *   resident.bin   2MB    -> code 平面 SoC ROM 0x0（soc-rom，np560.c:21021）
 *   system.nand.gz 66MB   -> 首次流式解压为 system.nand（528MiB raw，
 *                            data+OOB 2112B/页交错，QEMU checkpoint 格式）
 *
 * 启动语义对齐 QEMU portable（Start-NP560.ps1 权威配置）：
 *   boot-mode=firmware-vma03 --soc-rom resident.bin --soc-rom-skeleton
 *   reset PC = 0x30000（np560.c:22838），SP=0xfffe、DS1=1（cpu.c:497-500）。
 */
#include "np560_sd.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/m5stack_tab5.h"

#include <sys/stat.h>

extern "C" {
#include "dmcu_bus.h"
#include "np560_nand.h"
}

#if __has_include("miniz.h")
#include "miniz.h"
#define HAVE_TINFL 1
#else
#define HAVE_TINFL 0
#endif

static const char *TAG = "np560_sd";

#define SD_BASE_DIR    "/sd/noahnp560"
#define ROM_LOAD_MAX   0x00200000u  /* resident.bin = 2MB SoC ROM */
#define PRG_LOAD_ADDR  0x00030000u  /* firmware-vma03 链接基址 */
#define PRG_MAX        0x00400000u  /* DMCU_PROGRAM_SIZE */
#define ROM_DATA_ALIAS 0x00010000u  /* data 平面 ROM 镜像区（np560.c:21037） */
#define NAND_RAW_PATH  SD_BASE_DIR "/system.nand"
#define NAND_GZ_PATH   SD_BASE_DIR "/system.nand.gz"

static Np560NandFileMedium s_nand_file;

bool np560_sd_mount()
{
    esp_err_t err = bsp_sdcard_init((char *)"/sd", 8);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SD mounted at /sd");
    return true;
}

/* 首次启动：把 system.nand.gz 流式解压为 system.nand（gzip -> raw deflate）。 */
static bool ensure_nand_image()
{
    FILE *out;
    struct stat st;

    if (stat(NAND_RAW_PATH, &st) == 0 &&
        (uint64_t)st.st_size >=
            (uint64_t)NP560_NAND_PAGES * NP560_NAND_PAGE_TOTAL) {
        ESP_LOGI(TAG, "NAND image ready (%llu bytes)",
                 (unsigned long long)st.st_size);
        return true;
    }

    FILE *gz = fopen(NAND_GZ_PATH, "rb");
    if (!gz) {
        ESP_LOGE(TAG, "missing %s", NAND_GZ_PATH);
        return false;
    }
    if (stat(NAND_RAW_PATH, &st) == 0) {
        /* raw 镜像不完整（上次解压中断/掉电残留）。gz 是权威源，安全起见
         * 直接删除残片并重新解压（原设计要求手动删除，现改为自动）。 */
        ESP_LOGW(TAG, "partial %s exists (%llu bytes); removing and "
                 "re-decompressing", NAND_RAW_PATH,
                 (unsigned long long)st.st_size);
        remove(NAND_RAW_PATH);
    }

    /* 解析 gzip 头（RFC 1952）：10 字节固定 + 可选字段。 */
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, gz) != 10 || hdr[0] != 0x1f || hdr[1] != 0x8b ||
        hdr[2] != 8) {
        ESP_LOGE(TAG, "bad gzip header in %s", NAND_GZ_PATH);
        fclose(gz);
        return false;
    }
    uint8_t flg = hdr[3];
    if (flg & 0x04) {   /* FEXTRA */
        uint8_t xb[2];
        if (fread(xb, 1, 2, gz) != 2) goto bad;
        unsigned xlen = xb[0] | (xb[1] << 8);
        if (fseek(gz, xlen, SEEK_CUR) != 0) goto bad;
    }
    if (flg & 0x08) {   /* FNAME */
        int c;
        do { c = fgetc(gz); } while (c > 0);
    }
    if (flg & 0x10) {   /* FCOMMENT */
        int c;
        do { c = fgetc(gz); } while (c > 0);
    }
    if (flg & 0x02) {   /* FHCRC */
        if (fseek(gz, 2, SEEK_CUR) != 0) goto bad;
    }

    out = fopen(NAND_RAW_PATH, "wb");
    if (!out) {
        ESP_LOGE(TAG, "cannot create %s", NAND_RAW_PATH);
        fclose(gz);
        return false;
    }

#if HAVE_TINFL
    {
        /* tinfl 流式解压：32KB 输入块 -> 32KB 环形字典输出写文件。 */
        const size_t IN_SZ = 32 * 1024;
        uint8_t *in_buf = (uint8_t *)malloc(IN_SZ);
        tinfl_decompressor *infl =
            (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
        uint64_t written = 0;
        uint64_t expect = (uint64_t)NP560_NAND_PAGES * NP560_NAND_PAGE_TOTAL;
        bool ok = in_buf && infl;

        if (ok) {
            /* tinfl 流式契约（对齐 miniz tinfl_decompress_mem_to_callback）：
             * 1) 输出用 32KB 环形字典（wrapping 模式），LZ 回引历史由 tinfl
             *    内部字典维护；绝不能用 NON_WRAPPING + 每次换新缓冲——
             *    non-wrapping 模式下 tinfl 直接从输出缓冲读回引历史，
             *    换缓冲后 dist > dist_from_out_buf_start 立即 FAILED。
             * 2) 输入端契约（对齐 mz_inflate）：调用前 *pIn_buf_size = 可用
             *    字节数，返回后 = 已消费字节数。必须 avail -= consumed 再把
             *    未消费尾部 memmove 到缓冲区头部续填。（曾误把返回值当
             *    "未消费"处理，把已消费数据重新喂回解码器导致流失步。） */
            uint8_t *dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
            ok = dict != NULL;
            size_t dict_ofs = 0;
            size_t in_rem = 0;
            bool eof = false;
            if (ok) {
                in_rem = fread(in_buf, 1, IN_SZ, gz);
                eof = in_rem < IN_SZ;
                tinfl_init(infl);
            }
            for (; ok;) {
                size_t avail = in_rem;
                size_t dst_size = TINFL_LZ_DICT_SIZE - dict_ofs;
                uint32_t flags = 0;
                if (!eof) {
                    flags |= TINFL_FLAG_HAS_MORE_INPUT;
                }
                tinfl_status stt = tinfl_decompress(
                    infl, in_buf, &avail,
                    dict, dict + dict_ofs, &dst_size, flags);
                size_t consumed = avail;    /* 退出值 = 已消费字节数 */
                in_rem -= consumed;
                if (in_rem) {
                    memmove(in_buf, in_buf + consumed, in_rem);
                }
                if (dst_size) {
                    if (fwrite(dict + dict_ofs, 1, dst_size, out) != dst_size) {
                        ok = false;
                        break;
                    }
                    written += dst_size;
                    if ((written & 0x1FFFFFFu) < TINFL_LZ_DICT_SIZE) {
                        ESP_LOGI(TAG, "nand decompress %llu/%llu MiB",
                                 (unsigned long long)(written >> 20),
                                 (unsigned long long)(expect >> 20));
                    }
                }
                dict_ofs = (dict_ofs + dst_size) & (TINFL_LZ_DICT_SIZE - 1);
                if (stt == TINFL_STATUS_DONE) {
                    break;
                }
                if (stt < TINFL_STATUS_DONE) {
                    ESP_LOGE(TAG, "inflate failed status=%d @%llu", stt,
                             (unsigned long long)written);
                    ok = false;
                    break;
                }
                /* NEEDS_MORE_INPUT / HAS_MORE_OUTPUT：续填输入 */
                if (in_rem == 0) {
                    if (eof) {
                        ESP_LOGE(TAG, "truncated gzip stream @%llu",
                                 (unsigned long long)written);
                        ok = false;
                        break;
                    }
                    in_rem = fread(in_buf, 1, IN_SZ, gz);
                    if (in_rem < IN_SZ) {
                        eof = true;
                    }
                    if (in_rem == 0) {
                        ESP_LOGE(TAG, "gzip input exhausted before DONE");
                        ok = false;
                        break;
                    }
                } else if (!eof && in_rem < IN_SZ) {
                    size_t got = fread(in_buf + in_rem, 1, IN_SZ - in_rem, gz);
                    if (in_rem + got < IN_SZ) {
                        eof = true;
                    }
                    in_rem += got;
                }
                /* 镜像尾部有 +64 字节（gzip -l 实测 553648192 = 528MiB+64） */
                if (written > expect + NP560_NAND_PAGE_OOB) {
                    ESP_LOGE(TAG, "output exceeds expected NAND size");
                    ok = false;
                    break;
                }
            }
            free(dict);
        }
        free(in_buf);
        free(infl);
        fclose(gz);
        fclose(out);
        if (!ok) {
            remove(NAND_RAW_PATH);
            return false;
        }
        if (written < expect) {
            ESP_LOGE(TAG, "short image: %llu < %llu",
                     (unsigned long long)written, (unsigned long long)expect);
            remove(NAND_RAW_PATH);
            return false;
        }
        ESP_LOGI(TAG, "NAND image ready (%llu bytes)",
                 (unsigned long long)written);
        return true;
    }
#else
    ESP_LOGE(TAG, "miniz/tinfl unavailable; cannot decompress %s",
             NAND_GZ_PATH);
    fclose(gz);
    fclose(out);
    remove(NAND_RAW_PATH);
    return false;
#endif

bad:
    ESP_LOGE(TAG, "gzip header parse failed");
    fclose(gz);
    return false;
}

/* 读整个文件到 PSRAM 缓冲（上限 max）。返回实际字节数，失败返回 0。 */
static uint32_t read_file(const char *path, uint8_t **out, uint32_t max)
{
    FILE *f = fopen(path, "rb");
    struct stat st;

    *out = NULL;
    if (!f || stat(path, &st) != 0 || (uint64_t)st.st_size > max) {
        if (f) {
            fclose(f);
        }
        ESP_LOGE(TAG, "cannot read %s", path);
        return 0;
    }
    uint32_t size = (uint32_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf || fread(buf, 1, size, f) != size) {
        free(buf);
        fclose(f);
        ESP_LOGE(TAG, "read %s failed", path);
        return 0;
    }
    fclose(f);
    *out = buf;
    return size;
}

bool np560_sd_load_firmware(Np560Bus *bus)
{
    uint8_t *prg = NULL;
    uint8_t *rom = NULL;
    uint32_t prg_size, rom_size;
    bool ok = false;

    ESP_LOGI(TAG, "psram free=%u largest=%u before firmware load",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    if (!ensure_nand_image()) {
        return false;
    }

    prg_size = read_file(SD_BASE_DIR "/NP560.PRG", &prg, PRG_MAX);
    if (!prg_size) {
        return false;
    }
    rom_size = read_file(SD_BASE_DIR "/resident.bin", &rom, ROM_LOAD_MAX);
    if (!rom_size) {
        free(prg);
        return false;
    }

    do {
        /* data 平面：PRG 装到 vma03 基址 0x30000（np560.c:22624）。 */
        np560_bus_load(bus, PRG_LOAD_ADDR, prg, prg_size);
        /* data 平面 ROM 镜像：0x10000..0x2FFFF（data_soc_rom_alias，
         * np560.c:21037；其上的 menu/init-source 小 alias 内容一致）。 */
        uint32_t alias_end = rom_size < PRG_LOAD_ADDR ? rom_size
                                                      : PRG_LOAD_ADDR;
        if (alias_end > ROM_DATA_ALIAS) {
            np560_bus_load(bus, ROM_DATA_ALIAS, rom + ROM_DATA_ALIAS,
                           alias_end - ROM_DATA_ALIAS);
        }
        /* code 平面：resident.bin 全量 SoC ROM（0x0..2MB）。
         * take 语义：rom 缓冲所有权直接移交 bus，避免再复制 2MB
         * （16MB phys + 4MB PRG 临时 + 2MB ROM 之外，PSRAM 连续块
         * 已可能不够再一次 2MB 拷贝分配）。 */
        np560_bus_take_code_rom(bus, rom, rom_size);
        rom = NULL;             /* 所有权已转移，结尾 free(NULL) 安全 */
        ESP_LOGI(TAG, "firmware loaded: PRG %uKB @0x%x, ROM %uKB, "
                 "reset PC=0x%x", (unsigned)(prg_size / 1024),
                 (unsigned)PRG_LOAD_ADDR, (unsigned)(rom_size / 1024),
                 (unsigned)PRG_LOAD_ADDR);
        ok = true;
    } while (0);

    free(prg);
    free(rom);
    return ok;
}

bool np560_sd_attach_nand(Np560Nand *nand)
{
    if (!np560_nand_file_medium_open(&s_nand_file, NAND_RAW_PATH)) {
        ESP_LOGE(TAG, "cannot open %s", NAND_RAW_PATH);
        return false;
    }
    np560_nand_file_medium_attach(nand, &s_nand_file);
    ESP_LOGI(TAG, "NAND medium attached (%s)", NAND_RAW_PATH);
    return true;
}
