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
  void begin() override {
    STM32Board::begin();
#if defined(P_LORA_TX_LED)
    pinMode(P_LORA_TX_LED, OUTPUT);
    digitalWrite(P_LORA_TX_LED, HIGH);
#endif
  }

  const char* getManufacturerName() const override {
    return "STM32F103 BluePill KISS";
  }
};

extern BluePillBoard board;
extern WRAPPER_CLASS radio_driver;
extern VolatileRTCClock rtc_clock;
extern SensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
