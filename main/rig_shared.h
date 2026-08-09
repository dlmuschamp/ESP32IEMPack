#ifndef RIG_SHARED_H
#define RIG_SHARED_H

#include <stdint.h>

// --- ESP-NOW CONSTRAINTS ---
#define MAX_ESPNOW_PAYLOAD_BYTES 1470
#define ALIAS_BUFFER_SIZE 10
#define AUDIO_DATA_NUM_SAMPLES 734

// --- HARDWARE FLAGS (Bitwise) ---
#define FEATURE_48KHZ 0b00000001
#define FEATURE_STEREO 0b00000100

// --- OPERATING STATES ---
typedef enum States {
  STATE_BOOTING,
  STATE_SEARCHING,
  STATE_PAIRED
} iem_state_t;

// --- PACKET STRUCTURES ---
typedef struct Discovery {
  uint8_t flags;
  char alias[ALIAS_BUFFER_SIZE];
} discovery_packet_t;

typedef struct Audio {
  uint8_t seq_num;
  int16_t audio_data[AUDIO_DATA_NUM_SAMPLES];
} audio_packet_t;

#endif // RIG_SHARED_H
