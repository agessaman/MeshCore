#include <Arduino.h>
#include "target.h"

EByteE22PF103Board board;

// External SX1262 on the default SPI bus. NSS / DIO1 / RESET / BUSY are wired
// to GPIOs; SCLK/MISO/MOSI use the F103's default SPI1 pins (PA5/PA6/PA7).
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock rtc_clock;
SensorManager sensors;

bool radio_init() {
  return radio.std_init();
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
