#ifndef RIG_SHARED_H
#define RIG_SHARED_H

#include <stdint.h>

// --- ESP-NOW CONSTRAINTS ---
#define MAX_ESPNOW_PAYLOAD_BYTES 1470
#define AUDIO_DATA_NUM_SAMPLES 734
#define ALIAS_BUFFER_SIZE 10
#define SAMPLE_RATE 44100
#define DEFAULT_CHANNEL 0

#define BASE_RX_ALIAS "IEM_RX_" // concat using snprintf()

// --- HARDWARE FLAGS (Bitwise) ---

// --- OPERATING STATES ---

/**
 * @brief Move to rx.c
*/
typedef enum RX_States {
  STATE_RX_BOOTING,
  STATE_RX_SEARCHING,
  STATE_RX_CONNECTED,
  STATE_RX_DISCONNECTED
} rx_state_t;

// --- PACKET STRUCTURES ---

/**
 * @brief A packet containing the RX MAC address. Used to begin the
 * pairing process with the TX.
*/
typedef struct Discovery {
  char alias[ALIAS_BUFFER_SIZE];
} discovery_packet_t;

/**
 * @brief A packet including a packet_id and the recieved audio samples.
*/
typedef struct Audio {
  uint8_t packet_id;
  int16_t audio_data[AUDIO_DATA_NUM_SAMPLES];
} audio_packet_t;

#endif // RIG_SHARED_H
