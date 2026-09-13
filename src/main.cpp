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

#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_arduino_version.h>
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

enum class DisplayPage : uint8_t {
  SYSTEM,
  SOIL,
  WIFI,
  WEATHER,
  CLOUD,
  DEVICE,
  COUNT
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
};

struct WeatherData {
  int maxRainProbabilityNext6h = 0;
  float totalRainMmNext6h = 0.0f;
  float remoteTemperature2m = NAN;
  float remoteRelativeHumidity2m = NAN;
};

struct FutureLogRecord {
  char timestamp[24];
  unsigned long uptime;
  float remoteTemperature;
  float remoteAirHumidity;
  int soilRaw;
  int soilRawFiltered;
  int soilPercent;
  Status soilSensorStatus;
  int rainProbability6h;
  float rainMm6h;
  WeatherStatus weatherStatus;
  Status wifiStatus;
  int wifiRSSI;
  SystemState systemState;
  bool valveOpen;
  uint8_t irrigationPulse;
  Decision decision;
  const char* decisionReason;
  const char* firmwareVersion;
  const char* resetReason;
};

LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, LCD_COLUMNS, LCD_ROWS);

constexpr char SOIL_CAL_NAMESPACE[] = "soilcal";
constexpr char SOIL_CAL_KEY_DRY[] = "dry";
constexpr char SOIL_CAL_KEY_WET[] = "wet";
constexpr char SOIL_CAL_KEY_VALID[] = "valid";
constexpr char SOIL_CAL_KEY_VERSION[] = "version";
constexpr uint8_t SOIL_CAL_SUPPORTED_VERSION = 1;

WifiProfile wifiProfiles[] = {
  {WIFI_SSID_1, WIFI_PASSWORD_1},
  {WIFI_SSID_2, WIFI_PASSWORD_2},
};

SensorData sensors;
WeatherData weather;

SystemState systemState = SystemState::BOOT;
Status wifiStatus = Status::UNKNOWN;
WeatherStatus weatherStatus = WeatherStatus::NOT_CHECKED;
Status cloudStatus = ENABLE_CLOUD_LOGGING ? Status::UNKNOWN : Status::INACTIVE;
Status soilSensorStatus = Status::UNCALIBRATED;
Decision lastDecision = Decision::NONE;
DisplayPage currentDisplayPage = DisplayPage::SYSTEM;

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
unsigned long lastDisplayRefreshMs = 0;
unsigned long lastDisplayRotationMs = 0;
unsigned long manualDisplayUntilMs = 0;
unsigned long lastWeatherAttemptMs = 0;
unsigned long lastCloudLogMs = 0;
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

