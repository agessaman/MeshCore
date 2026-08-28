#include "HeltecV4R8Board.h"

#if defined(HELTEC_V4_R8_TFT) && defined(DISPLAY_CLASS)
  #include <target.h>
#endif

void HeltecV4R8Board::begin() {
  ESP32Board::begin();

  periph_power.begin();
  periph_power.claim();  // R8 VEXT also feeds the LoRa antenna boost rail.

  loRaFEMControl.init();

  // Expansion Kit V2 display/touch pins, verified against Heltec's
  // Expansion_board_V2.03 schematic and the V4-R8 datasheet pinout:
  //   GPIO 17/18  TP_SDA / TP_SCL - the module's OLED_SDA/OLED_SCL I2C bus
  //   GPIO 21     LCD_RST *and* TP_RST on one net (the module's OLED_RST)
  //   GPIO 43     TP_INT, optional via R13, and also U0TXD
  //   GPIO 44     LCD_LEDK backlight, also U0RXD
  // ST7789LCDDisplay owns GPIO 21, so there is no separate touch reset to do
  // here - and because that net is shared, it must not be parked low while the
  // display is off or the touch controller is held in reset with it.

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET;
    }

    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}

void HeltecV4R8Board::onBeforeTransmit(void) {
  digitalWrite(P_LORA_TX_LED, HIGH);
  loRaFEMControl.setTxModeEnable();
}

void HeltecV4R8Board::onAfterTransmit(void) {
  digitalWrite(P_LORA_TX_LED, LOW);
  loRaFEMControl.setRxModeEnable();
}

void HeltecV4R8Board::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
#if defined(HELTEC_V4_R8_TFT) && defined(DISPLAY_CLASS)
  display.turnOff();
#endif

  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);

  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);
  loRaFEMControl.setRxModeEnableWhenMCUSleep();

  if (pin_wake_btn < 0) {
    esp_sleep_enable_ext1_wakeup((1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);
  } else {
    esp_sleep_enable_ext1_wakeup((1L << P_LORA_DIO_1) | (1L << pin_wake_btn), ESP_EXT1_WAKEUP_ANY_HIGH);
  }

  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000);
  }

  esp_deep_sleep_start();
}

void HeltecV4R8Board::powerOff() {
  enterDeepSleep(0);
}

uint16_t HeltecV4R8Board::getBattMilliVolts() {
  analogReadResolution(12);

  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) {
    raw += analogReadMilliVolts(PIN_VBAT_READ);
  }
  raw = raw / 8;

  return (adc_mult * raw);
}

const char* HeltecV4R8Board::getManufacturerName() const {
#ifdef HELTEC_V4_R8_TFT
  return "Heltec V4 R8 TFT";
#else
  return "Heltec V4 R8 OLED";
#endif
}

bool HeltecV4R8Board::setLoRaFemLnaEnabled(bool enable) {
  if (!loRaFEMControl.isLnaCanControl()) {
    return false;
  }

  loRaFEMControl.setLNAEnable(enable);
  loRaFEMControl.setRxModeEnable();
  return true;
}

bool HeltecV4R8Board::canControlLoRaFemLna() const {
  return loRaFEMControl.isLnaCanControl();
}

bool HeltecV4R8Board::isLoRaFemLnaEnabled() const {
  return loRaFEMControl.isLNAEnabled();
}
