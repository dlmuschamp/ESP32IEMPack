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
#define PAIRING_INTERVAL_MS 500
// Link-loss poll period while playing audio.
#define LINK_CHECK_INTERVAL_MS 100
// No audio packet for this long → drop back to pairing.
#define CONNECTION_TIMEOUT_MS 2000
// Emergency jitter buffer only (steady state should stay near empty).
// 3 × ~2.0 ms ≈ 6 ms if completely full — should not be the normal path.
#define NUM_QUEUE_SLOTS 3
// Match one audio packet per DMA descriptor for low I2S path delay.
#define I2S_DMA_DESC_NUM 3
#define I2S_DMA_FRAME_NUM AUDIO_FRAMES_PER_PACKET
#define AUDIO_TASK_STACK 8192
#define AUDIO_TASK_PRIO 5
#define AUDIO_TASK_CORE 1
#define AUDIO_TASK_STOP_WAIT_MS 500

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
static QueueHandle_t audio_queue;
static char unique_alias[ALIAS_BUFFER_SIZE] = {0};
static uint32_t dropped_packets = 0;

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

    err = i2s_channel_init_std_mode(i2s_dac_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to init I2S DAC std mode.");
        return err;
    }
    err = i2s_channel_enable(i2s_dac_chan);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to enable I2S DAC channel.");
        return err;
    }
    return ESP_OK;
}

/**
 * @brief Create the FreeRTOS queue that decouples ESP-NOW recv from I2S write.
 * @return ESP_OK, or ESP_ERR_NO_MEM if creation fails.
 */
static esp_err_t init_audio_queue(void) {
    audio_queue = xQueueCreate(NUM_QUEUE_SLOTS, sizeof(audio_packet_t));
    if (!audio_queue) {
        ESP_LOGE(INIT, "Failed to create audio queue.");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// --- HELPERS ---

/** @brief Discard any buffered audio (used on disconnect / re-pair). */
static void flush_audio_queue(void) {
    if (!audio_queue) {
        return;
    }
    audio_packet_t discard;
    while (xQueueReceive(audio_queue, &discard, 0) == pdTRUE) {
    }
}

/**
 * @brief Apply TX pairing ACK/NACK. Primes link timer on success.
 * @param data Pointer to a pair_state_t payload.
 */
static void handle_pairing_result(const uint8_t *data) {
    const pair_state_t result = *(const pair_state_t *)data;

    if (result == PAIR_SUCCESS) {
        ESP_LOGI(PAIR, "Paired with TX; entering audio playback.");
        last_packet_time = xTaskGetTickCount();
        cur_mode = RX_MODE_AUDIO_PLAYBACK;
        return;
    }

    ESP_LOGW(PAIR, "Pairing failed; will keep advertising.");
    cur_mode = RX_MODE_PAIRING;
}

/**
 * @brief Enqueue one audio packet for the playback task (non-blocking).
 * @param audio_packet Pointer to an audio_packet_t payload.
 */
static void handle_audio_packet(const uint8_t *audio_packet) {
    if (!audio_queue) {
        return;
    }

    last_packet_time = xTaskGetTickCount();
    if (xQueueSend(audio_queue, audio_packet, 0) != pdTRUE) {
        dropped_packets++;
        ESP_LOGW(AUDIO, "Jitter queue full; dropped packet (total %lu).",
                 (unsigned long)dropped_packets);
    }
}

// --- TASKS ---

/**
 * @brief Core-1 consumer: queue → I2S DAC. Exits when audio_task_stop is set.
 */
static void audio_playback_task(void *pvParameters) {
    (void)pvParameters;
    audio_packet_t packet;
    size_t bytes_written;

    while (!audio_task_stop) {
        if (xQueueReceive(audio_queue, &packet, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        if (audio_task_stop) {
            break;
        }

        esp_err_t err =
            i2s_channel_write(i2s_dac_chan, packet.audio_data,
                              sizeof(packet.audio_data), &bytes_written,
                              portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(AUDIO, "I2S write failed: %s", esp_err_to_name(err));
        }
    }

    audio_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Broadcast pairing requests until cur_mode leaves PAIRING.
 */
static void send_pairing_req(void) {
    pairing_req_packet_t req = {0};
    snprintf(req.alias, sizeof(req.alias), "%s", unique_alias);

    ESP_LOGI(PAIR, "Advertising pairing request as %s.", req.alias);

    while (cur_mode == RX_MODE_PAIRING) {
        esp_err_t err = esp_now_send(BROADCAST_MAC, (uint8_t *)&req, sizeof(req));
        if (err != ESP_OK) {
            ESP_LOGW(PAIR, "Pairing broadcast failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(PAIRING_INTERVAL_MS));
    }
}

// --- CALLBACKS ---

/**
 * @brief ESP-NOW receive: pairing ACKs in PAIRING, audio in PLAYBACK only.
 */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data,
                    int data_size) {
    if (!info || !data) {
        ESP_LOGW(CB, "NULL pointer in recv_cb.");
        return;
    }

    const rx_mode_t mode = cur_mode;

    if (mode == RX_MODE_PAIRING && data_size == (int)sizeof(pair_state_t)) {
        handle_pairing_result(data);
        return;
    }

    if (mode == RX_MODE_AUDIO_PLAYBACK &&
        data_size == (int)sizeof(audio_packet_t)) {
        handle_audio_packet(data);
        return;
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
        dropped_packets++;
        ESP_LOGD(CB, "ESP-NOW send failed (total %lu).",
                 (unsigned long)dropped_packets);
    }
}

/**
 * @brief Run playback on core 1; return to pairing on link timeout.
 */
static void run_audio_playback_state(void) {
    audio_task_stop = false;
    last_packet_time = xTaskGetTickCount();
    flush_audio_queue();

    BaseType_t created = xTaskCreatePinnedToCore(
        audio_playback_task, "AudioPlayback", AUDIO_TASK_STACK, NULL,
        AUDIO_TASK_PRIO, &audio_task_handle, AUDIO_TASK_CORE);
    if (created != pdPASS) {
        ESP_LOGE(AUDIO, "Failed to create audio playback task.");
        cur_mode = RX_MODE_PAIRING;
        return;
    }

    while (cur_mode == RX_MODE_AUDIO_PLAYBACK) {
        vTaskDelay(pdMS_TO_TICKS(LINK_CHECK_INTERVAL_MS));

        if ((xTaskGetTickCount() - last_packet_time) >
            pdMS_TO_TICKS(CONNECTION_TIMEOUT_MS)) {
            ESP_LOGW(AUDIO, "TX link timeout; returning to pairing.");
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

            uint32_t espnow_ver = 0;
            ESP_ERROR_CHECK(esp_now_get_version(&espnow_ver));
            ESP_LOGI(INIT, "ESP-NOW version %lu (need 2 for >250 B packets).",
                     (unsigned long)espnow_ver);
            if (espnow_ver < 2) {
                ESP_LOGE(INIT,
                         "ESP-NOW v2 required for audio_packet_t (%u B).",
                         (unsigned)sizeof(audio_packet_t));
                abort();
            }

            ESP_ERROR_CHECK(init_broadcast_peer());
            ESP_ERROR_CHECK(init_unique_alias());
            ESP_ERROR_CHECK(init_audio_queue());
            ESP_ERROR_CHECK(init_i2s_pcm5102());
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
