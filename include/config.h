#pragma once

#include <Arduino.h>
#include <DHT.h>

constexpr char FIRMWARE_VERSION[] = "v15.2.0";

constexpr int LED_BUILTIN_PIN = 2;
constexpr int SOIL_MOISTURE_PIN = 36;
constexpr int RELAY_PIN = 23;
constexpr int BUZZER_PIN = 13;
constexpr int BOOT_BUTTON_PIN = 0;
constexpr int DHT_PIN = 15;
constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;

constexpr uint8_t DHT_TYPE = DHT11;

constexpr int LED_ON_LEVEL = HIGH;
constexpr int LED_OFF_LEVEL = LOW;
constexpr int VALVE_CLOSED_LEVEL = HIGH;
constexpr int VALVE_OPEN_LEVEL = LOW;
constexpr int BOOT_BUTTON_ACTIVE_LEVEL = LOW;

constexpr uint8_t LCD_I2C_ADDRESS = 0x3F;
constexpr uint8_t LCD_COLUMNS = 16;
constexpr uint8_t LCD_ROWS = 2;

constexpr bool ENABLE_CLOUD_LOGGING = false;
constexpr bool ENABLE_AUTOMATIC_IRRIGATION = false;

constexpr double WEATHER_LATITUDE = -16.736742064908807;
constexpr double WEATHER_LONGITUDE = -49.23936484532197;
constexpr float WEATHER_ELEVATION_M = 808.0f;
constexpr char WEATHER_TIMEZONE[] = "America%2FSao_Paulo";

constexpr int MIN_CALIBRATION_SPAN = 250;
constexpr int INVALID_SOIL_PERCENT = -1;
constexpr int SOIL_START_IRRIGATION_PERCENT = 30;
constexpr int SOIL_STOP_IRRIGATION_PERCENT = 45;

constexpr int RAIN_PROBABILITY_BLOCK_THRESHOLD = 70;
constexpr float RAIN_MM_BLOCK_THRESHOLD = 2.0f;

constexpr unsigned long WATCHDOG_TIMEOUT_MS = 20000UL;
constexpr unsigned long SPLASH_MS = 1500UL;
constexpr unsigned long SOIL_INTERVAL_MS = 3000UL;
constexpr unsigned long DHT_READ_INTERVAL_MS = 5000UL;
constexpr unsigned long DHT_STALE_MS = 3UL * DHT_READ_INTERVAL_MS;
constexpr unsigned long DISPLAY_REFRESH_MS = 1000UL;
constexpr unsigned long DISPLAY_ROTATION_MS = 6000UL;
constexpr unsigned long MANUAL_DISPLAY_HOLD_MS = 12000UL;
constexpr unsigned long DISPLAY_MODE_NOTICE_MS = 1500UL;
constexpr unsigned long DEVELOPER_MODE_TIMEOUT_MS = 2UL * 60UL * 1000UL;
constexpr unsigned long BUTTON_TRIPLE_CLICK_MAX_GAP_MS = 650UL;
constexpr unsigned long BUTTON_TRIPLE_CLICK_WINDOW_MS = 2000UL;
constexpr unsigned long WEATHER_INTERVAL_MS = 45UL * 60UL * 1000UL;
constexpr unsigned long WEATHER_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long CLOUD_LOG_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long PRE_IRRIGATION_MS = 15000UL;
constexpr unsigned long VALVE_PULSE_MS = 20000UL;
constexpr unsigned long SOAK_WAIT_MS = 90000UL;
constexpr unsigned long IRRIGATION_COOLDOWN_MS = 30UL * 60UL * 1000UL;
constexpr unsigned long MAX_TOTAL_VALVE_OPEN_TIME_MS = 70000UL;
constexpr unsigned long WIFI_START_DELAY_MS = 8000UL;
constexpr unsigned long WIFI_CONNECT_GRACE_MS = 12000UL;
constexpr unsigned long WIFI_RECONNECT_MIN_MS = 10000UL;
constexpr unsigned long WIFI_RECONNECT_MAX_MS = 120000UL;
constexpr unsigned long SERIAL_STATUS_INTERVAL_MS = 30000UL;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 40UL;
constexpr unsigned long BOOT_SHORT_PRESS_MAX_MS = 1500UL;
constexpr unsigned long BOOT_LONG_PRESS_MIN_MS = 3000UL;
constexpr unsigned long BOOT_VALVE_ARM_PRESS_MS = 8000UL;
constexpr unsigned long MANUAL_VALVE_TEST_MS = 3000UL;
constexpr unsigned long MANUAL_VALVE_CONFIRM_TIMEOUT_MS = 10000UL;

constexpr float DHT_HUMIDITY_OFFSET_PERCENT = 0.0f;
constexpr float DHT_TEMPERATURE_OFFSET_C = 0.0f;

constexpr uint8_t MAX_PULSES_PER_CYCLE = 3;
constexpr uint8_t SOIL_SAMPLE_COUNT = 11;

constexpr int BUZZ_FREQ_A = 3800;
constexpr int BUZZ_FREQ_B = 3200;
constexpr unsigned long BUZZ_ON_MS = 150UL;
constexpr unsigned long BUZZ_OFF_MS = 180UL;
