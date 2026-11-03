#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTT.h>
#include <WebSocketsClient.h>
#include <FreeRTOS.h>
#include <task.h>

static void probe(void*) {
  WiFiClient tcp;
  BearSSL::WiFiClientSecure tls;
  MQTTClient mqtt(1024, 1024);
  WebSocketsClient websocket;
  mqtt.begin(tcp);
  tls.setSSLVersion(BR_TLS12, BR_TLS12);
  websocket.setReconnectInterval(0);
  Serial.println("Pico MQTT transport compiled; no credentials or connections configured");
  vTaskDelete(nullptr);
}

void setup() {
  Serial.begin(115200);
  xTaskCreateAffinitySet(probe, "mqttprobe", 12288 / sizeof(StackType_t),
                        nullptr, 1, 2, nullptr);
}

void loop() { delay(1000); }
