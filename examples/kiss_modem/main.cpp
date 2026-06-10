#include <Arduino.h>
#include <target.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/IdentityStore.h>
#include "KissModem.h"

#if !defined(KISS_NO_CRYPTO)
#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#else
  #include <InternalFileSystem.h>
#endif
#endif

#if defined(KISS_UART_RX) && defined(KISS_UART_TX)
  #include <HardwareSerial.h>
#endif

#define NOISE_FLOOR_CALIB_INTERVAL_MS 2000
#define AGC_RESET_INTERVAL_MS 30000

// Optional liveness heartbeat: define KISS_HEARTBEAT_LED to a pin to toggle it from the
// top of loop(). It blinks iff loop() keeps iterating, so a frozen LED means loop() is
// hung in a blocking call (MCU/radio), whereas a still-blinking LED during a stuck modem
// points at a USB-TX/serial stall instead. Diagnostic only.
#if defined(KISS_HEARTBEAT_LED) && !defined(KISS_HEARTBEAT_INTERVAL_MS)
#define KISS_HEARTBEAT_INTERVAL_MS 250
#endif

#if defined(KISS_WATCHDOG_MS)
// Independent watchdog (STM32 IWDG): if loop() stalls past the timeout — e.g. RadioLib's
// unbounded BUSY-wait on TX entry (SX126x::launchMode) — the MCU resets and re-inits, so
// the modem self-recovers instead of staying frozen until a power cycle.
#include <IWatchdog.h>
#endif

StdRNG rng;
mesh::LocalIdentity identity;
KissModem* modem;
static uint32_t next_noise_floor_calib_ms = 0;
#ifndef KISS_DISABLE_AGC_RESET
static uint32_t next_agc_reset_ms = 0;
#endif

void halt() {
  while (1) ;
}

#ifdef KISS_HEARTBEAT_LED
// Override the core's weak HardFault handler (an infinite loop, indistinguishable from a
// hang) with a fast blink (~14Hz) on the heartbeat LED. With the watchdog active the MCU
// still resets after KISS_WATCHDOG_MS, so a fault shows as a fast-blink burst then reboot,
// while a wedged blocking call (e.g. RadioLib BUSY spin) shows as solid then reboot.
// On the F103, MemManage/BusFault/UsageFault are not separately enabled, so all faults
// escalate to HardFault and land here.
#if defined(BKP)
// Persist the fault record in the F103 backup registers, which survive the IWDG reset
// (anything short of backup-domain power loss). The board reports it via the device-name
// string after reboot; resolve the pc/lr against this build's firmware.elf with addr2line.
static void kiss_fault_record(uint32_t pc, uint32_t lr, uint32_t cfsr) {
  RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
  PWR->CR |= PWR_CR_DBP;
  BKP->DR1 = 0xFA17;  // magic: record valid
  BKP->DR2 = pc & 0xFFFF;
  BKP->DR3 = pc >> 16;
  BKP->DR4 = cfsr & 0xFFFF;
  BKP->DR5 = cfsr >> 16;
  BKP->DR6 = lr & 0xFFFF;
  BKP->DR7 = lr >> 16;
  BKP->DR8 = BKP->DR8 + 1;  // fault count since backup-domain power-up
}
#endif

// 'used' + 'noinline': the only reference is the asm branch in HardFault_Handler, which
// LTO can't see, so without these the -flto USB build discards the symbol and fails to link.
extern "C" __attribute__((used, noinline)) void kiss_fault_blink(uint32_t* frame) {
#if defined(BKP)
  // frame = exception stack: r0,r1,r2,r3,r12,lr,pc,xpsr
  kiss_fault_record(frame[6], frame[5], SCB->CFSR);
#else
  (void)frame;
#endif
  // Fault context: SysTick is masked so millis()/delay() are dead; burn cycles instead.
  pinMode(KISS_HEARTBEAT_LED, OUTPUT);
  for (;;) {
    digitalWrite(KISS_HEARTBEAT_LED, LOW);
    for (volatile uint32_t i = 0; i < 500000; i++) ;
    digitalWrite(KISS_HEARTBEAT_LED, HIGH);
    for (volatile uint32_t i = 0; i < 500000; i++) ;
  }
}

extern "C" __attribute__((naked)) void HardFault_Handler(void) {
  // Pick whichever stack the fault came from so the stacked pc/lr are read correctly.
  __asm volatile(
    ".syntax unified      \n"
    "tst lr, #4           \n"
    "ite eq               \n"
    "mrseq r0, msp        \n"
    "mrsne r0, psp        \n"
    "b kiss_fault_blink   \n"
  );
}
#endif

#if !defined(KISS_NO_CRYPTO)
void loadOrCreateIdentity() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "Filesystem not defined"
#endif

  if (!store.load("_main", identity)) {
    identity = radio_new_identity();
    while (identity.pub_key[0] == 0x00 || identity.pub_key[0] == 0xFF) {
      identity = radio_new_identity();
    }
    store.save("_main", identity);
  }
}
#endif

