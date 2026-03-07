# Heart Rate Monitor

An ESP32-based heart rate monitor that reads pulse data from a **MAX30102** optical sensor and displays live BPM on a **SSD1306 OLED display** (128×64).

---

## Hardware Required

| Component | Notes |
|-----------|-------|
| ESP32-DevKitC (ESP-32D) | Main microcontroller |
| MAX30102 | Pulse oximetry / heart rate sensor |
| SSD1306 OLED Display | 128×64, I2C |
| Breadboard + jumper wires | |

---

## Wiring

Both the MAX30102 and SSD1306 communicate over **I2C** and share the same SDA/SCL pins on the ESP32.

### MAX30102 → ESP32

| MAX30102 Pin | ESP32 Pin |
|--------------|-----------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| INT | *(not used)* |

### SSD1306 → ESP32

| SSD1306 Pin | ESP32 Pin |
|-------------|-----------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

> **Note:** Both modules share GPIO 21 (SDA) and GPIO 22 (SCL). Connect all SDA lines together and all SCL lines together on the breadboard.

---

## Installing via Arduino IDE

### 1. Install the ESP32 Board Package

1. Open **Arduino IDE** and go to **File → Preferences**.
2. In the *Additional Boards Manager URLs* field, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools → Board → Boards Manager**, search for **esp32**, and install the package by **Espressif Systems**.

### 2. Install Required Libraries

Go to **Sketch → Include Library → Manage Libraries** and install the following:

| Library Name | Author |
|---|---|
| SparkFun MAX3010x Pulse and Proximity Sensor Library | SparkFun Electronics |
| Adafruit SSD1306 | Adafruit |
| Adafruit GFX Library | Adafruit |

### 3. Open the Sketch

1. Clone or download this repository.
2. Open `heartRate.ino` in the Arduino IDE.

### 4. Configure the Board

Go to **Tools** and set:

| Setting | Value |
|---------|-------|
| Board | ESP32 Dev Module |
| Upload Speed | 921600 |
| Port | *(your ESP32's COM/serial port)* |

### 5. Upload

Click the **Upload** button (→). Hold the **BOOT** button on the ESP32 if it doesn't enter flash mode automatically.

---

## Usage

1. Power on the device.
2. The display will show **"Heart Rate Monitor"** and initialize the sensor.
3. Place your **fingertip gently** on the MAX30102 sensor.
4. After a few seconds, your live **BPM** and a rolling **average BPM** will appear on the display.
5. If no finger is detected, the display will prompt you to place your finger on the sensor.

---

## Troubleshooting

| Issue | Fix |
|-------|-----|
| `MAX30102 not found!` on display | Check SDA/SCL wiring; ensure 3.3V (not 5V) power |
| Display stays blank | Verify SSD1306 I2C address is `0x3C` (some modules use `0x3D`) |
| BPM reads 0 or erratic | Keep finger still; avoid pressing too hard or too lightly |
| Upload fails | Hold BOOT button on ESP32 during upload |
