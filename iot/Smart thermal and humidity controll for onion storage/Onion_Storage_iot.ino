#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"
#include <ArduinoJson.h>

// --- Wi-Fi Credentials ---
const char* ssid = "";
const char* password = "";

// --- MQTT Properties ---
const char* mqtt_server = "broker.hivemq.com";
const int   mqtt_port = 1883;
const char* mqtt_clientId = "ESP32_Onion_Controller_Smart_V3";

// --- MQTT TOPICS ---
const char* topic_control_subscribe = "iot/onionstorage/control";
const char* topic_data_publish = "iot/onionstorage/data";
const char* topic_status_publish = "iot/onionstorage/status";

// --- UPDATED Climate Control Thresholds with 26.5°C Target ---
const float TEMP_NORMAL_COOLING     = 29.0;   // Normal blower starts
const float TEMP_NORMAL_COOLING_OFF = 26.5;   // Normal blower stops → 26.5°C TARGET
const float TEMP_EXTREME_COOLING    = 32.0;   // Exhaust fan + humidifier starts
const float TEMP_EXTREME_COOLING_OFF = 26.5;  // Exhaust fan + humidifier stops → 26.5°C TARGET
const float TEMP_LOW_HEATING        = 25.0;   // Heater + heater blower starts
const float TEMP_LOW_HEATING_OFF    = 26.0;   // Heater + heater blower stops (conflict prevention)
const float HUMIDITY_HIGH_DEHUMID   = 80.0;   // Heater + heater fan for dehumidification
const float HUMIDITY_HIGH_DEHUMID_OFF = 70.0; // Stop dehumidification

// Conflict prevention zones
const float TEMP_DEAD_ZONE_LOW  = 25.5;  // Dead zone to prevent heating/cooling conflict
const float TEMP_DEAD_ZONE_HIGH = 26.0;  // Dead zone to prevent heating/cooling conflict

// --- Pin Definitions ---
const int DHT_PIN               = 26;
const int HEATER_RELAY_PIN      = 33;        // Heater relay
const int HUMIDIFIER_RELAY_PIN  = 25;        // Humidifier relay
const int EXHAUST_FAN_RELAY_PIN = 32;        // Exhaust fan relay (cyclic)

// Motor Driver Pins
const int HEATER_BLOWER_IN1_PIN = 15;        // Heater blower motor IN1
const int HEATER_BLOWER_IN2_PIN = 3;         // Heater blower motor IN2
const int NORMAL_BLOWER_IN3_PIN = 4;         // Normal blower motor IN3
const int NORMAL_BLOWER_IN4_PIN = 5;         // Normal blower motor IN4

#define DHTTYPE DHT22

// --- Objects ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHT_PIN, DHTTYPE);

// --- Timers ---
unsigned long previousMainMillis = 0;
const long mainInterval = 5000;
unsigned long previousReconnectMillis = 0;
const long reconnectInterval = 5000;

// --- Exhaust Fan Cyclic Timer ---
unsigned long previousExhaustMillis = 0;
bool exhaustFanCyclicMode = false;
bool exhaustFanManualOn = false;
int selectedDelayMinutes = 5;

// --- Global State Variables ---
bool isEmergencyStopOn = false;
bool isAutoMode = true;
float currentTemp = 25.0; 
float currentHumidity = 60.0;
String systemStatus = "STARTING...";

// --- Device State Variables ---
bool exhaustFanOn = false;
bool heaterOn = false;
bool humidifierOn = false;
bool heaterBlowerRunning = false;
bool normalBlowerRunning = false;

// --- Auto Control State Variables ---
String autoControlMode = "IDLE"; // IDLE, NORMAL_COOLING, EXTREME_COOLING, HEATING, DEHUMIDIFYING

