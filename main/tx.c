#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "rig_shared.h"

// --- MACROS ---
#define I2S_BCK_PIN 33
#define I2S_WS_PIN 32
#define I2S_DIN_PIN 35
#define I2S_MCLK_PIN 0

// --- GLOBALS ---
static const char *TAG = "TX_NODE";
i2s_chan_handle_t rx_adc_chan;
uint8_t peers_connected = 0x00;

// --- THE HANDSHAKE CALLBACK ---
void on_esp_now_recv(const esp_now_recv_info_t *esp_now_info,
                     const uint8_t *data, int data_len) {

    if (data_len != sizeof(discovery_packet_t)) {
        ESP_LOGE(TAG,"Received data size is different than size of discovery_packet_t.");
        return;
    }

    discovery_packet_t *cur_pac = (discovery_packet_t *)data;
    if (strncmp(cur_pac->alias, BASE_RX_ALIAS, strlen(BASE_RX_ALIAS)) != 0) {
        ESP_LOGI(TAG, "Current packet does not contain a valid RX alias.");
        return;
    }

    esp_now_peer_info_t new_peer = {
    .channel = DEFAULT_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false};

    memcpy(new_peer.peer_addr, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);

    esp_err_t add_status = esp_now_add_peer(&new_peer);

    if (add_status == ESP_OK) {
        ESP_LOGI(TAG, "RX paired successfully.");
        peers_connected |= (~peers_connected) & (peers_connected + 1);
    } else if (add_status == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGI(TAG, "RX already paired. Ignoring.");
    } else {
        ESP_LOGE(TAG, "Failed to add RX as peer.");
    }
}

// --- HARDWARE INIT ---
void init_nvs(void) {
    // TODO: Initialize NVS flash.
    // Handle ESP_ERR_NVS_NO_FREE_PAGES and ESP_ERR_NVS_NEW_VERSION_FOUND by
    // erasing and retrying.
}

void init_wifi_espnow(void) {
    // TODO: Initialize esp_netif and the default event loop.
    // TODO: Initialize Wi-Fi (WIFI_INIT_CONFIG_DEFAULT), set storage to RAM, set
    // mode to STA, and start it.

    // TODO: Initialize ESP-NOW.
    // TODO: Register the receive callback function (on_esp_now_recv).
}

void init_i2s_microphone(void) {
    // TODO: Create a default channel config (I2S_NUM_AUTO, I2S_ROLE_MASTER).
    // TODO: Allocate the channel. Pass NULL for the TX handle, and &rx_adc_chan
    // for the RX handle.

    // TODO: Create the standard config struct (i2s_std_config_t).
    // clk_cfg: default config for SAMPLE_RATE.
    // slot_cfg: default Philips format, 16-bit width, stereo.
    // gpio_cfg: route mclk, bclk, ws, and din. (dout is unused).

    // TODO: Initialize the channel in standard mode and enable it.
}

// --- MAIN THREAD ---
void app_main(void) {
    printf("Booting TX Node...\n");

    // TODO: Call your three initialization functions in order.

    printf("Waiting for RX bodypacks to ping...\n");

    // TODO: Create your audio_packet_t payload struct variable and initialize the
    // packet_id to 0.

    size_t bytes_read = 0;

    while (1) {
        if (peers_connected) {
            // TODO: Read from rx_adc_chan into your packet's audio_data array.
            // Use portMAX_DELAY so the task blocks until the buffer is 100% full.

            // TODO: If the read was successful and bytes_read > 0:
            // 1. Send the packet via esp_now_send(). Passing NULL for the MAC address
            // broadcasts to ALL registered peers.
            // 2. Increment packet_id.
        } else {
            vTaskDelay(pdMS_TO_TICKS(100)); // Idling
        }

        vTaskDelay(pdMS_TO_TICKS(1)); // Feed the FreeRTOS watchdog
    }
}
