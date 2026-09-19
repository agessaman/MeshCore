#include "UITask.h"
#include "target.h"
#include <Arduino.h>
#include <helpers/CommonCLI.h>

#ifdef DISPLAY_REDRAW_ON_CHANGE
#include <helpers/ui/DisplayFrameSignature.h>
#endif

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

#ifdef WITH_MQTT_BRIDGE
#include <WiFi.h>
#include <helpers/esp32/WebConfigServer.h>   // defines WITH_WEBCONFIG on ESP32
#endif

#ifndef AUTO_OFF_MILLIS
#define AUTO_OFF_MILLIS      20000  // 20 seconds; 0 keeps the screen on
#endif

#ifdef DISPLAY_TOUCH_TOGGLE
#define TOUCH_POLL_MILLIS    50
#endif

// Wrap-safe deadline test. `millis() >= deadline` fires early for the whole
// interval before a rollover, because the deadline has already wrapped to a
// small value while millis() is still near UINT32_MAX; the signed difference
// stays correct across it.
static inline bool millisReached(unsigned long now, unsigned long deadline) {
  return (int32_t)((uint32_t)now - (uint32_t)deadline) >= 0;
}

// Applies `display.flip` when it changes, forcing a complete repaint because
// the panel's existing contents are now the wrong way up.
void UITask::applyDisplayFlip() {
#ifdef WITH_MQTT_BRIDGE
  if (_observer_prefs == NULL || _observer_prefs->display_flip == _flip_seen) return;
  _flip_seen = _observer_prefs->display_flip;
  _display->setFlipped(_flip_seen != 0);
  // Logged unconditionally: this is persisted config, so it survives a reflash
  // and is otherwise invisible when someone is chasing a wrong orientation.
  Serial.printf("Display: flip %s\n", _flip_seen ? "on (rotated 180)" : "off");
#ifdef DISPLAY_REDRAW_ON_CHANGE
  _frame_valid = false;
#endif
#ifdef DISPLAY_ACTIVITY_DASHBOARD
  _rows_valid = false;
#endif
  _next_refresh = 0;
#endif
}

// `display.timeout` when the observer prefs are available, otherwise the
// compiled-in default. Read on every use so a `set display.timeout` takes
// effect immediately.
unsigned long UITask::displayTimeoutMillis() const {
#ifdef WITH_MQTT_BRIDGE
  if (_observer_prefs) return (unsigned long)_observer_prefs->display_timeout_secs * 1000UL;
#endif
  return AUTO_OFF_MILLIS;
}
#define BOOT_SCREEN_MILLIS   4000   // 4 seconds

#define POWEROFF_DELAY 3000

// 'meshcore', 128x13px
static const uint8_t meshcore_logo [] PROGMEM = {
    0x3c, 0x01, 0xe3, 0xff, 0xc7, 0xff, 0x8f, 0x03, 0x87, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 0x1f, 0xfe, 
    0x3c, 0x03, 0xe3, 0xff, 0xc7, 0xff, 0x8e, 0x03, 0x8f, 0xfe, 0x3f, 0xfe, 0x1f, 0xff, 0x1f, 0xfe, 
    0x3e, 0x03, 0xc3, 0xff, 0x8f, 0xff, 0x0e, 0x07, 0x8f, 0xfe, 0x7f, 0xfe, 0x1f, 0xff, 0x1f, 0xfc, 
    0x3e, 0x07, 0xc7, 0x80, 0x0e, 0x00, 0x0e, 0x07, 0x9e, 0x00, 0x78, 0x0e, 0x3c, 0x0f, 0x1c, 0x00, 
    0x3e, 0x0f, 0xc7, 0x80, 0x1e, 0x00, 0x0e, 0x07, 0x1e, 0x00, 0x70, 0x0e, 0x38, 0x0f, 0x3c, 0x00, 
    0x7f, 0x0f, 0xc7, 0xfe, 0x1f, 0xfc, 0x1f, 0xff, 0x1c, 0x00, 0x70, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x1f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x0e, 0x38, 0x0e, 0x3f, 0xf8, 
    0x7f, 0x3f, 0xc7, 0xfe, 0x0f, 0xff, 0x1f, 0xff, 0x1c, 0x00, 0xf0, 0x1e, 0x3f, 0xfe, 0x3f, 0xf0, 
    0x77, 0x3b, 0x87, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xfc, 0x38, 0x00, 
    0x77, 0xfb, 0x8f, 0x00, 0x00, 0x07, 0x1c, 0x0f, 0x3c, 0x00, 0xe0, 0x1c, 0x7f, 0xf8, 0x38, 0x00, 
    0x73, 0xf3, 0x8f, 0xff, 0x0f, 0xff, 0x1c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x78, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfe, 0x3c, 0x0e, 0x3f, 0xf8, 0xff, 0xfc, 0x70, 0x3c, 0x7f, 0xf8, 
    0xe3, 0xe3, 0x8f, 0xff, 0x1f, 0xfc, 0x3c, 0x0e, 0x1f, 0xf8, 0xff, 0xf8, 0x70, 0x3c, 0x7f, 0xf8, 
};

