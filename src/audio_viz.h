#ifndef AUDIO_VIZ_H
#define AUDIO_VIZ_H

// Receiver side of the audio visualizer feature.
// Initializes ESP-NOW, broadcasts a discovery beacon so the sender can find
// our WiFi channel, receives spectrum packets, and renders the bars.

#include <stdbool.h>

// Call once after WiFi has been started (STA mode). Safe to call even if WiFi
// is not yet connected; the beacon uses whatever channel the radio is on.
void initAudioViz();

// Call every loop(): emits the periodic discovery beacon.
void audioVizLoop();

// True if a spectrum packet arrived recently (i.e. music is playing).
bool audioVizActive();

// Reset render state (call when entering the visualizer screen).
void resetVisualizer();

// Draw/update the bars. Call from the display refresh path.
void drawVisualizer();

#endif // AUDIO_VIZ_H
