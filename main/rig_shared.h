#ifndef RIG_SHARED_H
#define RIG_SHARED_H

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdint.h>
#include <sys/types.h>

// --- ESP-NOW CONSTRAINTS ---
// ESP32-WROOM on IDF ≥ 5.4 speaks ESP-NOW v2 (up to ESP_NOW_MAX_DATA_LEN_V2).
// Keep audio_packet_t ≤ that limit. Confirm at runtime with
// esp_now_get_version().
#define MAX_ESPNOW_PAYLOAD_BYTES ESP_NOW_MAX_DATA_LEN_V2
// Interleaved stereo int16s per packet. Must be even (L/R pairs).
//
// lower_latency_optimizations branch — aim ~10 ms e2e (was 7.5 ms packets /
// ~40 ms typical). 240 samples = 120 frames @ 48 kHz = 2.5 ms/packet, 480 B,
// 400 pkt/s. Breadboard struggled at ~2 ms; 2.5 ms is the first retry with
// send_cb pacing + pointer-pool RX. If drops return, bump toward 288 (3 ms)
// before giving up on the 10 ms goal.
//
// Rough budget @ 2.5 ms pkt, prebuffer 2, IIR FLT on PCM5102:
//   TX fill 2.5 + air ~0.5 + RX prebuffer 5 + RX DMA ~2.5 + codec ~0.1 ≈ 10–11 ms
#define AUDIO_DATA_NUM_SAMPLES 240
#define AUDIO_FRAMES_PER_PACKET (AUDIO_DATA_NUM_SAMPLES / 2)
#define ALIAS_BUFFER_SIZE 16
#define MAC_ADDRESS_LEN 6
#define SAMPLE_RATE 48000
#define AUDIO_TASK_STACK 12288
// --- ESP-NOW CHANNELS ---
#define DEFAULT_CHANNEL 1
#define SECONDARY_CHANNEL WIFI_SECOND_CHAN_NONE

// --- ALIASES ---
#define BASE_RX_ALIAS "IEM_RX_" // concat using snprintf()

// --- ENUMS ---
typedef enum { PAIR_SUCCESS = 0x01, PAIR_FAIL = 0x02 } pair_state_t;
// On-wire pairing ACK/NACK length (always send this many bytes from TX).
#define PAIR_ACK_LEN 1

// --- STRUCTS ---
/**
 * @brief An unencrypted packet sent from an RX to the TX. Provides the TX
 * an RX alias (IEM_RX_000000) to add as a peer and begin transmitting audio
 * to.
 **/
typedef struct {
    char alias[ALIAS_BUFFER_SIZE];
} pairing_req_packet_t;

/**
 * @brief PCM payload from TX → RX (unencrypted for now).
 * Size must stay ≤ ESP_NOW_MAX_DATA_LEN_V2 (ESP-NOW v2 / IDF ≥ 5.4).
 **/
typedef struct {
    int16_t audio_data[AUDIO_DATA_NUM_SAMPLES];
} audio_packet_t;

//--- BOOT ---
/**
 * @brief attempts to flash the nvs on the current esp.
 * The init_nvs function itself will retry once if nvs_status shows
 * ESP_ERR_NVS_NO_FREE_PAGES or ESP_ERR_NVS_NEW_VERSION_FOUND.
 * @return ESP_OK on success, or the first failing NVS API esp_err_t.
 **/
esp_err_t init_nvs(void);

/**
 * @brief attempts to initialize wifi on the current esp.
 * Sets to default config, wifi mode to WIFI_MODE_STA, storage to
 * WIFI_STORAGE_RAM, and primary and secondary channels to the DEFAULT_CHANNEL
 * and SECONDARY_CHANNEL macros respectively.
 * @return ESP_OK on success, or the first failing Wi-Fi API esp_err_t.
 **/
esp_err_t init_wifi(void);

/**
 * @brief attempts to initialize the espnow device and set its callback
 * functions for recieving and transmitting packets.
 * @param recv_cb the callback function to run when a packet is recieved
 * @param send_cb the callback function to run when a packet is sent
 * successfully.
 * @return ESP_OK on success, or the first failing ESP-NOW API esp_err_t.
 **/
esp_err_t init_espnow(esp_now_recv_cb_t recv_cb, esp_now_send_cb_t send_cb);

/**
 * @brief Checks the current espnow version and aborts app_main if current
 * version is earlier than v2.
 **/
void verify_espnow_ver();
#endif // RIG_SHARED_H
