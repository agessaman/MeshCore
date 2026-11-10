#ifdef ESP_PLATFORM

#include "ESP32Board.h"
#include "NetworkLink.h"
#include <target.h>

// The RTC backup of ESP32RTCClock, declared in ESP32Board.h.
RTC_NOINIT_ATTR uint32_t _rtc_backup_time;
RTC_NOINIT_ATTR uint32_t _rtc_backup_magic;

#if defined(ADMIN_PASSWORD) && !defined(DISABLE_WIFI_OTA)   // Repeater or Room Server only
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <Update.h>

#include <SPIFFS.h>
#include <new>

#include "esp32/HttpPort80Lease.h"

namespace {

AsyncWebServer* ota_server = nullptr;
bool ota_routes_registered = false;
bool ota_server_running = false;
bool ota_raised_ap = false;
bool ota_started_radio = false;  // the OTA AP turned Wi-Fi on
bool ota_network_locked = false;
uint32_t ota_started_at = 0;
char ota_id_buf[60];
char ota_home_buf[90];
char ota_url_buf[80];

void otaRegisterRoutes() {
  ota_server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", ota_home_buf);
  });
  ota_server->on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, "/packet_log", "text/plain");
  });
  AsyncElegantOTA.begin(ota_server);
  ota_routes_registered = true;
}

void otaReleaseTransport() {
  if (ota_server) ota_server->end();
  if (ota_raised_ap) WiFi.softAPdisconnect(true);
  // softAPdisconnect leaves the STA prepareWifiForAp() started; restore off.
  if (ota_started_radio) WiFi.mode(WIFI_OFF);
  ota_raised_ap = false;
  ota_started_radio = false;
  ota_server_running = false;
  ota_started_at = 0;
  if (ota_network_locked) {
    activeNetworkLink().unlockSwitching();
    ota_network_locked = false;
  }
  HttpPort80Lease::release(HttpPort80Lease::Owner::Ota);
}

}  // namespace

bool ESP32Board::startOTAUpdate(const char* id, char reply[], bool force_ap) {
  if (ota_server_running) {
    ota_started_at = millis();
    snprintf(reply, 160, "Started: %s", ota_url_buf);
    return true;
  }
  if (!HttpPort80Lease::acquire(HttpPort80Lease::Owner::Ota)) {
    snprintf(reply, 160, "Error: port 80 is in use by %s",
             HttpPort80Lease::ownerName());
    return false;
  }

  inhibit_sleep = true;   // prevent sleep during OTA
  activeNetworkLink().lockSwitching();
  ota_network_locked = true;

  // If the device is already on its selected network, serve ElegantOTA on that
  // address so it is reachable without joining a separate AP. Otherwise raise
  // the MeshCore-OTA SoftAP.
  // force_ap ("start ota ap") always raises the SoftAP, so the OTA UI stays
  // reachable even when the joined network applies client isolation and the
  // station IP can't be reached.
  IPAddress ip;
  if (NetworkPolicy::startOtaUsesSelectedNetwork(
          force_ap, activeNetworkLink().isConnected())) {
    ip = activeNetworkLink().localIP();
  } else {
    // Cap first: OTA pairs sustained Wi-Fi TX with flash writes.
    ota_started_radio = activeNetworkLink().prepareWifiForAp();
    ota_raised_ap = WiFi.softAP("MeshCore-OTA", NULL);
    if (ota_raised_ap) activeNetworkLink().reapplyWifiTxPower();
    if (!ota_raised_ap) {
      otaReleaseTransport();
      inhibit_sleep = false;
      strcpy(reply, "Error: failed to start OTA AP");
      return false;
    }
    ip = WiFi.softAPIP();
  }

  snprintf(ota_id_buf, sizeof(ota_id_buf), "%s (%s)", id,
           getManufacturerName());
  snprintf(ota_home_buf, sizeof(ota_home_buf),
           "<H2>Hi! I am a MeshCore Repeater. ID: %s</H2>", id);
  snprintf(ota_url_buf, sizeof(ota_url_buf), "http://%s/update",
           ip.toString().c_str());

  if (!ota_server) {
    ota_server = new (std::nothrow) AsyncWebServer(80);
    if (!ota_server) {
      otaReleaseTransport();
      inhibit_sleep = false;
      strcpy(reply, "Error: insufficient memory for OTA server");
      return false;
    }
  }
  AsyncElegantOTA.setID(ota_id_buf);
  if (!ota_routes_registered) otaRegisterRoutes();
  ota_server->begin();
  if (ota_server->state() != LISTEN) {
    otaReleaseTransport();
    inhibit_sleep = false;
    strcpy(reply, "Error: failed to bind OTA server to port 80");
    return false;
  }

  ota_server_running = true;
  ota_started_at = millis();
  snprintf(reply, 160, "Started: %s", ota_url_buf);
  MESH_DEBUG_PRINTLN("startOTAUpdate: %s", reply);

  return true;
}

bool ESP32Board::stopOTAUpdate(char reply[]) {
  if (!ota_server_running) return false;
  if (Update.isRunning()) {
    strcpy(reply, "Error: firmware upload is in progress");
    return false;
  }

  otaReleaseTransport();
  inhibit_sleep = false;
  strcpy(reply, "OK - OTA web server stopped");
  return true;
}

bool ESP32Board::isOTAUpdateInProgress() const {
  return ota_server_running && Update.isRunning();
}

void ESP32Board::maintainOTAUpdate(uint32_t now_ms) {
  if (!ota_server_running || Update.isRunning()) return;
  if (!NetworkPolicy::manualOtaTimeoutDue(now_ms, ota_started_at, false)) return;
  char reply[160];
  if (stopOTAUpdate(reply)) MESH_DEBUG_PRINTLN("OTA: %s", reply);
}

#else
bool ESP32Board::startOTAUpdate(const char* id, char reply[], bool force_ap) {
  (void)id; (void)reply; (void)force_ap;
  return false; // not supported
}
bool ESP32Board::stopOTAUpdate(char reply[]) {
  (void)reply;
  return false;
}
bool ESP32Board::isOTAUpdateInProgress() const {
  return false;
}
void ESP32Board::maintainOTAUpdate(uint32_t now_ms) {
  (void)now_ms;
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

  // Keep LoRa inactive during deepsleep
#ifdef P_LORA_NSS
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
