/**
 * @file rx_temp_debug.h
 * @brief TEMPORARY RX monitor helpers — comment out RX_TEMP_DEBUG_ENABLE
 *        (or set to 0) after testing to restore the quiet production logs.
 *
 * These macros only add logging. They must not change control flow.
 */
#ifndef RX_TEMP_DEBUG_H
#define RX_TEMP_DEBUG_H

#include "esp_log.h"
#include <stdint.h>

/* --- flip this to 0 (or comment the include sites) when done testing --- */
#define RX_TEMP_DEBUG_ENABLE 1

#if RX_TEMP_DEBUG_ENABLE

#define RX_DBG_TAG "RX_DBG"

#define RX_DBG_LOGI(fmt, ...) ESP_LOGI(RX_DBG_TAG, fmt, ##__VA_ARGS__)
#define RX_DBG_LOGW(fmt, ...) ESP_LOGW(RX_DBG_TAG, fmt, ##__VA_ARGS__)

static inline void rx_dbg_mode(const char *where, int mode) {
    static const char *names[] = {"BOOTING", "PAIRING", "PLAYBACK"};
    const char *name =
        (mode >= 0 && mode < 3) ? names[mode] : "?";
    ESP_LOGI(RX_DBG_TAG, "%s: mode=%s (%d)", where, name, mode);
}

static inline void rx_dbg_recv(int mode, int data_size, const uint8_t *src,
                               int want_audio, int want_ack) {
    ESP_LOGI(RX_DBG_TAG,
             "recv mode=%d size=%d want_audio=%d want_ack=%d "
             "src=%02X:%02X:%02X:%02X:%02X:%02X",
             mode, data_size, want_audio, want_ack,
             src ? src[0] : 0, src ? src[1] : 0, src ? src[2] : 0,
             src ? src[3] : 0, src ? src[4] : 0, src ? src[5] : 0);
}

static inline void rx_dbg_link(const char *msg, uint32_t idle_ms,
                               uint32_t timeout_ms) {
    ESP_LOGW(RX_DBG_TAG, "%s idle=%lu ms (timeout=%lu ms)", msg,
             (unsigned long)idle_ms, (unsigned long)timeout_ms);
}

static inline void rx_dbg_pair_heartbeat(uint32_t n, int have_unicast) {
    ESP_LOGI(RX_DBG_TAG, "pairing heartbeat #%lu unicast=%d",
             (unsigned long)n, have_unicast);
}

#else /* RX_TEMP_DEBUG_ENABLE == 0 */

#define RX_DBG_LOGI(...) ((void)0)
#define RX_DBG_LOGW(...) ((void)0)
static inline void rx_dbg_mode(const char *where, int mode) {
    (void)where;
    (void)mode;
}
static inline void rx_dbg_recv(int mode, int data_size, const uint8_t *src,
                               int want_audio, int want_ack) {
    (void)mode;
    (void)data_size;
    (void)src;
    (void)want_audio;
    (void)want_ack;
}
static inline void rx_dbg_link(const char *msg, uint32_t idle_ms,
                               uint32_t timeout_ms) {
    (void)msg;
    (void)idle_ms;
    (void)timeout_ms;
}
static inline void rx_dbg_pair_heartbeat(uint32_t n, int have_unicast) {
    (void)n;
    (void)have_unicast;
}

#endif /* RX_TEMP_DEBUG_ENABLE */

#endif /* RX_TEMP_DEBUG_H */
