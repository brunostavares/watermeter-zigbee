# 💧 WaterMeter Zigbee

### Low-power Zigbee water meter based on ESP32-C6

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.2.x-E7352C?logo=espressif)](https://github.com/espressif/esp-idf)
[![Platform](https://img.shields.io/badge/Platform-ESP32--C6-blue)](https://www.espressif.com/en/products/socs/esp32-c6)
[![Zigbee](https://img.shields.io/badge/Zigbee-3.0-green)](https://www.zigbee2mqtt.io/)
[![License](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0.html)

A custom **Zigbee water meter** built around the **ESP32-C6**, using a **CNY70 reflective optical sensor** to detect the movement of a mechanical water meter.

The device counts water consumption, stores the accumulated value in non-volatile memory and reports the measurement through the **Zigbee Simple Metering cluster**.

It integrates with **Zigbee2MQTT** through a custom external converter.

---

> [!NOTE]
> This project is a working prototype under active development. The current priority is reliable water measurement, Zigbee communication and low-power operation.

---

## 📚 Contents

- [✨ Features](#-features)
- [⚙️ How It Works](#️-how-it-works)
- [🔌 Hardware](#-hardware)
- [📐 Measurement](#-measurement)
- [📡 Zigbee](#-zigbee)
- [📲 Zigbee2MQTT](#-zigbee2mqtt)
- [😴 Low Power](#-low-power)
- [💾 Data Persistence](#-data-persistence)
- [🚀 Getting Started](#-getting-started)
- [🛠️ Building](#️-building)
- [🔥 Flashing](#-flashing)
- [📁 Project Structure](#-project-structure)
- [🧪 Project Status](#-project-status)
- [⚠️ Current Limitations](#️-current-limitations)
- [📄 License](#-license)
- [👤 Author](#-author)

---

## ✨ Features

- 💧 Mechanical water-meter monitoring
- 🔴 CNY70 reflective optical sensor
- 📈 ADC-based detection
- 🔁 Hysteresis for reliable rotation detection
- 💾 Persistent accumulated counter using NVS
- 📡 Zigbee End Device
- 📊 Zigbee Simple Metering cluster
- 🔢 `CurrentSummationDelivered` support
- 📤 Zigbee attribute reporting
- 😴 ESP32-C6 Light Sleep
- 📲 Zigbee2MQTT integration
- 🔌 Custom `watermeter.mjs` external converter
- 🏠 Home Assistant compatible through Zigbee2MQTT

---

## ⚙️ How It Works

The WaterMeter detects the movement of the mechanical water meter using a **CNY70 reflective optical sensor**.

The sensor is connected to the ESP32-C6 ADC and periodically sampled.

```text
        Mechanical
        Water Meter
             │
             ▼
          CNY70
             │
             │ ADC
             ▼
         ESP32-C6
             │
       ┌─────┴─────┐
       │            │
       ▼            ▼
      NVS         Zigbee
       │            │
       │            ▼
       │       Zigbee2MQTT
       │            │
       │            ▼
       │      Home Assistant
       │
       ▼
 Accumulated
  consumption
```

Each detected rotation adds **10 liters** to the accumulated volume.

```text
1 rotation  = 10 L
100 rotations = 1,000 L
1,000 L      = 1 m³
```

---

## 🔌 Hardware

### Main components

| Component | Function |
|---|---|
| **ESP32-C6** | Microcontroller + Zigbee radio |
| **CNY70** | Reflective optical sensor |
| Mechanical water meter | Source of the measured movement |
| LED | CNY70 illumination |
| Resistor | LED current limiting |

### GPIO assignment

| Function | GPIO |
|---|---:|
| CNY70 LED | `GPIO4` |
| CNY70 analog output | `GPIO0` / ADC |

The CNY70 is read through the ESP32-C6 ADC.

---

## 📐 Measurement

The current firmware is configured for:

```text
┌─────────────────────────────┐
│ 1 mechanical rotation       │
│            ↓                │
│        +10 liters           │
└─────────────────────────────┘
```

### Sensor sampling

```text
Sampling interval:     10 seconds
LED stabilization:     20 ms
```

### ADC hysteresis

Two thresholds are used to make the detection more resistant to noise:

```text
ADC < 300    → Active
ADC > 1000   → Inactive
```

The transition from **inactive → active** is used to detect a rotation.

This prevents small fluctuations around a single threshold from generating false measurements.

---

## 📡 Zigbee

The WaterMeter operates as a **Zigbee End Device**.

The firmware uses Espressif's Zigbee stack and exposes a metering endpoint compatible with the Home Automation profile.

### Endpoint

```text
Endpoint: 1
```

### Clusters

The endpoint currently contains:

| Cluster | Purpose |
|---|---|
| Basic | Basic device information |
| Identify | Zigbee identification |
| Simple Metering | Water consumption |

---

### 💧 Simple Metering

The main measurement is provided through:

```text
CurrentSummationDelivered
```

This attribute contains the accumulated water consumption.

The meter is configured as:

```text
Device type: Water Meter
Unit:        m³
Multiplier:  1
Divisor:     1000
```

The firmware also enables reporting access for `CurrentSummationDelivered`.

---

## 📲 Zigbee2MQTT

The WaterMeter integrates with **Zigbee2MQTT** using a custom external converter.

```text
external_converters/
└── watermeter.mjs
```

The converter identifies the device as:

```text
Model:  WaterMeter
Vendor: TavaresLAB
```

It listens to the Zigbee:

```text
seMetering
```

cluster and processes:

```text
currentSummDelivered
```

---

## 🔌 External Converter

The file:

```text
external_converters/watermeter.mjs
```

is responsible for translating the raw Zigbee metering value into convenient Zigbee2MQTT entities.

### Exposed values

| Property | Unit | Description |
|---|---|---|
| `water_consumed` | `m³` | Total accumulated consumption |
| `water_consumed_liters` | `L` | Total accumulated consumption |

Example:

```text
water_consumed:        12.340 m³
water_consumed_liters: 12340 L
```

Both values represent **total accumulated consumption**, not instantaneous flow.

---

### 🔢 U48 conversion

The Zigbee `CurrentSummationDelivered` attribute is represented as a **48-bit unsigned integer (U48)**.

The external converter includes a parser capable of handling the U48 value when received as two 32-bit values.

Conceptually:

```text
U48
 │
 ├── Low 32 bits
 │
 └── High 16 bits
        │
        ▼
 Accumulated liters
        │
        ├───────────────┐
        ▼               ▼
       L                m³
```

The converter uses:

```text
1,000 L = 1 m³
```

and exposes the cubic-meter value with three decimal places.

---

## 📤 Zigbee Reporting

When the device is configured, the external converter:

1. Gets endpoint `1`
2. Binds the `seMetering` cluster to the coordinator
3. Reads the metering multiplier/divisor
4. Configures reporting for `currentSummDelivered`
5. Performs an initial read of the accumulated value

The current reporting configuration is:

```javascript
{
    min: 0,
    max: 3600,
    change: 1
}
```

This allows changes in the accumulated measurement to be reported to Zigbee2MQTT.

---

## 🏠 Home Assistant

The intended complete data path is:

```text
┌──────────────────┐
│ Mechanical Meter │
└────────┬─────────┘
         │
         ▼
     ┌───────┐
     │ CNY70 │
     └───┬───┘
         │
         ▼
    ┌──────────┐
    │ ESP32-C6 │
    └────┬─────┘
         │
       Zigbee
         │
         ▼
   ┌─────────────┐
   │ Zigbee2MQTT │
   └──────┬──────┘
          │
          ▼
   ┌──────────────┐
   │ Home Assistant│
   └──────────────┘
```

The external converter provides the water-consumption entities that can then be used by Home Assistant.

---

## 😴 Low Power

The WaterMeter is designed to operate as a low-power Zigbee End Device.

The firmware uses the ESP32-C6 **Light Sleep** functionality provided by ESP-IDF power management.

During the initial Zigbee connection and communication process, Light Sleep is temporarily prevented.

Once the connection process is completed, the power-management lock can be released.

The goal is to minimize power consumption while maintaining reliable Zigbee communication and periodic sensor measurement.

---

## 💾 Data Persistence

The accumulated water-meter counter is stored in **NVS (Non-Volatile Storage)**.

The firmware uses:

```text
Namespace:
water_meter

Key:
total_voltas
```

This allows the accumulated measurement to survive a reboot or power interruption.

```text
Water measurement
       │
       ▼
   Counter
       │
       ├──────────────► Zigbee
       │
       ▼
      NVS
       │
       ▼
 Persistent value
```

---

## 🚀 Getting Started

### Requirements

You will need:

- ESP32-C6 development board
- CNY70 sensor
- Mechanical water meter
- ESP-IDF
- USB connection for programming
- Zigbee coordinator
- Zigbee2MQTT
- Home Assistant (optional)

### Software versions

The current development environment uses:

```text
ESP-IDF:       5.2.6
Target:        ESP32-C6
esp-zigbee-lib: >= 2.0.0
```

---

## 📥 Clone the repository

```bash
git clone https://github.com/brunostavares/watermeter-zigbee.git
cd watermeter-zigbee
```

---

## 🛠️ Building

Set the target to ESP32-C6:

```bash
idf.py set-target esp32c6
```

Configure the project if necessary:

```bash
idf.py menuconfig
```

Build the firmware:

```bash
idf.py build
```

---

## 🔥 Flashing

Connect the ESP32-C6 to the computer and identify the serial port.

Then:

```bash
idf.py -p COMx flash
```

Example:

```bash
idf.py -p COM9 flash
```

To build, flash and open the serial monitor:

```bash
idf.py -p COM9 flash monitor
```

---

## 🖥️ Serial Monitor

To open the ESP-IDF monitor:

```bash
idf.py -p COM9 monitor
```

The firmware logs information related to:

- ESP32 startup
- Zigbee initialization
- Network commissioning
- Zigbee connection
- ADC measurements
- Rotation detection
- Accumulated consumption
- NVS
- Zigbee reports
- Power management

---

## 📲 Installing the Zigbee2MQTT Converter

Copy:

```text
external_converters/watermeter.mjs
```

to the Zigbee2MQTT `external_converters` directory.

Example:

```text
zigbee2mqtt/
├── configuration.yaml
├── database.db
├── state.json
└── external_converters/
    └── watermeter.mjs
```

Make sure the converter is included in the Zigbee2MQTT configuration when required by your installation.

For example:

```yaml
external_converters:
  - watermeter.mjs
```

Restart Zigbee2MQTT after adding or modifying the converter.

The WaterMeter can then be paired with the Zigbee network.

---

## 📁 Project Structure

```text
watermeter/
│
├── CMakeLists.txt
├── README.md
├── LICENSE
├── partitions.csv
│
├── sdkconfig.ci.enable_debug
├── sdkconfig.defaults
├── sdkconfig.defaults.esp32c6
├── sdkconfig.mbedtls_minimal
│
├── external_converters/
│   └── watermeter.mjs
│
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild
    ├── alarm_timer.h
    ├── idf_component.yml
    ├── watermeter.c
    └── watermeter.h
```

### Main files

| File | Purpose |
|---|---|
| `main/watermeter.c` | Main ESP32-C6 firmware |
| `main/watermeter.h` | Zigbee configuration |
| `main/alarm_timer.h` | Alarm timer interface |
| `main/idf_component.yml` | ESP-IDF component dependencies |
| `external_converters/watermeter.mjs` | Zigbee2MQTT external converter |
| `partitions.csv` | ESP32 partition table |
| `sdkconfig.defaults*` | ESP-IDF configuration |
| `LICENSE` | Project license |

---

## 🧪 Project Status

> [!WARNING]
> This project is still under development.

### Currently implemented

- [x] ESP32-C6 firmware
- [x] CNY70 sensor
- [x] ADC measurement
- [x] Sensor hysteresis
- [x] Water rotation detection
- [x] Accumulated water counter
- [x] NVS persistence
- [x] Zigbee End Device
- [x] Simple Metering cluster
- [x] `CurrentSummationDelivered`
- [x] Zigbee reporting
- [x] Light Sleep
- [x] Zigbee2MQTT external converter
- [x] Liter and m³ values

### Future improvements

- [ ] Further battery-life optimization
- [ ] Long-term sensor validation
- [ ] Improved calibration
- [ ] Configurable meter conversion factor
- [ ] Additional Zigbee metering attributes
- [ ] Further Light Sleep optimization
- [ ] Additional diagnostics

---

## ⚠️ Current Limitations

The current firmware is configured specifically for the water-meter mechanism used during development.

The conversion is currently:

```text
1 rotation = 10 liters
```

Changing to another water-meter model may require adjusting the conversion factor and sensor installation.

The project should therefore be considered a **prototype**, rather than a universal water-meter solution.

---

## 📄 License

This project is licensed under the:

**GNU General Public License v3.0 (GPLv3)**

See the [`LICENSE`](LICENSE) file for the complete license text.

---

## 👤 Author

**Bruno Tavares**

GitHub:

https://github.com/brunostavares/watermeter-zigbee

---

## 💧 Why WaterMeter?

The goal is simple:

> **Turn a conventional mechanical water meter into a low-power Zigbee device.**

```text
        💧
        │
        ▼
 Mechanical
 Water Meter
        │
        ▼
      CNY70
        │
        ▼
    ESP32-C6
        │
        ▼
      Zigbee
        │
        ▼
   Zigbee2MQTT
        │
        ▼
 Home Assistant
```

**Simple hardware. Reliable measurement. Zigbee.**

💧📡