#include <Arduino.h>

#if !defined(ESP32)
#error "This firmware targets ESP32-WROOM-32 / ESP32 Dev Module only."
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h. Copy include/secrets.example.h to include/secrets.h and fill local credentials. Never commit secrets.h."
#endif

#include "config.h"

#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <esp_arduino_version.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <stdlib.h>
#include <time.h>

enum class SystemState : uint8_t {
  BOOT,
  IDLE,
  PRE_IRRIGATION,
  IRRIGATING,
  SOAK_WAIT,
  COOLDOWN,
  SENSOR_FAULT
};

enum class Status : uint8_t {
  UNKNOWN,
  OK,
  OFFLINE,
  FAULT,
  UNCALIBRATED,
  INACTIVE
};

enum class WeatherStatus : uint8_t {
  NOT_CHECKED,
  OK,
  WIFI_OFFLINE,
  API_FAILURE
};

enum class Decision : uint8_t {
  NONE,
  WAITING,
  BLOCKED_DISABLED,
  BLOCKED_SOIL_UNCALIBRATED,
  BLOCKED_SOIL_INVALID,
  BLOCKED_TIME,
  BLOCKED_WEATHER,
  BLOCKED_COOLDOWN,
  START_PRE_IRRIGATION,
  IRRIGATING,
  SOAKING,
  COMPLETE
};

enum class DisplayMode : uint8_t {
  PUBLIC,
  DEVELOPER
};

enum class PublicDisplayPage : uint8_t {
  SYSTEM,
  SOIL,
  LOCAL_ENVIRONMENT,
  WEATHER,
  COUNT
};

enum class DeveloperDisplayPage : uint8_t {
  SYSTEM,
  SOIL_RAW,
  WIFI,
  WIFI_SSID,
  DHT,
  WEATHER,
  WEATHER_GRID,
  CLOUD,
  DEVICE,
  RESET_UPTIME,
  COUNT
};

enum class CalibrationUiState : uint8_t {
  INACTIVE,
  DRY_READY,
  DRY_MEASURING,
  WET_READY,
  WET_MEASURING,
  REVIEW,
  SAVING,
  COMPLETE,
  INVALID,
  CANCELLED
};

enum class ManualValveTestState : uint8_t {
  INACTIVE,
  ARMED,
  ACTIVE
};

struct WifiProfile {
  const char* ssid;
  const char* password;
};

struct SensorData {
  int soilRawInstant = 0;
  int soilRawFiltered = 0;
  int soilRawSlow = 0;
  int soilPercent = INVALID_SOIL_PERCENT;
  float localTemperatureC = NAN;
  float localRelativeHumidity = NAN;
};

struct WeatherData {
  int maxRainProbabilityNext6h = 0;
  float totalRainMmNext6h = 0.0f;
  float remoteTemperature2m = NAN;
  float remoteRelativeHumidity2m = NAN;
};

struct WeatherResponseLocation {
  float latitude = NAN;
  float longitude = NAN;
  float elevation = NAN;
  long utcOffsetSeconds = 0;
  char timezone[32] = "";
};

struct FutureLogRecord {
  char timestamp[24];
  uint64_t uptimeSeconds;
  float localTemperature;
  float localAirHumidity;
  Status dhtStatus;
  float remoteTemperature;
  float remoteAirHumidity;
  int soilRaw;
  int soilRawFiltered;
  int soilRawSlow;
  int soilPercent;
  Status soilSensorStatus;
  uint32_t dhtFailureCount;
  int rainProbability6h;
  float rainMm6h;
  WeatherStatus weatherStatus;
  Status wifiStatus;
  int wifiRSSI;
  const char* connectedSsid;
  SystemState systemState;
  bool valveOpen;
  uint8_t irrigationPulse;
  Decision decision;
  const char* firmwareVersion;
  const char* resetReason;
};

struct CalibrationMeasurement {
  int samples[31] = {};
  uint8_t count = 0;
  int minRaw = 4095;
  int maxRaw = 0;
  int lastRaw = 0;
  unsigned long lastSampleMs = 0;
};

LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, LCD_COLUMNS, LCD_ROWS);
DHT dht(DHT_PIN, DHT_TYPE);

constexpr char SOIL_CAL_NAMESPACE[] = "soilcal";
constexpr char SOIL_CAL_KEY_DRY[] = "dry";
constexpr char SOIL_CAL_KEY_WET[] = "wet";
constexpr char SOIL_CAL_KEY_VALID[] = "valid";
constexpr char SOIL_CAL_KEY_VERSION[] = "version";
constexpr uint8_t SOIL_CAL_SUPPORTED_VERSION = 1;
constexpr uint8_t SOIL_CAL_MEASUREMENT_COUNT = 31;
constexpr unsigned long SOIL_CAL_SAMPLE_INTERVAL_MS = 250UL;
constexpr unsigned long SOIL_CAL_RESULT_HOLD_MS = 5000UL;

constexpr char GOOGLE_GTS_ROOT_R1_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92w
O1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70pa
DPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrNV
jzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQIDA
QABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBADiWCu49
tJYeX++dnAsznyvgyv3SjgofQXSlfKqE1OXyHuY3UjKcC9FhHb8owbZEKTV1d5iyf
Nm9dKyKaOOpMQkpAWBz40d8U6iQSifvS9efk+eCNs6aaAyC58/UEBZvXw6ZXPYfc
X3v73svfuo21pdwCxXu11xWajOl40k4DLh9+42FpLFZXvRq4d2h9mREruZRgyFmx
hE+885H7pwoHyXa/6xmld01D1zvICxi/ZG6qcz8WpyTgYMpl0p8WnK0OdC3d8t5/
vJdXrsKNH9AQ6c1iFZgN10Hn3U8pLnW25fZ9fD3iZ7KqGracx3uZ4WcFvj06LCOO
gGfW3J0N9UreCz9sUv9NHW4zTQwF1scP3TgQnA8Tx0YSeLNb56dLyr9FgAXY36I2
3ceZNa2P/N7Jti4JS6vUOJT4FjiwNq4qN0B+M6rL5n0vab7T/EzW9ghB6zI4V4H4
4q9h5gY2VZHuC2u2JO2JvBDq5ZPHlH5YbFAy5LYtK7G3CZd4t8V8zZ9OEzZIw0c5
lmtN5/G9FJ31G2+xFb0NfGgxmjcv0R1BUNYDnELV5dY9y1G6KZ8JbQmC9l5s5z4N
-----END CERTIFICATE-----
)EOF";

WifiProfile wifiProfiles[] = {
  {WIFI_SSID_1, WIFI_PASSWORD_1},
  {WIFI_SSID_2, WIFI_PASSWORD_2},
};

SensorData sensors;
WeatherData weather;
WeatherResponseLocation weatherGrid;

SystemState systemState = SystemState::BOOT;
Status wifiStatus = Status::UNKNOWN;
WeatherStatus weatherStatus = WeatherStatus::NOT_CHECKED;
Status cloudStatus = ENABLE_CLOUD_LOGGING ? Status::UNKNOWN : Status::INACTIVE;
Status soilSensorStatus = Status::UNCALIBRATED;
Status dhtStatus = Status::UNKNOWN;
Decision lastDecision = Decision::NONE;
DisplayMode displayMode = DisplayMode::PUBLIC;
PublicDisplayPage currentPublicDisplayPage = PublicDisplayPage::SYSTEM;
DeveloperDisplayPage currentDeveloperDisplayPage = DeveloperDisplayPage::SYSTEM;
ManualValveTestState manualValveTestState = ManualValveTestState::INACTIVE;

bool soilCalibrationLoaded = false;
bool soilCalibrationValid = false;
int soilRawDry = 0;
int soilRawWet = 0;
uint8_t soilCalibrationVersion = 0;

char resetReasonText[24] = "UNKNOWN";
char lcdLineCache[2][17] = {"", ""};
char connectedSsid[33] = "";

unsigned long bootStartedMs = 0;
unsigned long lastSoilReadMs = 0;
unsigned long lastDhtReadMs = 0;
unsigned long lastDhtSuccessMs = 0;
unsigned long lastDisplayRefreshMs = 0;
unsigned long lastDisplayRotationMs = 0;
unsigned long manualDisplayUntilMs = 0;
unsigned long displayModeNoticeUntilMs = 0;
unsigned long lastDeveloperInteractionMs = 0;
unsigned long lastWeatherAttemptMs = 0;
unsigned long lastCloudLogMs = 0;
unsigned long nextCloudLogMs = 0;
unsigned long lastSerialStatusMs = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long wifiAttemptStartedMs = 0;
unsigned long wifiReconnectIntervalMs = WIFI_RECONNECT_MIN_MS;
unsigned long stateStartedMs = 0;
unsigned long lastIrrigationCycleEndedMs = 0;
unsigned long valveOpenedAtMs = 0;
unsigned long totalValveOpenThisCycleMs = 0;
unsigned long buzzerToggleMs = 0;
unsigned long lastButtonTransitionMs = 0;

uint8_t wifiProfileIndex = 0;
uint8_t currentPulse = 0;
bool valveOpen = false;
bool buzzerOn = false;
bool buzzerAlt = false;
bool timeConfigured = false;
bool wifiAttemptInProgress = false;
bool buttonStableLevel = HIGH;
bool buttonLastReading = HIGH;
bool buttonPressedArmed = false;
unsigned long buttonPressedAtMs = 0;
uint8_t buttonShortPressCount = 0;
unsigned long firstButtonShortPressMs = 0;
unsigned long lastButtonShortPressMs = 0;
unsigned long manualValveTestStateStartedMs = 0;
uint32_t dhtFailureCount = 0;

bool buzzerToneStarted = false;

CalibrationUiState calibrationUiState = CalibrationUiState::INACTIVE;
CalibrationMeasurement calibrationMeasurement;
int candidateDry = 0;
int candidateWet = 0;
unsigned long calibrationStateStartedMs = 0;
bool calibrationSaveFailed = false;

const char* statusName(Status status) {
  switch (status) {
    case Status::OK: return "OK";
    case Status::OFFLINE: return "OFFLINE";
    case Status::FAULT: return "FAULT";
    case Status::UNCALIBRATED: return "UNCALIBRATED";
    case Status::INACTIVE: return "INACTIVE";
    default: return "UNKNOWN";
  }
}

const char* weatherStatusName(WeatherStatus status) {
  switch (status) {
    case WeatherStatus::OK: return "OK";
    case WeatherStatus::WIFI_OFFLINE: return "WIFI_OFFLINE";
    case WeatherStatus::API_FAILURE: return "API_FAILURE";
    default: return "NOT_CHECKED";
  }
}

const char* stateName(SystemState state) {
  switch (state) {
    case SystemState::BOOT: return "BOOT";
    case SystemState::IDLE: return "IDLE";
    case SystemState::PRE_IRRIGATION: return "PRE_IRRIGATION";
    case SystemState::IRRIGATING: return "IRRIGATING";
    case SystemState::SOAK_WAIT: return "SOAK_WAIT";
    case SystemState::COOLDOWN: return "COOLDOWN";
    case SystemState::SENSOR_FAULT: return "SENSOR_FAULT";
    default: return "UNKNOWN";
  }
}

