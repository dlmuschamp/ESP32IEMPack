#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

// HARDWARE MAPPING
#define LRCK_PIN 32
#define BCK_PIN 33
#define DOUT_PIN 25

// AUDIO CONFIGURATION
#define SAMPLE_RATE 44100
#define TONE_FREQ 440
//use >> later
#define VOL_ATTEN 0.001

// HIGH and LOW signals
#define HIGH_SIG 32767
#define LOW_SIG -32767

// GLOBAL CONSTS
#define DEFAULT_BUF_SIZE 256

// Global handle for the I2S channel
i2s_chan_handle_t tx_chan;

// INITIALIZATION
void init_i2s(void)
{
    // FREEBIE 1: Allocating the channel.
    // ESP-IDF v5 requires allocating a channel handle before configuring it.
    i2s_chan_config_t alloc_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_new_channel(&alloc_cfg, &tx_chan, NULL);

    // Setting up the standard i2s config with my desired pins and info
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BCK_PIN,
            .ws = LRCK_PIN,
            .dout = DOUT_PIN,
            .din = I2S_GPIO_UNUSED
        }
    };
    // FREEBIE 2: Applying the configuration and turning it on.
    i2s_channel_init_std_mode(tx_chan, &std_cfg);
    i2s_channel_enable(tx_chan);
}

// MAIN APPLICATION TASK
void app_main(void)
{
    printf("Booting Audio Transmitter...\n");
    init_i2s();

    // We will generate a small chunk of audio at a time.
    int16_t sample_buffer[DEFAULT_BUF_SIZE];
    size_t bytes_written = 0;
    printf("Entering Audio Loop...\n");

    int cur_sample = 0;

    while (1)
    {
        // Calculate the number of samples per cycle and per half-cycle
        int samples_per_cycle = SAMPLE_RATE / TONE_FREQ;
        int samples_per_half_cycle = samples_per_cycle / 2;

        // Loop Indicies
        int buf_idx = 0;

        // 1. Create a loop to fill exactly the audio frames (remember, 1 frame = Left + Right channel).
        while (buf_idx < DEFAULT_BUF_SIZE)
        {
            // 2. Generate a square wave: If you are in the first half of the cycle, make the value high. If in the second half, make it low.
            int sig_mode = (cur_sample < samples_per_half_cycle) ? HIGH_SIG : LOW_SIG;

            // 3. Multiply raw wave value by your VOLUME_ATTEN macro.
            int final_sig = sig_mode * VOL_ATTEN;

            // 4. Assign this final value to both the left and right slots in the array.
        sample_buffer[buf_idx] = final_sig;
        sample_buffer[buf_idx + 1] = final_sig;

            // 5. Update indices
            buf_idx += 2;
            cur_sample++; 

            if (cur_sample >= samples_per_cycle) {
                cur_sample = 0;
            }

        }

        // Send the buffer to the DAC.
        i2s_channel_write(tx_chan, sample_buffer, sizeof(sample_buffer), &bytes_written, portMAX_DELAY);
    }
}