void onSetRadio(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio_driver.setParams(freq, bw, sf, cr);
}

void onSetTxPower(uint8_t power) {
  radio_driver.setTxPower(power);
}

float onGetCurrentRssi() {
  return radio_driver.getCurrentRSSI();
}

void onGetStats(uint32_t* rx, uint32_t* tx, uint32_t* errors) {
  *rx = radio_driver.getPacketsRecv();
  *tx = radio_driver.getPacketsSent();
  *errors = radio_driver.getPacketsRecvErrors();
}

void setup() {
  board.begin();

  if (!radio_init()) {
    halt();
  }

  radio_driver.begin();

  rng.begin(radio_driver.getRngSeed());
#if !defined(KISS_NO_CRYPTO)
  loadOrCreateIdentity();
#endif

  sensors.begin();

#if defined(KISS_UART_RX) && defined(KISS_UART_TX)
#if defined(ESP32)
  Serial1.setPins(KISS_UART_RX, KISS_UART_TX);
  Serial1.begin(115200);
#elif defined(NRF52_PLATFORM)
  ((Uart *)&Serial1)->setPins(KISS_UART_RX, KISS_UART_TX);
  Serial1.begin(115200);
#elif defined(RP2040_PLATFORM)
  ((SerialUART *)&Serial1)->setRX(KISS_UART_RX);
  ((SerialUART *)&Serial1)->setTX(KISS_UART_TX);
  Serial1.begin(115200);
#elif defined(STM32_PLATFORM)
  ((HardwareSerial *)&Serial1)->setRx(KISS_UART_RX);
  ((HardwareSerial *)&Serial1)->setTx(KISS_UART_TX);
  Serial1.begin(115200);
#else
  #error "KISS UART not supported on this platform"
#endif
  modem = new KissModem(Serial1, identity, rng, radio_driver, board, sensors);
#else
#if defined(ESP32) && (ARDUINO_USB_MODE == 1)
  Serial.setTxBufferSize(KISS_TX_BUFFER_SIZE);  // HWCDC ring must fit a whole KISS frame; set before begin()
#endif
  Serial.begin(115200);
#if defined(ESP32)
  Serial.setTxTimeoutMs(KISS_WRITE_TIMEOUT_MS);
#endif
  uint32_t start = millis();
  while (!Serial && millis() - start < 3000) delay(10);
  delay(100);
  modem = new KissModem(Serial, identity, rng, radio_driver, board, sensors);
#endif

  modem->setRadioCallback(onSetRadio);
  modem->setTxPowerCallback(onSetTxPower);
  modem->setGetCurrentRssiCallback(onGetCurrentRssi);
  modem->setGetStatsCallback(onGetStats);
  modem->begin();

#ifdef KISS_HEARTBEAT_LED
  pinMode(KISS_HEARTBEAT_LED, OUTPUT);
  digitalWrite(KISS_HEARTBEAT_LED, HIGH);
#endif

#if defined(KISS_WATCHDOG_MS)
  // Start last, after the slow init (radio bring-up, serial settle) has finished.
  IWatchdog.begin((uint32_t)KISS_WATCHDOG_MS * 1000);  // begin() takes microseconds
#endif

  board.onBootComplete();
}

void loop() {
#if defined(KISS_WATCHDOG_MS)
  IWatchdog.reload();  // a wedged loop() stops reloading -> watchdog resets the MCU
#endif

#ifdef KISS_HEARTBEAT_LED
  // Toggle at the very top so it reflects whether loop() is re-entering at all.
  static uint32_t hb_last = 0;
  static uint8_t hb_state = 0;
  if ((uint32_t)(millis() - hb_last) >= KISS_HEARTBEAT_INTERVAL_MS) {
    hb_last = millis();
    hb_state = !hb_state;
    digitalWrite(KISS_HEARTBEAT_LED, hb_state);
  }
#endif

  modem->loop();

  if (!modem->isActuallyTransmitting()) {
    if (!modem->isTxBusy()) {
#ifndef KISS_DISABLE_AGC_RESET
      if ((uint32_t)(millis() - next_agc_reset_ms) >= AGC_RESET_INTERVAL_MS) {
        radio_driver.resetAGC();
        next_agc_reset_ms = millis();
      }
#endif
    }

    uint8_t rx_buf[256];
    int rx_len = radio_driver.recvRaw(rx_buf, sizeof(rx_buf));
    if (rx_len > 0) {
      int8_t snr = (int8_t)(radio_driver.getLastSNR() * 4);
      int8_t rssi = (int8_t)radio_driver.getLastRSSI();
      modem->onPacketReceived(snr, rssi, rx_buf, rx_len);
    }
  }

  if ((uint32_t)(millis() - next_noise_floor_calib_ms) >= NOISE_FLOOR_CALIB_INTERVAL_MS) {
    radio_driver.triggerNoiseFloorCalibrate(0);
    next_noise_floor_calib_ms = millis();
  }
  radio_driver.loop();
}