const char* displayModeName(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::PUBLIC: return "PUBLIC";
    case DisplayMode::DEVELOPER: return "DEVELOPER";
    default: return "UNKNOWN";
  }
}

const char* publicDisplayPageName(PublicDisplayPage page) {
  switch (page) {
    case PublicDisplayPage::SYSTEM: return "SYSTEM";
    case PublicDisplayPage::SOIL: return "SOIL";
    case PublicDisplayPage::LOCAL_ENVIRONMENT: return "LOCAL_ENVIRONMENT";
    case PublicDisplayPage::WEATHER: return "WEATHER";
    default: return "UNKNOWN";
  }
}

const char* developerDisplayPageName(DeveloperDisplayPage page) {
  switch (page) {
    case DeveloperDisplayPage::SYSTEM: return "SYSTEM";
    case DeveloperDisplayPage::SOIL_RAW: return "SOIL_RAW";
    case DeveloperDisplayPage::WIFI: return "WIFI";
    case DeveloperDisplayPage::WIFI_SSID: return "WIFI_SSID";
    case DeveloperDisplayPage::DHT: return "DHT";
    case DeveloperDisplayPage::WEATHER: return "WEATHER";
    case DeveloperDisplayPage::WEATHER_GRID: return "WEATHER_GRID";
    case DeveloperDisplayPage::CLOUD: return "CLOUD";
    case DeveloperDisplayPage::DEVICE: return "DEVICE";
    case DeveloperDisplayPage::RESET_UPTIME: return "RESET_UPTIME";
    default: return "UNKNOWN";
  }
}

const char* decisionReason(Decision decision) {
  switch (decision) {
    case Decision::BLOCKED_DISABLED: return "AUTO_DISABLED";
    case Decision::BLOCKED_SOIL_UNCALIBRATED: return "SOIL_UNCALIBRATED";
    case Decision::BLOCKED_SOIL_INVALID: return "SOIL_INVALID";
    case Decision::BLOCKED_TIME: return "TIME_WINDOW";
    case Decision::BLOCKED_WEATHER: return "RAIN_EXPECTED";
    case Decision::BLOCKED_COOLDOWN: return "COOLDOWN";
    case Decision::START_PRE_IRRIGATION: return "SOIL_DRY";
    case Decision::IRRIGATING: return "VALVE_OPEN";
    case Decision::SOAKING: return "SOAK_WAIT";
    case Decision::COMPLETE: return "COMPLETE";
    case Decision::WAITING: return "WAITING";
    default: return "NONE";
  }
}

const char* decisionName(Decision decision) {
  switch (decision) {
    case Decision::WAITING: return "WAITING";
    case Decision::BLOCKED_DISABLED: return "BLOCKED_DISABLED";
    case Decision::BLOCKED_SOIL_UNCALIBRATED: return "BLOCKED_SOIL_UNCALIBRATED";
    case Decision::BLOCKED_SOIL_INVALID: return "BLOCKED_SOIL_INVALID";
    case Decision::BLOCKED_TIME: return "BLOCKED_TIME";
    case Decision::BLOCKED_WEATHER: return "BLOCKED_WEATHER";
    case Decision::BLOCKED_COOLDOWN: return "BLOCKED_COOLDOWN";
    case Decision::START_PRE_IRRIGATION: return "START_PRE_IRRIGATION";
    case Decision::IRRIGATING: return "IRRIGATING";
    case Decision::SOAKING: return "SOAKING";
    case Decision::COMPLETE: return "COMPLETE";
    default: return "NONE";
  }
}

bool isConfiguredText(const char* value, const char* placeholder) {
  return value != nullptr && value[0] != '\0' && strcmp(value, placeholder) != 0;
}

bool isCloudEndpointConfigured() {
  return isConfiguredText(GOOGLE_SCRIPT_URL, "YOUR_GOOGLE_APPS_SCRIPT_URL");
}

bool isCloudEndpointHttps() {
  return strncmp(GOOGLE_SCRIPT_URL, "https://", 8) == 0;
}

bool isCloudTokenConfigured() {
  return isConfiguredText(DEVICE_TOKEN, "YOUR_DEVICE_TOKEN");
}

bool isCloudConfigured() {
  return isCloudEndpointConfigured() && isCloudEndpointHttps() && isCloudTokenConfigured();
}

bool isWifiProfileConfigured(const WifiProfile& profile) {
  return isConfiguredText(profile.ssid, "YOUR_WIFI_SSID") &&
         isConfiguredText(profile.ssid, "OPTIONAL_SECOND_WIFI") &&
         profile.password != nullptr &&
         strcmp(profile.password, "YOUR_WIFI_PASSWORD") != 0 &&
         strcmp(profile.password, "OPTIONAL_SECOND_PASSWORD") != 0;
}

uint8_t configuredWifiProfileCount() {
  uint8_t count = 0;
  for (const WifiProfile& profile : wifiProfiles) {
    if (isWifiProfileConfigured(profile)) count++;
  }
  return count;
}

void clearLineCache();

bool selectNextConfiguredWifiProfile() {
  constexpr uint8_t profileCount = sizeof(wifiProfiles) / sizeof(wifiProfiles[0]);
  for (uint8_t offset = 0; offset < profileCount; offset++) {
    uint8_t candidate = (wifiProfileIndex + offset) % profileCount;
    if (isWifiProfileConfigured(wifiProfiles[candidate])) {
      wifiProfileIndex = candidate;
      return true;
    }
  }
  return false;
}

void startBuzzerTone(int frequency) {
  tone(BUZZER_PIN, frequency);
  buzzerToneStarted = true;
  buzzerOn = true;
}

void stopBuzzer() {
  if (buzzerToneStarted) {
    noTone(BUZZER_PIN);
    buzzerToneStarted = false;
  }
  digitalWrite(BUZZER_PIN, LOW);
  buzzerOn = false;
}

void closeValve(const char* reason) {
  if (valveOpen) {
    totalValveOpenThisCycleMs += millis() - valveOpenedAtMs;
  }
  digitalWrite(RELAY_PIN, VALVE_CLOSED_LEVEL);
  valveOpen = false;
  stopBuzzer();
  Serial.printf("[VALVE] CLOSED reason=%s\n", reason);
}

void openValve() {
  if (!ENABLE_AUTOMATIC_IRRIGATION || soilSensorStatus != Status::OK) {
    closeValve("FAILSAFE");
    return;
  }

  digitalWrite(RELAY_PIN, VALVE_OPEN_LEVEL);
  valveOpen = true;
  valveOpenedAtMs = millis();
  stopBuzzer();
  Serial.println("[VALVE] OPEN");
}

void openValveForManualTest(unsigned long now) {
  digitalWrite(RELAY_PIN, VALVE_OPEN_LEVEL);
  valveOpen = true;
  valveOpenedAtMs = now;
  stopBuzzer();
  Serial.println("[VALVE_TEST] OPEN");
}

void finishManualValveTest(const char* reason) {
  closeValve(reason);
  manualValveTestState = ManualValveTestState::INACTIVE;
  manualValveTestStateStartedMs = 0;
  manualDisplayUntilMs = millis() + MANUAL_DISPLAY_HOLD_MS;
  clearLineCache();
  Serial.printf("[VALVE_TEST] CLOSED reason=%s\n", reason);
}

void armManualValveTest(unsigned long now) {
  closeValve("VALVE_TEST_ARM");
  manualValveTestState = ManualValveTestState::ARMED;
  manualValveTestStateStartedMs = now;
  manualDisplayUntilMs = now + MANUAL_VALVE_CONFIRM_TIMEOUT_MS;
  clearLineCache();
  Serial.println("[VALVE_TEST] ARMED");
}

void confirmManualValveTest(unsigned long now) {
  manualValveTestState = ManualValveTestState::ACTIVE;
  manualValveTestStateStartedMs = now;
  manualDisplayUntilMs = now + MANUAL_VALVE_TEST_MS;
  clearLineCache();
  Serial.println("[VALVE_TEST] CONFIRMED");
  openValveForManualTest(now);
}

void cancelManualValveTest(const char* reason) {
  closeValve(reason);
  manualValveTestState = ManualValveTestState::INACTIVE;
  manualValveTestStateStartedMs = 0;
  manualDisplayUntilMs = millis() + MANUAL_DISPLAY_HOLD_MS;
  clearLineCache();
  Serial.println("[VALVE_TEST] CANCELLED");
}

void setState(SystemState nextState) {
  if (systemState == nextState) return;
  systemState = nextState;
  stateStartedMs = millis();
  Serial.printf("[STATE] %s\n", stateName(systemState));
}

void clearLineCache() {
  lcdLineCache[0][0] = '\0';
  lcdLineCache[1][0] = '\0';
}

void printLcdLine(uint8_t row, const char* text) {
  char padded[17];
  snprintf(padded, sizeof(padded), "%-16.16s", text);
  if (strncmp(lcdLineCache[row], padded, 16) == 0) return;

  lcd.setCursor(0, row);
  lcd.print(padded);
  memcpy(lcdLineCache[row], padded, sizeof(padded));
}

void getResetReasonText() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON: strncpy(resetReasonText, "POWERON", sizeof(resetReasonText)); break;
    case ESP_RST_EXT: strncpy(resetReasonText, "EXTERNAL", sizeof(resetReasonText)); break;
    case ESP_RST_SW: strncpy(resetReasonText, "SOFTWARE", sizeof(resetReasonText)); break;
    case ESP_RST_PANIC: strncpy(resetReasonText, "PANIC", sizeof(resetReasonText)); break;
    case ESP_RST_INT_WDT: strncpy(resetReasonText, "INT_WDT", sizeof(resetReasonText)); break;
    case ESP_RST_TASK_WDT: strncpy(resetReasonText, "TASK_WDT", sizeof(resetReasonText)); break;
    case ESP_RST_WDT: strncpy(resetReasonText, "OTHER_WDT", sizeof(resetReasonText)); break;
    case ESP_RST_DEEPSLEEP: strncpy(resetReasonText, "DEEPSLEEP", sizeof(resetReasonText)); break;
    case ESP_RST_BROWNOUT: strncpy(resetReasonText, "BROWNOUT", sizeof(resetReasonText)); break;
    default: strncpy(resetReasonText, "UNKNOWN", sizeof(resetReasonText)); break;
  }
  resetReasonText[sizeof(resetReasonText) - 1] = '\0';
}

void initWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WATCHDOG_TIMEOUT_MS,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtConfig);
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000UL, true);
#endif
  esp_task_wdt_add(nullptr);
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

bool readLocalTime(struct tm* timeInfo) {
  return getLocalTime(timeInfo, 5);
}

bool isAdcCalibrationEndpointValid(int raw) {
  return raw > 0 && raw < 4095;
}

