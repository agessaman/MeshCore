#ifndef _VARIANT_HELTEC_RC52_
#define _VARIANT_HELTEC_RC52_

#define VARIANT_MCK (64000000ul)
#define USE_LFXO

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HARD_VERSION_ADDR    (0xED000 + 7 * 4096 - 16 - 1)
#define HT_LICENSE_ADDR      (0xED000 + 7 * 4096 - 16)
#define HT_LICENSE_ADDR_BASE (0xED000 + 6 * 4096)

#define PINS_COUNT         (48)
#define NUM_DIGITAL_PINS   (48)
#define NUM_ANALOG_INPUTS  (1)
#define NUM_ANALOG_OUTPUTS (0)

#define PIN_LED1     (-1)
#define PIN_NEOPIXEL (-1)
#define NEOPIXEL_NUM 0
#define LED_BUILTIN  PIN_LED1
#define LED_BLUE     PIN_LED1
#define LED_STATE_ON 1

#define PIN_BUTTON1      (32 + 10) // P1.10, external pull-up, active low
#define PIN_BUTTON_USER  PIN_BUTTON1
#define PIN_USER_BTN     PIN_BUTTON1

#define ADC_RESOLUTION 14
#define PIN_VBAT_READ (0 + 31)
#define PIN_ADC_CTRL (0 + 4)
#define ADC_CTRL_ENABLED HIGH
#define ADC_MULTIPLIER 4.9F
#define MV_LSB (3000.0F / 4096.0F)

#define PWRMGT_VOLTAGE_BOOTLOCK 3300
#define PWRMGT_LPCOMP_AIN 7
#define PWRMGT_LPCOMP_REFSEL 1

#define WIRE_INTERFACES_COUNT 1

#define PIN_BOARD_SDA (32 + 11) // P1.11
#define PIN_BOARD_SCL (0 + 2)   // P0.02
#define PIN_WIRE_SDA PIN_BOARD_SDA
#define PIN_WIRE_SCL PIN_BOARD_SCL
#define SENSOR_POWER_CTRL_PIN (0 + 12)
#define SENSOR_POWER_ON HIGH
#define SENSOR_INT (0 + 20)
#define SENSOR_RST_PIN (32 + 15)


// GPS UART. Naming follows the MeshCore convention used by the other variants:
// PIN_GPS_TX is the line the GPS drives (bits TOWARDS the CPU) and PIN_GPS_RX is
// the line the CPU drives (bits TOWARDS the GPS). EnvironmentSensorManager calls
// Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX), so these must follow that convention.
// NOTE: Heltec's RC52 pinout and schematic label P0.08 "nRF_TX" and P0.07
// "nRF_RX", which reads the opposite way. Meshtastic's heltec-rc52 variant also
// puts the CPU's receive line on P0.08, so that is what we use here.
#define PIN_GPS_TX (0 + 8)   // P0.08
#define PIN_GPS_RX (0 + 7)   // P0.07
#define PIN_GPS_EN (32 + 9)
#define PIN_GPS_PPS (32 + 1)
#define PIN_GPS_RESET (32 + 6)
#define PIN_GPS_EN_ACTIVE HIGH
#define PIN_GPS_RESET_ACTIVE LOW
#define GPS_BAUD_RATE 9600
#define GPS_THREAD_INTERVAL 50
#define PERIPHERAL_WARMUP_MS 100

#define PIN_SERIAL1_RX PIN_GPS_TX  // bits coming FROM the GPS
#define PIN_SERIAL1_TX PIN_GPS_RX  // bits going TOWARDS the GPS

#define PIN_TFT_CS        (32 + 4)
#define PIN_TFT_RST       (0 + 10)
#define PIN_TFT_DC        (0 + 28)
#define PIN_TFT_SCL       (0 + 30)
#define PIN_TFT_SDA       (32 + 2)
#define PIN_TFT_EN        (32 + 13)
#define PIN_TFT_EN_ACTIVE LOW
#define PIN_TFT_BL        (0 + 9)
#define PIN_TFT_BL_ACTIVE HIGH
#define SPI_FREQUENCY 8000000

#define RADIOCORE_FEM_EN    (0 + 26)
#define RADIOCORE_VFEM_CTRL (0 + 16)

#define SPI_INTERFACES_COUNT 2
#define SPI_32MHZ_INTERFACE 1

#define PIN_SPI_MISO (0 + 14)
#define PIN_SPI_MOSI (0 + 22)
#define PIN_SPI_SCK  (0 + 25)

#define PIN_SPI1_MISO (0)   // 0 maps to 0xff in g_ADigitalPinMap, ie NRFX_SPIM_PIN_NOT_USED
#define PIN_SPI1_MOSI PIN_TFT_SDA
#define PIN_SPI1_SCK  PIN_TFT_SCL

#ifdef __cplusplus
}
#endif

#endif