// --- Message counters ---
int publishCount = 0;
int connectionAttempts = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 ONION STORAGE SYSTEM SMART V3 ===");
  Serial.println("🔧 Smart Auto Control: 26.5°C Cooling Target + No Conflicts");
  
  lcd.init();
  lcd.backlight();
  lcd.clear();
  delay(100);
  
  dht.begin();

  // Initialize pins
  pinMode(EXHAUST_FAN_RELAY_PIN, OUTPUT);
  pinMode(HEATER_RELAY_PIN, OUTPUT);
  pinMode(HUMIDIFIER_RELAY_PIN, OUTPUT);
  pinMode(HEATER_BLOWER_IN1_PIN, OUTPUT);
  pinMode(HEATER_BLOWER_IN2_PIN, OUTPUT);
  pinMode(NORMAL_BLOWER_IN3_PIN, OUTPUT);
  pinMode(NORMAL_BLOWER_IN4_PIN, OUTPUT);
  
  turnEverythingOff();
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Control");
  lcd.setCursor(0, 1);
  lcd.print("Target: 26.5C");
  delay(2000);

  // Configure MQTT
  mqttClient.setBufferSize(512);
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(callback);

  // Start WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("✅ WiFi connected!");
  Serial.printf("📱 IP address: %s\n", WiFi.localIP().toString().c_str());
  
  mqtt_connect();
  
  // Enable 5-minute cyclic mode by default
  if (isAutoMode && !isEmergencyStopOn) {
    exhaustFanCyclicMode = true;
    previousExhaustMillis = millis();
    Serial.println("🌪️ DEFAULT: Exhaust fan 5-minute cyclic mode enabled");
  }
  
  Serial.println("=== SMART CONTROL THRESHOLDS ===");
  Serial.printf("🌡️ Normal Cooling: %.1f°C → %.1f°C (Normal Blower)\n", TEMP_NORMAL_COOLING, TEMP_NORMAL_COOLING_OFF);
  Serial.printf("🌡️ Extreme Cooling: %.1f°C → %.1f°C (Exhaust + Humidifier)\n", TEMP_EXTREME_COOLING, TEMP_EXTREME_COOLING_OFF);
  Serial.printf("🌡️ Heating: %.1f°C → %.1f°C (Heater + Heater Blower)\n", TEMP_LOW_HEATING, TEMP_LOW_HEATING_OFF);
  Serial.printf("💧 Dehumidification: %.1f%% → %.1f%% (Heater + Heater Fan)\n", HUMIDITY_HIGH_DEHUMID, HUMIDITY_HIGH_DEHUMID_OFF);
  Serial.printf("🛡️ Dead Zone: %.1f°C - %.1f°C (Conflict Prevention)\n", TEMP_DEAD_ZONE_LOW, TEMP_DEAD_ZONE_HIGH);
  Serial.println("=== SYSTEM READY ===");
}

void loop() {
  // Handle cyclic exhaust fan (only when not in manual override mode)
  if (!exhaustFanManualOn) {
    handleCyclicExhaustFan();
  }

  if (millis() - previousMainMillis >= mainInterval) {
    previousMainMillis = millis();
    
    readSensors();
    
    if (isAutoMode && !isEmergencyStopOn) {
      updateSmartClimateControl(); 
    }
    
    updateDisplay();
    
    if (mqttClient.connected()) {
      publishSensorData();
      delay(100);
      publishStatus();
      publishCount++;
    }
  }

  if (millis() - previousReconnectMillis >= reconnectInterval) {
    previousReconnectMillis = millis();
    
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(ssid, password);
    } else if (!mqttClient.connected()) {
      mqtt_connect();
    }
  }
  
  if (mqttClient.connected()) {
    mqttClient.loop();
  }
}

