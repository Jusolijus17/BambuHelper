#include "audio_viz.h"
#include "display_ui.h"
#include "settings.h"
#include "layout.h"
#include "audio_viz_protocol.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------------------------------------------------------------------------
//  Received spectrum state (written from the ESP-NOW callback context)
// ---------------------------------------------------------------------------
static volatile uint8_t       rxBands[AVIZ_NUM_BANDS] = {0};
static volatile unsigned long rxLastMs = 0;

static const uint8_t BCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define BEACON_INTERVAL_MS  250    // how often we advertise our channel
#define AUDIO_TIMEOUT_MS    1500   // no packet for this long -> "music stopped"

static unsigned long lastBeaconMs = 0;
static bool          espNowReady  = false;

// ESP-NOW receive callback (handles arduino-esp32 v2 and v3 signatures)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len < (int)sizeof(AVizData)) return;
  const AVizData *d = (const AVizData *)data;
  if (d->magic != AVIZ_MAGIC || d->type != AVIZ_TYPE_DATA) return;
  uint8_t n = d->numBands;
  if (n > AVIZ_NUM_BANDS) n = AVIZ_NUM_BANDS;
  for (uint8_t i = 0; i < n; i++) rxBands[i] = d->bands[i];
  rxLastMs = millis();
}

void initAudioViz() {
  if (espNowReady) return;

  // WiFi is started by wifi_manager before this is called. ESP-NOW coexists
  // with the STA connection on the same channel.
  if (esp_now_init() != ESP_OK) {
    Serial.println("[AVIZ] esp_now_init failed");
    return;
  }
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.channel = 0;             // 0 = use current radio channel
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(BCAST_ADDR)) esp_now_add_peer(&peer);

  espNowReady = true;
  Serial.println("[AVIZ] ESP-NOW ready, broadcasting beacon");
}

void audioVizLoop() {
  if (!espNowReady) return;
  unsigned long now = millis();
  if (now - lastBeaconMs < BEACON_INTERVAL_MS) return;
  lastBeaconMs = now;

  AVizBeacon b;
  b.magic   = AVIZ_MAGIC;
  b.type    = AVIZ_TYPE_BEACON;
  b.version = AVIZ_VERSION;
  b.channel = WiFi.channel();
  esp_now_send(BCAST_ADDR, (const uint8_t *)&b, sizeof(b));
}

bool audioVizActive() {
  if (rxLastMs == 0) return false;
  return (millis() - rxLastMs) < AUDIO_TIMEOUT_MS;
}

// ---------------------------------------------------------------------------
//  Rendering
// ---------------------------------------------------------------------------
#define NB           AVIZ_NUM_BANDS
#define VIZ_GAP      3                       // px gap between bars
#define VIZ_TOP      (LY_H / 12)             // top margin
#define VIZ_MAX_H    (LY_H - VIZ_TOP)        // max bar height in px

static float    barH[NB];      // smoothed bar height (px)
static float    peakH[NB];     // falling peak height (px)
static int      drawnBar[NB];  // last drawn bar height (px)
static int      drawnPeak[NB]; // last drawn peak position (px)
static uint16_t bandColor[NB];
static bool     vizInit = false;

// Simple HSV(h in [0,360), s=v=1) -> RGB565 for a rainbow across the bands.
static uint16_t hueColor(float h) {
  float c = 1.0f, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
  float r = 0, g = 0, b = 0;
  if      (h < 60)  { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else              { r = c; b = x; }
  return ((uint16_t)(r * 31) << 11) | ((uint16_t)(g * 63) << 5) | (uint16_t)(b * 31);
}

void resetVisualizer() {
  vizInit = false;
}

void drawVisualizer() {
  uint16_t bg = dispSettings.bgColor;

  const int barPitch = LY_W / NB;
  const int drawW    = barPitch - VIZ_GAP;
  const int xOffset  = (LY_W - barPitch * NB) / 2;  // center the group

  if (!vizInit) {
    tft.fillScreen(bg);
    for (int i = 0; i < NB; i++) {
      barH[i] = peakH[i] = 0;
      drawnBar[i] = drawnPeak[i] = 0;
      // Rainbow from red (low freq) through to violet (high freq)
      bandColor[i] = hueColor((float)i / NB * 280.0f);
    }
    vizInit = true;
  }

  for (int i = 0; i < NB; i++) {
    // Target height for this band
    float target = (float)rxBands[i] / 255.0f * VIZ_MAX_H;

    // Attack fast, decay slow for a snappy-but-smooth look
    if (target > barH[i]) barH[i] += (target - barH[i]) * 0.6f;
    else                  barH[i] += (target - barH[i]) * 0.25f;

    // Peak follows the bar up instantly, then falls slowly
    if (barH[i] > peakH[i]) peakH[i] = barH[i];
    else                    peakH[i] -= VIZ_MAX_H * 0.012f;
    if (peakH[i] < 0) peakH[i] = 0;

    int newBar  = (int)(barH[i]  + 0.5f);
    int newPeak = (int)(peakH[i] + 0.5f);
    if (newBar  > VIZ_MAX_H) newBar  = VIZ_MAX_H;
    if (newPeak > VIZ_MAX_H) newPeak = VIZ_MAX_H;

    int x = xOffset + i * barPitch;

    // --- Bar: delta-fill only the part that changed ---
    if (newBar > drawnBar[i]) {
      tft.fillRect(x, LY_H - newBar, drawW, newBar - drawnBar[i], bandColor[i]);
    } else if (newBar < drawnBar[i]) {
      tft.fillRect(x, LY_H - drawnBar[i], drawW, drawnBar[i] - newBar, bg);
    }

    // --- Peak marker (2px) ---
    if (drawnPeak[i] != newPeak) {
      // erase old marker: restore bar color if it now sits inside the bar, else bg
      int oldY = LY_H - drawnPeak[i] - 1;
      if (oldY < 0) oldY = 0;
      tft.fillRect(x, oldY, drawW, 2, (drawnPeak[i] <= newBar) ? bandColor[i] : bg);
    }
    if (newPeak > newBar) {
      int py = LY_H - newPeak - 1;
      if (py < 0) py = 0;
      tft.fillRect(x, py, drawW, 2, TFT_WHITE);
    }

    drawnBar[i]  = newBar;
    drawnPeak[i] = newPeak;
  }
}
