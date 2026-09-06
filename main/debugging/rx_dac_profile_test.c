/**
 * @file rx_dac_profile_test.c
 * @brief RX pack local DAC sound-profile isolation (no ESP-NOW / no TX).
 *
 * Build: IEM_NODE=RX_DAC_TEST idf.py reconfigure build flash monitor
 *
 * Same 48 kHz Philips 16-bit I2S path + msb_right fix as shipping rx.c.
 */

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "../rig_shared.h"
#include "sound_profile_tones.h"

#define LRCK_PIN 32
#define BCK_PIN 33
#define DOUT_PIN 25
#define I2S_DMA_DESC_NUM 3
#define I2S_DMA_FRAME_NUM AUDIO_FRAMES_PER_PACKET
#define MODE_HOLD_MS 8000

static const char *TAG = "RX_DAC_TEST";
static i2s_chan_handle_t i2s_dac_chan;

static esp_err_t init_i2s_pcm5102(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;

    esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_dac_chan, NULL);
    if (err != ESP_OK) {
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
#if SOC_I2S_HW_VERSION_1
    std_cfg.slot_cfg.msb_right = false;
#endif
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(i2s_dac_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(i2s_dac_chan);
}

void app_main(void)
{
    ESP_LOGI(TAG, "RX DAC profile test @ %d Hz (no RF).", SAMPLE_RATE);
    ESP_ERROR_CHECK(init_i2s_pcm5102());

    static int16_t buf[AUDIO_DATA_NUM_SAMPLES];
    double phase = 0.0;
    uint32_t sample_clock = 0;
    tone_mode_t mode = TONE_SINE_440;
    TickType_t mode_started = xTaskGetTickCount();

    ESP_LOGI(TAG, "mode=%s (hold %d ms each)", tone_mode_name(mode), MODE_HOLD_MS);

    while (1) {
        TickType_t now = xTaskGetTickCount();
        if ((now - mode_started) >= pdMS_TO_TICKS(MODE_HOLD_MS)) {
            mode = (tone_mode_t)((mode + 1) % TONE_MODE_COUNT);
            phase = 0.0;
            sample_clock = 0;
            mode_started = now;
            ESP_LOGI(TAG, "mode=%s", tone_mode_name(mode));
        }

        fill_tone_stereo(buf, mode, &phase, &sample_clock);

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(i2s_dac_chan, buf, sizeof(buf),
                                          &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(err));
        }
    }
}