void handleCyclicExhaustFan() {
  // Don't run cyclic mode if in manual exhaust control or auto conditions override
  if (!exhaustFanCyclicMode || isEmergencyStopOn || 
      autoControlMode == "EXTREME_COOLING" || autoControlMode == "NORMAL_COOLING") {
    return;
  }

  unsigned long currentMillis = millis();
  unsigned long cycleTimeMs = selectedDelayMinutes * 60 * 1000;
  unsigned long onTimeMs = cycleTimeMs / 2;
  unsigned long offTimeMs = cycleTimeMs / 2;
  
  if (!exhaustFanOn) {
    if (currentMillis - previousExhaustMillis >= offTimeMs) {
      turnOnExhaustFan();
      previousExhaustMillis = currentMillis;
      Serial.printf("🌪️ Cyclic: Exhaust fan turned ON (cycle: %d min)\n", selectedDelayMinutes);
    }
  } else {
    if (currentMillis - previousExhaustMillis >= onTimeMs) {
      turnOffExhaustFan();
      previousExhaustMillis = currentMillis;
      Serial.printf("🌪️ Cyclic: Exhaust fan turned OFF (cycle: %d min)\n", selectedDelayMinutes);
    }
  }
}

void updateSmartClimateControl() {
  if (!isAutoMode || isEmergencyStopOn) return;

  String previousMode = autoControlMode;
  
  Serial.printf("🤖 Smart Control: T=%.1f°C, H=%.1f%%, Current Mode=%s\n", 
                currentTemp, currentHumidity, autoControlMode.c_str());

  // Priority 1: Extreme High Temperature (32°C+) - Most Critical
  // Cools down to 26.5°C
  if (currentTemp >= TEMP_EXTREME_COOLING && autoControlMode != "EXTREME_COOLING") {
    switchToExtremeCooling();
  }
  else if (currentTemp <= TEMP_EXTREME_COOLING_OFF && autoControlMode == "EXTREME_COOLING") {
    Serial.printf("🌬️ Extreme cooling complete: %.1f°C reached target %.1f°C\n", currentTemp, TEMP_EXTREME_COOLING_OFF);
    switchToIdle();
  }
  
  // Priority 2: High Humidity (80%+) - Dehumidification
  // Only if not in extreme cooling mode and temperature allows
  else if (currentHumidity >= HUMIDITY_HIGH_DEHUMID && 
           autoControlMode != "DEHUMIDIFYING" && 
           autoControlMode != "EXTREME_COOLING" &&
           currentTemp >= TEMP_DEAD_ZONE_HIGH) { // Prevent conflict with cooling
    switchToDehumidifying();
  }
  else if (currentHumidity <= HUMIDITY_HIGH_DEHUMID_OFF && autoControlMode == "DEHUMIDIFYING") {
    Serial.printf("💧 Dehumidification complete: %.1f%% reached target %.1f%%\n", currentHumidity, HUMIDITY_HIGH_DEHUMID_OFF);
    switchToIdle();
  }
  
  // Priority 3: Low Temperature (25°C-) - Heating
  // Only if not in extreme cooling or dehumidifying, and below dead zone
  else if (currentTemp <= TEMP_LOW_HEATING && 
           autoControlMode != "HEATING" && 
           autoControlMode != "EXTREME_COOLING" && 
           autoControlMode != "DEHUMIDIFYING" &&
           currentTemp <= TEMP_DEAD_ZONE_LOW) { // Prevent conflict with cooling
    switchToHeating();
  }
  else if (currentTemp >= TEMP_LOW_HEATING_OFF && autoControlMode == "HEATING") {
    Serial.printf("🔥 Heating complete: %.1f°C reached target %.1f°C\n", currentTemp, TEMP_LOW_HEATING_OFF);
    switchToIdle();
  }
  
  // Priority 4: Normal High Temperature (29°C+) - Normal Cooling
  // Cools down to 26.5°C, only if in idle mode and above dead zone
  else if (currentTemp >= TEMP_NORMAL_COOLING && 
           autoControlMode == "IDLE" && 
           currentTemp >= TEMP_DEAD_ZONE_HIGH) { // Prevent conflict with heating
    switchToNormalCooling();
  }
  else if (currentTemp <= TEMP_NORMAL_COOLING_OFF && autoControlMode == "NORMAL_COOLING") {
    Serial.printf("🌬️ Normal cooling complete: %.1f°C reached target %.1f°C\n", currentTemp, TEMP_NORMAL_COOLING_OFF);
    switchToIdle();
  }

  // Additional conflict prevention: If temperature is in dead zone (25.5-26°C)
  // and system is switching modes, prioritize stability
  if (currentTemp >= TEMP_DEAD_ZONE_LOW && currentTemp <= TEMP_DEAD_ZONE_HIGH) {
    if (autoControlMode == "HEATING" && currentTemp >= 25.8) {
      Serial.printf("🛡️ Dead zone protection: Stopping heating at %.1f°C\n", currentTemp);
      switchToIdle();
    }
    else if ((autoControlMode == "NORMAL_COOLING" || autoControlMode == "EXTREME_COOLING") && 
             currentTemp <= 25.8) {
      Serial.printf("🛡️ Dead zone protection: Stopping cooling at %.1f°C\n", currentTemp);
      switchToIdle();
    }
  }

  // Update system status based on current mode
  updateSystemStatus();
  
  if (previousMode != autoControlMode) {
    Serial.printf("🔄 Mode changed: %s -> %s at T=%.1f°C, H=%.1f%%\n", 
                  previousMode.c_str(), autoControlMode.c_str(), currentTemp, currentHumidity);
  }
}

