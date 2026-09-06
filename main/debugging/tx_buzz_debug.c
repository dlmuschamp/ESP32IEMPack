/**
 * @file tx_buzz_debug.c
 * @brief IEM transmitter (TX pack): accepts RX pairing over ESP-NOW, then
 *        streams PCM from a PCM1808 ADC via I2S.
 *
 * Boot → idle until an RX pairs → capture on core 1 and ESP-NOW send. ESP-NOW
 * callbacks only manage peers / mode; I2S reads stay off the Wi-Fi task. When
 * the last peer drops, the pack returns to idle and stops capture.
 *
 * PCM1808 (slave) wiring note:
 *   MCLK → SCK, BCLK → BCK, WS → LRCK, DIN ← DOUT.
 * In slave mode the ADC needs 64 BCK/frame (not 32). 16-bit stereo slots force
 * BCK = 32*fs and DOUT to silence; 32-bit stereo slots give BCK = 64*fs.
 * MCLK stays at 256*fs. See TI PCM1808 datasheet §7.3.5.1.2.
 */

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi_types_generic.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "portmacro.h"
#include "../rig_shared.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef INT16_MAX
#define INT16_MAX 32767
#endif
#ifndef INT16_MIN
#define INT16_MIN (-32768)
#endif

// --- HARDWARE ---
#define BCK_PIN 33
#define LRCK_PIN 32
#define DIN_PIN 35
#define SCK_PIN 0
// Digital gain after >>16. Keep at 1 while debugging harsh/muffled (2 clips easily).
#define TX_DIGITAL_GAIN 1
// How often to log peak |sample| (pre-gain) to judge ADC level vs noise floor.
#define TX_PEAK_LOG_INTERVAL_MS 2000
// How often to print ESP-NOW send-fail totals while transmitting.
#define DROP_LOG_INTERVAL_MS 2000

// --- TIMING / BUFFERS ---
// Poll period while idling for peers or watching peer count while transmitting.
#define MODE_POLL_INTERVAL_MS 50
// Match one audio packet per DMA descriptor for low I2S path delay.
#define I2S_DMA_DESC_NUM 3
#define I2S_DMA_FRAME_NUM AUDIO_FRAMES_PER_PACKET
#define AUDIO_TASK_PRIO 12
#define AUDIO_TASK_CORE 1
// How many times to fire PAIR_SUCCESS (audio TX can starve a single ACK).
#define PAIR_ACK_REPEAT 8
// After a re-pair request, skip this many audio sends so ACKs get airtime.
// ~24 * 2.5 ms ≈ 60 ms of silence for pairing frames to land.
#define PAIR_ACK_AUDIO_SKIP 24
#define SEND_IN_FLIGHT_WAIT_US 4000
// No MAC-layer send success for this long → peer is gone (ESP-IDF: send_cb
// returns FAIL when destination does not exist / frame never ACKed at MAC).
#define TX_LINK_TIMEOUT_MS 1500
// Consecutive send_cb FAILs before we treat the link as dead (faster path).
#define TX_SEND_FAIL_STREAK_LIMIT 40

// --- MODES ---
typedef enum {
    TX_MODE_BOOTING,
    TX_MODE_IDLING,
    TX_MODE_TRANSMITTING
} tx_mode_t;

// --- GLOBALS ---
static i2s_chan_handle_t i2s_adc_chan;
static uint8_t peers_connected = 0;
static uint32_t dropped_packets = 0;
static uint8_t paired_peer_mac[ESP_NOW_ETH_ALEN] = {0};
static bool have_paired_peer = false;
// Set from pairing handler; audio task skips sends briefly so ACK can go out.
static volatile int pair_ack_audio_skip = 0;
// Pairing work must NOT run inside esp_now recv_cb (Wi-Fi task).
typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    char alias[ALIAS_BUFFER_SIZE];
} pending_pair_req_t;
static QueueHandle_t pair_req_q;

// Updated from ESP-NOW recv callback; read on the app_main state machine.
static volatile tx_mode_t cur_mode = TX_MODE_BOOTING;
static volatile bool audio_task_stop = false;
static TaskHandle_t audio_task_handle = NULL;
// Link health from send_cb (Wi-Fi task) — read on transmit state machine.
static volatile TickType_t last_send_ok_tick = 0;
static volatile uint32_t send_fail_streak = 0;
// Docs: wait for prior send_cb before the next esp_now_send to avoid CB
// disorder / NO_MEM under burst load.
static volatile bool send_in_flight = false;