const char* validateStoredSoilCalibration(bool storedValid, int dry, int wet, uint8_t version) {
  if (!storedValid) return "NVS_VALID_FALSE";
  if (version != SOIL_CAL_SUPPORTED_VERSION) return "UNSUPPORTED_VERSION";
  if (dry == wet) return "ZERO_SPAN";
  if (dry < 0 || dry > 4095 || wet < 0 || wet > 4095) return "ADC_RANGE";
  if (!isAdcCalibrationEndpointValid(dry) || !isAdcCalibrationEndpointValid(wet)) return "ADC_RAIL_VALUE";
  if (abs(dry - wet) < MIN_CALIBRATION_SPAN) return "INSUFFICIENT_SPAN";
  return nullptr;
}

void setSoilCalibrationInvalid() {
  soilCalibrationValid = false;
  soilSensorStatus = Status::UNCALIBRATED;
  sensors.soilPercent = INVALID_SOIL_PERCENT;
}

void loadSoilCalibration() {
  Preferences preferences;
  if (!preferences.begin(SOIL_CAL_NAMESPACE, true)) {
    setSoilCalibrationInvalid();
    soilCalibrationLoaded = false;
    Serial.println("[SOILCAL] STATUS=MISSING");
    return;
  }

  bool hasValid = preferences.isKey(SOIL_CAL_KEY_VALID);
  bool hasDry = preferences.isKey(SOIL_CAL_KEY_DRY);
  bool hasWet = preferences.isKey(SOIL_CAL_KEY_WET);
  bool hasVersion = preferences.isKey(SOIL_CAL_KEY_VERSION);

  bool storedValid = preferences.getBool(SOIL_CAL_KEY_VALID, false);
  int storedDry = preferences.getInt(SOIL_CAL_KEY_DRY, 0);
  int storedWet = preferences.getInt(SOIL_CAL_KEY_WET, 0);
  uint8_t storedVersion = preferences.getUChar(SOIL_CAL_KEY_VERSION, 0);
  preferences.end();

  soilRawDry = storedDry;
  soilRawWet = storedWet;
  soilCalibrationVersion = storedVersion;

  if (!hasValid || !hasDry || !hasWet || !hasVersion) {
    setSoilCalibrationInvalid();
    soilCalibrationLoaded = false;
    Serial.println("[SOILCAL] STATUS=MISSING");
    return;
  }

  soilCalibrationLoaded = true;
  int span = abs(soilRawDry - soilRawWet);
  Serial.printf("[SOILCAL] NVS loaded version=%u dry=%d wet=%d span=%d\n",
                soilCalibrationVersion,
                soilRawDry,
                soilRawWet,
                span);

  const char* invalidReason = validateStoredSoilCalibration(storedValid,
                                                            soilRawDry,
                                                            soilRawWet,
                                                            soilCalibrationVersion);
  if (invalidReason != nullptr) {
    setSoilCalibrationInvalid();
    Serial.printf("[SOILCAL] STATUS=INVALID reason=%s\n", invalidReason);
    return;
  }

  soilCalibrationValid = true;
  soilSensorStatus = Status::UNKNOWN;
  Serial.println("[SOILCAL] STATUS=VALID");
}

bool calibrationSpanValid() {
  return soilCalibrationValid && abs(soilRawDry - soilRawWet) >= MIN_CALIBRATION_SPAN;
}

int soilRawToPercent(int raw) {
  if (!calibrationSpanValid()) return INVALID_SOIL_PERCENT;
  long denominator = static_cast<long>(soilRawWet) - static_cast<long>(soilRawDry);
  if (denominator == 0) return INVALID_SOIL_PERCENT;
  long numerator = (static_cast<long>(raw) - static_cast<long>(soilRawDry)) * 100L;
  return constrain(static_cast<int>(numerator / denominator), 0, 100);
}

void sortSamples(int* values, uint8_t count) {
  for (uint8_t i = 1; i < count; i++) {
    int key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
}

int readSoilFiltered() {
  int samples[SOIL_SAMPLE_COUNT];
  for (uint8_t i = 0; i < SOIL_SAMPLE_COUNT; i++) {
    samples[i] = analogRead(SOIL_MOISTURE_PIN);
    yield();
  }

  sensors.soilRawInstant = samples[SOIL_SAMPLE_COUNT - 1];
  sortSamples(samples, SOIL_SAMPLE_COUNT);

  long sum = 0;
  for (uint8_t i = 2; i < SOIL_SAMPLE_COUNT - 2; i++) {
    sum += samples[i];
  }
  return static_cast<int>(sum / (SOIL_SAMPLE_COUNT - 4));
}

void handleSoil(unsigned long now) {
  if (now - lastSoilReadMs < SOIL_INTERVAL_MS) return;
  lastSoilReadMs = now;

  sensors.soilRawFiltered = readSoilFiltered();
  if (sensors.soilRawSlow == 0) {
    sensors.soilRawSlow = sensors.soilRawFiltered;
  } else {
    sensors.soilRawSlow = (sensors.soilRawSlow * 9 + sensors.soilRawFiltered) / 10;
  }

  if (!soilCalibrationValid) {
    sensors.soilPercent = INVALID_SOIL_PERCENT;
    soilSensorStatus = Status::UNCALIBRATED;
  } else if (!calibrationSpanValid()) {
    sensors.soilPercent = INVALID_SOIL_PERCENT;
    soilSensorStatus = Status::FAULT;
  } else {
    sensors.soilPercent = soilRawToPercent(sensors.soilRawFiltered);
    soilSensorStatus = Status::OK;
  }

  Serial.printf("[SOIL] RAW=%d FILTERED=%d SLOW=%d CALIBRATED=%s\n",
                sensors.soilRawInstant,
                sensors.soilRawFiltered,
                sensors.soilRawSlow,
                soilCalibrationValid ? "YES" : "NO");
}

bool dhtValueValid(float temperature, float humidity) {
  if (!isfinite(temperature) || !isfinite(humidity)) return false;
  if (humidity < 0.0f || humidity > 100.0f) return false;
  return temperature > -20.0f && temperature < 80.0f;
}

void handleDht(unsigned long now) {
  if (now - lastDhtReadMs < DHT_READ_INTERVAL_MS) {
    if (lastDhtSuccessMs != 0 && dhtStatus == Status::OK &&
        now - lastDhtSuccessMs >= DHT_STALE_MS) {
      dhtStatus = Status::OFFLINE;
    }
    return;
  }
  lastDhtReadMs = now;

  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  if (dhtValueValid(temperature, humidity)) {
    sensors.localTemperatureC = temperature + DHT_TEMPERATURE_OFFSET_C;
    sensors.localRelativeHumidity = constrain(humidity + DHT_HUMIDITY_OFFSET_PERCENT, 0.0f, 100.0f);
    lastDhtSuccessMs = now;
    dhtStatus = Status::OK;
    Serial.printf("[DHT] LOCAL T=%.1fC RH=%.1f%% status=OK\n",
                  sensors.localTemperatureC,
                  sensors.localRelativeHumidity);
    return;
  }

  if (lastDhtSuccessMs != 0 && now - lastDhtSuccessMs >= DHT_STALE_MS) {
    dhtStatus = Status::OFFLINE;
  } else {
    dhtStatus = Status::FAULT;
  }
  dhtFailureCount++;
  Serial.printf("[DHT] status=%s\n", statusName(dhtStatus));
}

void handleSensors(unsigned long now) {
  handleSoil(now);
  handleDht(now);
}

bool calibrationModeActive() {
  return calibrationUiState != CalibrationUiState::INACTIVE;
}

void setCalibrationUiState(CalibrationUiState nextState, unsigned long now) {
  calibrationUiState = nextState;
  calibrationStateStartedMs = now;
  clearLineCache();
}

void resetCalibrationMeasurement(unsigned long now) {
  calibrationMeasurement = CalibrationMeasurement();
  calibrationMeasurement.lastSampleMs = now - SOIL_CAL_SAMPLE_INTERVAL_MS;
}

int medianOfCalibrationSamples() {
  int sorted[SOIL_CAL_MEASUREMENT_COUNT];
  for (uint8_t i = 0; i < SOIL_CAL_MEASUREMENT_COUNT; i++) {
    sorted[i] = calibrationMeasurement.samples[i];
  }
  sortSamples(sorted, SOIL_CAL_MEASUREMENT_COUNT);
  return sorted[SOIL_CAL_MEASUREMENT_COUNT / 2];
}

void finishCalibrationMeasurement(bool dryReference, unsigned long now) {
  int result = medianOfCalibrationSamples();
  if (dryReference) {
    candidateDry = result;
    Serial.printf("[SOILCAL] DRY_RESULT raw=%d min=%d max=%d\n",
                  candidateDry,
                  calibrationMeasurement.minRaw,
                  calibrationMeasurement.maxRaw);
    setCalibrationUiState(CalibrationUiState::WET_READY, now);
    return;
  }

  candidateWet = result;
  Serial.printf("[SOILCAL] WET_RESULT raw=%d min=%d max=%d\n",
                candidateWet,
                calibrationMeasurement.minRaw,
                calibrationMeasurement.maxRaw);

  int span = abs(candidateDry - candidateWet);
  const char* invalidReason = validateStoredSoilCalibration(true,
                                                            candidateDry,
                                                            candidateWet,
                                                            SOIL_CAL_SUPPORTED_VERSION);
  if (invalidReason == nullptr) {
    Serial.printf("[SOILCAL] CANDIDATE span=%d status=VALID\n", span);
  } else {
    Serial.printf("[SOILCAL] CANDIDATE span=%d status=INVALID reason=%s\n",
                  span,
                  invalidReason);
  }

  if (invalidReason == nullptr) {
    setCalibrationUiState(CalibrationUiState::REVIEW, now);
  } else {
    calibrationSaveFailed = false;
    setCalibrationUiState(CalibrationUiState::INVALID, now);
  }
}

void startCalibrationMeasurement(bool dryReference, unsigned long now) {
  resetCalibrationMeasurement(now);
  if (dryReference) {
    Serial.println("[SOILCAL] DRY_START");
    setCalibrationUiState(CalibrationUiState::DRY_MEASURING, now);
  } else {
    Serial.println("[SOILCAL] WET_START");
    setCalibrationUiState(CalibrationUiState::WET_MEASURING, now);
  }
}

void handleCalibrationMeasurement(unsigned long now) {
  bool dryReference = calibrationUiState == CalibrationUiState::DRY_MEASURING;
  bool wetReference = calibrationUiState == CalibrationUiState::WET_MEASURING;
  if (!dryReference && !wetReference) return;
  if (now - calibrationMeasurement.lastSampleMs < SOIL_CAL_SAMPLE_INTERVAL_MS) return;
  calibrationMeasurement.lastSampleMs = now;

  int raw = readSoilFiltered();
  if (calibrationMeasurement.count < SOIL_CAL_MEASUREMENT_COUNT) {
    calibrationMeasurement.samples[calibrationMeasurement.count++] = raw;
    calibrationMeasurement.lastRaw = raw;
    if (raw < calibrationMeasurement.minRaw) calibrationMeasurement.minRaw = raw;
    if (raw > calibrationMeasurement.maxRaw) calibrationMeasurement.maxRaw = raw;
  }

  if (calibrationMeasurement.count >= SOIL_CAL_MEASUREMENT_COUNT) {
    finishCalibrationMeasurement(dryReference, now);
  }
}

bool writeSoilCalibrationToNvs(int dry, int wet) {
  Preferences preferences;
  if (!preferences.begin(SOIL_CAL_NAMESPACE, false)) return false;

  bool ok = true;
  ok = preferences.putBool(SOIL_CAL_KEY_VALID, false) > 0 && ok;
  ok = preferences.putInt(SOIL_CAL_KEY_DRY, dry) > 0 && ok;
  ok = preferences.putInt(SOIL_CAL_KEY_WET, wet) > 0 && ok;
  ok = preferences.putUChar(SOIL_CAL_KEY_VERSION, SOIL_CAL_SUPPORTED_VERSION) > 0 && ok;
  if (ok) {
    ok = preferences.putBool(SOIL_CAL_KEY_VALID, true) > 0;
  }
  preferences.end();
  return ok;
}

bool saveCalibrationCandidate() {
  int previousDry = soilRawDry;
  int previousWet = soilRawWet;
  bool previousValid = soilCalibrationValid;

  if (!writeSoilCalibrationToNvs(candidateDry, candidateWet)) {
    if (previousValid) {
      writeSoilCalibrationToNvs(previousDry, previousWet);
      loadSoilCalibration();
    }
    return false;
  }

  loadSoilCalibration();
  return soilCalibrationValid &&
         soilRawDry == candidateDry &&
         soilRawWet == candidateWet &&
         soilCalibrationVersion == SOIL_CAL_SUPPORTED_VERSION;
}

void enterSoilCalibrationMode(unsigned long now) {
  closeValve("SOIL_CAL");
  stopBuzzer();
  candidateDry = 0;
  candidateWet = 0;
  calibrationSaveFailed = false;
  Serial.println("[SOILCAL] MODE_ENTER");
  setCalibrationUiState(CalibrationUiState::DRY_READY, now);
}

void cancelSoilCalibration(unsigned long now) {
  closeValve("SOIL_CAL_CANCEL");
  stopBuzzer();
  Serial.println("[SOILCAL] CALIBRATION_CANCELLED");
  setCalibrationUiState(CalibrationUiState::CANCELLED, now);
}

void exitSoilCalibrationMode(unsigned long now) {
  calibrationUiState = CalibrationUiState::INACTIVE;
  manualDisplayUntilMs = now + MANUAL_DISPLAY_HOLD_MS;
  lastDisplayRotationMs = now;
  clearLineCache();
}

void handleCalibrationLongPress(unsigned long now) {
  switch (calibrationUiState) {
    case CalibrationUiState::DRY_READY:
    case CalibrationUiState::WET_READY:
      cancelSoilCalibration(now);
      break;

    case CalibrationUiState::REVIEW:
      setCalibrationUiState(CalibrationUiState::SAVING, now);
      calibrationSaveFailed = !saveCalibrationCandidate();
      if (calibrationSaveFailed) {
        Serial.println("[SOILCAL] SAVE_FAILED");
        setCalibrationUiState(CalibrationUiState::INVALID, now);
      } else {
        Serial.println("[SOILCAL] SAVE_OK");
        setCalibrationUiState(CalibrationUiState::COMPLETE, now);
      }
      break;

    case CalibrationUiState::COMPLETE:
    case CalibrationUiState::INVALID:
    case CalibrationUiState::CANCELLED:
      exitSoilCalibrationMode(now);
      break;

    default:
      break;
  }
}

void handleCalibrationShortPress(unsigned long now) {
  switch (calibrationUiState) {
    case CalibrationUiState::DRY_READY:
      startCalibrationMeasurement(true, now);
      break;

    case CalibrationUiState::WET_READY:
      startCalibrationMeasurement(false, now);
      break;

    case CalibrationUiState::COMPLETE:
    case CalibrationUiState::INVALID:
    case CalibrationUiState::CANCELLED:
      exitSoilCalibrationMode(now);
      break;

    default:
      break;
  }
}

void handleCalibrationMode(unsigned long now) {
  if (!calibrationModeActive()) return;
  handleCalibrationMeasurement(now);

  if ((calibrationUiState == CalibrationUiState::COMPLETE ||
       calibrationUiState == CalibrationUiState::INVALID ||
       calibrationUiState == CalibrationUiState::CANCELLED) &&
      now - calibrationStateStartedMs >= SOIL_CAL_RESULT_HOLD_MS) {
    exitSoilCalibrationMode(now);
  }
}

void beginWifiAttempt(unsigned long now) {
  if (!selectNextConfiguredWifiProfile()) {
    wifiStatus = Status::OFFLINE;
    return;
  }

  const WifiProfile& profile = wifiProfiles[wifiProfileIndex];
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(profile.ssid, profile.password);
  wifiAttemptStartedMs = now;
  wifiAttemptInProgress = true;
  Serial.printf("[WIFI] TRY ssid=%s\n", profile.ssid);
}

void advanceWifiProfile() {
  constexpr uint8_t profileCount = sizeof(wifiProfiles) / sizeof(wifiProfiles[0]);
  wifiProfileIndex = (wifiProfileIndex + 1) % profileCount;
}

void handleWiFi(unsigned long now) {
  if (now - bootStartedMs < WIFI_START_DELAY_MS) return;

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    if (wifiStatus != Status::OK) {
      wifiStatus = Status::OK;
      wifiAttemptInProgress = false;
      wifiReconnectIntervalMs = WIFI_RECONNECT_MIN_MS;
      snprintf(connectedSsid, sizeof(connectedSsid), "%s", WiFi.SSID().c_str());
      Serial.printf("[WIFI] CONNECTED ssid=%s rssi=%d\n", connectedSsid, WiFi.RSSI());
    }
    return;
  }

  if (wifiStatus == Status::OK || wifiStatus == Status::UNKNOWN) {
    wifiStatus = Status::OFFLINE;
    connectedSsid[0] = '\0';
    Serial.println("[WIFI] OFFLINE");
  }

  if (configuredWifiProfileCount() == 0) return;

  if (wifiAttemptInProgress) {
    if (now - wifiAttemptStartedMs < WIFI_CONNECT_GRACE_MS) return;

    Serial.printf("[WIFI] TIMEOUT ssid=%s\n", wifiProfiles[wifiProfileIndex].ssid);
    WiFi.disconnect(false, false);
    wifiAttemptInProgress = false;
    advanceWifiProfile();
    lastWifiAttemptMs = now;
    if (wifiReconnectIntervalMs < WIFI_RECONNECT_MAX_MS) {
      wifiReconnectIntervalMs = min(wifiReconnectIntervalMs * 2UL, WIFI_RECONNECT_MAX_MS);
    }
    return;
  }

  if (now - lastWifiAttemptMs < wifiReconnectIntervalMs) return;
  lastWifiAttemptMs = now;
  beginWifiAttempt(now);
}