void switchToExtremeCooling() {
  Serial.printf("🔥 EXTREME COOLING: Temperature %.1f°C >= %.1f°C → Target: %.1f°C\n", 
                currentTemp, TEMP_EXTREME_COOLING, TEMP_EXTREME_COOLING_OFF);
  autoControlMode = "EXTREME_COOLING";
  
  // Turn off cyclic mode and conflicting devices
  exhaustFanCyclicMode = false;
  turnOffHeater();
  turnOffHeaterBlower();
  
  // Turn on extreme cooling devices
  turnOnExhaustFan();
  turnOnHumidifier();
  turnOnNormalBlower();
  
  Serial.printf("   ✅ Exhaust Fan ON, Humidifier ON, Normal Blower ON\n");
}

void switchToDehumidifying() {
  Serial.printf("💧 DEHUMIDIFYING: Humidity %.1f%% >= %.1f%% → Target: %.1f%%\n", 
                currentHumidity, HUMIDITY_HIGH_DEHUMID, HUMIDITY_HIGH_DEHUMID_OFF);
  autoControlMode = "DEHUMIDIFYING";
  
  // Turn off conflicting devices
  turnOffHumidifier();
  turnOffExhaustFan();
  turnOffNormalBlower();
  
  // Turn on dehumidification devices
  turnOnHeater();
  turnOnHeaterBlower();
  
  Serial.printf("   ✅ Heater ON, Heater Blower ON (for dehumidification)\n");
}

void switchToHeating() {
  Serial.printf("🔥 HEATING: Temperature %.1f°C <= %.1f°C → Target: %.1f°C\n", 
                currentTemp, TEMP_LOW_HEATING, TEMP_LOW_HEATING_OFF);
  autoControlMode = "HEATING";
  
  // Turn off conflicting devices
  turnOffHumidifier();
  turnOffExhaustFan();
  turnOffNormalBlower();
  
  // Turn on heating devices
  turnOnHeater();
  turnOnHeaterBlower();
  
  Serial.printf("   ✅ Heater ON, Heater Blower ON\n");
}

void switchToNormalCooling() {
  Serial.printf("🌬️ NORMAL COOLING: Temperature %.1f°C >= %.1f°C → Target: %.1f°C\n", 
                currentTemp, TEMP_NORMAL_COOLING, TEMP_NORMAL_COOLING_OFF);
  autoControlMode = "NORMAL_COOLING";
  
  // Turn off conflicting devices
  turnOffHeater();
  turnOffHeaterBlower();
  turnOffHumidifier();
  
  // Turn on normal cooling
  turnOnNormalBlower();
  // Disable cyclic exhaust fan during active cooling
  exhaustFanCyclicMode = false;
  
  Serial.printf("   ✅ Normal Blower ON (cooling to %.1f°C)\n", TEMP_NORMAL_COOLING_OFF);
}

