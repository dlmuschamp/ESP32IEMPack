#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi_types_generic.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "portmacro.h"
#include "rig_shared.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

// --- MACROS ---

/** ESP_TX (MASTER) to PCM1808 (SLAVE)
 *  MCLK ---> SCK  (System Clock)
 *  BCLK ---> BCK  (Bit Clock)
 *  WS   ---> LRCK (Word Select [Left or Right])
 *  DIN <---- DOUT (Microphone Digital Out)
 *
 *  PCM1808 note (why we use 32-bit slots):
 *  In slave mode the ADC accepts 64 BCK/frame (or 48 BCK/frame with 384fs),
 *  NOT 32 BCK/frame. 16-bit stereo slots make BCK = 32*fs, which forces DOUT
 *  to bipolar zero (all silence). 32-bit stereo slots make BCK = 64*fs.
 *  MCLK stays at 256*fs. See TI PCM1808 datasheet §7.3.5.1.2.
 **/

// --- HARDWARE CONSTRAINTS ---
#define I2S_BCK_PIN 33
#define I2S_LRCK_PIN 32
#define I2S_DIN_PIN 35
#define I2S_SCK_PIN 0

// --- GLOBALS ---
static const char *TX = "TX_PACK";
i2s_chan_handle_t i2s_adc_chan;
uint8_t peers_connected = 0x00;
uint32_t dropped_packets = 0;

// I2S delivers one 32-bit word per slot; pack down to int16 later for packets.
static int32_t raw_audio_samples[AUDIO_DATA_NUM_SAMPLES];

// --- STRUCTS ---
typedef enum {
    TX_MODE_BOOTING,     // refactor later so booting just runs boot a few times
    TX_MODE_IDLING,      // triggered if 0% vol or 0 peers
    TX_MODE_TRANSMITTING // num_peers > 0
} tx_mode_t;

// --- CONFIGS ---
esp_now_rate_config_t peer_rate_cfg = {.rate = WIFI_PHY_RATE_24M,
                                       .phymode = WIFI_PHY_MODE_11G,
                                       .ersu = false,
                                       .dcm = false};

/**
 * @brief checks if recieved packet is an RX-pack requesting to connect with the
 * TX-pack. Adds the RX-pack as a TX-pack peer if it contains a valid alias.
 * Ignores if already connected or not a valid RX-pack. Logs invalid packets
 * without aborting; packet loss and malformed RX pings are not fatal.
 * @param *esp_now_info the packet meta-data containing the SRC_ADDRESS
 * @param *data pointer to full packet data
 * @param data_size size of recieved packet
 **/