void handleTime() {
  if (wifiStatus != Status::OK || timeConfigured) return;
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  timeConfigured = true;
  Serial.println("[TIME] NTP_CONFIGURED");
}

bool getLocalHour(int* hourOut) {
  struct tm timeInfo;
  if (!readLocalTime(&timeInfo)) return false;
  *hourOut = timeInfo.tm_hour;
  return true;
}

void timestampNow(char* buffer, size_t size) {
  struct tm timeInfo;
  if (readLocalTime(&timeInfo)) {
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &timeInfo);
  } else {
    snprintf(buffer, size, "uptime-%lu", millis() / 1000UL);
  }
}

void formatHumanUptime(char* buffer, size_t size) {
  uint64_t seconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
  uint64_t minutes = seconds / 60ULL;
  if (minutes < 60ULL) {
    snprintf(buffer, size, "UP: %lluMIN", minutes);
    return;
  }

  uint64_t hours = minutes / 60ULL;
  uint64_t remainingMinutes = minutes % 60ULL;
  if (hours < 24ULL) {
    snprintf(buffer, size, "UP: %02lluH%02lluM", hours, remainingMinutes);
    return;
  }

  uint64_t days = hours / 24ULL;
  uint64_t remainingHours = hours % 24ULL;
  snprintf(buffer, size, "UP: %02lluD%02lluH", days, remainingHours);
}

void formatCloudScheduleLine(char* buffer, size_t size) {
  if (!ENABLE_CLOUD_LOGGING || !isCloudConfigured()) {
    snprintf(buffer, size, "INATIVO");
    return;
  }

  if (cloudStatus == Status::OFFLINE) {
    snprintf(buffer, size, "SEM WIFI");
    return;
  }

  if (cloudStatus == Status::FAULT) {
    unsigned long retrySeconds = 0;
    unsigned long now = millis();
    if (static_cast<long>(nextCloudLogMs - now) > 0) {
      retrySeconds = (nextCloudLogMs - now + 999UL) / 1000UL;
    }
    snprintf(buffer, size, "RETRY:%lus", retrySeconds);
    return;
  }

  snprintf(buffer, size, "ENVIO:5MIN");
}

bool readJsonFloat(JsonVariantConst value, float* out) {
  if (value.isNull() || !value.is<float>()) return false;
  float parsed = value.as<float>();
  if (!isfinite(parsed)) return false;
  *out = parsed;
  return true;
}

bool readJsonLong(JsonVariantConst value, long* out) {
  if (value.isNull() || !value.is<long>()) return false;
  *out = value.as<long>();
  return true;
}

float approximateDistanceKm(float lat1, float lon1, float lat2, float lon2) {
  if (!isfinite(lat1) || !isfinite(lon1) || !isfinite(lat2) || !isfinite(lon2)) return NAN;
  constexpr float earthRadiusKm = 6371.0f;
  float dLat = radians(lat2 - lat1);
  float dLon = radians(lon2 - lon1);
  float rLat1 = radians(lat1);
  float rLat2 = radians(lat2);
  float a = sin(dLat / 2.0f) * sin(dLat / 2.0f) +
            cos(rLat1) * cos(rLat2) * sin(dLon / 2.0f) * sin(dLon / 2.0f);
  float c = 2.0f * atan2(sqrt(a), sqrt(1.0f - a));
  return earthRadiusKm * c;
}

