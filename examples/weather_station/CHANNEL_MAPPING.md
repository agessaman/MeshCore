# Weather Station Telemetry Channel Mapping

This document describes the CayenneLPP telemetry channels and data types used by the Weather Station sensor.

## Channel 1 (TELEM_CHANNEL_SELF = 1) - BME280 Environmental Data

Multiple data types can share the same channel in CayenneLPP. All BME280 readings are on Channel 1:

1. **Type 116 (VOLTAGE)** - Battery voltage in Volts
   - Added by `SensorMesh::handleRequest()` before querying sensors
   - Represents the device's battery voltage (e.g., 3.98V)

2. **Type 103 (TEMPERATURE)** - Temperature in Celsius
   - BME280 temperature reading
   - Range: typically -40°C to +85°C

3. **Type 115 (BAROMETRIC_PRESSURE)** - Atmospheric pressure in hPa
   - BME280 pressure reading (converted from Pa to hPa)
   - Typical range: 300-1100 hPa

4. **Type 104 (RELATIVE_HUMIDITY)** - Relative humidity in %
   - BME280 humidity reading
   - Range: 0-100%

5. **Type 121 (ALTITUDE)** - Altitude in meters above sea level
   - Calculated from BME280 pressure using calibrated sea level pressure (1028.6 hPa)
   - Calibrated for 50.5m ASL location

## Channel 2 (TELEM_CHANNEL_SELF + 1 = 2) - Rainfall

1. **Type 100 (GENERIC_SENSOR)** - Cumulative rain gauge tips since boot
   - Each tip = 0.2794 mm (`WEATHER_MM_PER_TIP`); rainfall totals are computed by the receiver

2. **Type 133 (UNIXTIME)** - Device uptime in seconds (`millis() / 1000`)
   - Lets the receiver detect reboots (tip counter resets) and compute rates between readings

## Channel 3 (TELEM_CHANNEL_SELF + 2 = 3) - Momentary Wind

1. **Type 2 (ANALOG_INPUT)** - Wind speed in km/h
   - Resolution: 0.01 km/h; clamped to 0-327.67 (signed 16-bit limit)
   - Calculated from anemometer clicks: 2.4 km/h per click/second (`WEATHER_WIND_FACTOR`)
   - Always sent, even if 0

2. **Type 132 (DIRECTION)** - Wind direction in degrees
   - Range: 0-359 (0° = North, 90° = East, 180° = South, 270° = West)
   - Read from wind vane via analog input (WB_A1, pin 31) and mapped with the calibrated `vaneMap` table
   - Always sent (defaults to 0° North if ADC reading is out of range)

## Channel 4 (TELEM_CHANNEL_SELF + 3 = 4) - Peak Wind

- **Type 2 (ANALOG_INPUT)** - Peak wind speed over the last 5 minutes, in km/h
  - Same resolution and range as channel 3

## Channel 5 (TELEM_CHANNEL_SELF + 4 = 5) - MCU Temperature

- **Type 103 (TEMPERATURE)** - nRF52 internal die temperature in Celsius
  - Added by `SensorMesh` after querying sensors; omitted if the board reports NaN
  - Kept off channel 1 so it never collides with the BME280 temperature

## Data Type Reference

| Type ID | Name | Size | Resolution | Signed |
|---------|------|------|------------|--------|
| 116 | VOLTAGE | 2 bytes | 0.01V | No |
| 103 | TEMPERATURE | 2 bytes | 0.1°C | Yes |
| 115 | BAROMETRIC_PRESSURE | 2 bytes | 0.1 hPa | No |
| 104 | RELATIVE_HUMIDITY | 1 byte | 0.5% | No |
| 121 | ALTITUDE | 2 bytes | 1m | Yes |
| 100 | GENERIC_SENSOR | 4 bytes | 1 | No |
| 133 | UNIXTIME | 4 bytes | 1s | No |
| 2 | ANALOG_INPUT | 2 bytes | 0.01 | Yes |
| 132 | DIRECTION | 2 bytes | 1° | No |

## Example Telemetry Buffer Structure

Each entry is 1 byte channel + 1 byte type + data. With the BME280 present the payload is 47 bytes
(51 bytes in a request reply, 55 in a subscription push; packet limit is 184).

```
Bytes  0-3:   Channel 1, Type 116 (VOLTAGE) - Battery voltage
Bytes  4-7:   Channel 1, Type 103 (TEMPERATURE) - BME280 temperature
Bytes  8-11:  Channel 1, Type 115 (BAROMETRIC_PRESSURE) - Pressure
Bytes 12-14:  Channel 1, Type 104 (RELATIVE_HUMIDITY) - Humidity
Bytes 15-18:  Channel 1, Type 121 (ALTITUDE) - Altitude
Bytes 19-24:  Channel 2, Type 100 (GENERIC_SENSOR) - Rain tips
Bytes 25-30:  Channel 2, Type 133 (UNIXTIME) - Uptime
Bytes 31-34:  Channel 3, Type 2 (ANALOG_INPUT) - Wind speed
Bytes 35-38:  Channel 3, Type 132 (DIRECTION) - Wind direction
Bytes 39-42:  Channel 4, Type 2 (ANALOG_INPUT) - Peak wind (5 min)
Bytes 43-46:  Channel 5, Type 103 (TEMPERATURE) - MCU temperature
```