static void recv_cb(const esp_now_recv_info_t *esp_now_info,
                    const uint8_t *data, int data_size) {

    if (!esp_now_info) {
        ESP_LOGW(TX, "Recieved NULL pointer on recv_cb.");
        return;
    }

    if (data_size != sizeof(pairing_req_packet_t)) {
        ESP_LOGW(TX,
                 "Received data size differs from pairing_req_packet_t size.");
        return;
    }

    pairing_req_packet_t *cur_pac = (pairing_req_packet_t *)data;
    if (strncmp(cur_pac->alias, BASE_RX_ALIAS, strlen(BASE_RX_ALIAS)) != 0) {
        ESP_LOGI(TX, "Current packet does not contain a valid RX alias.");
        return;
    }

    esp_now_peer_info_t new_peer = {
        .channel = DEFAULT_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(new_peer.peer_addr, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);

    esp_err_t rate_status =
        esp_now_set_peer_rate_config(new_peer.peer_addr, &peer_rate_cfg);

    if (rate_status != ESP_OK) {
        ESP_LOGW(TX, "Failed to configure peer rate.");
        return;
    } else {
        ESP_LOGI(TX, "Configured peer rate successfully.");
        return;
    }

    esp_err_t add_status = esp_now_add_peer(&new_peer);

    if (add_status == ESP_OK) {
        ESP_LOGI(TX, "RX paired successfully.");
        pair_state_t succ_msg = PAIR_SUCCESS;
        esp_now_send(new_peer.peer_addr, (uint8_t *)&succ_msg,
                     sizeof(succ_msg));
        peers_connected |= (~peers_connected) & (peers_connected + 1);
    } else if (add_status == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGI(TX, "RX already paired.");
    } else {
        ESP_LOGW(TX, "Failed to add RX as peer.");
        pair_state_t fail_msg = PAIR_FAIL;
        esp_now_send(new_peer.peer_addr, (uint8_t *)&fail_msg,
                     sizeof(fail_msg));
    }
}

// --- HARDWARE INIT ---

/**
 * @brief logs if transmit meta-data is null. Increments dropped_packets if the
 * packet was not received by at least one RX. Packet loss is not fatal.
 * @param *info transmit packet meta-data
 * @param status transmitted packet status
 **/
static void send_cb(const esp_now_send_info_t *info,
                    esp_now_send_status_t status) {

    if (!info) {
        ESP_LOGW(TX, "TX info is a NULL pointer.");
        return;
    }

    if (status != ESP_NOW_SEND_SUCCESS) {
        dropped_packets++;
        ESP_LOGI(TX, "Dropped packets: %lu", (unsigned long)dropped_packets);
    }
}

/**
 * @brief configure the esp to communicate with the external PCM1808 ADC by
 * configuring the i2s channel, configs, and enabling it.
 * @return ESP_OK on success, or the first failing I2S API esp_err_t.
 **/
static esp_err_t init_i2s_pcm1808(void) {
    i2s_chan_config_t i2s_chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    i2s_std_config_t i2s_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),

        // Must be 32-bit slots so BCK = 64*fs (PCM1808 rejects 16-bit / 32*fs).
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),

        .gpio_cfg = {.bclk = I2S_BCK_PIN,
                     .ws = I2S_LRCK_PIN,
                     .din = I2S_DIN_PIN,
                     .mclk = I2S_SCK_PIN,
                     .dout = I2S_GPIO_UNUSED},
    };

    // 256*fs is a valid PCM1808 SCKI ratio and divides cleanly with 64*fs BCK.
    i2s_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_new_channel(&i2s_chan_cfg, NULL, &i2s_adc_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TX, "Failed to create I2S ADC channel.");
        return err;
    }
    err = i2s_channel_init_std_mode(i2s_adc_chan, &i2s_std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TX, "Failed to init I2S ADC std mode.");
        return err;
    }
    err = i2s_channel_enable(i2s_adc_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TX, "Failed to enable I2S ADC channel.");
        return err;
    }
    gpio_set_drive_capability(0, GPIO_DRIVE_CAP_3);
    return ESP_OK;
}

// --- MAIN THREAD ---
void app_main(void) {
    printf("Booting TX Node...\n");

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_wifi());
    ESP_ERROR_CHECK(init_espnow(recv_cb, send_cb));
    ESP_ERROR_CHECK(init_i2s_pcm1808());

    printf("Waiting for RX bodypacks to ping...\n");

    // Create your audio_packet_t payload struct variable and initialize
    // the packet_id to 0.
    audio_packet_t audio_samples = {.packet_id = 0};
    // Keep outside the loop so % 20 actually throttles prints.
    uint32_t loop_counter = 0;

    while (1) {
        size_t bytes_read = 0;
        if (1) { // TODO: replace with peers_connected when done testing
            // Read 32-bit I2S slots (needed for PCM1808 BCK timing), then
            // convert. Use portMAX_DELAY so the task blocks until the buffer is
            // 100% full. Do not idle with vTaskDelay after a successful read —
            // that overruns DMA once the ADC is actually streaming.
            esp_err_t i2s_read_status = i2s_channel_read(
                i2s_adc_chan, raw_audio_samples, sizeof(raw_audio_samples),
                &bytes_read, portMAX_DELAY);

            if (i2s_read_status != ESP_OK || bytes_read == 0) {
                ESP_LOGW(TX, "Failed to read bytes from I2S ADC Channel.");
                // TODO: Remove after testing
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            size_t samples_read = bytes_read / sizeof(raw_audio_samples[0]);
            int32_t peak_level = 0;

            for (size_t i = 0; i < samples_read; i++) {
                // PCM1808 is 24-bit, MSB-aligned in the 32-bit Philips slot. >>
                // 16 keeps the top 16 bits for our int16 packet payload.
                int32_t sample = raw_audio_samples[i] >> 16;
                audio_samples.audio_data[i] = (int16_t)sample;

                int32_t magnitude = sample < 0 ? -sample : sample;
                if (magnitude > peak_level) {
                    peak_level = magnitude;
                }
            }

            // TODO: remove this after testing
            // Peak over the buffer avoids printing a near-zero sine crossing.
            if (loop_counter++ % 20 == 0) {
                printf("Mic peak: %ld\n", (long)peak_level);
            }

            // Send once full and increment packet id
            // TODO: esp_now_send(NULL, audio_samples_address,
            // audio_samples_size);
            audio_samples.packet_id++;

        } else {
            vTaskDelay(pdMS_TO_TICKS(100)); // Idling
        }
    }
}
