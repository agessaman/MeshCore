#include <Arduino.h>
#include "target.h"

BluePillBoard board;

// External SX1262 on the default SPI bus. NSS / DIO1 / RESET / BUSY are wired
// to GPIOs; SCLK/MISO/MOSI use the F103's default SPI1 pins (PA5/PA6/PA7).
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock rtc_clock;
SensorManager sensors;

// Device name, optionally extended with the last HardFault record (see below).
static char s_device_name[80] = "STM32F103 BluePill KISS";

#if defined(BKP)
static char* put_hex32(char* p, uint32_t v) {
  static const char hexd[] = "0123456789abcdef";
  for (int i = 28; i >= 0; i -= 4) *p++ = hexd[(v >> i) & 0xF];
  return p;
}

// If the HardFault handler (main.cpp) left a record in the backup registers, append it
// to the device name: "... HF#n pc=xxxxxxxx cfsr=xxxxxxxx lr=xxxxxxxx". The record is
// sticky until backup-domain power loss or the next fault (DR8 counts faults), so a
// repeated count in the host log means no NEW fault since. Resolve pc/lr with
// arm-none-eabi-addr2line -e firmware.elf <pc> against the exact flashed build.
static void appendFaultRecord() {
  RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
  PWR->CR |= PWR_CR_DBP;
  if (BKP->DR1 != 0xFA17) return;

  uint32_t pc = ((uint32_t)BKP->DR3 << 16) | BKP->DR2;
  uint32_t cfsr = ((uint32_t)BKP->DR5 << 16) | BKP->DR4;
  uint32_t lr = ((uint32_t)BKP->DR7 << 16) | BKP->DR6;
  uint16_t count = BKP->DR8;

  char* p = s_device_name + strlen(s_device_name);
  const char* tag = " HF#";
  while (*tag) *p++ = *tag++;
  if (count > 999) count = 999;
  if (count >= 100) *p++ = '0' + (count / 100) % 10;
  if (count >= 10) *p++ = '0' + (count / 10) % 10;
  *p++ = '0' + count % 10;
  const char* pc_tag = " pc=";
  while (*pc_tag) *p++ = *pc_tag++;
  p = put_hex32(p, pc);
  const char* cfsr_tag = " cfsr=";
  while (*cfsr_tag) *p++ = *cfsr_tag++;
  p = put_hex32(p, cfsr);
  const char* lr_tag = " lr=";
  while (*lr_tag) *p++ = *lr_tag++;
  p = put_hex32(p, lr);
  *p = '\0';
}
#endif

void BluePillBoard::begin() {
  STM32Board::begin();
  // E22P EN must be forced high (PB13); RadioLib must NOT drive it as an RF switch.
  pinMode(PB13, OUTPUT);
  digitalWrite(PB13, HIGH);
#if defined(BKP)
  appendFaultRecord();
#endif
}

const char* BluePillBoard::getManufacturerName() const {
  return s_device_name;
}

bool radio_init() {
  return radio.std_init();
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
