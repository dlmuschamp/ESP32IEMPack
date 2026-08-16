#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "nvs.h"
#include "nvs_flash.h"
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
 *  SNAPSHOT: pre-fix copy of tx.c (16-bit I2S slots). Kept for review.
 *  Active firmware is tx.c — see comments there for the PCM1808 BCK fix.
 **/

// DEBUG LED PIN
#define INTERNAL_LED_PIN 2

#define I2S_BCK_PIN 33
#define I2S_LRCK_PIN 32
#define I2S_DIN_PIN 35
#define I2S_SCK_PIN 0 // 0

// --- GLOBALS ---
static const char *TAG = "TX_NODE";
i2s_chan_handle_t i2s_adc_chan;
uint8_t peers_connected = 0x00;
uint32_t dropped_packets = 0;

// --- THE HANDSHAKE CALLBACK ---
void on_esp_now_recv(const esp_now_recv_info_t *esp_now_info,
                     const uint8_t *data, int data_len) {

  if (data_len != sizeof(discovery_packet_t)) {
    ESP_LOGE(TAG, "Received data size is different than "
             "discovery_packet_t. Ignoring.");
    return;
  }

  discovery_packet_t *cur_pac = (discovery_packet_t *)data;
  if (strncmp(cur_pac->alias, BASE_RX_ALIAS, strlen(BASE_RX_ALIAS)) != 0) {
    ESP_LOGI(TAG,
             "Current packet does not contain a valid RX alias. Ignoring");
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
  esp_err_t nvs_status = nvs_flash_init();

  if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES ||
    nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "Failed to initialize NVS flash. Erasing and retrying.");
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  ESP_ERROR_CHECK(nvs_status);
  ESP_LOGI(TAG, "NVS initialized successfully.");
}

// not doing long range mode
void init_wifi(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  // TODO: replace rig shared with these wifi values
  ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
}

void transmit_cb(const esp_now_send_info_t *tx_info,
                 esp_now_send_status_t status) {

  if (!tx_info) {
    ESP_LOGE(TAG, "TX info passed a null pointer.");
    return;
  }

  if (status != ESP_NOW_SEND_SUCCESS) {
    dropped_packets++;
  }
}

// decomposing this functoin
void init_espnow(void) {
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_esp_now_recv));
  ESP_ERROR_CHECK(esp_now_register_send_cb(transmit_cb));
}

void init_i2s_microphone(void) {
  i2s_chan_config_t i2s_chan_cfg =
    I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

  i2s_std_config_t i2s_std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),

    .gpio_cfg = {.bclk = I2S_BCK_PIN,
      .ws = I2S_LRCK_PIN,
      .din = I2S_DIN_PIN,
      .mclk = I2S_SCK_PIN,
      .dout = I2S_GPIO_UNUSED
    },
  };


  i2s_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  ESP_ERROR_CHECK(i2s_new_channel(&i2s_chan_cfg, NULL, &i2s_adc_chan));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_adc_chan, &i2s_std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(i2s_adc_chan));
  gpio_set_drive_capability(0, GPIO_DRIVE_CAP_3);
}

// --- MAIN THREAD ---
void app_main(void) {
  printf("Booting TX Node...\n");

  init_nvs();
  init_wifi();
  init_espnow();
  init_i2s_microphone();

  printf("Waiting for RX bodypacks to ping...\n");

  // remove this after testing.
  gpio_reset_pin(INTERNAL_LED_PIN);
  gpio_set_direction(INTERNAL_LED_PIN, GPIO_MODE_OUTPUT);

  // Create your audio_packet_t payload struct variable and initialize
  // the packet_id to 0.
  audio_packet_t audio_samples = {.packet_id = 0};
  size_t audio_data_size = sizeof(audio_samples.audio_data);

  while (1) {
    size_t bytes_read = 0;
    if (1) { // TODO: replace with peers_connected when done testing
      // Read from i2s_adc_chan to audio_data array.
      // Use portMAX_DELAY so the task blocks until the buffer is 100%
      // full.
      esp_err_t i2s_read_status =
        i2s_channel_read(i2s_adc_chan, audio_samples.audio_data,
                         audio_data_size, &bytes_read, portMAX_DELAY);

      if (i2s_read_status != ESP_OK || bytes_read == 0) {
        ESP_LOGW(TAG, "Failed to read bytes from I2S ADC Channel.");
        // TODO: Remove after testing 
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      // TODO: remove this after testing

      int loop_counter = 0;

      if (loop_counter++ % 20 == 0) {
        int16_t sample_val = *((int16_t *)&audio_samples.audio_data[0]);
        printf("Mic level: %d\n", sample_val);


      }
      // Send once full and increment packet id
      // TODO: esp_now_send(NULL, audio_samples_address,
      // audio_samples_size);
      audio_samples.packet_id++;

      vTaskDelay(pdMS_TO_TICKS(100)); // Idling

    } else {
      vTaskDelay(pdMS_TO_TICKS(100)); // Idling
    }
  }
}
