IEM Pack — diagnostic / isolation / archive firmware (not shipping)
====================================================================
Kept for reference after 2026-08-21 sound-profile work and earlier
bring-up. Shipping images build from main/tx.c and main/rx.c only
(IEM_NODE=TX|RX).

Build (from project root):
  IEM_NODE=RX_DAC_TEST   idf.py reconfigure build flash
  IEM_NODE=TX_TONE       idf.py reconfigure build flash
  IEM_NODE=TX_BUZZ_DEBUG idf.py reconfigure build flash

Build targets:
  sound_profile_tones.h   — shared DDS tones + esp_timer packet pace
  rx_dac_profile_test.c   — local PCM5102 tone cycle (no RF)
  tx_tone_bypass.c        — ESP-NOW TX with ADC skipped (TX_TONE_BYPASS_ADC)
  tx_buzz_debug.c         — shipping TX + ADC pre-gain peak logs

Archive / helpers (not wired as IEM_NODE targets):
  square_wave_test.c      — early DAC I2S sanity
  tx_pre.c                — pre-shipping TX draft
  rx_temp_debug.h         — optional RX_DBG macros (included from rx.c)

See docs/SOUND_PROFILE_TESTS.txt and docs/BENCH_REALIZATIONS.txt.