void switchToIdle() {
  Serial.println("😴 IDLE: All conditions normal");
  String previousMode = autoControlMode;
  autoControlMode = "IDLE";
  
  // Turn off all condition-based devices
  if (previousMode == "EXTREME_COOLING") {
    turnOffExhaustFan();
    turnOffHumidifier();
    turnOffNormalBlower();
  }
  else if (previousMode == "DEHUMIDIFYING" || previousMode == "HEATING") {
    turnOffHeater();
    turnOffHeaterBlower();
  }
  else if (previousMode == "NORMAL_COOLING") {
    turnOffNormalBlower();
  }
  
  // Re-enable cyclic mode if it was originally enabled
  if (isAutoMode && !exhaustFanManualOn) {
    exhaustFanCyclicMode = true;
    previousExhaustMillis = millis();
    Serial.println("   🌪️ Re-enabled cyclic exhaust fan mode");
  }
  
  Serial.printf("   ✅ All condition-based controls stopped\n");
}

void updateSystemStatus() {
  if (autoControlMode == "EXTREME_COOLING") {
    systemStatus = "AUTO: EXT COOL";
  }
  else if (autoControlMode == "DEHUMIDIFYING") {
    systemStatus = "AUTO: DEHUMID";
  }
  else if (autoControlMode == "HEATING") {
    systemStatus = "AUTO: HEATING";
  }
  else if (autoControlMode == "NORMAL_COOLING") {
    systemStatus = "AUTO: COOLING";
  }
  else if (exhaustFanCyclicMode) {
    systemStatus = "AUTO: CYCLIC";
  }
  else {
    systemStatus = "AUTO: IDLE";
  }
}

void mqtt_connect() {
  connectionAttempts++;
  Serial.printf("🔄 MQTT connection attempt #%d\n", connectionAttempts);
  
  if (mqttClient.connect(mqtt_clientId)) {
    Serial.println("✅ MQTT Connected successfully!");
    bool subscribed = mqttClient.subscribe(topic_control_subscribe);
    Serial.printf("📥 Subscription to %s: %s\n", topic_control_subscribe, subscribed ? "SUCCESS" : "FAILED");
    publishInitialStatus();
  } else {
    Serial.printf("❌ MQTT connection failed, rc=%d\n", mqttClient.state());
  }
}

void publishInitialStatus() {
  StaticJsonDocument<512> doc;
  doc["mode"] = isAutoMode ? "AUTO" : "MANUAL";
  doc["emergency_stop"] = isEmergencyStopOn ? "ON" : "OFF";
  doc["exhaust_fan"] = exhaustFanOn ? "ON" : "OFF";
  doc["heater"] = heaterOn ? "ON" : "OFF";
  doc["humidifier"] = humidifierOn ? "ON" : "OFF";
  doc["exhaust_cyclic"] = exhaustFanCyclicMode ? "ON" : "OFF";
  doc["cycle_delay"] = selectedDelayMinutes;
  doc["heater_blower"] = heaterBlowerRunning ? "ON" : "OFF";
  doc["normal_blower"] = normalBlowerRunning ? "ON" : "OFF";
  doc["auto_control_mode"] = autoControlMode;
  doc["cooling_target"] = TEMP_NORMAL_COOLING_OFF;

  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer);
  mqttClient.publish(topic_status_publish, jsonBuffer);
  Serial.printf("📤 Initial status published: %s\n", jsonBuffer);
}

