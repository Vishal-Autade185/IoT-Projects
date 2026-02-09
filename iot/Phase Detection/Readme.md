# 3-Phase Power Failure Monitoring System (Firmware)

💻 Code Overview
This firmware runs on an ESP32 microcontroller to monitor the status of three-phase electrical lines (R, Y, B). It interfaces with a SIM800L GSM module to send real-time SMS alerts and make voice calls to registered phone numbers when a phase failure or complete blackout is detected.

⚙️ Key Functionalities

1. Phase Monitoring
* Continuous polling of digital pins 12 (R), 14 (Y), and 13 (B).
* Logic: Detects if any single phase goes down or if there is a complete power failure (all 3 phases down).

2. Alert System
* SMS Alerts: Sends a warning SMS after **10 seconds** of continuous failure.
    * Example Msg: "WARNING: Phase Failure - R Fuse Out"
    * Example Msg: "CRITICAL: Complete Power Failure - All Phases Down"
* Call Alerts: Initiates a phone call to registered users after **60 seconds** of failure to ensure the alert is noticed.
* Recovery Alert: Automatically sends a "SYSTEM RECOVERED" SMS when power is restored.

3. Remote User Management (via SMS)
Users can add or remove phone numbers dynamically by sending SMS commands to the device. Numbers are saved in **EEPROM**, so they survive power resets.
* Register Number: Send `REGISTER +91XXXXXXXXXX`
* Unregister Number: Send `UNREGISTER +91XXXXXXXXXX`
* List Numbers: Send `LIST` to see all stored contacts.

🛠️ Tech Stack
* Microcontroller: ESP32
* Communication: SIM800L (UART Serial)
* Storage: EEPROM (For persistent phone number storage)
