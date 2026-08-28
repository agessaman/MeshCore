#include <Arduino.h>
#include "target.h"

HeltecV4R8Board board;

#if defined(P_LORA_SCLK)
  static SPIClass spi;
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock, GPS_RESET, GPS_EN, &board.periph_power);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display(&board.periph_power);
  #ifndef USER_BTN_LONG_PRESS_MILLIS
    #define USER_BTN_LONG_PRESS_MILLIS 1000
  #endif
  // Multi-click detection holds a CLICK back for MULTI_CLICK_WINDOW_MS (280 ms)
  // after release, and folds a second press inside that window into a
  // DOUBLE_CLICK. Targets that only want a plain click set this to 0 so the
  // event fires on release instead of being delayed - or swallowed when an
  // impatient second press arrives.
  #ifndef USER_BTN_MULTICLICK
    #define USER_BTN_MULTICLICK 1
  #endif
  MomentaryButton user_btn(PIN_USER_BTN, USER_BTN_LONG_PRESS_MILLIS, true, false,
                           USER_BTN_MULTICLICK);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
