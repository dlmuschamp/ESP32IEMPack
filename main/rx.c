#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "hal/i2s_types.h"
#include "rig_shared.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
// Constants and Globals

// need to handle the audio pipeline last

//--- STRUCTS ---
typedef enum {
    RX_MODE_BOOTING,
    RX_MODE_PAIRING,
    RX_MODE_AUDIO_PLAYBACK
} rx_mode_t;

//--- GLOBALS ---
#define LRCK_PIN 32
#define BCK_PIN 33
#define DOUT_PIN 25
#define PAIR_REQUEST_TIME_MS 500
#define NUM_QUEUE_SLOTS 3

static uint32_t dropped_packets = 0;
static rx_mode_t cur_mode = RX_MODE_BOOTING;
static i2s_chan_handle_t i2s_dac_chan;
static char unique_alias[ALIAS_BUFFER_SIZE] = {0};
static QueueHandle_t audio_queue;

// --- CONSTANTS ---
static const char *RX = "RX_PACK";
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                        0xFF, 0xFF, 0xFF};

// --- CONFIGS ---
/**
 * @brief sets a unique alias with form IEM_RX_XX:XX:XX where IEM_RX is the
 * BASE_RX_ALIAS and the last 6 characters are taken from the unique last 3
 * characters from the ESP's mac address (padded 2 spaces and all caps).
 * @return ESP_OK on success, or the esp_err_t from esp_wifi_get_mac on failure.
 **/
static esp_err_t init_unique_alias(void) {
    uint8_t mac[MAC_ADDRESS_LEN];
    esp_err_t mac_status = esp_wifi_get_mac(WIFI_IF_STA, mac);

    if (mac_status != ESP_OK) {
        ESP_LOGE(RX, "Failed to read STA MAC for unique alias.");
        return mac_status;
    }

    snprintf(unique_alias, sizeof(unique_alias), "%s%02X:%02X:%02X",
             BASE_RX_ALIAS, mac[3], mac[4], mac[5]);

    ESP_LOGI(RX, "Unique alias: %s", unique_alias);
    return ESP_OK;
}

/**
 * @brief configure esp to send digital signal to the pcm5102. Setting
 * ret_rx_handle to NULL because both TX and RX are only going to operate with
 * one-way audio communication (simplex mode). Audio will go from TX -> RX
 * @return ESP_OK on success, or the first failing I2S API esp_err_t.
 **/
static esp_err_t init_i2s_pcm5102(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_dac_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(RX, "Failed to create I2S DAC channel.");
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                     .bclk = BCK_PIN,
                     .ws = LRCK_PIN,
                     .dout = DOUT_PIN,
                     .din = I2S_GPIO_UNUSED}};

    err = i2s_channel_init_std_mode(i2s_dac_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(RX, "Failed to init I2S DAC std mode.");
        return err;
    }
    err = i2s_channel_enable(i2s_dac_chan);
    if (err != ESP_OK) {
        ESP_LOGE(RX, "Failed to enable I2S DAC channel.");
        return err;
    }
    return ESP_OK;
}

/**
 * @brief allocates memory for an audio queue using xQueueCreate. Logs an
 * error if it fails.
 * @return ESP_OK on success, or ESP_ERR_NO_MEM if xQueueCreate returns NULL.
 **/
