/*
  Heart Rate Monitor
  Hardware: ESP32 + MAX30102 + SSD1306 (128x64)
  Both MAX30102 and SSD1306 share the I2C bus on GPIO 21 (SDA) / GPIO 22 (SCL)

  The MAX30102 is a PPG sensor. The raw IR signal drops when the heart beats
  (more blood → more light absorbed → lower reading). We apply a DC-removal
  high-pass filter, smooth with a moving average, invert the result, and
  auto-scale per-frame so the waveform always fills the display — matching
  the approach used by well-known open-source pulse oximeter projects.

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

// --- Signal processing ---
// DC removal high-pass filter state
float dcW = 0;        // filter memory
#define DC_ALPHA 0.95f // high-pass cutoff ~0.5 Hz at 50 sps — passes heartbeat, blocks DC drift

// Moving average filter for noise smoothing
#define MA_SIZE 4
int16_t maBuffer[MA_SIZE];
uint8_t maIndex = 0;
bool maFilled = false;

// Circular waveform buffer storing filtered+inverted values (raw int16 before display scaling)
#define WAVE_BUF_SIZE SCREEN_WIDTH
int16_t waveBuf[WAVE_BUF_SIZE];
uint8_t waveWriteIdx = 0;
bool waveFilled = false;    // true once the buffer has wrapped at least once

// Warmup: skip the first N samples while the DC filter settles
#define WARMUP_SAMPLES 50
int warmupCount = 0;

bool initSensor() {
  Wire.begin(21, 22);
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) return true;
  Serial.println(F("Fast I2C failed, retrying at standard speed..."));
  Wire.begin(21, 22);
  return particleSensor.begin(Wire, I2C_SPEED_STANDARD);
}

// DC-removal high-pass filter.
// Removes the large constant IR baseline so only the pulsatile AC component remains.
int16_t dcRemove(long rawValue) {
  float x = (float)rawValue;
  float oldW = dcW;
  dcW = x + DC_ALPHA * oldW;
  return (int16_t)(dcW - oldW);
}

// Moving average filter — smooths high-frequency noise from the AC signal.
int16_t maFilter(int16_t input) {
  maBuffer[maIndex] = input;
  maIndex = (maIndex + 1) % MA_SIZE;
  if (!maFilled && maIndex == 0) maFilled = true;
  int16_t count = maFilled ? MA_SIZE : maIndex;
  if (count == 0) return input;
  long sum = 0;
  for (int i = 0; i < count; i++) sum += maBuffer[i];
  return (int16_t)(sum / count);
}

void resetMeasurement() {
  rateSpot = 0;
  beatsPerMinute = 0;
  beatAvg = 0;
  samplesCollected = 0;
  dcW = 0;
  maIndex = 0;
  maFilled = false;
  memset(maBuffer, 0, sizeof(maBuffer));
  memset(waveBuf, 0, sizeof(waveBuf));
  waveWriteIdx = 0;
  waveFilled = false;
  warmupCount = 0;
}

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  if (initSensor()) {
    // Higher LED power (0x7F = 25.4mA) for stronger signal and more reliable beat detection
    particleSensor.setup(0x7F, 4, 2, 400, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x7F);
    particleSensor.setPulseAmplitudeIR(0x7F);
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
      particleSensor.setup(0x7F, 4, 2, 400, 411, 4096);
      particleSensor.setPulseAmplitudeRed(0x7F);
      particleSensor.setPulseAmplitudeIR(0x7F);
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

  // --- Signal processing for waveform ---
  int16_t dcFiltered = dcRemove(irValue);
  int16_t smoothed = maFilter(dcFiltered);
  // Invert: PPG drops during a heartbeat, inverting gives upward peaks
  int16_t inverted = -smoothed;

  // Let the DC filter settle before recording to the wave buffer
  if (warmupCount < WARMUP_SAMPLES) {
    warmupCount++;
  } else {
    waveBuf[waveWriteIdx] = inverted;
    waveWriteIdx = (waveWriteIdx + 1) % WAVE_BUF_SIZE;
    if (!waveFilled && waveWriteIdx == 0) waveFilled = true;
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
  } else if (warmupCount < WARMUP_SAMPLES) {
    display.setCursor(20, 28);
    display.println(F("Calibrating..."));
  } else {
    // Read the circular buffer in display order (oldest → newest, left → right)
    int count = waveFilled ? WAVE_BUF_SIZE : waveWriteIdx;
    if (count < 2) { display.display(); delay(20); return; }

    // Per-frame auto-scaling: find min/max across the entire visible buffer
    int16_t vMin = 32767, vMax = -32768;
    for (int i = 0; i < count; i++) {
      int idx = waveFilled ? (waveWriteIdx + i) % WAVE_BUF_SIZE : i;
      if (waveBuf[idx] < vMin) vMin = waveBuf[idx];
      if (waveBuf[idx] > vMax) vMax = waveBuf[idx];
    }

    int16_t range = vMax - vMin;
    if (range < 10) range = 10;  // avoid divide-by-zero when signal is flat

    // Map each sample to a display Y coordinate
    int xOffset = SCREEN_WIDTH - count;  // right-justify if buffer not full yet
    int prevY = -1;
    for (int i = 0; i < count; i++) {
      int idx = waveFilled ? (waveWriteIdx + i) % WAVE_BUF_SIZE : i;
      // Scale to display: vMax→top (WAVE_Y_START), vMin→bottom (WAVE_Y_END-1)
      int y = WAVE_Y_END - 1 - (int)((long)(waveBuf[idx] - vMin) * (WAVE_HEIGHT - 1) / range);
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

  display.display();
  delay(20);
}
