# Automated School Garden - ESP32

## Overview

Automated School Garden is an ESP32 firmware project for a school irrigation controller. The current revision is focused on safe local operation, observable diagnostics, and a clean PlatformIO workflow suitable for a public embedded/IoT portfolio.

The system is designed to keep running without Wi-Fi or Internet access. Cloud telemetry and automatic irrigation are intentionally disabled in this revision while hardware calibration and external services are still pending.

## Features

- ESP32-WROOM-32 / ESP32 Dev Module firmware using Arduino Framework and PlatformIO.
- Cooperative `millis()` scheduling with no blocking Wi-Fi boot loop.
- Soil-moisture raw ADC reading with median-trimmed filtering and slow smoothing.
- Irrigation finite state machine with fail-safe relay behavior.
- Open-Meteo weather integration for remote rain, temperature, and humidity data.
- 16x2 I2C LCD diagnostic pages with low-flicker line caching.
- Native ESP32 BOOT button on GPIO0 for LCD page navigation.
- Watchdog, heartbeat LED, serial diagnostics, and reset-reason reporting.
- Secret management through an ignored local `include/secrets.h`.

## Hardware

| Function | ESP32 Pin | Notes |
| --- | ---: | --- |
| Soil moisture analog input | GPIO36 | ADC input, raw values remain visible before calibration |
| Relay / valve control | GPIO23 | `HIGH` closes valve, `LOW` opens valve |
| Buzzer | GPIO13 | Pre-irrigation warning |
| I2C LCD SDA | GPIO21 | LCD address `0x3F` |
| I2C LCD SCL | GPIO22 | LCD address `0x3F` |
| Heartbeat LED | GPIO2 | On-board LED on many ESP32 Dev Modules |
| BOOT button | GPIO0 | Active low, runtime LCD navigation |

Holding BOOT while resetting or powering the ESP32 can enter flashing/download mode. The firmware only uses GPIO0 at runtime with `INPUT_PULLUP`.

## Firmware Architecture

The main loop is organized around small cooperative handlers:

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

## Safety / Fail-Safe Design

Implemented:

- Relay is driven closed at boot before other initialization.
- Valve closes on soil sensor fault or unknown irrigation state.
- Maximum valve-open time, pulse limit, cooldown, soak interval, and warning buzzer are preserved.
- Rain forecast can block irrigation when automatic irrigation is enabled in the future.
- Watchdog remains active.

Disabled by design:

- `ENABLE_AUTOMATIC_IRRIGATION = false`
- `ENABLE_CLOUD_LOGGING = false`

Pending:

- Final physical validation of stored dry/wet soil calibration values.
- Google Apps Script endpoint and cloud telemetry upload.

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

## Weather Integration

Open-Meteo remains supported using:

- Latitude: `-16.736933184033457`
- Longitude: `-49.23922053568986`
- Timezone: `America/Sao_Paulo`

Weather values are remote forecast/API data, not local physical sensor measurements.

## Secrets Management

Create a local secrets file before building:

```cpp
#include "secrets.example.h"
```

In practice, copy `include/secrets.example.h` to `include/secrets.h` and replace placeholders locally. `include/secrets.h` is ignored by Git and must not be committed.

Never place Wi-Fi passwords, private API keys, Google Apps Script URLs, or service tokens in tracked source files.

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
| DHT11 signal | GPIO15 | Under hardware investigation; GPIO15 is a strapping pin |
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
- Reintroduce an environmental sensor after hardware diagnosis.
- Configure Google Apps Script and enable cloud logging safely.
- Add structured telemetry payloads and long-term data storage.
