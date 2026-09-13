#include <Arduino.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#include "config.h"

constexpr int DHT_PIN = 15;
constexpr uint8_t DHT_TYPE = DHT11;
constexpr unsigned long SOIL_STREAM_INTERVAL_MS = 500UL;
constexpr unsigned long RELAY_PULSE_MS = 500UL;
constexpr unsigned long BUZZER_TEST_MS = 700UL;
constexpr int RELAY_OFF = HIGH;
constexpr int RELAY_ON = LOW;

static_assert(BUZZER_PIN == 13, "BUZZER_PIN must match the validated GPIO13 wiring.");
static_assert(RELAY_PIN == 23, "Relay diagnostic expects GPIO23.");
static_assert(SOIL_MOISTURE_PIN == 36, "Soil diagnostic expects GPIO36.");
static_assert(I2C_SDA_PIN == 21, "LCD SDA diagnostic expects GPIO21.");
static_assert(I2C_SCL_PIN == 22, "LCD SCL diagnostic expects GPIO22.");
static_assert(LCD_I2C_ADDRESS == 0x3F, "LCD diagnostic expects address 0x3F.");
static_assert(LCD_COLUMNS == 16 && LCD_ROWS == 2, "LCD diagnostic is written for 16x2 displays.");

LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, LCD_COLUMNS, LCD_ROWS);
DHT dht(DHT_PIN, DHT_TYPE);

bool soilStreamEnabled = false;
unsigned long lastSoilStreamMs = 0;

void lcdLine(uint8_t row, const char* text) {
  char padded[17];
  snprintf(padded, sizeof(padded), "%-16.16s", text);
  lcd.setCursor(0, row);
  lcd.print(padded);
}

void lcdMessage(const char* upper, const char* lower) {
  lcdLine(0, upper);
  lcdLine(1, lower);
}

void printPinout() {
  Serial.println();
  Serial.println("=== HARDWARE PINOUT ===");
  Serial.println();
  Serial.println("LED      : GPIO2");
  Serial.println("SOIL     : GPIO36");
  Serial.println("DHT11    : GPIO15");
  Serial.println("BUZZER   : GPIO13");
  Serial.println("RELAY    : GPIO23");
  Serial.println("LCD SDA  : GPIO21");
  Serial.println("LCD SCL  : GPIO22");
  Serial.println("LCD ADDR : 0x3F");
  Serial.println("BOOT     : GPIO0");
}

void printMenu() {
  Serial.println();
  Serial.println("=== HARDWARE DIAGNOSTIC MENU ===");
  Serial.println("h = help / menu");
  Serial.println("i = scan I2C bus");
  Serial.println("l = test LCD");
  Serial.println("s = single soil-sensor measurement");
  Serial.println("S = toggle continuous soil-sensor diagnostic stream");
  Serial.println("d = test DHT11");
  Serial.println("b = test buzzer");
  Serial.println("r = test relay with short safe pulse");
  Serial.println("e = test onboard LED");
  Serial.println("a = run passive diagnostic summary");
  Serial.println();
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

int readSoilFiltered(int* rawInstant) {
  int samples[SOIL_SAMPLE_COUNT];
  for (uint8_t i = 0; i < SOIL_SAMPLE_COUNT; i++) {
    samples[i] = analogRead(SOIL_MOISTURE_PIN);
    delay(2);
  }

  *rawInstant = samples[SOIL_SAMPLE_COUNT - 1];
  sortSamples(samples, SOIL_SAMPLE_COUNT);

  long sum = 0;
  for (uint8_t i = 2; i < SOIL_SAMPLE_COUNT - 2; i++) {
    sum += samples[i];
  }
  return static_cast<int>(sum / (SOIL_SAMPLE_COUNT - 4));
}

void showSoilReading() {
  int rawInstant = 0;
  int filtered = readSoilFiltered(&rawInstant);

  Serial.printf("[SOIL] RAW=%d FILTERED=%d\n", rawInstant, filtered);

  char lower[17];
  snprintf(lower, sizeof(lower), "RAW:%d", filtered);
  lcdMessage("SOLO GPIO36", lower);
}

bool scanI2CBus(bool updateDisplay) {
  bool foundExpected = false;
  uint8_t foundCount = 0;

  Serial.println("[I2C] SCAN START");
  for (uint8_t address = 1; address <= 126; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      foundCount++;
      Serial.printf("[I2C] FOUND 0x%02X\n", address);
      if (address == LCD_I2C_ADDRESS) {
        foundExpected = true;
      }
    }
  }

  if (foundExpected) {
    Serial.println("[LCD] EXPECTED ADDRESS FOUND");
    if (updateDisplay) {
      lcdMessage("I2C: 0x3F", "LCD ENCONTRADO");
    }
  } else {
    Serial.println("[LCD] EXPECTED ADDRESS NOT FOUND");
    if (updateDisplay) {
      lcdMessage("I2C: SEM LCD", "ADDR 0x3F?");
    }
  }

  if (foundCount == 0) {
    Serial.println("[I2C] NO DEVICES FOUND");
  }
  Serial.println("[I2C] SCAN END");
  return foundExpected;
}

void testLcd() {
  Serial.println("[LCD] 16x2 visual test");
  lcdMessage("LINHA SUPERIOR", "LINHA INFERIOR");
  delay(1000);
  lcdMessage("LCD 16x2 OK?", "SDA21 SCL22");
}

