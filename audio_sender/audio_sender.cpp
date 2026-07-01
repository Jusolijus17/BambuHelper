/*
 * BambuHelper — Audio visualizer SENDER
 * ----------------------------------------------------------------------------
 * Board   : ESP32 classic (DevKit)
 * Input   : PC/amp -> jack 3.5mm -> (capacitor + voltage divider) -> GPIO34 (ADC1)
 * Process : ADC sampling -> arduinoFFT -> 16 log-spaced bands -> normalize 0..255
 * Output  : ESP-NOW broadcast to the BambuHelper display (this repo's main fw)
 *
 * Two networking modes (chosen automatically at boot)
 * ----------------------------------------------------
 *  - WiFi mode  : if you set SENDER_WIFI_SSID/PASS, the sender joins your
 *                 router. That (a) enables ArduinoOTA wireless updates and
 *                 (b) puts it on the SAME channel as the display automatically,
 *                 so channel scanning is skipped.
 *  - ESP-NOW-only (no router): if no credentials, the display is a WiFi STA so
 *                 its radio is locked to the router's channel; this sender hops
 *                 across the scan channels listening for the display's BEACON,
 *                 locks onto that channel, and streams DATA. No OTA in this mode.
 *
 * Build      : pio run -e audio_sender -t upload          (USB)
 * OTA update : pio run -e audio_sender -t upload \
 *                  --upload-port <sender-ip> --upload-protocol espota
 * Lib        : kosme/arduinoFFT @ ^2.0 (declared in platformio.ini)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <arduinoFFT.h>
#include "audio_viz_protocol.h"

// ----- Optional WiFi + OTA --------------------------------------------------
// Set your WiFi credentials (here or via -D build flags in platformio.ini) to
// enable ArduinoOTA. Leave SSID empty for pure ESP-NOW (no router, no OTA).
#ifndef SENDER_WIFI_SSID
#define SENDER_WIFI_SSID ""
#endif
#ifndef SENDER_WIFI_PASS
#define SENDER_WIFI_PASS ""
#endif
#define SENDER_OTA_HOSTNAME "bambu-audio-sender"
#define SENDER_OTA_PASSWORD ""   // optional: require a password for OTA pushes

// ----- Audio / FFT config ---------------------------------------------------
#define ADC_PIN        34       // audio input = midpoint of the voltage divider
#define SAMPLES        512      // must be a power of 2
#define SAMPLING_FREQ  16000    // Hz (Nyquist = 8 kHz, plenty for music)
#define NUM_BANDS      AVIZ_NUM_BANDS  // 16 bars (shared with the display)

// Silence gate: below this AC RMS (in ADC counts) we treat the input as silent,
// stop streaming (the display reverts to its clock) and FREEZE the AGC so it
// never recalibrates onto ADC noise. Raise it if idle noise still triggers the
// visualizer; lower it if quiet music fails to wake it. Watch the serial log:
// it prints the live RMS so you can pick a value just above your idle reading.
#define SILENCE_RMS    18.0

// ----- Power / thermal management -------------------------------------------
#define PROBE_SAMPLES   128     // cheap silence probe (~3 ms)
#define SILENT_POLL_MS  150     // nap between probes while silent
#define CPU_IDLE_MHZ    80      // clock while silent (80 MHz = WiFi minimum)
#define CPU_ACTIVE_MHZ  160     // clock while music plays (use 240 for max performance, but hotter)

double vReal[SAMPLES];
double vImag[SAMPLES];

ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);

unsigned int samplingPeriodUs;
int    bandsRaw[NUM_BANDS];
double agc = 1.0;   // automatic gain control reference (running peak)

// ----- ESP-NOW discovery state ----------------------------------------------
static const uint8_t BCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t SCAN_CHANNELS[] = AVIZ_SCAN_CHANNELS;
static const uint8_t SCAN_COUNT = sizeof(SCAN_CHANNELS) / sizeof(SCAN_CHANNELS[0]);

static volatile bool          locked = false;
static volatile uint8_t       lockedChannel = 0;
static volatile unsigned long lastBeaconMs = 0;

static uint8_t  scanIdx = 0;
static uint8_t  curChannel = 0;
static unsigned long lastHopMs = 0;

static bool wifiMode      = false;  // connected to a router (OTA + auto channel)
static bool otaInProgress = false;  // pause sampling while an OTA upload runs

#define HOP_DWELL_MS       400    // listen this long per channel while scanning
#define BEACON_TIMEOUT_MS  5000   // no beacon for this long -> resume scanning

static void setChannel(uint8_t ch) {
  curChannel = ch;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

// ESP-NOW receive callback (handles arduino-esp32 v2 and v3 signatures)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len < (int)sizeof(AVizBeacon)) return;
  const AVizBeacon *b = (const AVizBeacon *)data;
  if (b->magic != AVIZ_MAGIC || b->type != AVIZ_TYPE_BEACON) return;

  // Heard the display: lock onto the channel we're currently listening on.
  lastBeaconMs = millis();
  if (!locked || lockedChannel != curChannel) {
    locked = true;
    lockedChannel = curChannel;
    Serial.printf("[ESP-NOW] Locked onto display, channel %u\n", curChannel);
  }
}

static void initOTA() {
  ArduinoOTA.setHostname(SENDER_OTA_HOSTNAME);
  if (strlen(SENDER_OTA_PASSWORD) > 0) ArduinoOTA.setPassword(SENDER_OTA_PASSWORD);
  ArduinoOTA.onStart([]() { otaInProgress = true; Serial.println("[OTA] start"); });
  ArduinoOTA.onEnd([]()   { Serial.println("[OTA] done"); });
  ArduinoOTA.onError([](ota_error_t e) {
    otaInProgress = false;
    Serial.printf("[OTA] error %u\n", e);
  });
  ArduinoOTA.begin();
  Serial.printf("[OTA] ready as '%s' (IP %s)\n",
                SENDER_OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

// Bring up the radio: try WiFi (for OTA + auto channel) if credentials are set,
// otherwise fall back to channel-hopping ESP-NOW. Then init ESP-NOW (both modes).
static void initRadio() {
  WiFi.mode(WIFI_STA);

  if (strlen(SENDER_WIFI_SSID) > 0) {
    Serial.printf("[WiFi] connecting to '%s'", SENDER_WIFI_SSID);
    WiFi.begin(SENDER_WIFI_SSID, SENDER_WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      wifiMode      = true;
      locked        = true;             // same router as display => same channel
      lockedChannel = WiFi.channel();
      curChannel    = lockedChannel;
      Serial.printf("[WiFi] connected, IP %s, channel %u\n",
                    WiFi.localIP().toString().c_str(), lockedChannel);
      initOTA();
    } else {
      Serial.println("[WiFi] failed -> ESP-NOW scan mode (no OTA)");
      WiFi.disconnect();
    }
  } else {
    WiFi.disconnect();                  // pure ESP-NOW, no AP
  }

  esp_wifi_start();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] init failed, restarting...");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onEspNowRecv);

  // Broadcast peer (channel 0 = use current radio channel)
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.channel = 0;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(BCAST_ADDR)) {
    esp_now_add_peer(&peer);
  }

  if (!wifiMode) {
    setChannel(SCAN_CHANNELS[0]);
    lastHopMs = millis();
    Serial.println("[ESP-NOW] Scanning for display beacon...");
  }
}

// Hop to the next scan channel (only while not locked / beacon lost).
// In WiFi mode we are permanently locked to the router channel, so do nothing.
static void serviceDiscovery() {
  if (wifiMode) return;

  unsigned long now = millis();

  if (locked && (now - lastBeaconMs > BEACON_TIMEOUT_MS)) {
    locked = false;
    Serial.println("[ESP-NOW] Beacon lost, resuming scan");
  }

  if (locked) {
    if (curChannel != lockedChannel) setChannel(lockedChannel);
    return;
  }

  if (now - lastHopMs >= HOP_DWELL_MS) {
    lastHopMs = now;
    scanIdx = (scanIdx + 1) % SCAN_COUNT;
    setChannel(SCAN_CHANNELS[scanIdx]);
  }
}

// Check for sound
static double probeLevel() {
  int s[PROBE_SAMPLES];
  long sum = 0;
  for (int i = 0; i < PROBE_SAMPLES; i++) { s[i] = analogRead(ADC_PIN); sum += s[i]; }
  double mean = (double)sum / PROBE_SAMPLES;
  double sumSq = 0;
  for (int i = 0; i < PROBE_SAMPLES; i++) { double d = s[i] - mean; sumSq += d * d; }
  return sqrt(sumSq / PROBE_SAMPLES);
}

// ----- Setup ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== BambuHelper Audio Sender ===");

  analogReadResolution(12);          // 12-bit ADC -> 0..4095
  analogSetAttenuation(ADC_11db);    // full 0..3.3 V input range
  samplingPeriodUs = round(1000000.0 / SAMPLING_FREQ);

  initRadio();
}

// ----- Loop -----------------------------------------------------------------
void loop() {
  if (wifiMode) ArduinoOTA.handle();
  if (otaInProgress) { delay(5); return; }   // don't sample during an OTA upload

  serviceDiscovery();

  // --- Cheap silence probe ---
  double rms = probeLevel();

  // Print the live RMS ~2x/s so the silence gate can be calibrated
  static unsigned long lastRmsPrint = 0;
  if (millis() - lastRmsPrint > 500) {
    lastRmsPrint = millis();
    Serial.printf("rms=%.1f agc=%.0f %uMHz %s\n", rms, agc,
                  getCpuFrequencyMhz(), rms < SILENCE_RMS ? "[SILENT]" : "");
  }

  if (rms < SILENCE_RMS) {
    if (getCpuFrequencyMhz() != CPU_IDLE_MHZ) setCpuFrequencyMhz(CPU_IDLE_MHZ);
    delay(SILENT_POLL_MS);
    return;
  }

  // Music present: run at full speed for a snappy visualizer.
  if (getCpuFrequencyMhz() != CPU_ACTIVE_MHZ) setCpuFrequencyMhz(CPU_ACTIVE_MHZ);

  // 1) Sample at a regular rate
  for (int i = 0; i < SAMPLES; i++) {
    unsigned long t = micros();
    vReal[i] = analogRead(ADC_PIN);
    vImag[i] = 0.0;
    while (micros() - t < samplingPeriodUs) { /* busy-wait to keep the rhythm */ }
  }

  // 2) Remove DC component (resting point ~2048)
  double mean = 0;
  for (int i = 0; i < SAMPLES; i++) mean += vReal[i];
  mean /= SAMPLES;
  for (int i = 0; i < SAMPLES; i++) vReal[i] -= mean;

  // 3) FFT
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // 4) Group bins into log-spaced bands (musical frequencies spread better)
  for (int b = 0; b < NUM_BANDS; b++) bandsRaw[b] = 0;
  const int firstBin = 2;  // skip bin 0 (DC) and bin 1
  for (int i = firstBin; i < SAMPLES / 2; i++) {
    int band = (int)(log10((double)i / firstBin)
                     / log10((double)(SAMPLES / 2) / firstBin) * NUM_BANDS);
    if (band < 0) band = 0;
    if (band >= NUM_BANDS) band = NUM_BANDS - 1;
    if ((int)vReal[i] > bandsRaw[band]) bandsRaw[band] = (int)vReal[i];  // band peak
  }

  // 5) Auto-gain normalize to 0..255
  int frameMax = 1;
  for (int b = 0; b < NUM_BANDS; b++) if (bandsRaw[b] > frameMax) frameMax = bandsRaw[b];
  // AGC: follow loud peaks instantly, decay slowly so quiet passages stay lively
  if (frameMax > agc) agc = frameMax;
  else                agc = agc * 0.995 + frameMax * 0.005;
  if (agc < 50) agc = 50;  // noise floor so silence stays near zero

  AVizData pkt;
  pkt.magic    = AVIZ_MAGIC;
  pkt.type     = AVIZ_TYPE_DATA;
  pkt.version  = AVIZ_VERSION;
  pkt.numBands = NUM_BANDS;
  for (int b = 0; b < NUM_BANDS; b++) {
    long v = (long)(bandsRaw[b] / agc * 255.0);
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    pkt.bands[b] = (uint8_t)v;
  }

  // 6) Send to the display (only meaningful once locked, but harmless otherwise)
  if (locked) {
    esp_now_send(BCAST_ADDR, (const uint8_t *)&pkt, sizeof(pkt));
  }
}
