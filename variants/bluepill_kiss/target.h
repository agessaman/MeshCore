#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/stm32/STM32Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>

// Minimal STM32F103C8 ("Blue Pill") board: Cortex-M3, 64KB flash, 20KB SRAM.
// External SX1262 LoRa radio on the default SPI1 bus (PA5/PA6/PA7).
// No on-chip filesystem / identity is used (KISS_NO_CRYPTO build).
class BluePillBoard : public STM32Board {
public:
  // Implemented in target.cpp: forces the E22P EN line (PB13) high, and if the previous
  // run ended in a HardFault (record left in the backup registers by the handler in
  // main.cpp) embeds "HF#n pc=... cfsr=... lr=..." into the device name so the host logs
  // it on the reconnect after the watchdog reboot.
  void begin() override;
  const char* getManufacturerName() const override;
};

extern BluePillBoard board;
extern WRAPPER_CLASS radio_driver;
extern VolatileRTCClock rtc_clock;
extern SensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
