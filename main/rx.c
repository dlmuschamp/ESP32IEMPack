/**
 * @file rx.c
 * @brief IEM receiver (RX pack): pairs with a TX over ESP-NOW, then plays
 *        streamed PCM on a PCM5102 DAC via I2S.
 *
 * Boot → pair (broadcast alias) → audio playback on core 1. ESP-NOW callbacks
 * only enqueue / update mode; I2S writes stay off the Wi-Fi task. On link loss
 * the pack returns to pairing and flushes the audio queue.
 */

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "hal/i2s_types.h"
#include "portmacro.h"
#include "rig_shared.h"
#include "rx_temp_debug.h" /* TEMP: set RX_TEMP_DEBUG_ENABLE 0 when done */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// --- HARDWARE ---
#define LRCK_PIN 32
#define BCK_PIN 33
#define DOUT_PIN 25

// --- TIMING / BUFFERS ---
// Pairing broadcast interval while unpaired.
#define PAIRING_INTERVAL_MS 200
// Link-loss poll period while playing audio.
#define LINK_CHECK_INTERVAL_MS 100
// No audio packet for this long → drop back to pairing.
#define CONNECTION_TIMEOUT_MS 2000
// Pointer-pool jitter buffer (queues hold pointers, not full packets).
// 12 × ~7.5 ms ≈ 90 ms worst case; drop-oldest keeps latency bounded.
#define NUM_QUEUE_SLOTS 12
// Start playback after this many packets so the first I2S write doesn't underrun.
#define PLAYBACK_PREBUFFER_PACKETS 3
// Match one audio packet per DMA descriptor for low I2S path delay.
#define I2S_DMA_DESC_NUM 4
#define I2S_DMA_FRAME_NUM AUDIO_FRAMES_PER_PACKET
// Above Wi-Fi callbacks enough to drain the queue under burst load.
#define AUDIO_TASK_PRIO 12
#define AUDIO_TASK_CORE 1
#define AUDIO_TASK_STOP_WAIT_MS 500
// How often to print jitter-queue drop totals while playing.
#define DROP_LOG_INTERVAL_MS 2000

// --- MODES ---
typedef enum {
    RX_MODE_BOOTING,
    RX_MODE_PAIRING,
    RX_MODE_AUDIO_PLAYBACK
} rx_mode_t;

// --- GLOBALS ---
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                        0xFF, 0xFF, 0xFF};

static i2s_chan_handle_t i2s_dac_chan;
// Pool + pointer queues: Wi-Fi task only memcpy's once; queues move pointers.
static audio_packet_t packet_pool[NUM_QUEUE_SLOTS];
static QueueHandle_t free_q;
static QueueHandle_t filled_q;
static char unique_alias[ALIAS_BUFFER_SIZE] = {0};
static uint32_t queue_drops = 0;
static bool count_queue_drops = false;
// Last TX that ACKed us — used for unicast re-pair (broadcast is easy to miss
// while TX is blasting audio).
static uint8_t paired_tx_mac[ESP_NOW_ETH_ALEN] = {0};
static bool have_paired_tx = false;

// Updated from ESP-NOW recv callback; read on the app_main state machine.
static volatile rx_mode_t cur_mode = RX_MODE_BOOTING;
static volatile TickType_t last_packet_time = 0;
static volatile bool audio_task_stop = false;
static TaskHandle_t audio_task_handle = NULL;

// --- LOG TAGS ---
static const char *INIT = "RX_INIT";
static const char *PAIR = "RX_PAIR";
static const char *AUDIO = "RX_AUDIO";
static const char *CB = "RX_CB";

// --- INIT ---

/**
 * @brief Build alias IEM_RX_XX:XX:XX from STA MAC octets 3–5.
 * @return ESP_OK, or the esp_err_t from esp_wifi_get_mac on failure.
 */
static esp_err_t init_unique_alias(void) {
    uint8_t mac[MAC_ADDRESS_LEN];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to read STA MAC for alias.");
        return err;
    }

    snprintf(unique_alias, sizeof(unique_alias), "%s%02X:%02X:%02X",
             BASE_RX_ALIAS, mac[3], mac[4], mac[5]);
    ESP_LOGI(INIT, "Unique alias: %s", unique_alias);
    return ESP_OK;
}

/**
 * @brief Register the ESP-NOW broadcast peer (required before broadcast send).
 * @return ESP_OK on success, or the first failing ESP-NOW esp_err_t.
 */
