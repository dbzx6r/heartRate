/*
  Heart Rate Monitor
  Hardware: ESP32 + MAX30102 + SSD1306 (128x64)
  Both MAX30102 and SSD1306 share the I2C bus on GPIO 21 (SDA) / GPIO 22 (SCL)

  The MAX30102 is a PPG (photoplethysmography) sensor — it measures blood
  volume changes via reflected IR light. The raw signal is a smooth sine-like
  wave, NOT the sharp QRS spikes of an ECG. To get the classic hospital-monitor
  look, we synthesize an ECG-like PQRST waveform triggered by the library's
  beat detector.

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

// Waveform display area
#define WAVE_Y_START  12
#define WAVE_HEIGHT   38
#define WAVE_Y_END    (WAVE_Y_START + WAVE_HEIGHT)
#define BASELINE      (WAVE_HEIGHT - 4)  // baseline near the bottom of the wave area

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

// Scrolling waveform buffer (one Y-offset per pixel column, 0=top, WAVE_HEIGHT-1=bottom)
uint8_t waveBuffer[SCREEN_WIDTH];

// Synthetic ECG PQRST waveform template.
// Values are Y-offsets from top of wave area; BASELINE = resting line.
// Pattern: flat → small P bump → flat → sharp QRS spike → dip below baseline → gentle T wave → flat
#define ECG_LEN 28
const uint8_t ecgTemplate[ECG_LEN] PROGMEM = {
  BASELINE,                         //  0  flat
  BASELINE,                         //  1  flat
  BASELINE - 3,                     //  2  P wave rise
  BASELINE - 5,                     //  3  P wave peak
  BASELINE - 3,                     //  4  P wave fall
  BASELINE,                         //  5  flat (PR segment)
  BASELINE,                         //  6  flat
  BASELINE + 2,                     //  7  Q dip (small down)
  BASELINE - 10,                    //  8  R upstroke
  BASELINE - 28,                    //  9  R peak (big spike!)
  BASELINE - 10,                    // 10  R downstroke
  BASELINE + 3,                     // 11  S dip (below baseline)
  BASELINE + 2,                     // 12  S recovery
  BASELINE,                         // 13  flat (ST segment)
  BASELINE,                         // 14  flat
  BASELINE,                         // 15  flat
  BASELINE - 2,                     // 16  T wave rise
  BASELINE - 4,                     // 17  T wave
  BASELINE - 6,                     // 18  T wave peak
  BASELINE - 6,                     // 19  T wave peak
  BASELINE - 4,                     // 20  T wave fall
  BASELINE - 2,                     // 21  T wave fall
  BASELINE,                         // 22  flat
  BASELINE,                         // 23  flat
  BASELINE,                         // 24  flat
  BASELINE,                         // 25  flat
  BASELINE,                         // 26  flat
  BASELINE,                         // 27  flat
};

// Index into the ECG template; -1 means idle (drawing flat baseline)
int ecgIndex = -1;

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
  ecgIndex = -1;
  memset(rates, 0, sizeof(rates));
  memset(waveBuffer, BASELINE, sizeof(waveBuffer));
}

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);
  memset(waveBuffer, BASELINE, sizeof(waveBuffer));

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  if (initSensor()) {
    particleSensor.setup(0x1F, 4, 2, 400, 411, 4096);
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

  // Beat detection triggers the synthetic ECG waveform
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

    // Start playing the ECG template from the beginning
    ecgIndex = 0;
  }

  // Scroll the waveform buffer left by one pixel
  memmove(waveBuffer, waveBuffer + 1, SCREEN_WIDTH - 1);

  // Push the next waveform value: ECG template if active, otherwise flat baseline
  if (ecgIndex >= 0 && ecgIndex < ECG_LEN) {
    waveBuffer[SCREEN_WIDTH - 1] = pgm_read_byte(&ecgTemplate[ecgIndex]);
    ecgIndex++;
    if (ecgIndex >= ECG_LEN) ecgIndex = -1;  // done, back to flat
  } else {
    waveBuffer[SCREEN_WIDTH - 1] = BASELINE;
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
    // Draw waveform as connected line segments
    for (int x = 0; x < SCREEN_WIDTH - 1; x++) {
      display.drawLine(x,     WAVE_Y_START + waveBuffer[x],
                       x + 1, WAVE_Y_START + waveBuffer[x + 1],
                       SSD1306_WHITE);
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