void callback(char* topic, byte* payload, unsigned int length) {
  String command;
  for (int i = 0; i < length; i++) {
    command += (char)payload[i];
  }
  command.trim();
  
  Serial.printf("📨 MQTT Command received: %s\n", command.c_str());
  bool statusChanged = false;

  if (command == "EMERGENCY_STOP_ON") {
    isEmergencyStopOn = true;
    autoControlMode = "IDLE";
    turnEverythingOff();
    systemStatus = "EMERGENCY STOP";
    Serial.println("🚨 EMERGENCY STOP ACTIVATED");
    statusChanged = true;
  } 
  else if (command == "EMERGENCY_STOP_OFF") {
    isEmergencyStopOn = false;
    systemStatus = isAutoMode ? "AUTO MODE" : "MANUAL MODE";
    Serial.println("▶️ EMERGENCY STOP DEACTIVATED");
    
    if (isAutoMode) {
      exhaustFanCyclicMode = true;
      autoControlMode = "IDLE";
      previousExhaustMillis = millis();
      Serial.println("🌪️ AUTO: Re-enabled smart control with 26.5°C target");
    }
    statusChanged = true;
  }
  
  else if (command == "MODE_AUTO") {
    isAutoMode = true;
    systemStatus = isEmergencyStopOn ? "EMERGENCY STOP" : "AUTO MODE";
    
    if (!isEmergencyStopOn) {
      exhaustFanCyclicMode = true;
      autoControlMode = "IDLE";
      previousExhaustMillis = millis();
      Serial.println("🤖 Switched to AUTO MODE - Smart Control with 26.5°C Target");
    }
    statusChanged = true;
  }
  else if (command == "MODE_MANUAL") {
    isAutoMode = false;
    autoControlMode = "IDLE";
    systemStatus = isEmergencyStopOn ? "EMERGENCY STOP" : "MANUAL MODE";
    
    // Turn off all auto-controlled devices
    exhaustFanCyclicMode = false;
    turnOffExhaustFan();
    turnOffHeater();
    turnOffHumidifier();
    turnOffHeaterBlower();
    turnOffNormalBlower();
    
    Serial.println("👆 Switched to MANUAL MODE - All devices stopped");
    statusChanged = true;
  }
  
  // Cycle delay commands
  else if (command.startsWith("CYCLE_DELAY_")) {
    String delayStr = command.substring(12);
    selectedDelayMinutes = delayStr.toInt();
    Serial.printf("⏰ Exhaust fan cycle set to %d minutes\n", selectedDelayMinutes);
    statusChanged = true;
  }
  
  // Manual device controls (only work in manual mode)
  else if (command == "HEATER_ON" && !isAutoMode && !isEmergencyStopOn) {
    turnOnHeater();
    Serial.println("🔥 Manual: Heater ON");
    statusChanged = true;
  }
  else if (command == "HEATER_OFF" && !isAutoMode && !isEmergencyStopOn) {
    turnOffHeater();
    Serial.println("🔥 Manual: Heater OFF");
    statusChanged = true;
  }
  else if (command == "HUMIDIFIER_ON" && !isAutoMode && !isEmergencyStopOn) {
    turnOnHumidifier();
    Serial.println("💧 Manual: Humidifier ON");
    statusChanged = true;
  }
  else if (command == "HUMIDIFIER_OFF" && !isAutoMode && !isEmergencyStopOn) {
    turnOffHumidifier();
    Serial.println("💧 Manual: Humidifier OFF");
    statusChanged = true;
  }
  else if (command == "EXHAUST_FAN_ON" && !isAutoMode && !isEmergencyStopOn) {
    exhaustFanCyclicMode = false;
    exhaustFanManualOn = true;
    turnOnExhaustFan();
    Serial.println("🌪️ Manual: Exhaust fan ON");
    statusChanged = true;
  }
  else if (command == "EXHAUST_FAN_OFF" && !isAutoMode && !isEmergencyStopOn) {
    exhaustFanCyclicMode = false;
    exhaustFanManualOn = false;
    turnOffExhaustFan();
    Serial.println("🌪️ Manual: Exhaust fan OFF");
    statusChanged = true;
  }
  else if (command == "HEATER_BLOWER_ON" && !isAutoMode && !isEmergencyStopOn) {
    turnOnHeaterBlower();
    Serial.println("🔄 Manual: Heater blower ON");
    statusChanged = true;
  }
  else if (command == "HEATER_BLOWER_OFF" && !isAutoMode && !isEmergencyStopOn) {
    turnOffHeaterBlower();
    Serial.println("🛑 Manual: Heater blower OFF");
    statusChanged = true;
  }
  else if (command == "NORMAL_BLOWER_ON" && !isAutoMode && !isEmergencyStopOn) {
    turnOnNormalBlower();
    Serial.println("🔄 Manual: Normal blower ON");
    statusChanged = true;
  }
  else if (command == "NORMAL_BLOWER_OFF" && !isAutoMode && !isEmergencyStopOn) {
    turnOffNormalBlower();
    Serial.println("🛑 Manual: Normal blower OFF");
    statusChanged = true;
  }

  if (statusChanged) {
    Serial.println("🔄 Status changed - publishing immediately...");
    publishStatus();
  }
}