// I2S delivers one 32-bit word per slot; pack down to int16 for packets.
static int32_t raw_audio_samples[AUDIO_DATA_NUM_SAMPLES];
// Keep off the audio task stack — sizeof(audio_packet_t) is 480 B @ 2.5 ms pkts.
static audio_packet_t tx_packet;

static esp_now_rate_config_t peer_rate_cfg = {
    // 12M is more robust than 24M; 480 B airtime is tiny vs 2.5 ms period.
    .rate = WIFI_PHY_RATE_12M,
    .phymode = WIFI_PHY_MODE_11G,
    .ersu = false,
    .dcm = false,
};

// --- LOG TAGS ---
static const char *INIT = "TX_INIT";
static const char *IDLE = "TX_IDLE";
static const char *TRANSMIT = "TX_TRANSMIT";
static const char *PAIR = "TX_PAIR";
static const char *CB = "TX_CB";

// --- INIT ---

/**
 * @brief I2S master RX from PCM1808 (DIN + MCLK; simplex TX pack).
 * @return ESP_OK on success, or the first failing I2S esp_err_t.
 */
static esp_err_t init_i2s_pcm1808(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // 32-bit slots so BCK = 64*fs (PCM1808 rejects 16-bit / 32*fs).
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = SCK_PIN,
                .bclk = BCK_PIN,
                .ws = LRCK_PIN,
                .dout = I2S_GPIO_UNUSED,
                .din = DIN_PIN,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    // 256*fs is a valid PCM1808 SCKI ratio and divides cleanly with 64*fs BCK.
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &i2s_adc_chan);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to create I2S ADC channel.");
        return err;
    }
    err = i2s_channel_init_std_mode(i2s_adc_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to init I2S ADC std mode.");
        return err;
    }
    err = i2s_channel_enable(i2s_adc_chan);
    if (err != ESP_OK) {
        ESP_LOGE(INIT, "Failed to enable I2S ADC channel.");
        return err;
    }

    gpio_set_drive_capability(SCK_PIN, GPIO_DRIVE_CAP_3);
    return ESP_OK;
}

// --- HELPERS ---

/** @brief Drop the active RX peer and return TX to a clean idle-ready state. */
static void clear_paired_peer(const char *reason) {
    if (have_paired_peer) {
        esp_err_t err = esp_now_del_peer(paired_peer_mac);
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_NOT_FOUND) {
            ESP_LOGW(PAIR, "del_peer failed (%s): %s", reason,
                     esp_err_to_name(err));
        }
        have_paired_peer = false;
        memset(paired_peer_mac, 0, sizeof(paired_peer_mac));
    }
    peers_connected = 0;
    ESP_LOGW(PAIR, "Cleared RX peer (%s); returning to idle.", reason);
    cur_mode = TX_MODE_IDLING;
}

/**
 * @brief Add RX peer, ACK at default rate, then set 11g/24M. Task context only.
 */
