/* host 构建占位：ESP32 的 IRAM_ATTR/DRAM_ATTR 在 host 上为空。 */
#ifndef HOST_SHIM_ESP_ATTR_H
#define HOST_SHIM_ESP_ATTR_H
#define IRAM_ATTR
#define DRAM_ATTR
#define IRAM_NOINIT_ATTR
#endif
