#include "driver/ledc.h" // Added for PWM LED control
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rig_shared.h" // Your custom struct definitions

// --- HARDWARE MAPPING ---
#define TX_LED_PIN 32
#define POT_WIPER_PIN 33
#define ADC_CH ADC_CHANNEL_5 // Pin 33 maps to ADC1 Channel 5

// --- GLOBAL VARIABLES ---
#define MAC_ADDRESS_SIZE 6
#define SAMPLE_RATE 44100
#define HIGH_SIG 16384
#define LOW_SIG -16384
iem_state_t current_state = STATE_BOOTING;
adc_oneshot_unit_handle_t adc1_handle;

// --- 1. NVS INITIALIZATION ---
void init_nvs(void) {
  esp_err_t nvs_flash_status = nvs_flash_init();
  if (nvs_flash_status == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_flash_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_flash_status = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_flash_status);
}

// --- 2. WI-FI INITIALIZATION ---
void init_wifi(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
}

// --- 3. ESP-NOW INITIALIZATION ---
void init_espnow(void) {
  uint8_t broadcast_mac[MAC_ADDRESS_SIZE] = {0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF};
  ESP_ERROR_CHECK(esp_now_init());
  esp_now_peer_info_t peer = {
      .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
  memcpy(peer.peer_addr, broadcast_mac, MAC_ADDRESS_SIZE);
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

// --- 4. INTERNAL ADC INITIALIZATION (Boilerplate) ---
void init_adc(void) {
  adc_oneshot_unit_init_cfg_t init_config = {
      .unit_id = ADC_UNIT_1,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

  adc_oneshot_chan_cfg_t config = {
      .bitwidth = ADC_BITWIDTH_12, // Reads values from 0 to 4095
      .atten = ADC_ATTEN_DB_12,    // Allows measuring up to ~3.3V
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CH, &config));
}

// --- 5. LED PWM INITIALIZATION (Boilerplate) ---
void init_led(void) {
  ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                               .duty_resolution =
                                   LEDC_TIMER_12_BIT, // 0-4095 brightness scale
                               .timer_num = LEDC_TIMER_0,
                               .freq_hz = 5000,
                               .clk_cfg = LEDC_AUTO_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&timer));

  ledc_channel_config_t channel = {.gpio_num = TX_LED_PIN,
                                   .speed_mode = LEDC_LOW_SPEED_MODE,
                                   .channel = LEDC_CHANNEL_0,
                                   .timer_sel = LEDC_TIMER_0,
                                   .duty = 0,
                                   .hpoint = 0};
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

// Helper to set LED brightness (Feed it 0 to 4095)
void set_led_brightness(uint32_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// --- MAIN APPLICATION ENTRY ---
void app_main(void) {
  printf("Booting Transmitter Node...\n");

  // Execute hardware initialization
  init_nvs();
  init_wifi();
  init_espnow();
  init_adc();
  init_led();

  current_state = STATE_SEARCHING;
  printf("Initialization complete. TX Node Active.\n");

  int pot_val = 0;
  uint8_t broadcast_mac[MAC_ADDRESS_SIZE] = {0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF};

  // Instantiate your custom audio packet struct here to use in the loop!

  while (1) {
    // TODO 1: Read the ADC value into 'pot_val'
    // Hint: adc_oneshot_read(adc1_handle, ADC_CH, &pot_val);
    adc_oneshot_read(adc1_handle, ADC_CH, &pot_val);

    // TODO 2: Update the LED brightness directly using pot_val
    set_led_brightness(pot_val);
    // TODO 3: Map pot_val (which is 0-4095) to a target frequency (e.g., 200Hz
    // to 2000Hz)

    // TODO 4: Fill your audio packet's payload array!
    // We need to generate a square wave at the target frequency.
    // Assuming a sample rate of 16000 Hz, how many samples in the array
    // stay HIGH (+16384) before flipping LOW (-16384)?

    // TODO 5: Transmit the packet via ESP-NOW.
    // Hint: esp_now_send(broadcast_mac, (uint8_t *) &your_packet,
    // sizeof(your_packet));

    vTaskDelay(pdMS_TO_TICKS(
        10)); // Run this loop 100 times a second to prevent watchdog crashes
  }
}

/*
// --- AT THE TOP OF YOUR FILE ---
#include <math.h> // Needed for powf()

#define NUM_INTERVALS 16 // Must be a power of 2! (8, 16, 32...)
#define NUM_POINTS (NUM_INTERVALS + 1)
#define MIN_FREQ 30.0f
#define MAX_FREQ 20000.0f
#define SAMPLE_RATE 16000 // Standard audio sample rate
#define AMPLITUDE 16384   // 50% volume for int16_t

int curve_lut[NUM_POINTS]; // Empty array to hold our frequencies

// Add this function above app_main()
void generate_lut(void) {
    float multiplier = powf((MAX_FREQ / MIN_FREQ), (1.0f / NUM_INTERVALS));
    curve_lut[0] = (int)MIN_FREQ;
    for (int i = 1; i < NUM_POINTS; i++) {
        curve_lut[i] = (int)(curve_lut[i-1] * multiplier);
    }
}void app_main(void) {
    // ... all your init functions ...
    generate_lut(); // Fill the array before the loop!

    int pot_val = 0;
    int target_freq = MIN_FREQ;

    // --- WAVE TRACKING VARIABLES (State) ---
    // You need variables here to remember where the wave is
    // when a packet ends and the next one begins.
    int current_wave_state = AMPLITUDE; // Starts HIGH
    int samples_until_flip = 0;         // How many samples before we flip from
HIGH to LOW?

    // Instantiate your audio packet here!
    // e.g., iem_audio_packet_t audio_packet;

    while (1) {
        // 1. Read the ADC
        adc_oneshot_read(adc1_handle, ADC_CH, &pot_val);
        set_led_brightness(pot_val);

        // TODO A: Calculate the target_freq using the bitwise lerp trick.
        // Hint: You'll need to figure out the right bit-shift for
NUM_INTERVALS.
        // If NUM_INTERVALS is 16, pot_val / 16 means shifting by...?


        // TODO B: Figure out the half-period of the target_freq.
        // How many total samples does it take for ONE phase (just the HIGH
part, or just the LOW part)?
        // Hint: SAMPLE_RATE / target_freq = samples for a FULL wave.


        // TODO C: Fill the packet's payload array sample by sample.
        // Loop through every index in your audio_packet.payload.
        // For each sample:
        //   1. Assign 'current_wave_state' to the array.
        //   2. Decrement 'samples_until_flip'.
        //   3. If 'samples_until_flip' hits 0:
        //        - Invert 'current_wave_state' (e.g., multiply by -1)
        //        - Reset 'samples_until_flip' using the half-period you
calculated in TODO B.


        // TODO D: Transmit the packet via ESP-NOW!

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}




















*/