bool parseOpenMeteo(const String& payload) {
  JsonDocument filter;
  filter["latitude"] = true;
  filter["longitude"] = true;
  filter["elevation"] = true;
  filter["timezone"] = true;
  filter["utc_offset_seconds"] = true;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["relative_humidity_2m"] = true;
  filter["hourly"]["precipitation_probability"] = true;
  filter["hourly"]["precipitation"] = true;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (error) {
    Serial.printf("[WEATHER] JSON_ERROR %s\n", error.c_str());
    return false;
  }

  JsonObjectConst current = doc["current"].as<JsonObjectConst>();
  JsonObjectConst hourly = doc["hourly"].as<JsonObjectConst>();
  JsonArrayConst probabilities = hourly["precipitation_probability"].as<JsonArrayConst>();
  JsonArrayConst precipitations = hourly["precipitation"].as<JsonArrayConst>();

  if (current.isNull() || hourly.isNull() ||
      probabilities.isNull() || precipitations.isNull() ||
      probabilities.size() < 6 || precipitations.size() < 6) {
    return false;
  }

  float temperature = NAN;
  float humidity = NAN;
  if (!readJsonFloat(current["temperature_2m"], &temperature) ||
      !readJsonFloat(current["relative_humidity_2m"], &humidity)) {
    return false;
  }
  if (humidity < 0.0f || humidity > 100.0f) return false;

  int maxProbability = 0;
  float totalMm = 0.0f;
  for (uint8_t i = 0; i < 6; i++) {
    float probabilityValue = NAN;
    float precipitationValue = NAN;
    if (!readJsonFloat(probabilities[i], &probabilityValue) ||
        !readJsonFloat(precipitations[i], &precipitationValue)) {
      return false;
    }
    if (probabilityValue < 0.0f || probabilityValue > 100.0f ||
        precipitationValue < 0.0f) {
      return false;
    }

    int probability = constrain(static_cast<int>(probabilityValue + 0.5f), 0, 100);
    if (probability > maxProbability) maxProbability = probability;
    totalMm += precipitationValue;
  }

  weather.maxRainProbabilityNext6h = maxProbability;
  weather.totalRainMmNext6h = totalMm;
  weather.remoteTemperature2m = temperature;
  weather.remoteRelativeHumidity2m = humidity;

  float gridLatitude = NAN;
  float gridLongitude = NAN;
  float gridElevation = NAN;
  long utcOffsetSeconds = 0;
  if (readJsonFloat(doc["latitude"], &gridLatitude)) weatherGrid.latitude = gridLatitude;
  if (readJsonFloat(doc["longitude"], &gridLongitude)) weatherGrid.longitude = gridLongitude;
  if (readJsonFloat(doc["elevation"], &gridElevation)) weatherGrid.elevation = gridElevation;
  if (readJsonLong(doc["utc_offset_seconds"], &utcOffsetSeconds)) {
    weatherGrid.utcOffsetSeconds = utcOffsetSeconds;
  }
  const char* responseTimezone = doc["timezone"] | "";
  if (responseTimezone[0] != '\0') {
    snprintf(weatherGrid.timezone, sizeof(weatherGrid.timezone), "%s", responseTimezone);
  }

  return true;
}

void handleWeather(unsigned long now) {
  if (systemState == SystemState::BOOT) return;

  if (wifiStatus != Status::OK) {
    if (weatherStatus != WeatherStatus::WIFI_OFFLINE) weatherStatus = WeatherStatus::WIFI_OFFLINE;
    return;
  }

  unsigned long interval = weatherStatus == WeatherStatus::OK ? WEATHER_INTERVAL_MS : WEATHER_RETRY_MS;
  if (lastWeatherAttemptMs != 0 && now - lastWeatherAttemptMs < interval) return;
  lastWeatherAttemptMs = now;

  char url[448];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&elevation=%.0f&current=temperature_2m,relative_humidity_2m&hourly=precipitation_probability,precipitation&forecast_hours=6&timezone=%s",
           WEATHER_LATITUDE,
           WEATHER_LONGITUDE,
           WEATHER_ELEVATION_M,
           WEATHER_TIMEZONE);

  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(url)) {
    weatherStatus = WeatherStatus::API_FAILURE;
    http.end();
    Serial.println("[WEATHER] HTTP_BEGIN_FAILED");
    return;
  }

  int httpCode = http.GET();
  bool ok = false;
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    ok = parseOpenMeteo(payload);
  }
  http.end();

  if (ok) {
    weatherStatus = WeatherStatus::OK;
    float gridOffsetKm = approximateDistanceKm(static_cast<float>(atof(WEATHER_LATITUDE)),
                                               static_cast<float>(atof(WEATHER_LONGITUDE)),
                                               weatherGrid.latitude,
                                               weatherGrid.longitude);
    Serial.printf("[WEATHER] requested lat=%s lon=%s elev=%.0f\n",
                  WEATHER_LATITUDE,
                  WEATHER_LONGITUDE,
                  WEATHER_ELEVATION_M);
    Serial.printf("[WEATHER] grid lat=%.6f lon=%.6f elev=%.0f timezone=%s utc_offset=%ld offset=%.2fkm\n",
                  weatherGrid.latitude,
                  weatherGrid.longitude,
                  weatherGrid.elevation,
                  weatherGrid.timezone[0] != '\0' ? weatherGrid.timezone : "UNKNOWN",
                  weatherGrid.utcOffsetSeconds,
                  gridOffsetKm);
    Serial.printf("[WEATHER] REMOTE T=%.1fC RH=%.1f%%\n",
                  weather.remoteTemperature2m,
                  weather.remoteRelativeHumidity2m);
    Serial.printf("[WEATHER] Rain6h=%d%% %.1fmm\n",
                  weather.maxRainProbabilityNext6h,
                  weather.totalRainMmNext6h);
  } else {
    weatherStatus = WeatherStatus::API_FAILURE;
    Serial.printf("[WEATHER] ERROR code=%d\n", httpCode);
  }
}

bool inAllowedIrrigationWindow() {
  int hour = 0;
  if (!getLocalHour(&hour)) return true;
  return hour >= 6 && hour <= 22;
}

bool rainShouldBlockIrrigation() {
  if (weatherStatus != WeatherStatus::OK) return false;
  return weather.maxRainProbabilityNext6h >= RAIN_PROBABILITY_BLOCK_THRESHOLD &&
         weather.totalRainMmNext6h >= RAIN_MM_BLOCK_THRESHOLD;
}

Decision evaluateIrrigationDecision(unsigned long now) {
  if (soilSensorStatus == Status::UNCALIBRATED) return Decision::BLOCKED_SOIL_UNCALIBRATED;
  if (soilSensorStatus != Status::OK || sensors.soilPercent == INVALID_SOIL_PERCENT) {
    return Decision::BLOCKED_SOIL_INVALID;
  }
  if (!ENABLE_AUTOMATIC_IRRIGATION) return Decision::BLOCKED_DISABLED;
  if (!inAllowedIrrigationWindow()) return Decision::BLOCKED_TIME;
  if (rainShouldBlockIrrigation()) return Decision::BLOCKED_WEATHER;
  if (lastIrrigationCycleEndedMs != 0 &&
      now - lastIrrigationCycleEndedMs < IRRIGATION_COOLDOWN_MS) {
    return Decision::BLOCKED_COOLDOWN;
  }
  if (sensors.soilPercent < SOIL_START_IRRIGATION_PERCENT) return Decision::START_PRE_IRRIGATION;
  return Decision::WAITING;
}

void startIrrigationCycle(unsigned long now) {
  currentPulse = 0;
  totalValveOpenThisCycleMs = 0;
  lastDecision = Decision::START_PRE_IRRIGATION;
  stopBuzzer();
  buzzerAlt = false;
  buzzerToggleMs = now;
  setState(SystemState::PRE_IRRIGATION);
}

void startValvePulse() {
  currentPulse++;
  lastDecision = Decision::IRRIGATING;
  openValve();
  setState(SystemState::IRRIGATING);
}

void finishIrrigationCycle(const char* reason) {
  closeValve(reason);
  lastIrrigationCycleEndedMs = millis();
  lastDecision = Decision::COMPLETE;
  currentPulse = 0;
  totalValveOpenThisCycleMs = 0;
  setState(SystemState::COOLDOWN);
}

void handleBuzzer(unsigned long now) {
  if (systemState != SystemState::PRE_IRRIGATION) {
    if (buzzerOn) {
      stopBuzzer();
    }
    return;
  }

  if (buzzerOn) {
    if (now - buzzerToggleMs >= BUZZ_ON_MS) {
      stopBuzzer();
      buzzerToggleMs = now;
    }
  } else if (now - buzzerToggleMs >= BUZZ_OFF_MS) {
    startBuzzerTone(buzzerAlt ? BUZZ_FREQ_A : BUZZ_FREQ_B);
    buzzerAlt = !buzzerAlt;
    buzzerToggleMs = now;
  }
}

void handleIrrigation(unsigned long now) {
  handleBuzzer(now);

  if (valveOpen && now - valveOpenedAtMs >= MAX_TOTAL_VALVE_OPEN_TIME_MS) {
    finishIrrigationCycle("MAX_OPEN_TIME");
    return;
  }

  if (systemState == SystemState::BOOT) {
    if (now - bootStartedMs >= SPLASH_MS) {
      setState(SystemState::IDLE);
      clearLineCache();
    }
    return;
  }

  if (soilSensorStatus == Status::FAULT) {
    closeValve("SOIL_FAULT");
    setState(SystemState::SENSOR_FAULT);
    lastDecision = Decision::BLOCKED_SOIL_INVALID;
    return;
  }

  switch (systemState) {
    case SystemState::IDLE: {
      Decision decision = evaluateIrrigationDecision(now);
      if (decision != lastDecision) {
        lastDecision = decision;
        Serial.printf("[DECISION] %s\n", decisionReason(decision));
      }
      if (decision == Decision::START_PRE_IRRIGATION) startIrrigationCycle(now);
      break;
    }

    case SystemState::PRE_IRRIGATION:
      if (now - stateStartedMs >= PRE_IRRIGATION_MS) {
        stopBuzzer();
        startValvePulse();
      }
      break;

    case SystemState::IRRIGATING:
      if (now - valveOpenedAtMs >= VALVE_PULSE_MS) {
        closeValve("PULSE_DONE");
        lastDecision = Decision::SOAKING;
        setState(SystemState::SOAK_WAIT);
      }
      break;

    case SystemState::SOAK_WAIT:
      if (now - stateStartedMs >= SOAK_WAIT_MS) {
        if (sensors.soilPercent != INVALID_SOIL_PERCENT &&
            sensors.soilPercent >= SOIL_STOP_IRRIGATION_PERCENT) {
          finishIrrigationCycle("TARGET_REACHED");
        } else if (currentPulse >= MAX_PULSES_PER_CYCLE ||
                   totalValveOpenThisCycleMs >= MAX_TOTAL_VALVE_OPEN_TIME_MS) {
          finishIrrigationCycle("PULSE_LIMIT");
        } else {
          setState(SystemState::PRE_IRRIGATION);
        }
      }
      break;

    case SystemState::COOLDOWN:
      if (now - stateStartedMs >= IRRIGATION_COOLDOWN_MS) {
        setState(SystemState::IDLE);
      }
      break;

    case SystemState::SENSOR_FAULT:
      if (soilSensorStatus == Status::OK || soilSensorStatus == Status::UNCALIBRATED) {
        setState(SystemState::IDLE);
      }
      break;

    default:
      closeValve("UNKNOWN_STATE");
      setState(SystemState::IDLE);
      break;
  }
}

