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

const byte RATE_SIZE = 4;
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

// Peak detector state — operates on the same inverted AC signal as the waveform
long  pdPrevAC = 0;
bool  pdRising = false;
long  pdPeakVal = 0;
long  pdLastBeat = 0;
#define PD_MIN_INTERVAL 333  // ms, caps at ~180 BPM

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
  pdPrevAC = 0;
  pdRising = false;
  pdPeakVal = 0;
  pdLastBeat = 0;
}

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  if (initSensor()) {
    // sampleAverage=1: checkForBeat() needs a fast raw stream to detect peaks
    particleSensor.setup(0x1F, 1, 2, 400, 411, 4096);
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
      particleSensor.setup(0x1F, 1, 2, 400, 411, 4096);
      particleSensor.setPulseAmplitudeRed(0x1F);
      particleSensor.setPulseAmplitudeIR(0x1F);
      particleSensor.setPulseAmplitudeGreen(0);
      resetMeasurement();
      sensorReady = true;
    }
    return;
  }

  long irValue = particleSensor.getIR();

  // Store raw IR in buffer first so the mean used for beat detection
  // is identical to the mean used for drawing — if it shows on screen, it detects.
  if (irValue >= 7000) {
    rawBuf[bufWriteIdx] = (uint32_t)irValue;
    bufWriteIdx = (bufWriteIdx + 1) % WAVE_BUF_SIZE;
    if (!bufFilled && bufWriteIdx == 0) bufFilled = true;
  }

  int count = bufFilled ? WAVE_BUF_SIZE : bufWriteIdx;

  // Beat detection: find peaks in the same inverted AC signal the waveform shows.
  // We need at least half a buffer to have a stable mean.
  if (irValue >= 7000 && count >= WAVE_BUF_SIZE / 2) {
    uint64_t sum = 0;
    for (int i = 0; i < count; i++) {
      int idx = bufFilled ? (bufWriteIdx + i) % WAVE_BUF_SIZE : i;
      sum += rawBuf[idx];
    }
    long mean = (long)(sum / count);
    long ac = -(irValue - mean);  // inverted: rises when heart beats

    // Peak detection: when signal was rising and is now falling, we passed a peak.
    // Only register a beat if the peak was meaningfully above the midline and
    // enough time has passed since the last beat.
    if (pdRising && ac < pdPrevAC) {
      // Just passed a peak — pdPeakVal holds the peak height
      long range = 0;
      long rMin = 0, rMax = 0;
      for (int i = 0; i < count; i++) {
        int idx = bufFilled ? (bufWriteIdx + i) % WAVE_BUF_SIZE : i;
        long v = -(long)((long)rawBuf[idx] - mean);
        if (v < rMin) rMin = v;
        if (v > rMax) rMax = v;
      }
      range = rMax - rMin;

      // Only count as a beat if peak is in the upper 30% of the signal range
      if (range > 10 && pdPeakVal > (rMin + range * 7 / 10)) {
        long now = millis();
        if (now - pdLastBeat > PD_MIN_INTERVAL) {
          long delta = now - pdLastBeat;
          pdLastBeat = now;

          if (delta > 0) {
            float bpm = 60000.0f / delta;
            if (bpm > 20 && bpm < 255) {
              rates[rateSpot++] = (byte)bpm;
              rateSpot %= RATE_SIZE;
              if (samplesCollected < RATE_SIZE) samplesCollected++;
              beatAvg = 0;
              for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
              beatAvg /= RATE_SIZE;
            }
          }
        }
      }
      pdRising = false;
    } else if (ac > pdPrevAC) {
      pdRising = true;
      pdPeakVal = ac;
    }
    if (pdRising && ac > pdPeakVal) pdPeakVal = ac;
    pdPrevAC = ac;
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
    if (count >= 2) {
      // Compute buffer mean for display scaling
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