void UITask::begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version) {
  _prevBtnState = HIGH;
  _timeout_seen = displayTimeoutMillis();
  _auto_off = millis() + displayTimeoutMillis();
  _started_at = millis();
  _node_prefs = node_prefs;
#ifdef DISPLAY_ACTIVITY_DASHBOARD
  ObserverDashboard::applyDarkPalette();   // retunes UIColor for this target only
#endif
  _display->turnOn();
  applyDisplayFlip();
#ifdef DISPLAY_TOUCH_TOGGLE
  _touch.begin();
#endif
#ifdef DISPLAY_REDRAW_ON_CHANGE
  _frame_valid = false;
#endif

#if defined(PIN_USER_BTN) && defined(DISPLAY_CLASS)
  user_btn.begin();
#endif

  // strip off dash and commit hash by changing dash to null terminator
  // e.g: v1.2.3-abcdef -> v1.2.3
  char *version = strdup(firmware_version);
  char *dash = strchr(version, '-');
  if(dash){
    *dash = 0;
  }

  // v1.2.3 (1 Jan 2025)
  snprintf(_version_info, sizeof(_version_info), "%s (%s)", version, build_date);
  free(version);
}

void UITask::renderCurrScreen() {
  char tmp[80];
#ifdef DISPLAY_ACTIVITY_DASHBOARD
  _rows_valid = false;
#endif
  if (millis() < _started_at + BOOT_SCREEN_MILLIS) { // boot screen
    // meshcore logo
    _display->setColor(UIColor::corp_blue);
    int logoWidth = 128;
    _display->drawXbm((_display->width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    _display->setColor(UIColor::primary_txt);
    _display->setTextSize(1);
    _display->drawTextCentered(_display->width() / 2, 22, website);

    // version info
    _display->setTextSize(1);
    _display->drawTextCentered(_display->width() / 2, 35, _version_info);

    // node type
    const char* node_type = "< Repeater >";
    _display->drawTextCentered(_display->width() / 2, 48, node_type);
  } else if (_powering_off_at > 0) {
    // meshcore logo
    _display->setColor(UIColor::corp_blue);
    int logoWidth = 128;
    _display->drawXbm((_display->width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    _display->setColor(UIColor::primary_txt);
    _display->setTextSize(1);
    _display->drawTextCentered(_display->width()/ 2, 22, website);

    // Powering off
    const char* poweroff_string = "Turning OFF";
    uint16_t poffWidth = _display->getTextWidth(poweroff_string);
    _display->setCursor((_display->width() - poffWidth) / 2, 48);
    _display->drawTextCentered(_display->width()/2, 48, poweroff_string);
  } else {  // home screen
#ifdef WITH_WEBCONFIG
    if (WebConfigServer::isRebootPending()) {
      // save confirmed on-device: show ground truth even if the browser
      // lost its connection before the confirmation reached it
      _display->setTextSize(1);
      _display->setColor(UIColor::corp_blue);
      _display->setCursor(0, 14);
      _display->print("Config saved!");
      _display->setColor(UIColor::primary_txt);
      _display->setCursor(0, 30);
      _display->print("Rebooting...");
      return;
    }
    char wc_ssid[33], wc_ip[16];
    if (WebConfigServer::getSetupInfo(wc_ssid, sizeof(wc_ssid), wc_ip, sizeof(wc_ip))) {
      const bool lan_setup = WebConfigServer::isLanSetup();
      // setup portal active: show join instructions instead of the home screen
      _display->setTextSize(1);
      _display->setColor(UIColor::corp_blue);
      _display->setCursor(0, 0);
      _display->print(lan_setup ? "Observer Ethernet Setup" : "Observer WiFi Setup");

      _display->setColor(UIColor::primary_txt);
      _display->setCursor(0, 14);
      _display->print(lan_setup ? "Login code:" : "Join WiFi:");
      _display->setColor(UIColor::warning_txt);
      _display->setCursor(6, 24);
      _display->print(wc_ssid);

      _display->setColor(UIColor::primary_txt);
      _display->setCursor(0, 40);
      _display->print("Then browse to:");
      _display->setColor(UIColor::warning_txt);
      _display->setCursor(6, 50);
      _display->print(wc_ip);
      return;
    }
#endif
#ifdef DISPLAY_ACTIVITY_DASHBOARD
    renderDashboard();
    return;
#endif
    // node name
    _display->setCursor(0, 0);
    _display->setTextSize(1);
    _display->setColor(UIColor::primary_txt);
    _display->print(_node_prefs->node_name);

    // freq / sf
    _display->setCursor(0, 20);
    sprintf(tmp, "FREQ: %06.3f SF%d", _node_prefs->freq, _node_prefs->sf);
    _display->print(tmp);

    // bw / cr
    _display->setCursor(0, 30);
    sprintf(tmp, "BW: %03.2f CR: %d", _node_prefs->bw, _node_prefs->cr);
    _display->print(tmp);

#ifdef WITH_MQTT_BRIDGE
    // Display IP address for MQTT bridge devices
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      _display->setCursor(0, 40);
      _display->setColor(UIColor::primary_txt);
      snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
      _display->print(tmp);
    }
#endif
  }
}

#ifdef DISPLAY_REDRAW_ON_CHANGE
uint32_t UITask::getFrameSignature() {
  uint32_t signature = DisplayFrameSignature::INITIAL;
  char tmp[80];

  if (millis() < _started_at + BOOT_SCREEN_MILLIS) {
    signature = DisplayFrameSignature::append(signature, "boot");
    return DisplayFrameSignature::append(signature, _version_info);
  }

  if (_powering_off_at > 0) {
    return DisplayFrameSignature::append(signature, "powering-off");
  }

#ifdef WITH_WEBCONFIG
  if (WebConfigServer::isRebootPending()) {
    return DisplayFrameSignature::append(signature, "rebooting");
  }

  char wc_ssid[33], wc_ip[16];
  if (WebConfigServer::getSetupInfo(wc_ssid, sizeof(wc_ssid), wc_ip, sizeof(wc_ip))) {
    signature = DisplayFrameSignature::append(signature, "setup");
    signature = DisplayFrameSignature::append(signature, wc_ssid);
    return DisplayFrameSignature::append(signature, wc_ip);
  }
#endif

  signature = DisplayFrameSignature::append(signature, "home");
  signature = DisplayFrameSignature::append(signature, _node_prefs->node_name);
  snprintf(tmp, sizeof(tmp), "FREQ: %06.3f SF%d", _node_prefs->freq, _node_prefs->sf);
  signature = DisplayFrameSignature::append(signature, tmp);
  snprintf(tmp, sizeof(tmp), "BW: %03.2f CR: %d", _node_prefs->bw, _node_prefs->cr);
  signature = DisplayFrameSignature::append(signature, tmp);

#if defined(WITH_MQTT_BRIDGE) && !defined(DISPLAY_ACTIVITY_DASHBOARD)
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    signature = DisplayFrameSignature::append(signature, tmp);
  } else {
    signature = DisplayFrameSignature::append(signature, "wifi-disconnected");
  }
#endif

  return signature;
}
#endif

#ifdef DISPLAY_ACTIVITY_DASHBOARD
#define ACTIVITY_REFRESH_MILLIS 5000

bool UITask::buildDashboardContext(ObserverDashboard::Context* ctx) {
  if (_node_prefs == NULL) return false;
  ctx->node_name = _node_prefs->node_name;
  ctx->role_label = "REPEATER";
  ctx->freq = _node_prefs->freq;
  ctx->sf = _node_prefs->sf;
  ctx->bw = _node_prefs->bw;
#ifdef WITH_MQTT_BRIDGE
  ctx->link_up = (WiFi.status() == WL_CONNECTED);
#else
  ctx->link_up = false;
#endif
  return true;
}

void UITask::renderDashboard() {
  ObserverDashboard::Context ctx;
  if (!buildDashboardContext(&ctx)) return;

  RadioActivitySnapshot snap;
  if (_activity) {
    _activity->snapshot(millis(), &snap);
  } else {
    memset(&snap, 0, sizeof(snap));
  }

  const ObserverDashboard::Layout& layout = ObserverDashboard::activeLayout();
  ObserverDashboard::drawFull(*_display, layout, ctx, snap);
  ObserverDashboard::allRowSignatures(layout, ctx, snap, _row_signatures);
  _rows_valid = true;
  _next_activity = millis() + ACTIVITY_REFRESH_MILLIS;
}

// Repaints just the analytics rows whose contents moved. No startFrame(), so
// the header, the radio strip and the rest of the panel are never cleared.
void UITask::updateActivityRows() {
  if (!_rows_valid || _activity == NULL) return;   // not showing the dashboard

  ObserverDashboard::Context ctx;
  if (!buildDashboardContext(&ctx)) return;

  RadioActivitySnapshot snap;
  _activity->snapshot(millis(), &snap);
  ObserverDashboard::drawChangedRows(*_display, ObserverDashboard::activeLayout(), ctx, snap,
                                     _row_signatures);
}
#endif

#ifdef DISPLAY_TOUCH_TOGGLE
void UITask::toggleDisplay(const char* source) {
  if (_display->isOn()) {
    _display->turnOff();
  } else {
    _display->turnOn();
  }
#ifdef DISPLAY_TOUCH_DEBUG
  Serial.printf("Display: %s -> %s\n", source, _display->isOn() ? "on" : "off");
#else
  (void)source;
#endif
#ifdef DISPLAY_REDRAW_ON_CHANGE
  _frame_valid = false;   // wake draws one complete current frame
#endif
#ifdef DISPLAY_ACTIVITY_DASHBOARD
  _rows_valid = false;
#endif
  _next_refresh = 0;   // redraw at once rather than showing the stale frame
  _auto_off = millis() + displayTimeoutMillis();
}
#endif

void UITask::loop() {
#if defined(PIN_USER_BTN) && defined(DISPLAY_CLASS)
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
#ifdef DISPLAY_TOUCH_TOGGLE
    toggleDisplay("button");   // same action as tapping the panel
#else
    if (_display->isOn()) {
      // TODO: any action ?
    } else {
      _display->turnOn();
#ifdef DISPLAY_REDRAW_ON_CHANGE
      _frame_valid = false;
#endif
#ifdef DISPLAY_ACTIVITY_DASHBOARD
      _rows_valid = false;
#endif
    }
    _auto_off = millis() + displayTimeoutMillis();   // extend auto-off timer
#endif
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      _display->turnOn();
      Serial.println("Powering Off");
      _powering_off_at = millis() + POWEROFF_DELAY; 
  }
#endif

#ifdef WITH_WEBCONFIG
  // While the setup portal is up there's no user button to wake the screen
  // reliably - keep it on so the join instructions stay visible.
  if (WebConfigServer::getSetupInfo(NULL, 0, NULL, 0)) {
    if (!_display->isOn()) {
      _display->turnOn();
#ifdef DISPLAY_REDRAW_ON_CHANGE
      _frame_valid = false;
#endif
#ifdef DISPLAY_ACTIVITY_DASHBOARD
      _rows_valid = false;
#endif
    }
    _auto_off = millis() + displayTimeoutMillis();
  }
#endif

#ifdef DISPLAY_TOUCH_TOGGLE
  {
    unsigned long now = millis();
    if (millisReached(now, _next_touch)) {
      _next_touch = now + TOUCH_POLL_MILLIS;
      if (_touch.checkTap(now)) toggleDisplay("touch");
    }
  }
#endif

  if (_display->isOn()) {
    if (millis() >= _next_refresh) {
      bool redraw = true;
#ifdef DISPLAY_REDRAW_ON_CHANGE
      uint32_t frame_signature = getFrameSignature();
      redraw = !_frame_valid || frame_signature != _last_frame_signature;
#endif
      if (redraw) {
        _display->startFrame();
        renderCurrScreen();
        _display->endFrame();
#ifdef DISPLAY_REDRAW_ON_CHANGE
        _last_frame_signature = frame_signature;
        _frame_valid = true;
#endif
      }
#ifdef DISPLAY_ACTIVITY_DASHBOARD
      else if (millisReached(millis(), _next_activity)) {
        updateActivityRows();
        _next_activity = millis() + ACTIVITY_REFRESH_MILLIS;
      }
#endif

      _next_refresh = millis() + 1000;   // check for visible changes every second
    }
    // `_auto_off` is only armed on activity, so a timeout changed at runtime has
    // to restart the countdown here - otherwise 0 -> 60 blanks instantly off a
    // boot-time deadline, and 60 -> 3600 still blanks at the old 60 s mark.
    applyDisplayFlip();
    unsigned long timeout = displayTimeoutMillis();
    if (timeout != _timeout_seen) {
      _timeout_seen = timeout;
      _auto_off = millis() + timeout;
    }
    if (timeout > 0 && millisReached(millis(), _auto_off)) {
      _display->turnOff();
#ifdef DISPLAY_REDRAW_ON_CHANGE
      _frame_valid = false;
#endif
#ifdef DISPLAY_ACTIVITY_DASHBOARD
      _rows_valid = false;
#endif
    }
  }

  if (_powering_off_at > 0) { // power off timer armed
#ifdef LED_PIN
    digitalWrite(LED_PIN, LED_STATE_ON); // switch on the led until poweroff
#endif
    if (millis() > _powering_off_at) {
      _board->powerOff();  // should not return
    }
  }
}