void handleManualValveTest(unsigned long now) {
  switch (manualValveTestState) {
    case ManualValveTestState::ARMED:
      if (now - manualValveTestStateStartedMs >= MANUAL_VALVE_CONFIRM_TIMEOUT_MS) {
        finishManualValveTest("CONFIRM_TIMEOUT");
      }
      break;

    case ManualValveTestState::ACTIVE:
      if (now - manualValveTestStateStartedMs >= MANUAL_VALVE_TEST_MS) {
        finishManualValveTest("TIMEOUT");
      }
      break;

    case ManualValveTestState::INACTIVE:
    default:
      break;
  }
}

void formatSystemLine(char* line, size_t size) {
  if (systemState == SystemState::SENSOR_FAULT) {
    snprintf(line, size, "HORTA: SENSOR");
  } else if (soilSensorStatus == Status::UNCALIBRATED) {
    snprintf(line, size, "HORTA: ATENCAO");
  } else {
    snprintf(line, size, "HORTA: NORMAL");
  }
}

void formatShortSsid(char* buffer, size_t size) {
  if (wifiStatus != Status::OK || connectedSsid[0] == '\0') {
    snprintf(buffer, size, "OFFLINE");
    return;
  }
  snprintf(buffer, size, "%.15s", connectedSsid);
}

void displayPublicPage(PublicDisplayPage page, char* line1, size_t line1Size, char* line2, size_t line2Size) {
  switch (page) {
    case PublicDisplayPage::SYSTEM:
      formatSystemLine(line1, line1Size);
      snprintf(line2, line2Size, "VALVULA: %s", valveOpen ? "ON" : "OFF");
      break;

    case PublicDisplayPage::SOIL:
      if (soilSensorStatus == Status::UNCALIBRATED) {
        snprintf(line1, line1Size, "SOLO: CALIBRAR");
        snprintf(line2, line2Size, "RAW:%4d", sensors.soilRawFiltered);
      } else {
        snprintf(line1, line1Size, "UMIDADE SOLO");
        if (sensors.soilPercent == INVALID_SOIL_PERCENT) {
          snprintf(line2, line2Size, "INDISPONIVEL");
        } else {
          snprintf(line2, line2Size, "%3d%%", sensors.soilPercent);
        }
      }
      break;

    case PublicDisplayPage::LOCAL_ENVIRONMENT:
      snprintf(line1, line1Size, "AMBIENTE LOCAL");
      if (dhtStatus == Status::OK &&
          isfinite(sensors.localTemperatureC) &&
          isfinite(sensors.localRelativeHumidity)) {
        char candidate[24];
        snprintf(candidate, sizeof(candidate), "T:%.1fC U:%.0f%%",
                 sensors.localTemperatureC,
                 sensors.localRelativeHumidity);
        if (strlen(candidate) <= 16) {
          snprintf(line2, line2Size, "%s", candidate);
        } else {
          snprintf(line2, line2Size, "T:%.0fC U:%.0f%%",
                   sensors.localTemperatureC,
                   sensors.localRelativeHumidity);
        }
      } else {
        snprintf(line2, line2Size, "INDISPONIVEL");
      }
      break;

    case PublicDisplayPage::WEATHER:
      if (weatherStatus == WeatherStatus::OK) {
        snprintf(line1, line1Size, "PREVISAO 6H");
        char candidate[24];
        snprintf(candidate, sizeof(candidate), "CHUVA:%d%% %.1fMM",
                 weather.maxRainProbabilityNext6h,
                 weather.totalRainMmNext6h);
        if (strlen(candidate) <= 16) {
          snprintf(line2, line2Size, "%s", candidate);
        } else {
          snprintf(line2, line2Size, "CHUVA: %d%%", weather.maxRainProbabilityNext6h);
        }
      } else {
        snprintf(line1, line1Size, "PREVISAO 6H");
        snprintf(line2, line2Size, "INDISPONIVEL");
      }
      break;

    default:
      snprintf(line1, line1Size, "DISPLAY");
      snprintf(line2, line2Size, "PAGINA INVALIDA");
      break;
  }
}

void displayDeveloperPage(DeveloperDisplayPage page, char* line1, size_t line1Size, char* line2, size_t line2Size) {
  switch (page) {
    case DeveloperDisplayPage::SYSTEM:
      snprintf(line1, line1Size, "%s %s", stateName(systemState), valveOpen ? "V:ON" : "V:OFF");
      snprintf(line2, line2Size, "AUTO:%s %s", ENABLE_AUTOMATIC_IRRIGATION ? "ON" : "OFF", decisionReason(lastDecision));
      break;

    case DeveloperDisplayPage::SOIL_RAW:
      snprintf(line1, line1Size, "RAW:%4d F:%4d", sensors.soilRawInstant, sensors.soilRawFiltered);
      snprintf(line2, line2Size, "S:%4d P:%3d%%", sensors.soilRawSlow, sensors.soilPercent);
      break;

    case DeveloperDisplayPage::WIFI:
      snprintf(line1, line1Size, "WIFI: %s", wifiStatus == Status::OK ? "ONLINE" : "OFFLINE");
      if (wifiStatus == Status::OK) {
        snprintf(line2, line2Size, "RSSI:%4d DBM", WiFi.RSSI());
      } else {
        snprintf(line2, line2Size, "RECONECTANDO");
      }
      break;

    case DeveloperDisplayPage::WIFI_SSID: {
      char ssid[17];
      formatShortSsid(ssid, sizeof(ssid));
      snprintf(line1, line1Size, "SSID");
      snprintf(line2, line2Size, "%s", ssid);
      break;
    }

    case DeveloperDisplayPage::DHT:
      snprintf(line1, line1Size, "DHT: %s", statusName(dhtStatus));
      if (isfinite(sensors.localTemperatureC) && isfinite(sensors.localRelativeHumidity)) {
        snprintf(line2, line2Size, "T%.1f RH%.0f%%", sensors.localTemperatureC, sensors.localRelativeHumidity);
      } else {
        snprintf(line2, line2Size, "SEM LEITURA");
      }
      break;

    case DeveloperDisplayPage::WEATHER:
      if (weatherStatus == WeatherStatus::OK) {
        snprintf(line1, line1Size, "REM T%.1f RH%.0f", weather.remoteTemperature2m, weather.remoteRelativeHumidity2m);
        snprintf(line2, line2Size, "RAIN%3d%% %.1fMM", weather.maxRainProbabilityNext6h, weather.totalRainMmNext6h);
      } else if (weatherStatus == WeatherStatus::WIFI_OFFLINE) {
        snprintf(line1, line1Size, "CLIMA: SEM WIFI");
        snprintf(line2, line2Size, "CONTROLE LOCAL");
      } else if (weatherStatus == WeatherStatus::API_FAILURE) {
        snprintf(line1, line1Size, "CLIMA: FALHA");
        snprintf(line2, line2Size, "API OPEN-METEO");
      } else {
        snprintf(line1, line1Size, "CLIMA: AGUARDE");
        snprintf(line2, line2Size, "NAO VERIFICADO");
      }
      break;

    case DeveloperDisplayPage::WEATHER_GRID:
      if (isfinite(weatherGrid.latitude) && isfinite(weatherGrid.longitude)) {
        snprintf(line1, line1Size, "GRID %.4f", weatherGrid.latitude);
        snprintf(line2, line2Size, "LON %.4f", weatherGrid.longitude);
      } else {
        snprintf(line1, line1Size, "GRID");
        snprintf(line2, line2Size, "INDISPONIVEL");
      }
      break;

    case DeveloperDisplayPage::CLOUD:
      snprintf(line1, line1Size, "CLOUD: %s", statusName(cloudStatus));
      formatCloudScheduleLine(line2, line2Size);
      break;

    case DeveloperDisplayPage::DEVICE:
      snprintf(line1, line1Size, "FW %s", FIRMWARE_VERSION);
      formatHumanUptime(line2, line2Size);
      break;

    case DeveloperDisplayPage::RESET_UPTIME:
      snprintf(line1, line1Size, "RESET %s", resetReasonText);
      formatHumanUptime(line2, line2Size);
      break;

    default:
      snprintf(line1, line1Size, "DISPLAY");
      snprintf(line2, line2Size, "PAGINA INVALIDA");
      break;
  }
}

void displayCalibrationPage(char* line1, size_t line1Size, char* line2, size_t line2Size) {
  switch (calibrationUiState) {
    case CalibrationUiState::DRY_READY:
      snprintf(line1, line1Size, "SECO PRONTO?");
      snprintf(line2, line2Size, "TOQUE BOOT");
      break;

    case CalibrationUiState::DRY_MEASURING:
      snprintf(line1, line1Size, "MEDINDO SECO");
      snprintf(line2, line2Size, "%02u/%02u RAW:%4d",
               calibrationMeasurement.count,
               SOIL_CAL_MEASUREMENT_COUNT,
               calibrationMeasurement.lastRaw);
      break;

    case CalibrationUiState::WET_READY:
      snprintf(line1, line1Size, "SOLO UMIDO");
      snprintf(line2, line2Size, "TOQUE BOOT");
      break;

    case CalibrationUiState::WET_MEASURING:
      snprintf(line1, line1Size, "MEDINDO UMIDO");
      snprintf(line2, line2Size, "%02u/%02u RAW:%4d",
               calibrationMeasurement.count,
               SOIL_CAL_MEASUREMENT_COUNT,
               calibrationMeasurement.lastRaw);
      break;

    case CalibrationUiState::REVIEW:
      snprintf(line1, line1Size, "D%04d W%04d", candidateDry, candidateWet);
      snprintf(line2, line2Size, "SEGURE P/SALVAR");
      break;

    case CalibrationUiState::SAVING:
      snprintf(line1, line1Size, "SALVANDO CAL");
      snprintf(line2, line2Size, "AGUARDE");
      break;

    case CalibrationUiState::COMPLETE:
      snprintf(line1, line1Size, "CAL SALVA");
      snprintf(line2, line2Size, "D%04d W%04d", soilRawDry, soilRawWet);
      break;

    case CalibrationUiState::INVALID:
      snprintf(line1, line1Size, "%s", calibrationSaveFailed ? "SALVAR FALHOU" : "CAL INVALIDA");
      snprintf(line2, line2Size, "ANTERIOR MANTIDA");
      break;

    case CalibrationUiState::CANCELLED:
      snprintf(line1, line1Size, "CAL CANCELADA");
      snprintf(line2, line2Size, "SEM ALTERACAO");
      break;

    default:
      snprintf(line1, line1Size, "CALIBRACAO");
      snprintf(line2, line2Size, "SOLO");
      break;
  }
}

