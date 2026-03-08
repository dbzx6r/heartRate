/*
  Heart Rate Monitor
  Hardware: ESP32 + MAX30102 + SSD1306 (128x64)
  Both MAX30102 and SSD1306 share the I2C bus on GPIO 21 (SDA) / GPIO 22 (SCL)

  Waveform approach: store raw IR readings in a circular buffer. At draw
  time, compute the buffer mean (= DC baseline), subtract it from each
  sample to isolate the pulse (AC component), invert (PPG drops on
  heartbeat), and auto-scale to fill the display. No online filters
  needed — just arithmetic on the buffered data.

  Libraries required:
    - SparkFun MAX3010x Pulse and Proximity Sensor Library
    - Adafruit SSD1306
    - Adafruit GFX Library
*/

#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

#define WAVE_Y_START  12
#define WAVE_HEIGHT   38
#define WAVE_Y_END    (WAVE_Y_START + WAVE_HEIGHT)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MAX30105 particleSensor;

const byte RATE_SIZE = 8;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;
byte samplesCollected = 0;
bool sensorReady = false;

// Raw IR circular buffer — DC removal and scaling happen at draw time
#define WAVE_BUF_SIZE SCREEN_WIDTH
uint32_t rawBuf[WAVE_BUF_SIZE];
uint8_t  bufWriteIdx = 0;
bool     bufFilled = false;

bool initSensor() {
  Wire.begin(21, 22);
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) return true;
  Serial.println(F("Fast I2C failed, retrying at standard speed..."));
  Wire.begin(21, 22);
  return particleSensor.begin(Wire, I2C_SPEED_STANDARD);
}

void resetMeasurement() {
  rateSpot = 0;
  beatsPerMinute = 0;
  beatAvg = 0;
  samplesCollected = 0;
  bufWriteIdx = 0;
  bufFilled = false;
  memset(rawBuf, 0, sizeof(rawBuf));
}

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  if (initSensor()) {
    // sampleAverage=8 for cleaner signal from cheap sensors
    particleSensor.setup(0x1F, 8, 2, 400, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
    particleSensor.setPulseAmplitudeGreen(0);
    sensorReady = true;
  }
}

void loop() {
  if (!sensorReady) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Heart Rate Monitor"));
    display.drawLine(0, 10, SCREEN_WIDTH - 1, 10, SSD1306_WHITE);
    display.setCursor(10, 25);
    display.println(F("Sensor not found."));
    display.setCursor(10, 40);
    display.println(F("Check wiring..."));
    display.display();
    delay(2000);

    if (initSensor()) {
      particleSensor.setup(0x1F, 8, 2, 400, 411, 4096);
      particleSensor.setPulseAmplitudeRed(0x1F);
      particleSensor.setPulseAmplitudeIR(0x1F);
      particleSensor.setPulseAmplitudeGreen(0);
      resetMeasurement();
      sensorReady = true;
    }
    return;
  }

  long irValue = particleSensor.getIR();

  // BPM calculation via SparkFun beat detector
  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    beatsPerMinute = 60.0 / (delta / 1000.0);

    if (beatsPerMinute > 20 && beatsPerMinute < 255) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      if (samplesCollected < RATE_SIZE) samplesCollected++;

      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
  }

  // --- Draw ---
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Heart Rate Monitor"));
  display.drawLine(0, 10, SCREEN_WIDTH - 1, 10, SSD1306_WHITE);

  if (irValue < 7000) {
    resetMeasurement();
    display.setCursor(10, 22);
    display.println(F("No finger detected."));
    display.setCursor(18, 38);
    display.println(F("Place finger on"));
    display.setCursor(30, 50);
    display.println(F("sensor..."));
  } else {
    // Store raw IR reading in circular buffer
    rawBuf[bufWriteIdx] = (uint32_t)irValue;
    bufWriteIdx = (bufWriteIdx + 1) % WAVE_BUF_SIZE;
    if (!bufFilled && bufWriteIdx == 0) bufFilled = true;

    int count = bufFilled ? WAVE_BUF_SIZE : bufWriteIdx;

    if (count >= 2) {
      // Compute buffer mean (= DC baseline) using 64-bit sum to avoid overflow
      uint64_t sum = 0;
      for (int i = 0; i < count; i++) {
        int idx = bufFilled ? (bufWriteIdx + i) % WAVE_BUF_SIZE : i;
        sum += rawBuf[idx];
      }
      long mean = (long)(sum / count);

      // Compute AC values: subtract mean, then invert (PPG drops on heartbeat)
      // Find min/max of inverted AC for auto-scaling
      long acMin = 0, acMax = 0;
      for (int i = 0; i < count; i++) {
        int idx = bufFilled ? (bufWriteIdx + i) % WAVE_BUF_SIZE : i;
        long ac = -(long)((long)rawBuf[idx] - mean);  // inverted
        if (ac < acMin) acMin = ac;
        if (ac > acMax) acMax = ac;
      }

      long range = acMax - acMin;
      if (range < 10) range = 10;

      // Draw waveform: map each AC value to display coordinates
      int xOffset = SCREEN_WIDTH - count;
      int prevY = -1;
      for (int i = 0; i < count; i++) {
        int idx = bufFilled ? (bufWriteIdx + i) % WAVE_BUF_SIZE : i;
        long ac = -(long)((long)rawBuf[idx] - mean);

        int y = WAVE_Y_END - 1 - (int)((ac - acMin) * (WAVE_HEIGHT - 1) / range);
        y = constrain(y, WAVE_Y_START, WAVE_Y_END - 1);

        int x = xOffset + i;
        if (prevY >= 0 && i > 0) {
          display.drawLine(x - 1, prevY, x, y, SSD1306_WHITE);
        }
        prevY = y;
      }

      // Footer
      display.drawLine(0, WAVE_Y_END + 1, SCREEN_WIDTH - 1, WAVE_Y_END + 1, SSD1306_WHITE);
      display.setCursor(0, WAVE_Y_END + 3);
      if (samplesCollected < RATE_SIZE) {
        display.print(F("BPM: Measuring..."));
      } else {
        display.print(F("BPM: "));
        display.print(beatAvg);
      }
    }
  }

  display.display();
  delay(20);
}
