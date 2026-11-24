/*
 * Minimal ESP-IDF shim for host regression builds only.
 * The np560_soc sources are shared between ESP32-P4 and Linux host; the
 * host runner does not need real logging or timing, just compilable stubs.
 */
#ifndef HOST_SHIM_ESP_LOG_H
#define HOST_SHIM_ESP_LOG_H

#include <stdint.h>
#include <time.h>

#define ESP_LOGE(tag, fmt, ...) do { } while (0)
#define ESP_LOGW(tag, fmt, ...) do { } while (0)
#define ESP_LOGI(tag, fmt, ...) do { } while (0)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)

/* ESP-IDF esp_timer_get_time() returns microseconds since boot as int64. */
static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

#endif
