#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

// ============================================================================
// HARDWARE MAPPING
// ============================================================================

// TODO: Define your three GPIO pins for the DAC (BCK, WS/LRCK, DO/DIN).
// Hint: You can use standard #define macros here. Check your physical breadboard wiring.

// ============================================================================
// AUDIO CONFIGURATION
// ============================================================================

#define SAMPLE_RATE 44100
#define TONE_FREQ 440
// TODO: Define a VOLUME_ATTEN macro.
// Keep this extremely low (e.g., 0.01) to protect your ears and the IEM drivers.

// Global handle for the I2S channel
i2s_chan_handle_t tx_chan;

// ============================================================================
// INITIALIZATION
// ============================================================================

void init_i2s(void)
{
    // FREEBIE 1: Allocating the channel.
    // ESP-IDF v5 requires allocating a channel handle before configuring it.
    i2s_chan_alloc_config_t alloc_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_new_channel(&alloc_cfg, &tx_chan, NULL);

    // TODO: Configure the I2S standard mode parameters.
    // 1. You need to declare an 'i2s_std_config_t' struct.
    // 2. You will need to map your defined GPIO pins into the 'gpio_cfg' section of this struct.
    // 3. You can set the 'mclk' (Master Clock) and 'din' (Data In from DAC to ESP) to I2S_GPIO_UNUSED.
    // Hint: Look up the 'I2S_STD_CLK_DEFAULT_CONFIG' and 'I2S_STD_MSB_SLOT_DEFAULT_CONFIG' macros in the ESP-IDF documentation to fill the clock and slot fields.

    // FREEBIE 2: Applying the configuration and turning it on.
    // i2s_channel_init_std_mode(tx_chan, &YOUR_STRUCT_NAME_HERE);
    // i2s_channel_enable(tx_chan);
}

// ============================================================================
// MAIN APPLICATION TASK
// ============================================================================

void app_main(void)
{
    printf("Booting Audio Transmitter...\n");
    init_i2s();

    // We will generate a small chunk of audio at a time.
    int16_t sample_buffer[200];
    size_t bytes_written = 0;

    // TODO: Calculate how many total samples make up one single cycle of your target frequency (440Hz).
    // Hint: How does Sample Rate relate to Frequency?
    int samples_per_cycle = 0;
    int current_sample = 0;

    printf("Entering Audio Loop...\n");

    while (1)
    {
        // TODO: Fill the sample_buffer array.
        // 1. Create a loop to fill exactly 100 audio frames (remember, 1 frame = Left + Right channel).
        // 2. Generate a square wave: If you are in the first half of the cycle, make the value high (e.g., 32767). If in the second half, make it low (e.g., -32767).
        // 3. Multiply your raw wave value by your VOLUME_ATTEN macro.
        // 4. Assign this final value to both the left and right slots in the array.

        // TODO: Send the buffer to the DAC.
        // Hint: Use the 'i2s_channel_write' function. You will need to pass your channel handle, the buffer, the size of the buffer in bytes, and a pointer to 'bytes_written'.
    }
}