static esp_err_t init_broadcast_peer(void) {
    if (esp_now_is_peer_exist(BROADCAST_MAC)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {
        .channel = DEFAULT_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to add ESP-NOW broadcast peer.");
        return err;
    }
    return ESP_OK;
}

/**
 * @brief I2S master TX to PCM5102 (DOUT only; simplex RX pack).
 *        Native 16-bit Philips slots (PCM5102-friendly). Left disabled until
 *        playback so BCK/LRCK don't click in the IEMs during pairing.
 * @return ESP_OK on success, or the first failing I2S esp_err_t.
 */
static esp_err_t init_i2s_pcm5102(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;

    esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_dac_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to create I2S DAC channel.");
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = BCK_PIN,
                .ws = LRCK_PIN,
                .dout = DOUT_PIN,
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    // Classic ESP32 defaults msb_right=true for ≤16-bit, which often dulls /
    // smears HF into PCM5102. Force MSB-first alignment.
#if SOC_I2S_HW_VERSION_1
    std_cfg.slot_cfg.msb_right = false;
#endif
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(i2s_dac_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to init I2S DAC std mode.");
        return err;
    }
    // Intentionally not enabled here — enable in run_audio_playback_state().
    return ESP_OK;
}

static esp_err_t enable_i2s_dac(void) {
    esp_err_t err = i2s_channel_enable(i2s_dac_chan);
    // Already-enabled is fine on re-enter after a partial teardown.
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(AUDIO, "Failed to enable I2S DAC channel.");
    }
    return err;
}