static esp_err_t init_audio_queue(void) {
    audio_queue = xQueueCreate(NUM_QUEUE_SLOTS, sizeof(audio_packet_t));

    if (!audio_queue) {
        ESP_LOGE(
            RX,
            "Failed to create audio queue to store incoming audio packets.");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// --- HELPERS ---
/**
 * @brief swaps RX into audio playback mode after a successful pairing ACK from
 * the TX, or enters pairing mode to retry.
 * @param *data the incoming pairing result from the TX
 **/
static void handle_pairing_result(const uint8_t *data) {
    pair_state_t *result = (pair_state_t *)data;
    if (*result == PAIR_SUCCESS) {
        ESP_LOGI(RX,
                 "Successfully paired RX to TX. Swapping to audio playback.");
        cur_mode = RX_MODE_AUDIO_PLAYBACK;
        return;
    } else
        ESP_LOGW(RX, "Failed to pair RX to TX. Retrying pairing.");
    cur_mode = RX_MODE_PAIRING;
    return;
}

/**
 * @brief copies the recieved audio data from the TX to the RX's audio buffer
 * for playback. Playback happens outside of the callback function to prevent
 * interruptions.
 * @param *data the incoming audio buffer from the TX
 **/
static void handle_audio_packet(const uint8_t *data) {
    // TODO: copy this packet to a buffer
    // add another error checking place here for audio_queue != 0
    // return type of xqueuesend says pdTRUE value but no type.
    if (!audio_queue) {
        ESP_LOGW(RX, "Audio queue does not exist.");
        return;
    }

    xQueueSend(audio_queue, data, 0);
}

/**
 * @brief sends pairing requests to all ESP32 peers at PAIR_REQUEST_TIME_MS
 * intervals. Only TX-packs will respond to this request; other RX's will
 * ignore.
 **/
static void send_pairing_req(void) {
    ESP_LOGI(RX, "Transmitting pairing request to TX.");

    pairing_req_packet_t cur_req;
    strncpy(cur_req.alias, unique_alias, sizeof(cur_req.alias));

    while (cur_mode == RX_MODE_PAIRING) {
        esp_now_send(BROADCAST_MAC, (uint8_t *)&cur_req,
                                     sizeof(pairing_req_packet_t));
        vTaskDelay(pdMS_TO_TICKS(PAIR_REQUEST_TIME_MS));
    }
}

//--- CALLBACKS ---
/**
 * @brief on recieving a packet from the tx, the rx will either handle the
 * pairing result status and update the current rx mode (if applicable) or will
 * enter the audio playback mode. Logs invalid packets without aborting.
 * @param *esp_now_recv_info unused by required to match the esp_now_recv_info
 * struct
 * @param *data pointer to full data values
 * @param size of recieved data packet
 **/
static void recv_cb(const esp_now_recv_info_t *esp_now_recv_info,
                    const uint8_t *data, int data_size) {

    (void)esp_now_recv_info;

    if (!esp_now_recv_info || !data) {
        ESP_LOGW(RX, "Recieved a NULL pointer on recv_cb.");
        return;
    }

    if (data_size != sizeof(pair_state_t) &&
        data_size != sizeof(audio_packet_t)) {
        ESP_LOGW(RX, "Recieved packet with an unrecognized size.");
        return;
    }

    if (data_size == sizeof(pair_state_t)) {
        handle_pairing_result(data);
        return;
    }

    if (data_size == sizeof(audio_packet_t)) {
        handle_audio_packet(data);
        return;
    }
}

/**
 * @brief logs if send meta-data is null. Increments dropped_packets if
 * ESP-NOW did not succeed. Packet loss is not fatal.
 * @param *esp_now_send_info check if null pointer
 * @param status increment if esp_now did not succeed
 **/
static void send_cb(const esp_now_send_info_t *esp_now_send_info,
                    esp_now_send_status_t status) {
    if (!esp_now_send_info) {
        ESP_LOGW(RX, "esp_now_send_info meta-data is a NULL pointer.");
        return;
    }

    if (status != ESP_NOW_SEND_SUCCESS) {
        dropped_packets++;
        ESP_LOGI(RX, "Dropped packets: %lu", (unsigned long)dropped_packets);
        return;
    }
}

void app_main(void) {

    // need to think moer about control flow here. pairing request will keep
    // sending until we get an ack. this will trigger the recv_cb implictly.
    // this will cause the mode to either stay in pairing or swap into audio
    // playback. when in audio playback, the handle_audio function will already
    // be called so there isnt much for me to call directly on that case that
    // isnt already handled by the recv_cb manager function.
    while (1) {
        switch (cur_mode) {
        case RX_MODE_BOOTING:
            ESP_ERROR_CHECK(init_nvs());
            ESP_ERROR_CHECK(init_wifi());
            ESP_ERROR_CHECK(init_espnow(recv_cb, send_cb));
            ESP_ERROR_CHECK(init_unique_alias());
            ESP_ERROR_CHECK(init_audio_queue());
            ESP_ERROR_CHECK(init_i2s_pcm5102());
            cur_mode = RX_MODE_PAIRING;
            break;
        case RX_MODE_PAIRING:
            send_pairing_req();
            break;
        case RX_MODE_AUDIO_PLAYBACK:
            // actually play the audio. make sure to pin this to core 1 instead
            // of core 0. the recv_cb acts like the function dispatcher which
            // will already port the audio to the lower priority task to be
            // called here.
            break;
        default:
            cur_mode = RX_MODE_BOOTING;
            break;
        }
    }
}
