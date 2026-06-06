#include "local_mqtt.h"
#include "settings.h"
#include "display_ui.h"
#include <PubSubClient.h>
#include <WiFi.h>

#define TOPIC_STATE   "bambuhelper/screen/state"
#define TOPIC_SET     "bambuhelper/screen/set"
#define RECONNECT_MS  10000UL

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);
static unsigned long lastReconnectAttempt = 0;
static char         lastPublishedState[8] = "";

static void onMessage(const char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_SET) != 0 || length == 0 || length > 6) return;
  char msg[8];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if      (strcmp(msg, "on")    == 0) { setBacklight(getEffectiveBrightness()); setScreenState(SCREEN_IDLE); }
  else if (strcmp(msg, "off")   == 0) { setBacklight(0); setScreenState(SCREEN_OFF); }
  else if (strcmp(msg, "night") == 0) { setScreenState(SCREEN_NIGHT); }
}

static bool tryConnect() {
  if (!mqttClient.connect("BambuHelper")) return false;
  mqttClient.subscribe(TOPIC_SET);
  // re-publish current state after reconnect
  if (lastPublishedState[0]) mqttClient.publish(TOPIC_STATE, lastPublishedState, true);
  return true;
}

void initLocalMqtt() {
  if (netSettings.localMqttHost[0] == '\0') return;
  mqttClient.setServer(netSettings.localMqttHost, netSettings.localMqttPort);
  mqttClient.setCallback(onMessage);
}

void loopLocalMqtt() {
  if (netSettings.localMqttHost[0] == '\0') return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt < RECONNECT_MS) return;
    lastReconnectAttempt = now;
    tryConnect();
    return;
  }
  mqttClient.loop();
}

void localMqttPublishScreen(const char* state) {
  strlcpy(lastPublishedState, state, sizeof(lastPublishedState));
  if (!mqttClient.connected()) return;
  mqttClient.publish(TOPIC_STATE, state, true);  // retained
}
