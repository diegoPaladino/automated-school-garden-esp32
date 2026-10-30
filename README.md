# Automated School Garden - ESP32

## Overview

Automated School Garden is an ESP32 firmware project for a school irrigation controller. The current revision is focused on safe local operation, observable diagnostics, and a clean PlatformIO workflow suitable for a public embedded/IoT portfolio.

The system is designed to keep running without Wi-Fi or Internet access. Real cloud telemetry is enabled through the deployed Google Apps Script backend, and automatic irrigation is enabled when a valid calibrated soil reading permits it. See [installation and field validation](src/INSTALACAO.md) before unattended operation.

## Features

- ESP32-WROOM-32 / ESP32 Dev Module firmware using Arduino Framework and PlatformIO.
- Cooperative `millis()` scheduling with no blocking Wi-Fi boot loop.
- Soil-moisture raw ADC reading with median-trimmed filtering and slow smoothing.
- Local DHT11 environmental readings for temperature and air humidity.
- Runtime soil calibration stored in ESP32 NVS, with BOOT/LCD maintenance workflow.
- Irrigation finite state machine with fail-safe relay behavior.
- Open-Meteo weather integration for remote/modelled current temperature/humidity and next-6-hours rain forecast.
- 16x2 I2C LCD with PUBLIC and DEVELOPER page rotations and low-flicker line caching.
- Native ESP32 BOOT button on GPIO0 for LCD navigation, display-mode toggle, soil calibration, and arming the manual valve test.
- Human-readable uptime on DEVELOPER LCD pages.
- Watchdog, heartbeat LED, serial diagnostics, and reset-reason reporting.
- HTTPS telemetry to Google Apps Script, which writes Google Sheets rows and drives backend email/report monitoring.
- Secret management through an ignored local `include/secrets.h`.

## Hardware

| Function | ESP32 Pin | Notes |
| --- | ---: | --- |
| Soil moisture analog input | GPIO36 | ADC input, raw values remain visible before calibration |
| DHT11 data | GPIO15 | Local temperature and local air humidity; GPIO15 is strapping-capable, avoid holding it low during boot |
| Relay / valve control | GPIO23 | `HIGH` closes valve, `LOW` opens valve |
| Buzzer | GPIO13 | Pre-irrigation warning |
| I2C LCD SDA | GPIO21 | LCD address `0x3F` |
| I2C LCD SCL | GPIO22 | LCD address `0x3F` |
| Heartbeat LED | GPIO2 | On-board LED on many ESP32 Dev Modules |
| BOOT button | GPIO0 | Active low, runtime LCD navigation |

Holding BOOT while resetting or powering the ESP32 can enter flashing/download mode. The firmware only uses GPIO0 at runtime with `INPUT_PULLUP`.

Runtime BOOT button behavior:

- Short press under 1.5 seconds: next LCD page.
- Three quick short presses, each separated by about 600 ms and completed within about 2 seconds: toggle PUBLIC / DEVELOPER LCD mode.
- Medium press from 1.5 seconds to under 3 seconds: safely ignored.
- Long press from 3 seconds to under 8 seconds: enter soil calibration mode.
- Very long press of 8 seconds or more: arm manual valve test only; a second short click within 10 seconds confirms a maximum 3-second valve test, and any BOOT press during the test closes the valve immediately.

DEVELOPER mode is runtime-only and is not persisted to NVS. Every reset starts in PUBLIC mode. If no BOOT interaction occurs for about 2 minutes while in DEVELOPER mode, the LCD automatically returns to PUBLIC mode.

## Firmware Architecture

The main loop gives valve control priority over blocking network requests and uses these cooperative handlers:

- `handleSensors()`
- `handleWiFi()`
- `handleTime()`
- `handleWeather()`
- `handleIrrigation()`
- `handleButton()`
- `handleDisplay()`
- `handleCloudLogging()`
- `handleHeartbeat()`
- `handleSerialStatus()`

Irrigation states include boot, idle, pre-irrigation warning, irrigating, soak wait, cooldown, and sensor fault. Uncertain conditions fail safe with the valve closed.

The DHT11 is sampled cooperatively about once every 5 seconds. Invalid DHT values are never converted to zero, never reset the ESP32, and never block irrigation. If the sensor fails, the last valid values remain internal telemetry but the public LCD shows `INDISPONIVEL` until fresh data is available.

LCD PUBLIC mode is intended for students, teachers, families, and visitors:

