#include "LoRaFEMControl.h"
#include <driver/rtc_io.h>
#include <Arduino.h>

// GC1109 FEM (see platformio.ini): VFEM/CSD/CPS on P_LORA_PA_*; CTX from SX1262 DIO2.

void LoRaFEMControl::init(void) {
  pinMode(P_LORA_PA_POWER, OUTPUT);
  digitalWrite(P_LORA_PA_POWER, HIGH);
  rtc_gpio_hold_dis((gpio_num_t)P_LORA_PA_POWER);

  pinMode(P_LORA_PA_EN, OUTPUT);
  digitalWrite(P_LORA_PA_EN, HIGH);
  rtc_gpio_hold_dis((gpio_num_t)P_LORA_PA_EN);

  pinMode(P_LORA_PA_TX_EN, OUTPUT);
  digitalWrite(P_LORA_PA_TX_EN, LOW);

  setLnaCanControl(true);
}

void LoRaFEMControl::setSleepModeEnable(void) {
  digitalWrite(P_LORA_PA_EN, LOW);
}

void LoRaFEMControl::setTxModeEnable(void) {
  pinMode(P_LORA_PA_TX_EN, OUTPUT);
  digitalWrite(P_LORA_PA_TX_EN, HIGH);
}

void LoRaFEMControl::setRxModeEnable(void) {
  digitalWrite(P_LORA_PA_TX_EN, LOW);
  pinMode(P_LORA_PA_TX_EN, INPUT);
}

void LoRaFEMControl::setRxModeEnableWhenMCUSleep(void) {
  digitalWrite(P_LORA_PA_EN, HIGH);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_PA_EN);
  rtc_gpio_hold_en((gpio_num_t)P_LORA_PA_POWER);
}

void LoRaFEMControl::setLNAEnable(bool enabled) {
  lna_enabled = enabled;
}
