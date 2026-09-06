/**
 * @file sound_profile_tones.h
 * @brief Shared DDS tone fillers for sound-profile isolation tests.
 *
 * Used by RX_DAC_TEST (local PCM5102) and TX_TONE (ESP-NOW, ADC bypass).
 * Designed to expose HF roll-off that a lone 440 Hz tone cannot reveal.
 */
#ifndef SOUND_PROFILE_TONES_H
#define SOUND_PROFILE_TONES_H

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../rig_shared.h"
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** Headroom below int16 full scale (avoids DAC/amp clipping). */
#define TONE_AMP 12000

typedef enum {
    TONE_SINE_440 = 0, /**< Mid reference — LPF usually leaves this alone */
    TONE_SINE_8K,      /**< HF probe — dull/missing if soft LPF */
    TONE_ALT_440_8K,   /**< 1 s each — easiest ear A/B */
    TONE_SQUARE_440,   /**< Harmonics up the spectrum (buzz vs sine) */
    TONE_CHIRP,        /**< 200 Hz → 12 kHz — hear where energy dies */
    TONE_MODE_COUNT
} tone_mode_t;

static inline const char *tone_mode_name(tone_mode_t mode)
{
    switch (mode) {
    case TONE_SINE_440: return "sine_440";
    case TONE_SINE_8K: return "sine_8k";
    case TONE_ALT_440_8K: return "alt_440_8k";
    case TONE_SQUARE_440: return "square_440";
    case TONE_CHIRP: return "chirp_200_12k";
    default: return "unknown";
    }
}

/**
 * @brief Fill interleaved stereo int16 PCM from a continuous phase.
 */
static inline void fill_tone_stereo(int16_t *out, tone_mode_t mode,
                                    double *phase_inout, uint32_t *sample_clock)
{
    const double two_pi = 2.0 * M_PI;
    const double dt = 1.0 / (double)SAMPLE_RATE;
    double phase = *phase_inout;
    uint32_t clock = *sample_clock;

    for (int i = 0; i < AUDIO_DATA_NUM_SAMPLES; i += 2) {
        double freq_hz = 0.0;
        int16_t s = 0;

        switch (mode) {
        case TONE_SINE_440:
            freq_hz = 440.0;
            s = (int16_t)(sinf((float)phase) * (float)TONE_AMP);
            phase += two_pi * freq_hz * dt;
            break;
        case TONE_SINE_8K:
            freq_hz = 8000.0;
            s = (int16_t)(sinf((float)phase) * (float)TONE_AMP);
            phase += two_pi * freq_hz * dt;
            break;
        case TONE_ALT_440_8K: {
            uint32_t sec = (clock / SAMPLE_RATE) & 1u;
            freq_hz = sec ? 8000.0 : 440.0;
            s = (int16_t)(sinf((float)phase) * (float)TONE_AMP);
            phase += two_pi * freq_hz * dt;
            break;
        }
        case TONE_SQUARE_440:
            freq_hz = 440.0;
            s = (sinf((float)phase) >= 0.0f) ? TONE_AMP : (int16_t)(-TONE_AMP);
            phase += two_pi * freq_hz * dt;
            break;
        case TONE_CHIRP: {
            const double chirp_len_s = 2.0;
            const double f0 = 200.0;
            const double f1 = 12000.0;
            double t = fmod((double)clock * dt, chirp_len_s) / chirp_len_s;
            freq_hz = f0 + (f1 - f0) * t;
            s = (int16_t)(sinf((float)phase) * (float)TONE_AMP);
            phase += two_pi * freq_hz * dt;
            break;
        }
        default:
            break;
        }

        if (phase >= two_pi) {
            phase = fmod(phase, two_pi);
        }

        out[i] = s;
        out[i + 1] = s;
        clock++;
    }

    *phase_inout = phase;
    *sample_clock = clock;
}

/**
 * @brief Pace one packet to realtime (AUDIO_FRAMES_PER_PACKET @ SAMPLE_RATE).
 *
 * Do NOT use pdMS_TO_TICKS for sub-10 ms periods: with CONFIG_FREERTOS_HZ=100
 * that truncates to 0 ticks, so TX_TONE floods ESP-NOW and RX queue-drops.
 * Pass a zeroed int64_t once; updated in place across calls.
 */
static inline void tone_pace_packet(int64_t *next_due_us)
{
    const int64_t period_us =
        ((int64_t)AUDIO_FRAMES_PER_PACKET * 1000000LL) / (int64_t)SAMPLE_RATE;
    int64_t now = esp_timer_get_time();

    if (*next_due_us == 0) {
        *next_due_us = now + period_us;
        return;
    }

    while ((now = esp_timer_get_time()) < *next_due_us) {
        taskYIELD();
    }

    *next_due_us += period_us;
    now = esp_timer_get_time();
    if (now > *next_due_us) {
        /* Fell behind — resync rather than bursting catch-up packets. */
        *next_due_us = now + period_us;
    }
}

#endif /* SOUND_PROFILE_TONES_H */