- `HORTA: NORMAL` / `VALVULA: OFF`.
- `UMIDADE SOLO` as a calibrated percentage, or calibration guidance.
- `AMBIENTE LOCAL` with local DHT11 temperature and relative humidity on one page.
- `PREVISAO 6H` with Open-Meteo rain probability and, when it fits, total forecast precipitation.

LCD DEVELOPER mode exposes maintenance diagnostics such as Wi-Fi/RSSI, connected SSID, raw/filtered soil ADC values, DHT status and local values, Open-Meteo remote values, Open-Meteo grid coordinates, rain probability/mm, cloud status/retry cadence, firmware version, reset reason, uptime, system state, decision state, valve state, and automatic-irrigation state.

## Cloud Telemetry

The production telemetry path is:

```text
ESP32 -> HTTPS POST -> Google Apps Script Web App -> Google Sheets / email / reports
```

The ESP32 sends authenticated JSON telemetry to the configured Google Apps Script `/exec` URL. The device token is sent only in the JSON body, never in the URL, query string, LCD, or serial logs. The firmware treats a transmission as successful only when the HTTP request completes and the returned JSON contains `ok: true`.

Production firmware v15.4.4 was built and flashed to the ESP32 on 2026-10-29. Two authenticated telemetry rows were confirmed in Google Sheets exactly 300 seconds apart. This bench validation used disconnected local sensors: invalid local measurements were stored as null with FAULT status, and the valve remained closed. Sensor calibration and water-flow validation on site remain pending.

Normal telemetry runs every 5 minutes. A successful telemetry POST is also the heartbeat; there is no separate heartbeat HTTP request. The external Google Apps Script watchdog can therefore mark the device offline if no successful telemetry row is received for approximately 15 minutes.

If Wi-Fi, DNS, TLS, Google Apps Script, JSON parsing, or token validation fails, the local controller continues to run and a bounded retry is scheduled. Cloud telemetry is skipped while the valve is open or the irrigation state machine is in a time-sensitive valve-actuation phase.

## Safety / Fail-Safe Design

Implemented:

- Relay is driven closed at boot before other initialization.
- Valve closes on soil sensor fault or unknown irrigation state.
- Maximum valve-open time, pulse limit, cooldown, soak interval, and warning buzzer are preserved.
- Rain forecast can block irrigation when automatic irrigation is enabled.
- Watchdog remains active.

Automatic irrigation configuration:

- `ENABLE_AUTOMATIC_IRRIGATION = true` (requires valid soil calibration and readings)

Pending:

- Final physical validation of stored dry/wet soil calibration values.

## Soil Sensor Calibration

Production soil calibration is loaded at boot from the ESP32 NVS flash using the Arduino `Preferences` API. There is no filesystem path for this data.

NVS location:

```text
namespace: soilcal
```

Keys:

```text
valid
dry
wet
version
```

The production firmware opens the `soilcal` namespace read-only, reads the keys, validates them independently, then closes the Preferences handle. Calibration endpoints at ADC rail values `0` or `4095`, unsupported versions, zero span, insufficient span, missing keys, or a false `valid` flag are treated as invalid. When calibration is missing or invalid, raw soil readings still work, no moisture percentage is fabricated, and irrigation remains blocked.

Normal firmware uploads and reboots usually preserve NVS, so the calibration remains available after changing firmware. Erase-flash operations can remove it. Replacing the physical ESP32 requires recalibration or migration, and replacing the soil probe should trigger recalibration. The resulting percentage is relative soil moisture for this installation, not laboratory volumetric water content.

Production calibration mode is available on the 16x2 LCD:

1. Hold BOOT for 3 to under 8 seconds to enter calibration.
2. With the probe in dry reference condition, short-press BOOT to measure dry soil.
3. With the probe in wet reference condition, short-press BOOT to measure wet soil.
4. Review the candidate dry/wet values on the LCD.
5. Hold BOOT to save the candidate calibration to NVS.

The candidate stays in RAM until the review/save step. Cancelling or producing an invalid candidate preserves the previous NVS calibration. Long-press during the dry-ready or wet-ready screens cancels calibration. During calibration the valve is kept fail-safe closed and automatic irrigation is suspended.

## Weather Integration

Local environmental readings and remote weather data are intentionally separate:

- DHT11 on GPIO15: local temperature and local air humidity near the garden.
- Open-Meteo: remote/external current weather and next-6-hours forecast.

