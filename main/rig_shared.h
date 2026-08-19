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
#define MAX_ESPNOW_PAYLOAD_BYTES 1470
#define AUDIO_DATA_NUM_SAMPLES 734
#define ALIAS_BUFFER_SIZE 16
#define MAC_ADDRESS_LEN 6
#define SAMPLE_RATE 44100

// --- ESP-NOW CHANNELS ---
#define DEFAULT_CHANNEL 1
#define SECONDARY_CHANNEL WIFI_SECOND_CHAN_NONE

// --- ALIASES ---
#define BASE_RX_ALIAS "IEM_RX_" // concat using snprintf()

// --- ENUMS ---
typedef enum { PAIR_SUCCESS = 0x01, PAIR_FAIL = 0x02 } pair_state_t;

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
 * @brief An (encrypted after refactor) packet sent from the TX to the RX.
 * Includes a packet_id to keep track of the number of dropped packets in
 * addition to the actual audio data to be played by the .
 **/
typedef struct {
    uint8_t packet_id;
    int16_t audio_data[AUDIO_DATA_NUM_SAMPLES];
} audio_packet_t;

//--- BOOT ---
/**
 * @brief attempts to flash the nvs on the current esp.
 * The init_nvs function itself will retry once if nvs_status shows
 * ESP_ERR_NVS_NO_FREE_PAGES or ESP_ERR_NVS_NEW_VERSION_FOUND. Throws a fatal
 * error otherwise.
 **/
void init_nvs(void);

/**
 * @brief attempts to initialize wifi on the current esp.
 * Sets to default config, wifi mode to WIFI_MODE_STA, storage to
 * WIFI_STORAGE_RAM, and primary and secondary channels to the DEFAULT_CHANNEL
 * and SECONDARY_CHANNEL macros respectively. Throws a fatal error otherwise.
 **/
void init_wifi(void);

/**
 * @brief attempts to initialize the espnow device and set its callback
 * functions for recieving and transmitting packets. Throws a fatal error
 * otherwise.
 * @param recv_cb the callback function to run when a packet is recieved
 * @param send_cb the callback function to run when a packet is sent
 * successfully.
 **/
void init_espnow(esp_now_recv_cb_t recv_cb, esp_now_send_cb_t send_cb);

#endif // RIG_SHARED_H
