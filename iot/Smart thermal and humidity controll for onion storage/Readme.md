# IoT Smart Onion Storage System (Firmware)

## 📌 Project Overview
This firmware powers an ESP32-based smart storage system designed to maintain optimal environmental conditions for onion preservation. It uses a DHT22 sensor to monitor temperature and humidity and automatically controls various actuators (heaters, blowers, exhaust fans, humidifiers) to keep the environment stable. The system features WiFi connectivity and MQTT integration for remote monitoring and control via a dashboard.

## ⚙️ Key Features

### 1. Intelligent Climate Control (Auto Mode)
The system automatically switches between different modes based on sensor readings:
* **Extreme Cooling:** Activates if temperature > 32.0°C. Uses Exhaust Fan + Humidifier. Target: 26.5°C.
* **Normal Cooling:** Activates if temperature > 29.0°C. Uses Normal Blower. Target: 26.5°C.
* **Heating:** Activates if temperature < 25.0°C. Uses Heater + Heater Blower. Target: 26.0°C.
* **Dehumidification:** Activates if humidity > 80%. Uses Heater + Heater Blower. Target: 70%.
* **Conflict Prevention:** Includes dead zones (25.5°C - 26.0°C) to prevent rapid toggling between heating and cooling.

### 2. Cyclic Ventilation
* Runs the exhaust fan in automatic cycles (default 5 minutes ON / 5 minutes OFF) to ensure fresh air circulation when no active cooling/heating is required.
* Cycle duration is remotely adjustable.

### 3. Remote Monitoring & Control (MQTT)
* **Real-time Data:** Publishes temperature and humidity every 5 seconds.
* **Status Updates:** Reports the status of all devices (ON/OFF) and the current operating mode.
* **Remote Commands:** Allows users to manually toggle devices, switch modes (Auto/Manual), enable Emergency Stop, or change cycle timers from a remote dashboard.

### 4. Local Interface
* **LCD Display (16x2):** Shows real-time temperature, humidity, WiFi/MQTT connection status, and current system mode.
* **Serial Monitor:** Provides detailed debugging logs for troubleshooting.

### 5. Safety Features
* **Emergency Stop:** A dedicated command immediately shuts down all actuators and locks the system until released.
* **Failsafes:** Prevents conflicting devices from running simultaneously (e.g., Heater and Cooler cannot be ON at the same time).

## 🛠️ Hardware Configuration
* **Microcontroller:** ESP32
* **Sensor:** DHT22 (Temperature & Humidity)
* **Actuators (via Relays/Drivers):**
    * Heater & Heater Blower
    * Normal Blower
    * Humidifier
    * Exhaust Fan
* **Display:** LiquidCrystal I2C (16x2)
* **Connectivity:** WiFi & MQTT (HiveMQ Broker)

## 🚀 How It Works
1.  The ESP32 connects to WiFi and the MQTT broker.
2.  It continuously reads temperature and humidity data.
3.  In Auto Mode, it compares readings against defined thresholds and activates the necessary devices to reach the target temperature (26.5°C) or humidity.
4.  It publishes live data to the MQTT topic `iot/onionstorage/data`.
5.  It listens for incoming control commands on `iot/onionstorage/control`.