bool readDhtOnce(float* temperature, float* humidity) {
  // GPIO15 is an ESP32 strapping pin. A boot failure with DHT connected is evidence.
  *humidity = dht.readHumidity();
  *temperature = dht.readTemperature();
  return !isnan(*temperature) && !isnan(*humidity);
}

bool testDht() {
  float temperature = NAN;
  float humidity = NAN;
  bool valid = readDhtOnce(&temperature, &humidity);

  if (valid) {
    Serial.printf("[DHT] OK T=%.1fC RH=%.1f%%\n", temperature, humidity);
    char lower[17];
    snprintf(lower, sizeof(lower), "T:%dC U:%d%%",
             static_cast<int>(temperature + 0.5f),
             static_cast<int>(humidity + 0.5f));
    lcdMessage("DHT11 GPIO15", lower);
  } else {
    Serial.println("[DHT] ERROR invalid reading");
    lcdMessage("DHT11: FALHA", "SEM LEITURA");
  }

  return valid;
}

void testBuzzer() {
  Serial.println("[BUZZER] TEST GPIO13");
  lcdMessage("BUZZER GPIO13", "TESTE SONORO");

  tone(BUZZER_PIN, BUZZ_FREQ_A);
  delay(BUZZER_TEST_MS);
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println("[BUZZER] OK command completed");
}

void testRelay() {
  Serial.println("[RELAY] TEST START GPIO23");
  Serial.println("[RELAY] WARNING physical output will activate for 500ms");
  lcdMessage("RELE GPIO23", "PULSO 500ms");

  digitalWrite(RELAY_PIN, RELAY_ON);
  Serial.println("[RELAY] ACTIVE 500ms");
  delay(RELAY_PULSE_MS);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  Serial.println("[RELAY] SAFE/OFF");
}

void testLed() {
  Serial.println("[LED] GPIO2 test");
  lcdMessage("LED GPIO2", "3 PULSOS");

  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN_PIN, LED_ON_LEVEL);
    delay(250);
    digitalWrite(LED_BUILTIN_PIN, LED_OFF_LEVEL);
    delay(250);
  }
  digitalWrite(LED_BUILTIN_PIN, LED_OFF_LEVEL);
}

void passiveSummary() {
  Serial.println("================================");
  Serial.println(" HARDWARE DIAGNOSTIC SUMMARY");
  Serial.println("================================");
  Serial.println("GPIO MAP : LED2 SOIL36 DHT15 BUZZ13 RELAY23 SDA21 SCL22");

  bool lcdDetected = scanI2CBus(false);
  Serial.printf("LCD      : %s\n", lcdDetected ? "detected" : "not detected");

  int rawInstant = 0;
  int filtered = readSoilFiltered(&rawInstant);
  Serial.printf("SOIL     : RAW=%d FILTERED=%d\n", rawInstant, filtered);

  float temperature = NAN;
  float humidity = NAN;
  bool dhtOk = readDhtOnce(&temperature, &humidity);
  if (dhtOk) {
    Serial.printf("DHT11    : OK T=%.1fC RH=%.1f%%\n", temperature, humidity);
  } else {
    Serial.println("DHT11    : ERROR");
  }

  Serial.println("BUZZER   : manual test required");
  Serial.println("RELAY    : manual test required");
  Serial.println("================================");

  char lower[17];
  snprintf(lower, sizeof(lower), "SOLO:%d", filtered);
  lcdMessage(lcdDetected ? "LCD DETECTADO" : "LCD NAO DETEC", lower);
}

void handleCommand(char command) {
  if (command == '\r' || command == '\n') return;

  if (command == 'S') {
    soilStreamEnabled = !soilStreamEnabled;
    Serial.printf("[SOIL] STREAM %s\n", soilStreamEnabled ? "ON" : "OFF");
    lcdMessage("SOLO STREAM", soilStreamEnabled ? "ATIVO" : "DESLIGADO");
    return;
  }

  switch (tolower(static_cast<unsigned char>(command))) {
    case 'h':
      printMenu();
      break;
    case 'i':
      scanI2CBus(true);
      break;
    case 'l':
      testLcd();
      break;
    case 's':
      showSoilReading();
      break;
    case 'd':
      testDht();
      break;
    case 'b':
      testBuzzer();
      break;
    case 'r':
      testRelay();
      break;
    case 'e':
      testLed();
      break;
    case 'a':
      passiveSummary();
      break;
    default:
      Serial.printf("[MENU] Unknown command '%c'\n", command);
      Serial.println("[MENU] Enter h for help");
      break;
  }
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.begin(115200);
  delay(200);

  pinMode(LED_BUILTIN_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN_PIN, LED_OFF_LEVEL);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_MOISTURE_PIN, ADC_11db);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcdMessage("DIAGNOSTICO HW", "HORTA ESP32");

  dht.begin();

  Serial.println();
  Serial.println("[BOOT] Hardware diagnostic firmware");
  Serial.println("[BOOT] Relay forced OFF/HIGH before peripheral init");
  Serial.println("[BOOT] No Wi-Fi, cloud, NTP, LittleFS, telemetry, or irrigation logic");
  printPinout();
  printMenu();
}

void loop() {
  while (Serial.available() > 0) {
    handleCommand(static_cast<char>(Serial.read()));
  }

  if (soilStreamEnabled && millis() - lastSoilStreamMs >= SOIL_STREAM_INTERVAL_MS) {
    lastSoilStreamMs = millis();
    showSoilReading();
  }

  yield();
}