Open-Meteo remains supported using:

- Latitude: `-16.736742064908807`
- Longitude: `-49.23936484532197`
- Elevation: `808 m`
- Timezone: `America/Sao_Paulo`

Weather values are remote forecast/API data, not local physical sensor measurements. The DHT11 remains the local sensor and is not calibrated against Internet weather. The firmware requests current `temperature_2m` and `relative_humidity_2m`, plus hourly `precipitation_probability` and `precipitation` for `forecast_hours=6`. The public `PREVISAO 6H` page shows unavailable weather as `INDISPONIVEL`, which is distinct from a valid 0% / 0.0 mm forecast. Developer diagnostics retain the response grid latitude, longitude, elevation, timezone, UTC offset, remote temperature/humidity, rain probability, and rain millimeters.

The request structure is:

```text
https://api.open-meteo.com/v1/forecast
  ?latitude=-16.736742064908807
  &longitude=-49.23936484532197
  &elevation=808
  &current=temperature_2m,relative_humidity_2m
  &hourly=precipitation_probability,precipitation
  &forecast_hours=6
  &timezone=America/Sao_Paulo
```

Malformed or incomplete weather responses are reported as API failures instead of being treated as valid readings.

## Secrets Management

Create a local secrets file before building:

```cpp
#include "secrets.example.h"
```

In practice, copy `include/secrets.example.h` to `include/secrets.h` and replace placeholders locally. `include/secrets.h` is ignored by Git and must not be committed.

The tracked `include/secrets.example.h` contains only placeholders, including `YOUR_GOOGLE_APPS_SCRIPT_URL` and `YOUR_DEVICE_TOKEN`. Never place Wi-Fi passwords, Google Apps Script URLs, service tokens, or other private credentials in tracked source files.

## Building with PlatformIO

```bash
pio run
```

## Flashing

Upload only when the ESP32 is connected and you intentionally want to flash it:

```bash
pio run --target upload
```

## Serial Monitor

```bash
pio device monitor -b 115200
```

Serial logs use concise tags such as `[BOOT]`, `[WIFI]`, `[SOIL]`, `[WEATHER]`, `[DECISION]`, `[VALVE]`, `[BUTTON]`, and `[STATUS]`.

## Hardware Diagnostics

An isolated bench-test firmware is available in the `hardware_diag` PlatformIO environment. It does not use Wi-Fi, cloud services, NTP, secrets, LittleFS, automatic irrigation, or production telemetry.

Current validated pinout:

| Function | ESP32 Pin | Diagnostic note |
| --- | ---: | --- |
| On-board LED | GPIO2 | Manual blink test |
| Soil moisture analog input | GPIO36 | Raw ADC only, uncalibrated |
| DHT11 signal | GPIO15 | Physically validated local temperature/air humidity sensor; GPIO15 is a strapping pin |
| Buzzer | GPIO13 | Physically validated audible test pin |
| Relay input IN2 | GPIO23 | `HIGH` off, `LOW` active |
| LCD SDA | GPIO21 | I2C address `0x3F` |
| LCD SCL | GPIO22 | 16 columns x 2 rows |
| Native BOOT button | GPIO0 | ESP32 boot/download strapping button |

Build diagnostic firmware:

```bash
pio run -e hardware_diag
```

Upload manually only when you intentionally want to flash the diagnostic firmware:

```bash
pio run -e hardware_diag -t upload
```

Open Serial Monitor:

```bash
pio device monitor -b 115200
```

Diagnostic commands:

| Command | Action |
| --- | --- |
| `h` | Help/menu |
| `i` | Scan I2C bus |
| `l` | Test LCD rows |
| `s` | Single soil raw measurement |
| `S` | Toggle 500 ms soil raw stream |
| `d` | One DHT11 read attempt |
| `b` | Buzzer command test on GPIO13 |
| `r` | Relay pulse test on GPIO23 |
| `e` | On-board LED blink test |
| `a` | Passive summary only |

Warning: `r` physically actuates the relay for approximately 500 ms. The passive summary never activates the relay or buzzer. The soil sensor remains uncalibrated, so diagnostics report raw ADC values only and do not show percentages.

## Roadmap

- Measure real soil dry/wet calibration values.
- Validate sensor readings and valve water flow on site before unattended operation.
- Consider persistent offline telemetry buffering in a future scoped change.
