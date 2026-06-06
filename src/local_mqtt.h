#ifndef LOCAL_MQTT_H
#define LOCAL_MQTT_H

void initLocalMqtt();
void loopLocalMqtt();
void localMqttPublishScreen(const char* state);  // "on", "off", "night"

#endif // LOCAL_MQTT_H
