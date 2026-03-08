/*
  Heart Rate Monitor
  Hardware: ESP32 + MAX30102 + SSD1306 (128x64)
  Both MAX30102 and SSD1306 share the I2C bus on GPIO 21 (SDA) / GPIO 22 (SCL)

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

// Waveform display area (rows 12–49 = 38px tall, leaving room for header + BPM footer)
#define WAVE_Y_START  12
#define WAVE_HEIGHT   38
#define WAVE_Y_END    (WAVE_Y_START + WAVE_HEIGHT)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MAX30105 particleSensor;

// Increased from 4 → 8 samples for a much smoother averaged BPM
const byte RATE_SIZE = 8;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;
byte samplesCollected = 0;
bool sensorReady = false;

// Scrolling waveform buffer (one Y value per pixel column)
uint8_t waveBuffer[SCREEN_WIDTH];

// Auto-scaling: tracks IR signal range to fill the waveform height.
// irMin starts high and irMax starts low so both get replaced on the first real reading.
long irMin = 300000;
long irMax = 0;

bool initSensor() {
  Wire.begin(21, 22);
  if (particleSensor.begin(Wire, I2C_SPEED_FAST)) return true;
  Serial.println(F("Fast I2C failed, retrying at standard speed..."));
  Wire.begin(21, 22);
  return particleSensor.begin(Wire, I2C_SPEED_STANDARD);
}

// Maps an IR value into a waveform Y offset (0 = top, WAVE_HEIGHT-1 = bottom).
// Updates running min/max for auto-scaling so the pulse fills the display.
uint8_t irToWaveY(long irValue) {
  if (irValue < irMin) irMin = irValue;
  if (irValue > irMax) irMax = irValue;
  long range = irMax - irMin;
  if (range < 500) return WAVE_HEIGHT / 2;  // flat line until signal stabilises
  long mapped = map(irValue, irMin, irMax, WAVE_HEIGHT - 1, 0);  // invert so peaks go up
  return (uint8_t)constrain(mapped, 0, WAVE_HEIGHT - 1);
}

void resetMeasurement() {
  rateSpot = 0;
  beatsPerMinute = 0;
  beatAvg = 0;
  samplesCollected = 0;
  memset(rates, 0, sizeof(rates));
  memset(waveBuffer, WAVE_HEIGHT / 2, sizeof(waveBuffer));
  irMin = 300000;
  irMax = 0;
}

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);
  memset(waveBuffer, WAVE_HEIGHT / 2, sizeof(waveBuffer));

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  // Attempt first sensor init; if it fails, loop() will keep retrying
  if (initSensor()) {
    particleSensor.setup(0x1F, 4, 2, 400, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
    particleSensor.setPulseAmplitudeGreen(0);
    sensorReady = true;
  }
}

void loop() {
  // Retry sensor init every 2 seconds until found
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
      particleSensor.setup(0x1F, 4, 2, 400, 411, 4096);
      particleSensor.setPulseAmplitudeRed(0x1F);
      particleSensor.setPulseAmplitudeIR(0x1F);
      particleSensor.setPulseAmplitudeGreen(0);
      resetMeasurement();
      sensorReady = true;
    }
    return;
  }

  long irValue = particleSensor.getIR();

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

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Heart Rate Monitor"));
  display.drawLine(0, 10, SCREEN_WIDTH - 1, 10, SSD1306_WHITE);

  if (irValue < 7000) {
    // Finger lifted — reset so next placement starts fresh
    resetMeasurement();
    display.setCursor(10, 22);
    display.println(F("No finger detected."));
    display.setCursor(18, 38);
    display.println(F("Place finger on"));
    display.setCursor(30, 50);
    display.println(F("sensor..."));
  } else {
    // Scroll waveform left and push the latest IR sample on the right
    memmove(waveBuffer, waveBuffer + 1, SCREEN_WIDTH - 1);
    waveBuffer[SCREEN_WIDTH - 1] = irToWaveY(irValue);

    // Draw the waveform as connected line segments
    for (int x = 0; x < SCREEN_WIDTH - 1; x++) {
      display.drawLine(x,     WAVE_Y_START + waveBuffer[x],
                       x + 1, WAVE_Y_START + waveBuffer[x + 1],
                       SSD1306_WHITE);
    }

    // Footer separator + BPM (only shown once enough samples are collected)
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