void handleDisplay(unsigned long now) {
  if (now - lastDisplayRefreshMs < DISPLAY_REFRESH_MS) return;
  lastDisplayRefreshMs = now;

  if (calibrationModeActive()) {
    char line1[17];
    char line2[17];
    displayCalibrationPage(line1, sizeof(line1), line2, sizeof(line2));
    printLcdLine(0, line1);
    printLcdLine(1, line2);
    return;
  }

  if (displayMode == DisplayMode::DEVELOPER &&
      now - lastDeveloperInteractionMs >= DEVELOPER_MODE_TIMEOUT_MS) {
    displayMode = DisplayMode::PUBLIC;
    currentPublicDisplayPage = PublicDisplayPage::SYSTEM;
    manualDisplayUntilMs = 0;
    lastDisplayRotationMs = now;
    displayModeNoticeUntilMs = now + DISPLAY_MODE_NOTICE_MS;
    clearLineCache();
    Serial.println("[DISPLAY] developer timeout -> PUBLIC");
    Serial.println("[DISPLAY] mode=PUBLIC");
  }

  if (manualValveTestState == ManualValveTestState::ARMED) {
    printLcdLine(0, "TESTE VALVULA?");
    printLcdLine(1, "CLIQUE CONFIRMA");
    return;
  }

  if (manualValveTestState == ManualValveTestState::ACTIVE) {
    long secondsLeft = static_cast<long>((MANUAL_VALVE_TEST_MS - (now - manualValveTestStateStartedMs)) / 1000UL);
    if (secondsLeft < 0) secondsLeft = 0;
    char line2[17];
    snprintf(line2, sizeof(line2), "FECHA:%ldS", secondsLeft);
    printLcdLine(0, "TESTE VALVULA");
    printLcdLine(1, line2);
    return;
  }

  if (systemState == SystemState::BOOT) {
    printLcdLine(0, "HORTA ESP32");
    printLcdLine(1, "BELA VISTA");
    return;
  }

  if (now >= manualDisplayUntilMs && now - lastDisplayRotationMs >= DISPLAY_ROTATION_MS) {
    lastDisplayRotationMs = now;
    if (displayMode == DisplayMode::PUBLIC) {
      uint8_t page = static_cast<uint8_t>(currentPublicDisplayPage);
      page = (page + 1) % static_cast<uint8_t>(PublicDisplayPage::COUNT);
      currentPublicDisplayPage = static_cast<PublicDisplayPage>(page);
    } else {
      uint8_t page = static_cast<uint8_t>(currentDeveloperDisplayPage);
      page = (page + 1) % static_cast<uint8_t>(DeveloperDisplayPage::COUNT);
      currentDeveloperDisplayPage = static_cast<DeveloperDisplayPage>(page);
    }
  }

  char line1[17];
  char line2[17];

  if (now < displayModeNoticeUntilMs) {
    if (displayMode == DisplayMode::DEVELOPER) {
      snprintf(line1, sizeof(line1), "MODO TECNICO");
      snprintf(line2, sizeof(line2), "ATIVADO");
    } else {
      snprintf(line1, sizeof(line1), "MODO PUBLICO");
      snprintf(line2, sizeof(line2), "ATIVADO");
    }
  } else if (systemState == SystemState::PRE_IRRIGATION) {
    long secondsLeft = static_cast<long>((PRE_IRRIGATION_MS - (now - stateStartedMs)) / 1000UL);
    if (secondsLeft < 0) secondsLeft = 0;
    snprintf(line1, sizeof(line1), "IRRIGACAO EM:");
    snprintf(line2, sizeof(line2), "%ld SEGUNDOS", secondsLeft);
  } else if (systemState == SystemState::IRRIGATING) {
    long secondsLeft = static_cast<long>((VALVE_PULSE_MS - (now - valveOpenedAtMs)) / 1000UL);
    if (secondsLeft < 0) secondsLeft = 0;
    snprintf(line1, sizeof(line1), "IRRIGANDO %u/%u", currentPulse, MAX_PULSES_PER_CYCLE);
    snprintf(line2, sizeof(line2), "PULSO: %ldS", secondsLeft);
  } else {
    if (displayMode == DisplayMode::PUBLIC) {
      displayPublicPage(currentPublicDisplayPage, line1, sizeof(line1), line2, sizeof(line2));
    } else {
      displayDeveloperPage(currentDeveloperDisplayPage, line1, sizeof(line1), line2, sizeof(line2));
    }
  }

  printLcdLine(0, line1);
  printLcdLine(1, line2);
}

void cycleDisplayPage(unsigned long now) {
  if (displayMode == DisplayMode::PUBLIC) {
    uint8_t page = static_cast<uint8_t>(currentPublicDisplayPage);
    page = (page + 1) % static_cast<uint8_t>(PublicDisplayPage::COUNT);
    currentPublicDisplayPage = static_cast<PublicDisplayPage>(page);
  } else {
    uint8_t page = static_cast<uint8_t>(currentDeveloperDisplayPage);
    page = (page + 1) % static_cast<uint8_t>(DeveloperDisplayPage::COUNT);
    currentDeveloperDisplayPage = static_cast<DeveloperDisplayPage>(page);
    lastDeveloperInteractionMs = now;
  }
  manualDisplayUntilMs = now + MANUAL_DISPLAY_HOLD_MS;
  lastDisplayRotationMs = now;
  clearLineCache();
  Serial.println("[BUTTON] BOOT short press");
  if (displayMode == DisplayMode::PUBLIC) {
    Serial.printf("[DISPLAY] mode=PUBLIC page=%s\n", publicDisplayPageName(currentPublicDisplayPage));
  } else {
    Serial.printf("[DISPLAY] mode=DEVELOPER page=%s\n", developerDisplayPageName(currentDeveloperDisplayPage));
  }
}

void resetButtonShortPressSequence() {
  buttonShortPressCount = 0;
  firstButtonShortPressMs = 0;
  lastButtonShortPressMs = 0;
}

void toggleDisplayMode(unsigned long now) {
  if (displayMode == DisplayMode::PUBLIC) {
    displayMode = DisplayMode::DEVELOPER;
    currentDeveloperDisplayPage = DeveloperDisplayPage::SYSTEM;
    lastDeveloperInteractionMs = now;
  } else {
    displayMode = DisplayMode::PUBLIC;
    currentPublicDisplayPage = PublicDisplayPage::SYSTEM;
  }
  manualDisplayUntilMs = now + MANUAL_DISPLAY_HOLD_MS;
  lastDisplayRotationMs = now;
  displayModeNoticeUntilMs = now + DISPLAY_MODE_NOTICE_MS;
  clearLineCache();
  Serial.printf("[DISPLAY] mode=%s\n", displayModeName(displayMode));
}

bool registerButtonShortPressForModeToggle(unsigned long now) {
  if (calibrationModeActive()) return false;

  if (buttonShortPressCount == 0 ||
      now - lastButtonShortPressMs > BUTTON_TRIPLE_CLICK_MAX_GAP_MS ||
      now - firstButtonShortPressMs > BUTTON_TRIPLE_CLICK_WINDOW_MS) {
    buttonShortPressCount = 1;
    firstButtonShortPressMs = now;
  } else {
    buttonShortPressCount++;
  }
  lastButtonShortPressMs = now;

  if (buttonShortPressCount >= 3) {
    resetButtonShortPressSequence();
    toggleDisplayMode(now);
    return true;
  }
  return false;
}

void handleButtonShortPress(unsigned long now) {
  if (calibrationModeActive()) {
    resetButtonShortPressSequence();
    handleCalibrationShortPress(now);
  } else if (registerButtonShortPressForModeToggle(now)) {
    Serial.println("[BUTTON] BOOT triple short press");
  } else {
    cycleDisplayPage(now);
  }
}

void handleButtonLongPress(unsigned long now) {
  resetButtonShortPressSequence();
  if (calibrationModeActive()) {
    handleCalibrationLongPress(now);
  } else {
    enterSoilCalibrationMode(now);
  }
}

void handleButtonVeryLongPress(unsigned long now) {
  resetButtonShortPressSequence();
  if (calibrationModeActive()) {
    handleCalibrationLongPress(now);
    return;
  }
  Serial.println("[BUTTON] VERY_LONG -> VALVE_TEST_ARMED");
  armManualValveTest(now);
}

void handleButtonRelease(unsigned long now, unsigned long durationMs) {
  if (manualValveTestState == ManualValveTestState::ARMED) {
    if (durationMs < BOOT_SHORT_PRESS_MAX_MS) {
      Serial.println("[BUTTON] SHORT");
      confirmManualValveTest(now);
    } else {
      cancelManualValveTest("CONFIRM_NOT_SHORT");
    }
    return;
  }

  if (durationMs < BOOT_SHORT_PRESS_MAX_MS) {
    Serial.println("[BUTTON] SHORT");
    handleButtonShortPress(now);
    return;
  }

  if (durationMs >= BOOT_VALVE_ARM_PRESS_MS) {
    handleButtonVeryLongPress(now);
    return;
  }

  if (durationMs >= BOOT_LONG_PRESS_MIN_MS) {
    Serial.println("[BUTTON] LONG -> SOIL_CALIBRATION");
    handleButtonLongPress(now);
    return;
  }

  resetButtonShortPressSequence();
  Serial.println("[BUTTON] IGNORED duration=MEDIUM");
}

void handleButton(unsigned long now) {
  bool reading = digitalRead(BOOT_BUTTON_PIN);
  if (reading != buttonLastReading) {
    buttonLastReading = reading;
    lastButtonTransitionMs = now;
  }

  if (now - lastButtonTransitionMs < BUTTON_DEBOUNCE_MS) return;

  if (reading != buttonStableLevel) {
    buttonStableLevel = reading;
    if (buttonStableLevel == BOOT_BUTTON_ACTIVE_LEVEL) {
      if (manualValveTestState == ManualValveTestState::ACTIVE) {
        Serial.println("[VALVE_TEST] CANCELLED_BY_BUTTON");
        cancelManualValveTest("CANCELLED_BY_BUTTON");
        buttonPressedArmed = false;
        return;
      }
      buttonPressedArmed = true;
      buttonPressedAtMs = now;
      return;
    }

    if (buttonPressedArmed) {
      unsigned long durationMs = now - buttonPressedAtMs;
      buttonPressedArmed = false;
      handleButtonRelease(now, durationMs);
    }
    return;
  }
}

void buildFutureLogRecord(FutureLogRecord* record) {
  timestampNow(record->timestamp, sizeof(record->timestamp));
  record->uptimeSeconds = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
  record->localTemperature = sensors.localTemperatureC;
  record->localAirHumidity = sensors.localRelativeHumidity;
  record->dhtStatus = dhtStatus;
  record->remoteTemperature = weather.remoteTemperature2m;
  record->remoteAirHumidity = weather.remoteRelativeHumidity2m;
  record->soilRaw = sensors.soilRawInstant;
  record->soilRawFiltered = sensors.soilRawFiltered;
  record->soilRawSlow = sensors.soilRawSlow;
  record->soilPercent = sensors.soilPercent;
  record->soilSensorStatus = soilSensorStatus;
  record->dhtFailureCount = dhtFailureCount;
  record->rainProbability6h = weather.maxRainProbabilityNext6h;
  record->rainMm6h = weather.totalRainMmNext6h;
  record->weatherStatus = weatherStatus;
  record->wifiStatus = wifiStatus;
  record->wifiRSSI = wifiStatus == Status::OK ? WiFi.RSSI() : 0;
  record->connectedSsid = connectedSsid;
  record->systemState = systemState;
  record->valveOpen = valveOpen;
  record->irrigationPulse = currentPulse;
  record->decision = lastDecision;
  record->firmwareVersion = FIRMWARE_VERSION;
  record->resetReason = resetReasonText;
}

