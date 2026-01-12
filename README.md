# Pulsar — ESP32 Heart-Rate Monitor + Wi-Fi Telemetry + LCD UI

Pulsar is an ESP32 firmware project that reads an analog PPG pulse sensor, computes a stable BPM (beats per minute) in real time, shows it on a 20x4 I2C LCD, and streams BPM data over Wi-Fi to a PC for logging and further processing.

The project also includes an optional PC-side **label-free adaptive filter** that improves BPM stability/accuracy and sends a corrected BPM back to the ESP32 for display.

> Note: This is a prototype for engineering and research. It is **not** a medical device and is not intended for clinical diagnosis.

---

## Features

- **Real-time BPM computation** from an analog PPG sensor (ESP-IDF, C++)
- **Digital preprocessing**: baseline/DC removal, smoothing, envelope tracking, adaptive thresholding, peak detection, refractory timing, IBI averaging
- **FreeRTOS task architecture** (sampling, detection, UI, telemetry)
- **Friendly LCD UI** on 20x4 I2C LCD (PCF8574 backpack), including:
  - BPM display
  - blinking heart icon
  - critical alarm takeover screen + backlight flashing
  - NO_SIGNAL treated as status (non-critical)
- **Alarm logic** (BPM-based): bradycardia, tachycardia, rapid change, no signal (status)
- **Wi-Fi UDP telemetry**: ESP → PC
- **Optional adaptive correction loop**: PC computes corrected BPM → ESP displays it

---

## Hardware

### Required
- ESP32 dev board
- Analog pulse sensor (3-wire: VCC / GND / Analog)
- 20x4 I2C LCD2004 module with PCF8574 backpack (common address `0x27`)

### Typical wiring (example)
#### Pulse sensor
- Sensor VCC → 3.3V
- Sensor GND → GND
- Sensor Analog Out → GPIO34 (ADC1_CH6)

#### LCD (I2C)
- LCD VCC → 3.3V (recommended if you don’t use level shifting)
- LCD GND → GND
- LCD SDA → GPIO21
- LCD SCL → GPIO22

> If powering the LCD backpack from **5V**, ensure SDA/SCL levels are safe for ESP32 (level shifter recommended). Many backpacks pull up SDA/SCL to VCC.

---

## Repository Structure (Components)

Typical ESP-IDF component layout.

---

## Build & Flash (ESP-IDF)

### Prerequisites
- ESP-IDF v5.x installed (project developed on v5.5.x)
- Python environment set up by ESP-IDF tools
- USB drivers for your ESP32 board

### Build
From the repo root

### Using the Project
1) Power up and verify BPM on LCD
-Place a finger steadily on the sensor.
-The LCD shows BPM with a blinking heart icon.
-If no contact: it shows guidance and/or NO_SIGNAL status.

2) Verify alarms
=Alarms are BPM-based and include persistence/hysteresis.
=Critical alarms use a takeover screen and flashing backlight.
=NO_SIGNAL is a non-critical status (no takeover).

---

## PC Telemetry (ESP → PC)

Pulsar sends UDP packets containing BPM telemetry.
This mode improves BPM stability/accuracy without needing ground-truth labels.
PC computes bpm_corr from (bpm_raw, quality, stable) and sends it back to ESP over UDP.

-Run the adaptive bridge
From tools/python/: python run_adaptive_bridge.py

-What it does:
Listens on UDP 7777 for ESP packets
Writes a CSV log on the PC
Sends corrected BPM back to the ESP on UDP 7778
ESP LCD displays corrected BPM (fallback to raw if no corrected value is available)

---

## Logging
PC scripts generate CSV logs containing fields like:
-pc_time_iso
-src_ip
-t_ms
-bpm_raw
-quality
-stable
-alarm_type
-bpm_corr (adaptive)
These logs can be used for offline analysis, plotting, or future supervised training if a reference device becomes available.

---

## Troubleshooting
1-LCD shows backlight but no characters:
-Adjust LCD contrast potentiometer (very common).
-Confirm I2C address (0x27 or 0x3F typical).
-Confirm SDA/SCL pins and shared ground.
-Avoid 5V pull-ups to SDA/SCL without a level shifter.

2-Wi-Fi connects but PC receives nothing
-Verify PC IPv4 address and UDP destination IP in firmware.
-Check Windows Firewall (allow UDP port).
-Ensure ESP32 and PC are on same network (no client isolation).

3-BPM is unstable
-Improve sensor contact stability (steady pressure, reduce ambient light).
-Use decoupling (0.1 µF + 10 µF across sensor VCC/GND near sensor).
-Verify sampling rate and ADC settings.

---

## License
MIT License
