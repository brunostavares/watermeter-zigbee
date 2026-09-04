#💧 WaterMeter Zigbee

WaterMeter is a Zigbee water meter based on the ESP32-C6, designed to monitor water consumption using a CNY70 reflective optical sensor.

The device detects the movement of a mechanical water meter, counts the measured volume, stores the accumulated value in non-volatile memory and reports the measurement through Zigbee.

The project is developed using ESP-IDF and Espressif's Zigbee SDK, with an external converter for Zigbee2MQTT.

#🚧 Project status

Working prototype

The current firmware implements:

Water consumption measurement using a CNY70 sensor
ADC-based sensor reading
Hysteresis for reliable pulse detection
Accumulated volume counter
Persistence of the counter using NVS
Zigbee End Device configuration
Zigbee Simple Metering cluster
CurrentSummationDelivered attribute
Zigbee attribute reporting
Zigbee network commissioning
Light Sleep support
Periodic sensor sampling
Zigbee2MQTT support through an external converter
#🧠 How it works

The WaterMeter monitors the mechanical movement of the water meter using a CNY70 reflective optical sensor.

Each detected rotation of the water meter corresponds to:

1 rotation = 10 liters

The firmware periodically monitors the CNY70 through the ESP32-C6 ADC.

To avoid false detections caused by small variations in the sensor reading, two ADC thresholds are used:

ADC_TRUE_LIMIT = 300
ADC_FALSE_LIMIT = 1000

This creates a hysteresis region between the two thresholds.

When the sensor changes from the inactive state to the active state, the firmware registers one rotation and adds 10 liters to the accumulated volume.

The sensor is sampled every 10 seconds.

#📐 Measurement

The internal counter represents the number of detected water-meter rotations.

1 rotation = 10 liters

Therefore:

Total volume = number of rotations × 10 liters

The accumulated volume is exposed through the Zigbee Simple Metering cluster using the CurrentSummationDelivered attribute.

The meter is configured as a water meter.

The metering configuration uses:

Unit: m³
Multiplier: 1
Divisor: 1000
Device type: Water Metering

#🔌 Hardware
Main components
Component	Description
ESP32-C6	Main microcontroller and Zigbee radio
CNY70	Reflective optical sensor
Water meter	Mechanical water meter with rotating indicator
LED	CNY70 illumination
Resistor	LED current limiting / sensor circuit
ESP32-C6 connections
Function	GPIO
CNY70 LED	GPIO 4
CNY70 analog output	GPIO 0 / ADC

The CNY70 is read through the ESP32-C6 ADC.

#📡 Zigbee

The device operates as a Zigbee End Device.

The current firmware implements a Home Automation-compatible metering endpoint.

Endpoint
Endpoint: 1
Main clusters

The endpoint contains:

Basic
Identify
Simple Metering
Simple Metering

The main attribute used by the project is:

CurrentSummationDelivered

This attribute contains the accumulated water consumption.

The firmware configures this attribute for Zigbee reporting.

#📲 Zigbee2MQTT

The WaterMeter is designed to be used with Zigbee2MQTT through an external converter.

The converter is:

external_converters/watermeter.mjs

Zigbee2MQTT external converters allow support for devices that are not yet included in the standard device definitions. They use the same converter framework as built-in device definitions.

The external converter translates the WaterMeter's Zigbee data into the features exposed by Zigbee2MQTT.

External converter
external_converters/
└── watermeter.mjs

The converter is maintained together with the firmware in this repository so that a complete working version of the WaterMeter can be reproduced from the repository.

#⚙️ Installing the external converter

Copy:

watermeter.mjs

to the Zigbee2MQTT external converters directory.

The directory used by Zigbee2MQTT is:

external_converters/

at the same level as the Zigbee2MQTT configuration.yaml file.

For example:

zigbee2mqtt/
├── configuration.yaml
├── database.db
├── state.json
└── external_converters/
    └── watermeter.mjs

Recent Zigbee2MQTT versions have external JavaScript loading disabled by default for new installations, so enable_external_js may need to be enabled in the Zigbee2MQTT configuration.

After installing the converter, restart Zigbee2MQTT.

The device can then be paired with the Zigbee network.

#🔄 Zigbee reporting

When a new water-meter rotation is detected, the firmware:

Increments the accumulated rotation counter.
Updates the CurrentSummationDelivered attribute.
Stores the accumulated value in NVS.
Sends a Zigbee attribute report when the device is connected.

An initial measurement is also sent after the device establishes its Zigbee connection.

#🏠 Zigbee2MQTT → Home Assistant

The intended data flow is:

Mechanical water meter
        ↓
      CNY70
        ↓
     ESP32-C6
        ↓
      Zigbee
        ↓
   Zigbee2MQTT
        ↓
 Home Assistant

The external converter allows Zigbee2MQTT to correctly identify and expose the WaterMeter's metering information.

