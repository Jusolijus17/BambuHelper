#ifndef AUDIO_VIZ_PROTOCOL_H
#define AUDIO_VIZ_PROTOCOL_H

// ---------------------------------------------------------------------------
//  Shared ESP-NOW protocol between the audio sender (ESP32 + jack/FFT) and
//  the BambuHelper display (this project).
//
//  This header lives in include/ so it is automatically on the include path
//  for every PlatformIO environment, including the standalone [env:audio_sender]
//  firmware.
//
//  Channel discovery:
//    The display is connected to WiFi as a STA, so its radio is locked to the
//    router's channel. The sender is NOT on WiFi, so it must transmit on that
//    same channel to be heard. To find it, the display periodically broadcasts
//    a small BEACON packet on its current channel; the sender hops across the
//    common channels (1/6/11) until it catches a beacon, then locks onto that
//    channel and starts streaming DATA packets.
// ---------------------------------------------------------------------------

#include <stdint.h>

// Magic byte identifying our packets (avoid reacting to foreign ESP-NOW traffic)
#define AVIZ_MAGIC      0xB6   // "Bambu" viz
#define AVIZ_VERSION    1

// Packet types
#define AVIZ_TYPE_BEACON 1     // display  -> broadcast : "I'm here on this channel"
#define AVIZ_TYPE_DATA   2     // sender   -> display   : spectrum bands

// Number of frequency bands (bars). Must match on both sides.
#define AVIZ_NUM_BANDS  16

// Channels the sender scans during discovery (2.4 GHz non-overlapping first).
#define AVIZ_SCAN_CHANNELS { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 }

// Beacon broadcast by the display so the sender can find its WiFi channel.
typedef struct __attribute__((packed)) {
  uint8_t magic;    // AVIZ_MAGIC
  uint8_t type;     // AVIZ_TYPE_BEACON
  uint8_t version;  // AVIZ_VERSION
  uint8_t channel;  // display's current WiFi channel (informational)
} AVizBeacon;

// Spectrum data sent by the audio sender to the display.
typedef struct __attribute__((packed)) {
  uint8_t magic;                 // AVIZ_MAGIC
  uint8_t type;                  // AVIZ_TYPE_DATA
  uint8_t version;               // AVIZ_VERSION
  uint8_t numBands;              // AVIZ_NUM_BANDS
  uint8_t bands[AVIZ_NUM_BANDS]; // 0..255 normalized magnitude per band
} AVizData;

#endif // AUDIO_VIZ_PROTOCOL_H
