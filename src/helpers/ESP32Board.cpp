#ifdef ESP_PLATFORM

#include "ESP32Board.h"
#include <target.h>

#if defined(ADMIN_PASSWORD) && !defined(DISABLE_WIFI_OTA)   // Repeater or Room Server only
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

#include <SPIFFS.h>

bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  inhibit_sleep = true;   // prevent sleep during OTA

  if (ota_server != nullptr) {
    sprintf(reply, "Started: http://%s/update", WiFi.softAPIP().toString().c_str());
    return true;
  }

  WiFi.softAP("MeshCore-OTA", NULL);

  sprintf(reply, "Started: http://%s/update", WiFi.softAPIP().toString().c_str());
  MESH_DEBUG_PRINTLN("startOTAUpdate: %s", reply);

  static char id_buf[60];
  sprintf(id_buf, "%s (%s)", id, getManufacturerName());
  static char home_buf[90];
  sprintf(home_buf, "<H2>Hi! I am a MeshCore Repeater. ID: %s</H2>", id);

  ota_server = new AsyncWebServer(80);

  ota_server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", home_buf);
  });
  ota_server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/packet_log", "text/plain");
  });

  AsyncElegantOTA.setID(id_buf);
  AsyncElegantOTA.begin(ota_server);    // Start ElegantOTA
  ota_server->begin();

  return true;
}

bool ESP32Board::stopOTAUpdate(char reply[]) {
  if (ota_server == nullptr) {
    strcpy(reply, "OK - OTA not running");
    return true;
  }

  ota_server->end();
  delete ota_server;
  ota_server = nullptr;
  WiFi.softAPdisconnect(true);
  inhibit_sleep = false;

  strcpy(reply, "OK - OTA stopped");
  MESH_DEBUG_PRINTLN("stopOTAUpdate: %s", reply);

  return true;
}

#else
bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  return false; // not supported
}

bool ESP32Board::stopOTAUpdate(char reply[]) {
  return false; // not supported
}
#endif

void ESP32Board::powerOff() {
  enterDeepSleep(0); // Do not wakeup
}

void ESP32Board::enterDeepSleep(uint32_t secs) {
  // Power off the display if any
#ifdef DISPLAY_CLASS
  display.turnOff();
#endif

  // Power off LoRa
  radio_driver.powerOff();

  #ifdef P_LORA_NSS
  // Keep LoRa inactive during deepsleep
  digitalWrite(P_LORA_NSS, HIGH);
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
  gpio_hold_en((gpio_num_t)P_LORA_NSS);
#else
  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);
#endif
  #endif

  // Power off GPS if any
  if (sensors.getLocationProvider() != NULL) {
    sensors.getLocationProvider()->stop();
  }

  // Flush serial buffers
  Serial.flush();
  delay(100);

  // Clear stale wakeup sources to avoid ghost wakeup
  // This is required when Power Management and automatic lightsleep are enabled
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }

  // Finally set ESP32 into deepsleep
  esp_deep_sleep_start(); // CPU halts here and never returns!
}
#endif