#😴 Light Sleep

The project is designed to operate as a low-power Zigbee End Device.

Light Sleep is enabled through ESP-IDF power management.

The firmware temporarily prevents Light Sleep while the Zigbee device is establishing its connection and completing the initial communication with the network.

After the connection process, the device releases the power-management lock and allows Light Sleep.

The objective is to reduce power consumption while maintaining the functionality required by the Zigbee End Device.

#💾 Data persistence

The accumulated water consumption is stored in the ESP32-C6's non-volatile storage (NVS).

The firmware uses the following NVS namespace:

water_meter

The accumulated rotation counter is stored under:

total_voltas

This allows the accumulated measurement to survive a restart or power interruption.

#🛠️ Software

The project is developed using:

ESP-IDF
Espressif Zigbee SDK
esp-zigbee-lib
FreeRTOS
ESP-IDF ADC driver
ESP-IDF NVS
ESP-IDF power-management APIs
Zigbee2MQTT external converter
ESP-IDF

Development and testing are currently performed using:

ESP-IDF 5.2.6
Target
ESP32-C6

#📦 Project structure
watermeter/
│
├── CMakeLists.txt
├── README.md
├── LICENSE
├── partitions.csv
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

#🚀 Building the firmware

Clone the repository:

git clone https://github.com/brunostavares/watermeter-zigbee.git
cd watermeter-zigbee

Set the ESP32-C6 target:

idf.py set-target esp32c6

Configure the project if necessary:

idf.py menuconfig

Build:

idf.py build

##🔥 Flashing the ESP32-C6

Connect the ESP32-C6 development board to the computer and identify the serial port.

Then flash the firmware:

idf.py -p COMx flash

For example:

idf.py -p COM9 flash

To build, flash and monitor:

idf.py -p COM9 flash monitor

##🖥️ Serial monitor

The firmware outputs diagnostic information through the ESP-IDF logging system.

To open the monitor:

idf.py -p COM9 monitor

The monitor can be used to observe:

ESP32 startup
Zigbee initialization
Network commissioning
Zigbee connection state
ADC measurements
Water-meter detection
Accumulated volume
Zigbee reports
Power-management events

##📊 Water measurement logic

The CNY70 is periodically illuminated and read through the ADC.

The firmware currently uses:

Sensor sampling interval: 10 seconds
LED stabilization time:   20 ms

The detection algorithm uses hysteresis:

ADC < 300   → sensor active
ADC > 1000  → sensor inactive

A water-meter rotation is counted when the sensor transitions from inactive to active.

Each detected rotation adds:

+10 liters

to the accumulated measurement.

##⚙️ Zigbee configuration

The project uses the Zigbee channel configuration provided through ESP-IDF Kconfig.

The following configuration values are used:

CONFIG_ZB_EXAMPLE_PRIMARY_CHANNEL
CONFIG_ZB_EXAMPLE_SECONDARY_CHANNEL_MASK

The Zigbee storage partition is:

nvs

and is initialized during application startup.

#🔋 Power considerations

The project is intended to eventually operate from batteries, making low-power operation an important part of the design.

The current firmware uses ESP-IDF Light Sleep and configures the ESP32-C6 as a Zigbee End Device.

Power consumption depends on:

Zigbee network conditions
Sleep/wake frequency
Sensor circuit
CNY70 LED current
Zigbee reporting frequency
Battery characteristics
Voltage regulator efficiency

Power optimization remains part of the ongoing development.

#🧪 Development notes

The repository represents the current working state of the WaterMeter prototype.

The fundamental operation is intentionally kept simple:

CNY70
   ↓
ADC
   ↓
Rotation detection
   ↓
Volume counter
   ↓
NVS
   ↓
Zigbee Simple Metering
   ↓
External Converter
   ↓
Zigbee2MQTT
   ↓
Home Assistant

The priority is reliable measurement and reliable Zigbee communication before adding additional functionality.

#📋 Current limitations

The project is still under development.

Areas that may be improved in future revisions include:

Further battery-life optimization
More extensive sensor calibration
Long-term measurement validation
Configuration of different water-meter conversion factors
Additional Zigbee attributes
Improved device identification
Further testing of Light Sleep behavior
Additional diagnostics and error handling
Integration of the converter into Zigbee2MQTT's official device definitions

#📄 License

This project is licensed under the:

GNU General Public License v3 (GPL v3)

See the LICENSE file for the complete license text.

The GPLv3 is a free software license and a copyleft license published by the Free Software Foundation.

#👤 Author

Bruno Tavares

GitHub repository:

WaterMeter Zigbee — GitHub

#💧 Project goal

The goal of the project is to transform a conventional mechanical water meter into a low-power Zigbee water meter capable of providing accumulated water-consumption data to a home automation system.

       Mechanical
       water meter
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

Simple hardware. Simple measurement. Zigbee. 💧📡