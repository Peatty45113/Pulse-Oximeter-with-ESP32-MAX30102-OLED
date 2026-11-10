
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"

// ======================================
// ESP32 + OLED + MAX30102
// ======================================
#define SDA_PIN 21
#define SCL_PIN 22
#define OLED_ADDR 0x3C

Adafruit_SH1106G display(128, 64, &Wire, -1);
MAX30105 sensor;

bool oledReady = false;
bool fingerPresent = false;

// ======================================
// SENSOR CONFIGURATION
// ======================================
const uint32_t FINGER_THRESHOLD = 50000;

// Sensor runs at 100 SPS.
// SpO2 algorithm expects 25 samples/sec.
const int SAMPLE_COUNT = 100;
const int DECIMATION = 4;

uint32_t irBuffer[SAMPLE_COUNT];
uint32_t redBuffer[SAMPLE_COUNT];

int bufferCount = 0;
int decimationCount = 0;

unsigned long lastFingerTime = 0;
unsigned long lastDisplayTime = 0;
unsigned long lastSampleTime = 0;

// ======================================
// BPM VARIABLES
// ======================================
const byte RATE_SIZE = 5;

float bpmHistory[RATE_SIZE];
byte rateSpot = 0;
byte rateCount = 0;

float currentBPM = 0;
float averageBPM = 0;

unsigned long lastBeat = 0;

// ======================================
// SPO2 VARIABLES
// ======================================
int32_t spo2 = 0;
int32_t algorithmBPM = 0;

int8_t validSpO2 = 0;
int8_t validAlgorithmBPM = 0;

int shownSpO2 = -1;

// ======================================
// RESET
// ======================================
void resetReadings() {
  currentBPM = 0;
  averageBPM = 0;

  lastBeat = 0;
  rateSpot = 0;
  rateCount = 0;

  bufferCount = 0;
  decimationCount = 0;

  shownSpO2 = -1;
  spo2 = 0;
  algorithmBPM = 0;

  validSpO2 = 0;
  validAlgorithmBPM = 0;
}

// ======================================
// OLED DISPLAY
// ======================================
void showScreen(const char* line1,
                const char* line2,
                const char* line3) {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(line1);

  display.setTextSize(2);
  display.setCursor(0, 18);
  display.println(line2);

  display.setCursor(0, 43);
  display.println(line3);

  display.display();
}

void updateOLED() {
  if (!fingerPresent) {
    showScreen("PULSE OXIMETER",
               "PLACE",
               "FINGER");
    return;
  }

  char bpmText[20];
  char spo2Text[20];

  if (averageBPM > 0) {
    snprintf(bpmText, sizeof(bpmText),
             "BPM:%d", (int)round(averageBPM));
  } else {
    snprintf(bpmText, sizeof(bpmText),
             "BPM:--");
  }

  if (shownSpO2 > 0) {
    snprintf(spo2Text, sizeof(spo2Text),
             "O2:%d%%", shownSpO2);
  } else {
    snprintf(spo2Text, sizeof(spo2Text),
             "O2:--");
  }

  showScreen("PULSE OXIMETER",
             bpmText, spo2Text);
}

// ======================================
// BPM CALCULATION
// ======================================
void calculateBPM(uint32_t irValue,
                  unsigned long sampleTime) {

  if (checkForBeat(irValue)) {
    Serial.println("BEAT DETECTED");

    if (lastBeat != 0) {
      unsigned long delta = sampleTime - lastBeat;

      if (delta > 0) {
        float calculatedBPM = 60000.0f / delta;

        if (calculatedBPM >= 40 &&
            calculatedBPM <= 220) {

          currentBPM = calculatedBPM;

          bpmHistory[rateSpot] = currentBPM;
          rateSpot = (rateSpot + 1) % RATE_SIZE;

          if (rateCount < RATE_SIZE) {
            rateCount++;
          }

          float total = 0;

          for (byte i = 0; i < rateCount; i++) {
            total += bpmHistory[i];
          }

          averageBPM = total / rateCount;
        }
      }
    }

    lastBeat = sampleTime;
  }
}

