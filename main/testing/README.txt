IEM Pack — diagnostic / isolation firmware (not shipping)
=========================================================
Kept for reference after 2026-08-21 sound-profile work. Shipping images
build from main/tx.c and main/rx.c only (IEM_NODE=TX|RX).

Build (from project root, same as before):
  IEM_NODE=RX_DAC_TEST idf.py reconfigure build flash
  IEM_NODE=TX_TONE     idf.py reconfigure build flash
  IEM_NODE=TX_BUZZ_DEBUG idf.py reconfigure build flash

Files:
  sound_profile_tones.h   — shared DDS tones + esp_timer packet pace
  rx_dac_profile_test.c   — local PCM5102 tone cycle (no RF)
  tx_tone_bypass.c        — ESP-NOW TX with ADC skipped (TX_TONE_BYPASS_ADC)
  tx_buzz_debug.c         — shipping TX + ADC pre-gain peak logs

See docs/SOUND_PROFILE_TESTS.txt and docs/BENCH_REALIZATIONS.txt.
