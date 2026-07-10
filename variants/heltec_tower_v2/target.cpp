#include "target.h"

#include <Arduino.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>

HeltecTowerV2Board board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);

#ifdef DISPLAY_CLASS
DISPLAY_CLASS display;
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  rtc_clock.begin(Wire);
  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
