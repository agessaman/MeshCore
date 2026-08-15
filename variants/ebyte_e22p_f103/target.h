#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/stm32/STM32Board.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>

// EBYTE E22P-915MBH-SC evaluation kit: STM32F103C8T6 ("Blue Pill"-class MCU,
// Cortex-M3, 64KB flash, 20KB SRAM) + E22P-915M30S (SX1262, 30dBm PA).
// External SX1262 LoRa radio on the default SPI1 bus (PA5/PA6/PA7).
// No on-chip filesystem / identity is used (KISS_NO_CRYPTO build).
class EByteE22PF103Board : public STM32Board {
public:
  void begin() override {
    // Free PA15 (JTDI) for the TX LED: switch the debug port to SW-DP only (drops the
    // 5-wire JTAG, keeps 2-wire SWD, so flashing/debug via ST-Link is unaffected). Must
    // run before anything configures PA15.
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG();

    STM32Board::begin();

    // E22P EN line forced high (RadioLib does not drive it as an RF switch pin).
    pinMode(PB13, OUTPUT);
    digitalWrite(PB13, HIGH);

#if defined(P_LORA_TX_LED)
    // TX-activity LED (active-low). STM32Board::onBeforeTransmit/onAfterTransmit toggle
    // it; configure the pin here since the base class never does.
    pinMode(P_LORA_TX_LED, OUTPUT);
    digitalWrite(P_LORA_TX_LED, HIGH);  // off
#endif
  }

  const char* getManufacturerName() const override {
    return "EBYTE E22P F103 KISS";
  }
};

extern EByteE22PF103Board board;
extern WRAPPER_CLASS radio_driver;
extern VolatileRTCClock rtc_clock;
extern SensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