static void disable_i2s_dac(void) {
    if (!i2s_dac_chan) {
        return;
    }
    esp_err_t err = i2s_channel_disable(i2s_dac_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(AUDIO, "Failed to disable I2S DAC: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Create free/filled pointer queues and stock the free pool.
 * @return ESP_OK, or ESP_ERR_NO_MEM if creation fails.
 */
static esp_err_t init_audio_queue(void) {
    free_q = xQueueCreate(NUM_QUEUE_SLOTS, sizeof(audio_packet_t *));
    filled_q = xQueueCreate(NUM_QUEUE_SLOTS, sizeof(audio_packet_t *));
    if (!free_q || !filled_q) {
        ESP_LOGE(INIT, "Failed to create audio pointer queues.");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < NUM_QUEUE_SLOTS; i++) {
        audio_packet_t *slot = &packet_pool[i];
        if (xQueueSend(free_q, &slot, 0) != pdTRUE) {
            ESP_LOGE(INIT, "Failed to stock free packet pool.");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

// --- HELPERS ---

/** @brief Return all filled slots to the free pool (disconnect / re-pair). */
static void flush_audio_queue(void) {
    if (!filled_q || !free_q) {
        return;
    }
    audio_packet_t *slot = NULL;
    while (xQueueReceive(filled_q, &slot, 0) == pdTRUE) {
        xQueueSend(free_q, &slot, 0);
    }
}

/**
 * @brief Remember TX MAC and ensure it is an ESP-NOW peer for unicast re-pair.
 */
static void remember_tx_peer(const uint8_t *tx_mac) {
    if (!tx_mac) {
        return;
    }
    memcpy(paired_tx_mac, tx_mac, ESP_NOW_ETH_ALEN);
    have_paired_tx = true;

    if (esp_now_is_peer_exist(paired_tx_mac)) {
        return;
    }

    esp_now_peer_info_t peer = {
        .channel = DEFAULT_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, paired_tx_mac, ESP_NOW_ETH_ALEN);
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(PAIR, "Failed to add TX peer for unicast re-pair: %s",
                 esp_err_to_name(err));
        have_paired_tx = false;
    }
}

/**
 * @brief Apply TX pairing ACK/NACK. Only flips mode here — peer add is deferred
 *        so we do not call esp_now_add_peer from the Wi-Fi recv task.
 */
static void handle_pairing_result(const uint8_t *data, const uint8_t *tx_mac) {
    const pair_state_t result = (pair_state_t)data[0];

    if (result == PAIR_SUCCESS) {
        if (tx_mac) {
            memcpy(paired_tx_mac, tx_mac, ESP_NOW_ETH_ALEN);
            have_paired_tx = true;
        }
        ESP_LOGI(PAIR, "Paired with TX; entering audio playback.");
        rx_dbg_mode("PAIR_SUCCESS", RX_MODE_AUDIO_PLAYBACK);
        last_packet_time = xTaskGetTickCount();
        cur_mode = RX_MODE_AUDIO_PLAYBACK;
        return;
    }

    ESP_LOGW(PAIR, "Pairing failed; will keep advertising.");
    cur_mode = RX_MODE_PAIRING;
}

/**
 * @brief Soft join/re-pair from live audio while advertising.
 *
 * Evidence from bench (RX monitor): TX was already streaming correctly-sized
 * audio (1440 B) while RX waited for PAIR_SUCCESS. Soft reconnect previously
 * required have_paired_tx, so first boot ignored the stream forever when the
 * ACK was lost/starved. ESP-IDF notes app-layer proof of delivery; a matching
 * audio_packet_t is that proof for this single-TX IEM.
 *
 * Only flips mode — do not enqueue here (playback entry flushes the queue).
 *
 * @return true if we entered playback.
 */
static bool try_soft_reconnect_from_audio(const uint8_t *src_mac) {
    if (!src_mac) {
        return false;
    }

    if (have_paired_tx &&
        memcmp(src_mac, paired_tx_mac, ESP_NOW_ETH_ALEN) != 0) {
        RX_DBG_LOGW("audio while pairing from unknown TX; ignoring");
        return false;
    }

    if (!have_paired_tx) {
        memcpy(paired_tx_mac, src_mac, ESP_NOW_ETH_ALEN);
        have_paired_tx = true;
        ESP_LOGI(PAIR,
                 "First-pair soft join from audio "
                 "%02X:%02X:%02X:%02X:%02X:%02X (ACK missed/starved).",
                 src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4],
                 src_mac[5]);
    } else {
        ESP_LOGI(PAIR, "Soft reconnect: audio from known TX while pairing.");
    }

    rx_dbg_mode("SOFT_JOIN", RX_MODE_AUDIO_PLAYBACK);
    last_packet_time = xTaskGetTickCount();
    cur_mode = RX_MODE_AUDIO_PLAYBACK;
    return true;
}

/**
 * @brief Copy one audio packet into a pool slot and hand it to playback.
 *        If no free slot, steal the oldest filled slot (stay realtime).
 * @param audio_packet Pointer to an audio_packet_t payload.
 */
static void handle_audio_packet(const uint8_t *audio_packet) {
    if (!free_q || !filled_q || !audio_packet) {
        return;
    }

    last_packet_time = xTaskGetTickCount();

    audio_packet_t *slot = NULL;
    if (xQueueReceive(free_q, &slot, 0) != pdTRUE) {
        // Pool empty: drop oldest filled packet and reuse its slot.
        if (xQueueReceive(filled_q, &slot, 0) != pdTRUE) {
            return;
        }
        if (count_queue_drops) {
            queue_drops++;
        }
    }

    memcpy(slot, audio_packet, sizeof(*slot));
    if (xQueueSend(filled_q, &slot, 0) != pdTRUE) {
        // Should be rare; return slot to free pool.
        xQueueSend(free_q, &slot, 0);
        if (count_queue_drops) {
            queue_drops++;
        }
    }
}

// --- TASKS ---

/**
 * @brief Core-1 consumer: filled pool → I2S DAC. Exits when audio_task_stop set.
 */
static void audio_playback_task(void *pvParameters) {
    (void)pvParameters;
    audio_packet_t *slot = NULL;
    static int16_t silence[AUDIO_DATA_NUM_SAMPLES];
    size_t bytes_written;
    TickType_t last_drop_log = xTaskGetTickCount();
    uint32_t last_logged_drops = 0;
    bool primed = false;

    memset(silence, 0, sizeof(silence));

    while (!audio_task_stop) {
        if (!primed) {
            if (uxQueueMessagesWaiting(filled_q) < PLAYBACK_PREBUFFER_PACKETS) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            primed = true;
            count_queue_drops = true;
            ESP_LOGI(AUDIO, "Playback primed (%d packets). depth=%u",
                     PLAYBACK_PREBUFFER_PACKETS,
                     (unsigned)uxQueueMessagesWaiting(filled_q));
        }

        if (xQueueReceive(filled_q, &slot, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (audio_task_stop) {
                xQueueSend(free_q, &slot, 0);
                break;
            }

            esp_err_t err = i2s_channel_write(
                i2s_dac_chan, slot->audio_data, sizeof(slot->audio_data),
                &bytes_written, portMAX_DELAY);
            xQueueSend(free_q, &slot, 0);
            slot = NULL;
            if (err != ESP_OK) {
                ESP_LOGW(AUDIO, "I2S write failed: %s", esp_err_to_name(err));
            }
        } else {
            // Underrun: feed silence so the DAC clock keeps running.
            i2s_channel_write(i2s_dac_chan, silence, sizeof(silence),
                              &bytes_written, portMAX_DELAY);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_drop_log) >= pdMS_TO_TICKS(DROP_LOG_INTERVAL_MS)) {
            uint32_t delta = queue_drops - last_logged_drops;
            UBaseType_t depth = uxQueueMessagesWaiting(filled_q);
            if (delta > 0 || depth > PLAYBACK_PREBUFFER_PACKETS + 1) {
                ESP_LOGI(AUDIO,
                         "queue drops +%lu / %d ms (total %lu), depth=%u",
                         (unsigned long)delta, DROP_LOG_INTERVAL_MS,
                         (unsigned long)queue_drops, (unsigned)depth);
            }
            last_logged_drops = queue_drops;
            last_drop_log = now;
        }
    }

    if (slot != NULL) {
        xQueueSend(free_q, &slot, 0);
    }
    count_queue_drops = false;
    audio_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Broadcast (and unicast-to-last-TX) pairing requests until mode changes.
 */
static void send_pairing_req(void) {
    pairing_req_packet_t req = {0};
    snprintf(req.alias, sizeof(req.alias), "%s", unique_alias);
    uint32_t heartbeat = 0;

    ESP_LOGI(PAIR, "Advertising pairing request as %s%s.", req.alias,
             have_paired_tx ? " (broadcast+unicast)" : " (broadcast)");
    rx_dbg_mode("enter_pairing", RX_MODE_PAIRING);

    while (cur_mode == RX_MODE_PAIRING) {
        esp_err_t err =
            esp_now_send(BROADCAST_MAC, (uint8_t *)&req, sizeof(req));
        if (err != ESP_OK) {
            ESP_LOGW(PAIR, "Pairing broadcast failed: %s",
                     esp_err_to_name(err));
        }

        // Unicast hits a busy TX much more reliably than broadcast.
        if (have_paired_tx) {
            err = esp_now_send(paired_tx_mac, (uint8_t *)&req, sizeof(req));
            if (err != ESP_OK) {
                ESP_LOGW(PAIR, "Pairing unicast failed: %s",
                         esp_err_to_name(err));
            }
        }

        heartbeat++;
        if ((heartbeat % 5) == 0) {
            rx_dbg_pair_heartbeat(heartbeat, have_paired_tx ? 1 : 0);
        }

        vTaskDelay(pdMS_TO_TICKS(PAIRING_INTERVAL_MS));
    }
    ESP_LOGI(PAIR, "Left pairing advertise loop (mode changed).");
    rx_dbg_mode("left_pairing", (int)cur_mode);
}

// --- CALLBACKS ---

/**
 * @brief ESP-NOW receive: pairing ACKs in PAIRING, audio in PLAYBACK only.
 *        Soft-reconnect: audio from a remembered TX while PAIRING re-enters
 *        playback (see try_soft_reconnect_from_audio).
 */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data,
                    int data_size) {
    if (!info || !data) {
        ESP_LOGW(CB, "NULL pointer in recv_cb.");
        return;
    }

    const rx_mode_t mode = cur_mode;

    // Accept 1-byte ACK (current TX) or legacy sizeof(pair_state_t) (old TX).
    // Status is always in the first byte on little-endian.
    if (mode == RX_MODE_PAIRING && data_size >= PAIR_ACK_LEN &&
        (data_size == PAIR_ACK_LEN ||
         data_size == (int)sizeof(pair_state_t))) {
        ESP_LOGI(PAIR, "Got pairing ACK/NACK (%d bytes, status=0x%02X).",
                 data_size, data[0]);
        rx_dbg_recv((int)mode, data_size, info->src_addr,
                    (int)sizeof(audio_packet_t), PAIR_ACK_LEN);
        handle_pairing_result(data, info->src_addr);
        return;
    }

    if (data_size == (int)sizeof(audio_packet_t)) {
        if (mode == RX_MODE_AUDIO_PLAYBACK) {
            handle_audio_packet(data);
            return;
        }
        if (mode == RX_MODE_PAIRING &&
            try_soft_reconnect_from_audio(info->src_addr)) {
            // Mode is now PLAYBACK; this packet is intentionally not queued
            // (playback entry flushes). Subsequent packets will stream.
            return;
        }
    }

    if (mode == RX_MODE_PAIRING) {
        // Rate-limit: TX audio flood was spamming the monitor (~133 Hz).
        static TickType_t last_ignore_log = 0;
        TickType_t now = xTaskGetTickCount();
        if ((now - last_ignore_log) >= pdMS_TO_TICKS(1000)) {
            last_ignore_log = now;
            rx_dbg_recv((int)mode, data_size, info->src_addr,
                        (int)sizeof(audio_packet_t), PAIR_ACK_LEN);
            ESP_LOGW(CB,
                     "Ignoring size=%d while pairing (want %d-byte ACK or "
                     "%d-byte audio for soft join).",
                     data_size, PAIR_ACK_LEN, (int)sizeof(audio_packet_t));
        }
    }
}

/**
 * @brief ESP-NOW send status for pairing broadcasts. Loss is non-fatal.
 */
static void send_cb(const esp_now_send_info_t *info,
                    esp_now_send_status_t status) {
    if (!info) {
        ESP_LOGW(CB, "NULL send_info in send_cb.");
        return;
    }
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(CB, "Pairing send failed.");
    }
}

/**
 * @brief Run playback on core 1; return to pairing on link timeout.
 */
static void run_audio_playback_state(void) {
    audio_task_stop = false;
    count_queue_drops = false;
    queue_drops = 0;
    last_packet_time = xTaskGetTickCount();
    flush_audio_queue();

    // Safe context for esp_now_add_peer (not inside recv_cb).
    if (have_paired_tx) {
        remember_tx_peer(paired_tx_mac);
    }

    if (enable_i2s_dac() != ESP_OK) {
        cur_mode = RX_MODE_PAIRING;
        return;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        audio_playback_task, "AudioPlayback", AUDIO_TASK_STACK, NULL,
        AUDIO_TASK_PRIO, &audio_task_handle, AUDIO_TASK_CORE);
    if (created != pdPASS) {
        ESP_LOGE(AUDIO, "Failed to create audio playback task.");
        disable_i2s_dac();
        cur_mode = RX_MODE_PAIRING;
        return;
    }

    while (cur_mode == RX_MODE_AUDIO_PLAYBACK) {
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_INTERVAL_MS));

        TickType_t idle_ticks = xTaskGetTickCount() - last_packet_time;
        if (idle_ticks > pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS)) {
            uint32_t idle_ms =
                (uint32_t)(idle_ticks * portTICK_PERIOD_MS);
            ESP_LOGW(AUDIO, "TX link timeout; returning to pairing.");
            rx_dbg_link("link timeout", idle_ms, CONNECTION_TIMEOUT_MS);
            cur_mode = RX_MODE_PAIRING;
        }
    }

    audio_task_stop = true;
    TickType_t wait_start = xTaskGetTickCount();
    while (audio_task_handle != NULL &&
           (xTaskGetTickCount() - wait_start) <
               pdMS_TO_TICKS(AUDIO_TASK_STOP_WAIT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (audio_task_handle != NULL) {
        vTaskDelete(audio_task_handle);
        audio_task_handle = NULL;
    }

    flush_audio_queue();
    disable_i2s_dac();
}

void app_main(void) {
    _Static_assert(sizeof(audio_packet_t) <= ESP_NOW_MAX_DATA_LEN_V2,
                   "audio_packet_t exceeds ESP-NOW v2 max");

    while (1) {
        switch (cur_mode) {
        case RX_MODE_BOOTING: {
            ESP_ERROR_CHECK(init_nvs());
            ESP_ERROR_CHECK(init_wifi());
            ESP_ERROR_CHECK(init_espnow(recv_cb, send_cb));
            verify_espnow_ver();
            ESP_ERROR_CHECK(init_broadcast_peer());
            ESP_ERROR_CHECK(init_unique_alias());
            ESP_ERROR_CHECK(init_audio_queue());
            ESP_ERROR_CHECK(init_i2s_pcm5102());
            RX_DBG_LOGI("boot ok; audio_packet_t=%u B samples=%d",
                        (unsigned)sizeof(audio_packet_t),
                        AUDIO_DATA_NUM_SAMPLES);
            cur_mode = RX_MODE_PAIRING;
            break;
        }

        case RX_MODE_PAIRING:
            send_pairing_req();
            break;

        case RX_MODE_AUDIO_PLAYBACK:
            run_audio_playback_state();
            break;

        default:
            ESP_LOGW(INIT, "Unknown mode; rebooting state machine.");
            cur_mode = RX_MODE_BOOTING;
            break;
        }
    }
}
