#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/stm32/STM32Board.h>
#include <helpers/radiolib/CustomSTM32WLxWrapper.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>

#define PIN_ESP_EN     PA0   // ESP8684 CHIP_EN; held low (Wi-Fi/BLE co-processor off)
#define PIN_LED_GREEN  PA10
#define PIN_LED_BLUE   PA1

#ifndef ADC_MULTIPLIER
  #define ADC_MULTIPLIER (5 * 1.73 * 1000)
#endif

class RAK11160Board : public STM32Board {
public:
    void begin() override {
        STM32Board::begin();
        pinMode(PIN_ESP_EN, OUTPUT);
        digitalWrite(PIN_ESP_EN, LOW);
        pinMode(PIN_LED_GREEN, OUTPUT);
        pinMode(PIN_LED_BLUE, OUTPUT);
    }

    const char* getManufacturerName() const override {
        return "RAK 11160";
    }

    uint16_t getBattMilliVolts() override {
#ifdef PIN_VBAT_READ
        analogReadResolution(12);
        uint32_t raw = 0;
        for (int i=0; i<8;i++) {
            raw += analogRead(PIN_VBAT_READ);
        }
        return ((double)raw) * ADC_MULTIPLIER / 8 / 4096;
#else
        return 0;  // no battery divider on the module itself
#endif
    }

    void setGpio(uint32_t values) override {
        digitalWrite(PIN_LED_GREEN, values & 1);
        digitalWrite(PIN_LED_BLUE, (values & 2) >> 1);
    }

    uint32_t getGpio() override {
        return (digitalRead(PIN_LED_BLUE) << 1) | digitalRead(PIN_LED_GREEN);
    }
};

extern RAK11160Board board;
extern WRAPPER_CLASS radio_driver;
extern VolatileRTCClock rtc_clock;
extern SensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
