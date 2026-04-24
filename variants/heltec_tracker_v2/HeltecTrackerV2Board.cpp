#include "HeltecTrackerV2Board.h"

void HeltecTrackerV2Board::begin() {
  ESP32Board::begin();

  pinMode(PIN_ADC_CTRL, OUTPUT);
  digitalWrite(PIN_ADC_CTRL, LOW);

  loRaFEMControl.init();

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason != ESP_RST_DEEPSLEEP) {
    delay(1);
  }

  periph_power.begin();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET;
    }

    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_PA_POWER);
    rtc_gpio_hold_dis((gpio_num_t)P_LORA_PA_EN);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}

void HeltecTrackerV2Board::onBeforeTransmit(void) {
  digitalWrite(P_LORA_TX_LED, HIGH);
  loRaFEMControl.setTxModeEnable();
}

void HeltecTrackerV2Board::onAfterTransmit(void) {
  digitalWrite(P_LORA_TX_LED, LOW);
  loRaFEMControl.setRxModeEnable();
}

void HeltecTrackerV2Board::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  rtc_gpio_set_direction((gpio_num_t)P_LORA_DIO_1, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_en((gpio_num_t)P_LORA_DIO_1);

  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);

  loRaFEMControl.setRxModeEnableWhenMCUSleep();

  if (pin_wake_btn < 0) {
    esp_sleep_enable_ext1_wakeup((1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);
  } else {
    esp_sleep_enable_ext1_wakeup((1L << P_LORA_DIO_1) | (1L << pin_wake_btn),
                                 ESP_EXT1_WAKEUP_ANY_HIGH);
  }

  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000);
  }

  esp_deep_sleep_start();
}

void HeltecTrackerV2Board::powerOff() {
  enterDeepSleep(0);
}

uint16_t HeltecTrackerV2Board::getBattMilliVolts() {
  analogReadResolution(10);
  digitalWrite(PIN_ADC_CTRL, HIGH);
  delay(10);
  uint32_t raw = 0;
  for (int i = 0; i < 8; i++) {
    raw += analogRead(PIN_VBAT_READ);
  }
  raw = raw / 8;

  digitalWrite(PIN_ADC_CTRL, LOW);

  return (5.42 * (3.3 / 1024.0) * raw) * 1000;
}

const char* HeltecTrackerV2Board::getManufacturerName() const {
  return "Heltec Tracker V2";
}
