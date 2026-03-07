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

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MAX30105 particleSensor;

const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

void setup() {
  Serial.begin(115200);

  // Explicitly initialise I2C with ESP32 default pins before any peripheral
  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Heart Rate Monitor"));
  display.println(F("Initializing..."));
  display.display();

  // Try fast speed first, fall back to standard if the sensor isn't found
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("Fast I2C failed, retrying at standard speed..."));
    Wire.begin(21, 22);
    if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
      Serial.println(F("MAX30102 not found. Check wiring."));
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println(F("MAX30102 not found!"));
      display.println(F("Check wiring."));
      display.display();
      for (;;);
    }
  }

  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);

  delay(1000);
}

void loop() {
  long irValue = particleSensor.getIR();

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    beatsPerMinute = 60.0 / (delta / 1000.0);

    if (beatsPerMinute > 20 && beatsPerMinute < 255) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;

      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) {
        beatAvg += rates[x];
      }
      beatAvg /= RATE_SIZE;
    }
  }

  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Heart Rate Monitor"));
  display.drawLine(0, 10, SCREEN_WIDTH - 1, 10, SSD1306_WHITE);

  if (irValue < 50000) {
    display.setCursor(10, 22);
    display.setTextSize(1);
    display.println(F("No finger detected."));
    display.setCursor(18, 38);
    display.println(F("Place finger on"));
    display.setCursor(30, 50);
    display.println(F("sensor..."));
  } else {
    display.setCursor(0, 15);
    display.setTextSize(1);
    display.println(F("BPM:"));
    display.setCursor(0, 25);
    display.setTextSize(3);
    display.print((int)beatsPerMinute);

    display.setTextSize(1);
    display.setCursor(72, 15);
    display.println(F("Avg BPM:"));
    display.setCursor(72, 27);
    display.setTextSize(2);
    display.print(beatAvg);
  }

  display.display();
  delay(20);
}