void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("❌ DHT sensor read failed! Using previous values");
  } else {
    currentHumidity = h;
    currentTemp = t;
  }
}

void publishSensorData() {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<128> doc;
  doc["temperature"] = String(currentTemp, 1);
  doc["humidity"] = String(currentHumidity, 1);

  char jsonBuffer[128];
  serializeJson(doc, jsonBuffer);
  mqttClient.publish(topic_data_publish, jsonBuffer);
}

void publishStatus() {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<512> doc;
  doc["exhaust_fan"] = exhaustFanOn ? "ON" : "OFF";
  doc["heater"] = heaterOn ? "ON" : "OFF";
  doc["humidifier"] = humidifierOn ? "ON" : "OFF";
  doc["mode"] = isAutoMode ? "AUTO" : "MANUAL";
  doc["emergency_stop"] = isEmergencyStopOn ? "ON" : "OFF";
  doc["exhaust_cyclic"] = exhaustFanCyclicMode ? "ON" : "OFF";
  doc["cycle_delay"] = selectedDelayMinutes;
  doc["heater_blower"] = heaterBlowerRunning ? "ON" : "OFF";
  doc["normal_blower"] = normalBlowerRunning ? "ON" : "OFF";
  doc["auto_control_mode"] = autoControlMode;
  doc["cooling_target"] = TEMP_NORMAL_COOLING_OFF;

  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer);
  
  bool success = mqttClient.publish(topic_status_publish, jsonBuffer);
  if (success) {
    Serial.printf("📤 Status: Mode=%s, T=%.1f°C, H=%.1f%%, Target=%.1f°C\n", 
                  autoControlMode.c_str(), currentTemp, currentHumidity, TEMP_NORMAL_COOLING_OFF);
  }
}

void updateDisplay() {
  lcd.clear();
  delay(10);
  
  lcd.setCursor(0, 0);
  String wifiStatus = (WiFi.status() == WL_CONNECTED) ? "W" : "-";
  String mqttStatus = (mqttClient.connected()) ? "M" : "-";
  
  String line1 = "T:" + String(currentTemp, 1) + " H:" + String(currentHumidity, 0) + "% " + wifiStatus + mqttStatus;
  if (line1.length() > 16) line1 = line1.substring(0, 16);
  lcd.print(line1);
  
  lcd.setCursor(0, 1);
  String mode, displayStatus;
  
  if (isEmergencyStopOn) {
    mode = "EMERGENCY";
    displayStatus = "STOP";
  } else if (isAutoMode) {
    mode = "AUTO";
    if (autoControlMode == "EXTREME_COOLING") displayStatus = "EXT-COOL";
    else if (autoControlMode == "DEHUMIDIFYING") displayStatus = "DEHUMID";
    else if (autoControlMode == "HEATING") displayStatus = "HEATING";
    else if (autoControlMode == "NORMAL_COOLING") displayStatus = "COOLING";
    else if (exhaustFanCyclicMode) displayStatus = "CYCL" + String(selectedDelayMinutes) + "m";
    else displayStatus = "IDLE";
  } else {
    mode = "MANUAL";
    displayStatus = "READY";
  }
  
  String line2 = mode + " " + displayStatus;
  if (line2.length() > 16) line2 = line2.substring(0, 16);
  while (line2.length() < 16) line2 += " ";
  lcd.print(line2);
}