void setJsonFloatOrNull(JsonDocument& doc, const char* key, float value) {
  if (isfinite(value)) {
    doc[key] = value;
  } else {
    doc[key] = nullptr;
  }
}

void setJsonSoilPercentOrNull(JsonDocument& doc, const char* key, int value) {
  if (value == INVALID_SOIL_PERCENT) {
    doc[key] = nullptr;
  } else {
    doc[key] = value;
  }
}

void setJsonIntOrNull(JsonDocument& doc, const char* key, bool valid, int value) {
  if (valid) {
    doc[key] = value;
  } else {
    doc[key] = nullptr;
  }
}

void setJsonFloatOrNull(JsonDocument& doc, const char* key, bool valid, float value) {
  if (valid && isfinite(value)) {
    doc[key] = value;
  } else {
    doc[key] = nullptr;
  }
}

bool buildCloudPayload(const FutureLogRecord& record, String* payloadOut) {
  JsonDocument doc;
  doc["device_token"] = DEVICE_TOKEN;
  doc["device_id"] = DEVICE_ID;
  doc["firmware_version"] = record.firmwareVersion;
  doc["uptime_s"] = record.uptimeSeconds;

  doc["soil_raw"] = record.soilRaw;
  doc["soil_filtered"] = record.soilRawFiltered;
  doc["soil_slow"] = record.soilRawSlow;
  setJsonSoilPercentOrNull(doc, "soil_percent", record.soilPercent);
  doc["soil_status"] = statusName(record.soilSensorStatus);

  bool dhtValuesValid = record.dhtStatus == Status::OK;
  setJsonFloatOrNull(doc, "local_temperature_c", dhtValuesValid, record.localTemperature);
  setJsonFloatOrNull(doc, "local_humidity_pct", dhtValuesValid, record.localAirHumidity);
  doc["dht_status"] = statusName(record.dhtStatus);
  doc["dht_failure_count"] = record.dhtFailureCount;

  bool weatherValuesValid = record.weatherStatus == WeatherStatus::OK;
  setJsonIntOrNull(doc, "rain_probability_6h_pct", weatherValuesValid, record.rainProbability6h);
  setJsonFloatOrNull(doc, "rain_6h_mm", weatherValuesValid, record.rainMm6h);
  setJsonFloatOrNull(doc, "remote_temperature_c", weatherValuesValid, record.remoteTemperature);
  setJsonFloatOrNull(doc, "remote_humidity_pct", weatherValuesValid, record.remoteAirHumidity);
  doc["weather_status"] = weatherStatusName(record.weatherStatus);

  doc["wifi_status"] = statusName(record.wifiStatus);
  doc["wifi_rssi_dbm"] = record.wifiRSSI;
  doc["wifi_ssid"] = record.connectedSsid != nullptr ? record.connectedSsid : "";

  doc["valve_open"] = record.valveOpen;
  doc["irrigation_pulse"] = record.irrigationPulse;
  doc["system_state"] = stateName(record.systemState);
  doc["decision"] = decisionName(record.decision);
  doc["decision_reason"] = decisionReason(record.decision);
  doc["reset_reason"] = record.resetReason;

  payloadOut->remove(0);
  return serializeJson(doc, *payloadOut) > 0;
}

bool parseCloudAck(const String& payload, char* errorOut, size_t errorOutSize) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    snprintf(errorOut, errorOutSize, "json_%s", error.c_str());
    return false;
  }

  JsonVariantConst okValue = doc["ok"];
  if (okValue.isNull() || !okValue.is<bool>()) {
    snprintf(errorOut, errorOutSize, "missing_ok");
    return false;
  }

  if (!okValue.as<bool>()) {
    const char* serverError = doc["error"] | "server_rejected";
    snprintf(errorOut, errorOutSize, "%s", serverError);
    return false;
  }

  errorOut[0] = '\0';
  return true;
}

bool cloudPostTelemetry(const FutureLogRecord& record) {
  String payload;
  if (!buildCloudPayload(record, &payload)) {
    Serial.println("[CLOUD] PAYLOAD_FAILED");
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(GOOGLE_GTS_ROOT_R1_CA);

  HTTPClient http;
  http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, GOOGLE_SCRIPT_URL)) {
    http.end();
    Serial.println("[CLOUD] HTTP_BEGIN_FAILED");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  Serial.printf("[CLOUD] POST start bytes=%u\n", static_cast<unsigned>(payload.length()));
  int httpCode = http.POST(payload);

  bool ok = false;
  if (httpCode >= 200 && httpCode < 300) {
    String response = http.getString();
    char ackError[48] = "";
    ok = parseCloudAck(response, ackError, sizeof(ackError));
    if (ok) {
      Serial.println("[CLOUD] ACK ok");
    } else if (strcmp(ackError, "missing_ok") == 0 || strncmp(ackError, "json_", 5) == 0) {
      Serial.printf("[CLOUD] RESPONSE_INVALID error=%s\n", ackError);
    } else {
      Serial.printf("[CLOUD] SERVER_REJECTED error=%s\n", ackError);
    }
  } else {
    Serial.printf("[CLOUD] HTTP code=%d\n", httpCode);
  }

  http.end();
  return ok;
}

bool cloudCanStartNow() {
  return !valveOpen &&
         systemState != SystemState::PRE_IRRIGATION &&
         systemState != SystemState::IRRIGATING;
}

void handleCloudLogging(unsigned long now) {
  if (!ENABLE_CLOUD_LOGGING) {
    cloudStatus = Status::INACTIVE;
    return;
  }
  if (wifiStatus != Status::OK) {
    cloudStatus = Status::OFFLINE;
    return;
  }
  if (!isCloudConfigured()) {
    cloudStatus = Status::INACTIVE;
    static bool loggedCloudConfig = false;
    if (!loggedCloudConfig) {
      Serial.printf("[CLOUD] endpointConfigured=%s tokenConfigured=%s\n",
                    (isCloudEndpointConfigured() && isCloudEndpointHttps()) ? "YES" : "NO",
                    isCloudTokenConfigured() ? "YES" : "NO");
      loggedCloudConfig = true;
    }
    return;
  }
  if (cloudStatus == Status::INACTIVE || cloudStatus == Status::OFFLINE) {
    cloudStatus = Status::UNKNOWN;
  }
  if (nextCloudLogMs == 0) {
    nextCloudLogMs = bootStartedMs + CLOUD_FIRST_SEND_DELAY_MS;
  }
  if (static_cast<long>(now - nextCloudLogMs) < 0) return;
  if (!cloudCanStartNow()) {
    Serial.println("[CLOUD] skipped valve/state busy");
    nextCloudLogMs = now + CLOUD_RETRY_INTERVAL_MS;
    return;
  }

  FutureLogRecord record;
  buildFutureLogRecord(&record);

  lastCloudLogMs = now;
  bool ok = cloudPostTelemetry(record);
  if (ok) {
    cloudStatus = Status::OK;
    nextCloudLogMs = now + CLOUD_LOG_INTERVAL_MS;
  } else {
    cloudStatus = Status::FAULT;
    nextCloudLogMs = now + CLOUD_RETRY_INTERVAL_MS;
    Serial.println("[CLOUD] RETRY scheduled");
  }
}

void handleHeartbeat(unsigned long now) {
  unsigned long interval = 1000UL;
  if (systemState == SystemState::BOOT || systemState == SystemState::IRRIGATING) interval = 250UL;
  else if (wifiStatus != Status::OK) interval = 500UL;
  if (systemState == SystemState::SENSOR_FAULT || soilSensorStatus == Status::FAULT) {
    interval = 150UL;
  }

  if (now - lastHeartbeatMs >= interval) {
    lastHeartbeatMs = now;
    digitalWrite(LED_BUILTIN_PIN, !digitalRead(LED_BUILTIN_PIN));
  }
}

void handleSerialStatus(unsigned long now) {
  if (now - lastSerialStatusMs < SERIAL_STATUS_INTERVAL_MS) return;
  lastSerialStatusMs = now;

  Serial.printf("[STATUS] state=%s wifi=%s weather=%s cloud=%s soil=%s dht=%s valve=%s decision=%s auto=%s cloudEnabled=%s\n",
                stateName(systemState),
                statusName(wifiStatus),
                weatherStatusName(weatherStatus),
                statusName(cloudStatus),
                statusName(soilSensorStatus),
                statusName(dhtStatus),
                valveOpen ? "OPEN" : "CLOSED",
                decisionReason(lastDecision),
                ENABLE_AUTOMATIC_IRRIGATION ? "ON" : "OFF",
                ENABLE_CLOUD_LOGGING ? "ON" : "OFF");
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, VALVE_CLOSED_LEVEL);
  valveOpen = false;

  Serial.begin(115200);
  getResetReasonText();
  Serial.printf("\n[BOOT] Horta %s\n", FIRMWARE_VERSION);
  Serial.printf("[BOOT] Reset=%s Arduino-ESP32=%d.%d.%d\n",
                resetReasonText,
                ESP_ARDUINO_VERSION_MAJOR,
                ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
  Serial.println("[BOOT] BOOT button GPIO0 active-low; holding it during reset may enter download mode.");
  Serial.printf("[WEATHER] requested lat=%s lon=%s elev=%.0fm timezone=%s\n",
                WEATHER_LATITUDE,
                WEATHER_LONGITUDE,
                WEATHER_ELEVATION_M,
                WEATHER_TIMEZONE);
  Serial.println("[VALVE] CLOSED");
  loadSoilCalibration();

  initWatchdog();

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN_PIN, LED_ON_LEVEL);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  buttonStableLevel = digitalRead(BOOT_BUTTON_PIN);
  buttonLastReading = buttonStableLevel;

  dht.begin();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  printLcdLine(0, "HORTA ESP32");
  printLcdLine(1, "BELA VISTA");

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_MOISTURE_PIN, ADC_11db);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  bootStartedMs = millis();
  stateStartedMs = bootStartedMs;
  lastSoilReadMs = bootStartedMs - SOIL_INTERVAL_MS;
  lastDhtReadMs = bootStartedMs - DHT_READ_INTERVAL_MS;
  lastDisplayRefreshMs = bootStartedMs - DISPLAY_REFRESH_MS;
  lastDisplayRotationMs = bootStartedMs;
  lastDeveloperInteractionMs = bootStartedMs;
  lastWifiAttemptMs = bootStartedMs - WIFI_RECONNECT_MIN_MS;

  Serial.printf("[BOOT] WiFi profiles=%u\n", configuredWifiProfileCount());
  Serial.println("[BOOT] Local controller ready");
}

void loop() {
  unsigned long now = millis();

  feedWatchdog();
  handleSensors(now);
  handleWiFi(now);
  handleTime();
  handleWeather(now);
  handleIrrigation(now);
  handleButton(now);
  handleManualValveTest(now);
  handleCalibrationMode(now);
  handleDisplay(now);
  handleCloudLogging(now);
  handleHeartbeat(now);
  handleSerialStatus(now);
  yield();
}