// ======================================
// SPO2 CALCULATION
// ======================================
void calculateSpO2() {

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    SAMPLE_COUNT,
    redBuffer,
    &spo2,
    &validSpO2,
    &algorithmBPM,
    &validAlgorithmBPM
  );

  if (validSpO2 &&
      spo2 >= 70 &&
      spo2 <= 100) {
    shownSpO2 = spo2;
  } else {
    shownSpO2 = -1;
  }

  Serial.print("RAW_SPO2=");
  Serial.print(spo2);

  Serial.print(" VALID_SPO2=");
  Serial.print((int)validSpO2);

  Serial.print(" ALGORITHM_BPM=");
  Serial.println(algorithmBPM);
}

void addSpO2Sample(uint32_t irValue,
                   uint32_t redValue) {

  // Downsample from 100 Hz to 25 Hz
  decimationCount++;

  if (decimationCount < DECIMATION) {
    return;
  }

  decimationCount = 0;

  irBuffer[bufferCount] = irValue;
  redBuffer[bufferCount] = redValue;
  bufferCount++;

  if (bufferCount >= SAMPLE_COUNT) {

    calculateSpO2();

    // Keep last 75 samples
    for (int i = 0; i < 75; i++) {
      irBuffer[i] = irBuffer[i + 25];
      redBuffer[i] = redBuffer[i + 25];
    }

    bufferCount = 75;
  }
}

// ======================================
// ACTUAL SAMPLE RATE TEST
// ======================================
void testSampleRate() {

  Serial.println("TESTING SAMPLE RATE...");

  sensor.clearFIFO();

  unsigned long start = millis();
  unsigned long count = 0;

  while (millis() - start < 5000) {

    sensor.check();

    while (sensor.available()) {
      sensor.nextSample();
      count++;
    }

    delay(1);
  }

  Serial.print("Actual sample rate: ");
  Serial.print(count / 5.0);
  Serial.println(" samples/sec");

  sensor.clearFIFO();
}

// ======================================
// SETUP
// ======================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  oledReady = display.begin(OLED_ADDR, true);

  if (oledReady) {
    Serial.println("OLED READY");
    showScreen("PULSE OXIMETER",
               "STARTING",
               "WAIT...");
  } else {
    Serial.println("OLED NOT FOUND");
  }

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 NOT FOUND");

    showScreen("SENSOR ERROR",
               "MAX30102",
               "NOT FOUND");

    while (true) delay(1000);
  }

  Serial.println("MAX30102 READY");

  // 100 SPS, no averaging
  sensor.setup(60, 1, 2, 100, 411, 4096);

  sensor.setPulseAmplitudeRed(0x24);
  sensor.setPulseAmplitudeIR(0x24);
  sensor.setPulseAmplitudeGreen(0);

  testSampleRate();

  resetReadings();

  Serial.println("SYSTEM READY");

  showScreen("PULSE OXIMETER",
             "PLACE",
             "FINGER");
}

// ======================================
// MAIN LOOP
// ======================================
void loop() {

  sensor.check();

  while (sensor.available()) {

    uint32_t irValue = sensor.getIR();
    uint32_t redValue = sensor.getRed();

    sensor.nextSample();

    unsigned long now = millis();

    // Finger detection
    if (irValue < FINGER_THRESHOLD) {

      if (fingerPresent &&
          now - lastFingerTime > 1000) {

        fingerPresent = false;
        resetReadings();

        Serial.println("NO FINGER");
      }

      continue;
    }

    lastFingerTime = now;

    if (!fingerPresent) {
      fingerPresent = true;
      resetReadings();

      Serial.println("FINGER DETECTED");
    }

    calculateBPM(irValue, now);
    addSpO2Sample(irValue, redValue);
  }

  if (millis() - lastDisplayTime >= 500) {
    lastDisplayTime = millis();

    updateOLED();

    Serial.print("IR=");
    Serial.print(sensor.getIR());

    Serial.print(" BPM=");
    Serial.print(currentBPM);

    Serial.print(" AVG_BPM=");
    Serial.print(averageBPM);

    Serial.print(" SPO2=");
    Serial.println(shownSpO2);
  }

  delay(1);
}