static void handle_connect_new_peer(esp_now_peer_info_t *new_peer) {
    if (!new_peer) {
        ESP_LOGW(PAIR, "NULL peer info; skipping.");
        return;
    }

    esp_err_t add_status = esp_now_add_peer(new_peer);
    bool peer_ok = false;

    if (add_status == ESP_OK) {
        ESP_LOGI(PAIR, "RX paired successfully.");
        peers_connected++;
        peer_ok = true;
    } else if (add_status == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGI(PAIR, "RX already paired; re-ACKing.");
        if (peers_connected == 0) {
            peers_connected = 1;
        }
        peer_ok = true;
    } else {
        ESP_LOGW(PAIR, "Failed to add RX as peer: %s",
                 esp_err_to_name(add_status));
        uint8_t fail_byte = (uint8_t)PAIR_FAIL;
        esp_now_send(new_peer->peer_addr, &fail_byte, PAIR_ACK_LEN);
        return;
    }

    memcpy(paired_peer_mac, new_peer->peer_addr, ESP_NOW_ETH_ALEN);
    have_paired_peer = true;
    last_send_ok_tick = xTaskGetTickCount();
    send_fail_streak = 0;

    // ACK before rate_config — default PHY is more reliable for pairing.
    // Docs: esp_now_set_peer_rate_config must run AFTER esp_now_add_peer.
    uint8_t succ_byte = (uint8_t)PAIR_SUCCESS;
    int ack_ok = 0;
    for (int i = 0; i < PAIR_ACK_REPEAT; i++) {
        if (esp_now_send(new_peer->peer_addr, &succ_byte, PAIR_ACK_LEN) ==
            ESP_OK) {
            ack_ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    pair_ack_audio_skip = PAIR_ACK_AUDIO_SKIP;
    ESP_LOGI(PAIR, "Sent PAIR_SUCCESS ACK x%d (%d ok).", PAIR_ACK_REPEAT,
             ack_ok);

    esp_err_t rate_status =
        esp_now_set_peer_rate_config(new_peer->peer_addr, &peer_rate_cfg);
    if (rate_status != ESP_OK) {
        ESP_LOGW(PAIR, "Peer OK but rate config failed: %s",
                 esp_err_to_name(rate_status));
    }

    if (peers_connected > 0) {
        cur_mode = TX_MODE_TRANSMITTING;
    }
}

/** @brief Drain pairing queue — safe place to call esp_now_send/add_peer. */
static void process_pending_pair_requests(void) {
    if (!pair_req_q) {
        return;
    }
    pending_pair_req_t req;
    while (xQueueReceive(pair_req_q, &req, 0) == pdTRUE) {
        if (strncmp(req.alias, BASE_RX_ALIAS, strlen(BASE_RX_ALIAS)) != 0) {
            ESP_LOGI(PAIR, "Ignoring non-RX alias '%s'.", req.alias);
            continue;
        }
        ESP_LOGI(PAIR,
                 "Handling pairing from %02X:%02X:%02X:%02X:%02X:%02X (%s).",
                 req.mac[0], req.mac[1], req.mac[2], req.mac[3], req.mac[4],
                 req.mac[5], req.alias);

        esp_now_peer_info_t new_peer = {
            .channel = DEFAULT_CHANNEL,
            .ifidx = WIFI_IF_STA,
            .encrypt = false,
        };
        memcpy(new_peer.peer_addr, req.mac, ESP_NOW_ETH_ALEN);
        handle_connect_new_peer(&new_peer);
    }
}

// --- TASKS ---

/**
 * @brief Scale int16 sample by TX_DIGITAL_GAIN and clamp to int16 range.
 */
static inline int16_t apply_tx_gain(int32_t sample_i16) {
    int32_t gained = sample_i16 * TX_DIGITAL_GAIN;
    if (gained > INT16_MAX) {
        return INT16_MAX;
    }
    if (gained < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)gained;
}

/**
 * @brief Core-1 producer: I2S ADC → pack int16 → ESP-NOW. Exits on stop flag.
 *        Always sends while transmitting (silence = zeros, not a skipped packet).
 */
static void audio_capture_task(void *pvParameters) {
    (void)pvParameters;
    TickType_t last_drop_log = xTaskGetTickCount();
    uint32_t last_logged_drops = 0;
    TickType_t last_peak_log = xTaskGetTickCount();
    int32_t peak_abs_pre = 0;

    while (!audio_task_stop) {
        size_t bytes_read = 0;
        esp_err_t i2s_read_status = i2s_channel_read(
            i2s_adc_chan, raw_audio_samples, sizeof(raw_audio_samples),
            &bytes_read, portMAX_DELAY);

        if (i2s_read_status != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TRANSMIT, "I2S ADC read failed.");
            continue;
        }
        if (audio_task_stop) {
            break;
        }

        size_t samples_read = bytes_read / sizeof(raw_audio_samples[0]);
        if (bytes_read != sizeof(raw_audio_samples)) {
            ESP_LOGW(TRANSMIT, "Partial I2S read: %u / %u bytes.",
                     (unsigned)bytes_read, (unsigned)sizeof(raw_audio_samples));
        }
        if (samples_read > AUDIO_DATA_NUM_SAMPLES) {
            samples_read = AUDIO_DATA_NUM_SAMPLES;
        }
        if (samples_read < AUDIO_DATA_NUM_SAMPLES) {
            memset(&tx_packet.audio_data[samples_read], 0,
                   (AUDIO_DATA_NUM_SAMPLES - samples_read) * sizeof(int16_t));
        }

        for (size_t i = 0; i < samples_read; i++) {
            int32_t sample = raw_audio_samples[i] >> 16;
            int32_t a = (sample < 0) ? -sample : sample;
            if (a > peak_abs_pre) {
                peak_abs_pre = a;
            }
            tx_packet.audio_data[i] = apply_tx_gain(sample);
        }

        if (pair_ack_audio_skip > 0) {
            pair_ack_audio_skip--;
            continue;
        }

        // Pace sends to send_cb (ESP-IDF recommendation). µs deadline — with
        // CONFIG_FREERTOS_HZ=100, pdMS_TO_TICKS(1..9) is 0 and a 10 ms tick is
        // already four packet periods.
        const int64_t wait_deadline_us =
            esp_timer_get_time() + SEND_IN_FLIGHT_WAIT_US;
        while (send_in_flight && esp_timer_get_time() < wait_deadline_us) {
            taskYIELD();
        }

        send_in_flight = true;
        esp_err_t send_err =
            esp_now_send(have_paired_peer ? paired_peer_mac : NULL,
                         (uint8_t *)&tx_packet, sizeof(tx_packet));
        if (send_err != ESP_OK) {
            send_in_flight = false;
            dropped_packets++;
            // Docs: ESP_ERR_ESPNOW_NO_MEM → delay before the next send.
            if (send_err == ESP_ERR_ESPNOW_NO_MEM) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_peak_log) >= pdMS_TO_TICKS(TX_PEAK_LOG_INTERVAL_MS)) {
            ESP_LOGI(TRANSMIT,
                     "ADC peak|pre-gain|=%ld / 32767 (digital_gain=%d)",
                     (long)peak_abs_pre, TX_DIGITAL_GAIN);
            peak_abs_pre = 0;
            last_peak_log = now;
        }
        if ((now - last_drop_log) >= pdMS_TO_TICKS(DROP_LOG_INTERVAL_MS)) {
            uint32_t delta = dropped_packets - last_logged_drops;
            if (delta > 0) {
                ESP_LOGI(TRANSMIT,
                         "ESP-NOW send fails: +%lu in last %d ms (total %lu).",
                         (unsigned long)delta, DROP_LOG_INTERVAL_MS,
                         (unsigned long)dropped_packets);
            }
            last_logged_drops = dropped_packets;
            last_drop_log = now;
        }
    }

    audio_task_handle = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Wait until cur_mode leaves IDLING (an RX paired).
 */
static void run_idling_state(void) {
    ESP_LOGI(IDLE, "Waiting for RX peers to connect.");
    while (cur_mode == TX_MODE_IDLING) {
        process_pending_pair_requests();
        vTaskDelay(pdMS_TO_TICKS(MODE_POLL_INTERVAL_MS));
    }
}

/**
 * @brief Run capture on core 1; return to idle when peer count hits zero
 *        or MAC-layer send health says the RX is gone.
 */
static void run_transmitting_state(void) {
    audio_task_stop = false;
    last_send_ok_tick = xTaskGetTickCount();
    send_fail_streak = 0;

    BaseType_t created = xTaskCreatePinnedToCore(
        audio_capture_task, "AudioCapture", AUDIO_TASK_STACK, NULL,
        AUDIO_TASK_PRIO, &audio_task_handle, AUDIO_TASK_CORE);
    if (created != pdPASS) {
        ESP_LOGE(TRANSMIT, "Failed to create audio capture task.");
        clear_paired_peer("audio task create failed");
        return;
    }

    while (cur_mode == TX_MODE_TRANSMITTING) {
        process_pending_pair_requests();
        vTaskDelay(pdMS_TO_TICKS(MODE_POLL_INTERVAL_MS));
        if (peers_connected == 0) {
            cur_mode = TX_MODE_IDLING;
            break;
        }
        // If the capture task died (stack/overflow/abort), drop the peer so
        // RX can re-pair and we can start a fresh transmit session.
        if (audio_task_handle == NULL) {
            clear_paired_peer("audio task died");
            break;
        }

        // Link loss: ESP-IDF send_cb FAIL means MAC did not confirm delivery
        // (peer gone / channel mismatch / air loss). Drop peer so we idle and
        // can accept a clean re-pair instead of blasting into the void.
        TickType_t now = xTaskGetTickCount();
        bool timed_out = (now - last_send_ok_tick) >
                         pdMS_TO_TICKS(TX_LINK_TIMEOUT_MS);
        bool fail_streak = send_fail_streak >= TX_SEND_FAIL_STREAK_LIMIT;
        if (timed_out || fail_streak) {
            ESP_LOGW(TRANSMIT,
                     "RX link lost (timeout=%d streak=%lu); clearing peer.",
                     timed_out ? 1 : 0, (unsigned long)send_fail_streak);
            clear_paired_peer(timed_out ? "send timeout" : "send fail streak");
            break;
        }
    }

    audio_task_stop = true;
    while (audio_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// --- CALLBACKS ---

/**
 * @brief ESP-NOW receive: copy pairing requests to a queue (no esp_now_send here).
 */
static void recv_cb(const esp_now_recv_info_t *esp_now_info,
                    const uint8_t *pair_req, int data_size) {
    if (!esp_now_info || !pair_req || !pair_req_q) {
        return;
    }

    if (data_size != (int)sizeof(pairing_req_packet_t)) {
        ESP_LOGD(CB, "Ignoring non-pairing packet (size %d).", data_size);
        return;
    }

    pending_pair_req_t req = {0};
    memcpy(req.mac, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);
    const pairing_req_packet_t *pac = (const pairing_req_packet_t *)pair_req;
    memcpy(req.alias, pac->alias, ALIAS_BUFFER_SIZE);
    req.alias[ALIAS_BUFFER_SIZE - 1] = '\0';

    if (xQueueSend(pair_req_q, &req, 0) != pdTRUE) {
        ESP_LOGW(PAIR, "Pairing queue full; dropping request.");
    } else {
        ESP_LOGI(PAIR, "Queued pairing request (%d bytes).", data_size);
    }
}

/**
 * @brief ESP-NOW send status. Air failures counted; used for TX link-loss.
 *
 * Evidence (ESP-IDF ESP-NOW docs): send_cb returns ESP_NOW_SEND_SUCCESS only if
 * the data was received successfully on the MAC layer; otherwise FAIL (peer
 * missing, channel mismatch, air loss, etc.).
 */
static void send_cb(const esp_now_send_info_t *info,
                    esp_now_send_status_t status) {
    if (!info) {
        ESP_LOGW(CB, "NULL send_info in send_cb.");
        return;
    }
    if (status == ESP_NOW_SEND_SUCCESS) {
        last_send_ok_tick = xTaskGetTickCount();
        send_fail_streak = 0;
        send_in_flight = false;
        return;
    }
    dropped_packets++;
    send_fail_streak++;
    send_in_flight = false;
}

void app_main(void) {
    _Static_assert(sizeof(audio_packet_t) <= ESP_NOW_MAX_DATA_LEN_V2,
                   "audio_packet_t exceeds ESP-NOW v2 max");

    while (1) {
        switch (cur_mode) {
        case TX_MODE_BOOTING: {
            ESP_ERROR_CHECK(init_nvs());
            ESP_ERROR_CHECK(init_wifi());
            ESP_ERROR_CHECK(init_espnow(recv_cb, send_cb));
            verify_espnow_ver();
            // Re-lock channel after esp_now_init (IDF can forget it).
            ESP_ERROR_CHECK(
                esp_wifi_set_channel(DEFAULT_CHANNEL, SECONDARY_CHANNEL));
            ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

            pair_req_q = xQueueCreate(4, sizeof(pending_pair_req_t));
            if (!pair_req_q) {
                ESP_LOGE(INIT, "Failed to create pairing queue.");
                abort();
            }

            // Enter IDLING before slow I2S bring-up so mid-init pairs are kept.
            cur_mode = TX_MODE_IDLING;
            ESP_ERROR_CHECK(init_i2s_pcm1808());
            break;
        }

        case TX_MODE_IDLING:
            run_idling_state();
            break;

        case TX_MODE_TRANSMITTING:
            run_transmitting_state();
            break;

        default:
            ESP_LOGW(INIT, "Unknown mode; rebooting state machine.");
            cur_mode = TX_MODE_BOOTING;
            break;
        }
    }
}