const char* displayPageName(DisplayPage page) {
  switch (page) {
    case DisplayPage::SYSTEM: return "SYSTEM";
    case DisplayPage::SOIL: return "SOIL";
    case DisplayPage::WIFI: return "WIFI";
    case DisplayPage::WEATHER: return "WEATHER";
    case DisplayPage::CLOUD: return "CLOUD";
    case DisplayPage::DEVICE: return "DEVICE";
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

bool isConfiguredText(const char* value, const char* placeholder) {
  return value != nullptr && value[0] != '\0' && strcmp(value, placeholder) != 0;
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

void closeValve(const char* reason) {
  if (valveOpen) {
    totalValveOpenThisCycleMs += millis() - valveOpenedAtMs;
  }
  digitalWrite(RELAY_PIN, VALVE_CLOSED_LEVEL);
  valveOpen = false;
  noTone(BUZZER_PIN);
  buzzerOn = false;
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
  noTone(BUZZER_PIN);
  buzzerOn = false;
  Serial.println("[VALVE] OPEN");
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

void handleSensors(unsigned long now) {
  handleSoil(now);
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

int parseJsonNumberArray(const char* json, const char* key, float* values, int maxValues) {
  const char* keyPos = strstr(json, key);
  if (keyPos == nullptr) return 0;

  const char* cursor = strchr(keyPos, '[');
  if (cursor == nullptr) return 0;
  cursor++;

  int count = 0;
  while (*cursor != '\0' && *cursor != ']' && count < maxValues) {
    while (*cursor != '\0' && *cursor != ']' &&
           *cursor != '-' && *cursor != '.' &&
           (*cursor < '0' || *cursor > '9')) {
      cursor++;
    }

    if (*cursor == '\0' || *cursor == ']') break;

    char* endPtr = nullptr;
    float value = strtof(cursor, &endPtr);
    if (endPtr == cursor) break;

    values[count++] = value;
    cursor = endPtr;
  }

  return count;
}

bool parseOpenMeteo(const String& payload) {
  const char* json = payload.c_str();
  float probabilities[6];
  float precipitations[6];
  float temperatures[1];
  float humidities[1];

  int probabilityCount = parseJsonNumberArray(json, "\"precipitation_probability\"", probabilities, 6);
  int precipitationCount = parseJsonNumberArray(json, "\"precipitation\"", precipitations, 6);
  int count = min(probabilityCount, precipitationCount);
  if (count <= 0) return false;

  int maxProbability = 0;
  float totalMm = 0.0f;

  for (int i = 0; i < count; i++) {
    int probability = constrain(static_cast<int>(probabilities[i] + 0.5f), 0, 100);
    if (probability > maxProbability) maxProbability = probability;
    totalMm += precipitations[i];
  }

  weather.maxRainProbabilityNext6h = maxProbability;
  weather.totalRainMmNext6h = totalMm;

  if (parseJsonNumberArray(json, "\"temperature_2m\"", temperatures, 1) == 1) {
    weather.remoteTemperature2m = temperatures[0];
  }
  if (parseJsonNumberArray(json, "\"relative_humidity_2m\"", humidities, 1) == 1) {
    weather.remoteRelativeHumidity2m = humidities[0];
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

  char url[384];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.12f&longitude=%.12f&hourly=precipitation_probability,precipitation,temperature_2m,relative_humidity_2m&forecast_days=1&timezone=%s",
           WEATHER_LATITUDE,
           WEATHER_LONGITUDE,
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
    Serial.printf("[WEATHER] Rain6h=%d%% %.1fmm Tremote=%.1f RHremote=%.1f\n",
                  weather.maxRainProbabilityNext6h,
                  weather.totalRainMmNext6h,
                  weather.remoteTemperature2m,
                  weather.remoteRelativeHumidity2m);
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
  noTone(BUZZER_PIN);
  buzzerOn = false;
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
      noTone(BUZZER_PIN);
      buzzerOn = false;
    }
    return;
  }

  if (buzzerOn) {
    if (now - buzzerToggleMs >= BUZZ_ON_MS) {
      noTone(BUZZER_PIN);
      buzzerOn = false;
      buzzerToggleMs = now;
    }
  } else if (now - buzzerToggleMs >= BUZZ_OFF_MS) {
    tone(BUZZER_PIN, buzzerAlt ? BUZZ_FREQ_A : BUZZ_FREQ_B);
    buzzerAlt = !buzzerAlt;
    buzzerOn = true;
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
        noTone(BUZZER_PIN);
        buzzerOn = false;
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

void formatSystemLine(char* line, size_t size) {
  if (systemState == SystemState::SENSOR_FAULT) {
    snprintf(line, size, "Horta: SENSOR");
  } else if (soilSensorStatus == Status::UNCALIBRATED) {
    snprintf(line, size, "Horta: ATENCAO");
  } else {
    snprintf(line, size, "Horta: NORMAL");
  }
}

void formatShortSsid(char* buffer, size_t size) {
  if (wifiStatus != Status::OK || connectedSsid[0] == '\0') {
    snprintf(buffer, size, "OFFLINE");
    return;
  }
  snprintf(buffer, size, "%.15s", connectedSsid);
}

void displayPage(DisplayPage page, char* line1, size_t line1Size, char* line2, size_t line2Size) {
  switch (page) {
    case DisplayPage::SYSTEM:
      formatSystemLine(line1, line1Size);
      snprintf(line2, line2Size, "Irrig: %s", valveOpen ? "ON" : "OFF");
      break;

    case DisplayPage::SOIL:
      if (soilSensorStatus == Status::UNCALIBRATED) {
        snprintf(line1, line1Size, "Solo: CALIBRAR");
      } else if (soilSensorStatus == Status::FAULT) {
        snprintf(line1, line1Size, "Solo: FALHA");
      } else {
        snprintf(line1, line1Size, "Umidade solo");
      }
      if (sensors.soilPercent == INVALID_SOIL_PERCENT) {
        snprintf(line2, line2Size, "RAW:%4d", sensors.soilRawFiltered);
      } else {
        snprintf(line2, line2Size, "%3d%%", sensors.soilPercent);
      }
      break;

    case DisplayPage::WIFI:
      snprintf(line1, line1Size, "WiFi: %s", wifiStatus == Status::OK ? "ONLINE" : "OFFLINE");
      if (wifiStatus == Status::OK) {
        snprintf(line2, line2Size, "RSSI:%4d dBm", WiFi.RSSI());
      } else {
        char ssid[17];
        formatShortSsid(ssid, sizeof(ssid));
        snprintf(line2, line2Size, "SSID: %s", ssid);
      }
      break;

    case DisplayPage::WEATHER:
      if (weatherStatus == WeatherStatus::OK) {
        snprintf(line1, line1Size, "Clima: ONLINE");
        snprintf(line2, line2Size, "Chuva6h:%3d%%", weather.maxRainProbabilityNext6h);
      } else if (weatherStatus == WeatherStatus::WIFI_OFFLINE) {
        snprintf(line1, line1Size, "Clima: SEM WIFI");
        snprintf(line2, line2Size, "Controle LOCAL");
      } else if (weatherStatus == WeatherStatus::API_FAILURE) {
        snprintf(line1, line1Size, "Clima: FALHA");
        snprintf(line2, line2Size, "API Open-Meteo");
      } else {
        snprintf(line1, line1Size, "Clima: AGUARDE");
        snprintf(line2, line2Size, "Nao verificado");
      }
      break;

    case DisplayPage::CLOUD:
      snprintf(line1, line1Size, "Cloud: INATIVO");
      snprintf(line2, line2Size, "Script: PENDENTE");
      break;

    case DisplayPage::DEVICE:
      snprintf(line1, line1Size, "FW %s", FIRMWARE_VERSION);
      snprintf(line2, line2Size, "Up:%lus %s", millis() / 1000UL, resetReasonText);
      break;

    default:
      snprintf(line1, line1Size, "Display");
      snprintf(line2, line2Size, "Pagina invalida");
      break;
  }
}

void handleDisplay(unsigned long now) {
  if (now - lastDisplayRefreshMs < DISPLAY_REFRESH_MS) return;
  lastDisplayRefreshMs = now;

  if (systemState == SystemState::BOOT) {
    printLcdLine(0, "Horta ESP32");
    printLcdLine(1, "BELA VISTA");
    return;
  }

  if (now >= manualDisplayUntilMs && now - lastDisplayRotationMs >= DISPLAY_ROTATION_MS) {
    lastDisplayRotationMs = now;
    uint8_t page = static_cast<uint8_t>(currentDisplayPage);
    page = (page + 1) % static_cast<uint8_t>(DisplayPage::COUNT);
    currentDisplayPage = static_cast<DisplayPage>(page);
  }

  char line1[17];
  char line2[17];

  if (systemState == SystemState::PRE_IRRIGATION) {
    long secondsLeft = static_cast<long>((PRE_IRRIGATION_MS - (now - stateStartedMs)) / 1000UL);
    if (secondsLeft < 0) secondsLeft = 0;
    snprintf(line1, sizeof(line1), "IRRIGACAO EM:");
    snprintf(line2, sizeof(line2), "%ld segundos", secondsLeft);
  } else if (systemState == SystemState::IRRIGATING) {
    long secondsLeft = static_cast<long>((VALVE_PULSE_MS - (now - valveOpenedAtMs)) / 1000UL);
    if (secondsLeft < 0) secondsLeft = 0;
    snprintf(line1, sizeof(line1), "IRRIGANDO %u/%u", currentPulse, MAX_PULSES_PER_CYCLE);
    snprintf(line2, sizeof(line2), "Pulso: %lds", secondsLeft);
  } else {
    displayPage(currentDisplayPage, line1, sizeof(line1), line2, sizeof(line2));
  }

  printLcdLine(0, line1);
  printLcdLine(1, line2);
}

void cycleDisplayPage(unsigned long now) {
  uint8_t page = static_cast<uint8_t>(currentDisplayPage);
  page = (page + 1) % static_cast<uint8_t>(DisplayPage::COUNT);
  currentDisplayPage = static_cast<DisplayPage>(page);
  manualDisplayUntilMs = now + MANUAL_DISPLAY_HOLD_MS;
  lastDisplayRotationMs = now;
  clearLineCache();
  Serial.println("[BUTTON] BOOT short press");
  Serial.printf("[DISPLAY] page=%s\n", displayPageName(currentDisplayPage));
}

void handleButton(unsigned long now) {
  bool reading = digitalRead(BOOT_BUTTON_PIN);
  if (reading != buttonLastReading) {
    buttonLastReading = reading;
    lastButtonTransitionMs = now;
  }

  if (now - lastButtonTransitionMs < BUTTON_DEBOUNCE_MS) return;
  if (reading == buttonStableLevel) return;

  buttonStableLevel = reading;
  if (buttonStableLevel == BOOT_BUTTON_ACTIVE_LEVEL) {
    buttonPressedArmed = true;
    return;
  }

  if (buttonPressedArmed) {
    buttonPressedArmed = false;
    cycleDisplayPage(now);
  }
}

void buildFutureLogRecord(FutureLogRecord* record) {
  timestampNow(record->timestamp, sizeof(record->timestamp));
  record->uptime = millis();
  record->remoteTemperature = weather.remoteTemperature2m;
  record->remoteAirHumidity = weather.remoteRelativeHumidity2m;
  record->soilRaw = sensors.soilRawInstant;
  record->soilRawFiltered = sensors.soilRawFiltered;
  record->soilPercent = sensors.soilPercent;
  record->soilSensorStatus = soilSensorStatus;
  record->rainProbability6h = weather.maxRainProbabilityNext6h;
  record->rainMm6h = weather.totalRainMmNext6h;
  record->weatherStatus = weatherStatus;
  record->wifiStatus = wifiStatus;
  record->wifiRSSI = wifiStatus == Status::OK ? WiFi.RSSI() : 0;
  record->systemState = systemState;
  record->valveOpen = valveOpen;
  record->irrigationPulse = currentPulse;
  record->decision = lastDecision;
  record->decisionReason = decisionReason(lastDecision);
  record->firmwareVersion = FIRMWARE_VERSION;
  record->resetReason = resetReasonText;
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
  if (!isConfiguredText(GOOGLE_SCRIPT_URL, "YOUR_GOOGLE_APPS_SCRIPT_URL")) {
    cloudStatus = Status::INACTIVE;
    return;
  }
  if (now - lastCloudLogMs < CLOUD_LOG_INTERVAL_MS) return;
  lastCloudLogMs = now;

  FutureLogRecord record;
  buildFutureLogRecord(&record);

  cloudStatus = Status::INACTIVE;
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

  Serial.printf("[STATUS] state=%s wifi=%s weather=%s cloud=%s soil=%s valve=%s decision=%s auto=%s cloudEnabled=%s\n",
                stateName(systemState),
                statusName(wifiStatus),
                weatherStatusName(weatherStatus),
                statusName(cloudStatus),
                statusName(soilSensorStatus),
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
  Serial.println("[VALVE] CLOSED");
  loadSoilCalibration();

  initWatchdog();

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN_PIN, LED_ON_LEVEL);
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  buttonStableLevel = digitalRead(BOOT_BUTTON_PIN);
  buttonLastReading = buttonStableLevel;

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  printLcdLine(0, "Horta ESP32");
  printLcdLine(1, "BELA VISTA");

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_MOISTURE_PIN, ADC_11db);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  bootStartedMs = millis();
  stateStartedMs = bootStartedMs;
  lastSoilReadMs = bootStartedMs - SOIL_INTERVAL_MS;
  lastDisplayRefreshMs = bootStartedMs - DISPLAY_REFRESH_MS;
  lastDisplayRotationMs = bootStartedMs;
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
  handleDisplay(now);
  handleCloudLogging(now);
  handleHeartbeat(now);
  handleSerialStatus(now);
  yield();
}