// Device control functions
void turnOnExhaustFan() {
  if (!isEmergencyStopOn && !exhaustFanOn) {
    digitalWrite(EXHAUST_FAN_RELAY_PIN, LOW);
    exhaustFanOn = true;
    Serial.printf("🌪️ Exhaust fan (Pin %d) ON\n", EXHAUST_FAN_RELAY_PIN);
  }
}

void turnOffExhaustFan() {
  digitalWrite(EXHAUST_FAN_RELAY_PIN, HIGH);
  exhaustFanOn = false;
  Serial.printf("🌪️ Exhaust fan (Pin %d) OFF\n", EXHAUST_FAN_RELAY_PIN);
}

void turnOnHeater() {
  if (!isEmergencyStopOn && !heaterOn) {
    digitalWrite(HEATER_RELAY_PIN, LOW);
    heaterOn = true;
    Serial.printf("🔥 Heater (Pin %d) ON\n", HEATER_RELAY_PIN);
  }
}

void turnOffHeater() {
  digitalWrite(HEATER_RELAY_PIN, HIGH);
  heaterOn = false;
  Serial.printf("🔥 Heater (Pin %d) OFF\n", HEATER_RELAY_PIN);
}

void turnOnHumidifier() {
  if (!isEmergencyStopOn && !humidifierOn) {
    digitalWrite(HUMIDIFIER_RELAY_PIN, LOW);
    humidifierOn = true;
    Serial.printf("💧 Humidifier (Pin %d) ON\n", HUMIDIFIER_RELAY_PIN);
  }
}

void turnOffHumidifier() {
  digitalWrite(HUMIDIFIER_RELAY_PIN, HIGH);
  humidifierOn = false;
  Serial.printf("💧 Humidifier (Pin %d) OFF\n", HUMIDIFIER_RELAY_PIN);
}

void turnOnHeaterBlower() {
  if (!isEmergencyStopOn && !heaterBlowerRunning) {
    digitalWrite(HEATER_BLOWER_IN1_PIN, HIGH);
    digitalWrite(HEATER_BLOWER_IN2_PIN, LOW);
    heaterBlowerRunning = true;
    Serial.println("🔄 Heater blower ON");
  }
}

void turnOffHeaterBlower() {
  digitalWrite(HEATER_BLOWER_IN1_PIN, LOW);
  digitalWrite(HEATER_BLOWER_IN2_PIN, LOW);
  heaterBlowerRunning = false;
  Serial.println("🛑 Heater blower OFF");
}

void turnOnNormalBlower() {
  if (!isEmergencyStopOn && !normalBlowerRunning) {
    digitalWrite(NORMAL_BLOWER_IN3_PIN, HIGH);
    digitalWrite(NORMAL_BLOWER_IN4_PIN, LOW);
    normalBlowerRunning = true;
    Serial.println("🔄 Normal blower ON");
  }
}

void turnOffNormalBlower() {
  digitalWrite(NORMAL_BLOWER_IN3_PIN, LOW);
  digitalWrite(NORMAL_BLOWER_IN4_PIN, LOW);
  normalBlowerRunning = false;
  Serial.println("🛑 Normal blower OFF");
}

void turnEverythingOff() {
  turnOffHeater();
  turnOffExhaustFan();
  turnOffHumidifier();
  turnOffHeaterBlower();
  turnOffNormalBlower();
  exhaustFanCyclicMode = false;
  exhaustFanManualOn = false;
  autoControlMode = "IDLE";
  Serial.println("⭕ All devices OFF");
}