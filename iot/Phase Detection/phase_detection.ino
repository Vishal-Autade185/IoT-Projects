#include <EEPROM.h>
#define MODEM_TX 17
#define MODEM_RX 16
#define R_PHASE_PIN 12
#define Y_PHASE_PIN 14
#define B_PHASE_PIN 13

HardwareSerial sim800l(2); 
const int MAX_NUMBERS = 5;
const int NUMBER_LENGTH = 14; 
String registeredNumbers[MAX_NUMBERS];

unsigned long phaseDownTime = 0;
bool smsSent = false;
bool callMade = false;
bool completeFailureActive = false;


void loadRegisteredNumbers() {
  for (int i = 0; i < MAX_NUMBERS; i++) {
    String num = "";
    for (int j = 0; j < NUMBER_LENGTH; j++) {
      char c = EEPROM.read(i * NUMBER_LENGTH + j);
      if (c > 0 && c < 127) num += c;
    }
    if (num.length() >= 10) registeredNumbers[i] = num;
    else registeredNumbers[i] = "";
  }
}

void writeToEEPROM(int slot, String num) {
  for (int j = 0; j < NUMBER_LENGTH; j++) {
    char c = (j < num.length()) ? num[j] : 0;
    EEPROM.write(slot * NUMBER_LENGTH + j, c);
  }
  EEPROM.commit();
}

void sendSMS(String number, String message) {
  sim800l.println("AT+CMGF=1");
  delay(500);
  sim800l.print("AT+CMGS=\"");
  sim800l.print(number);
  sim800l.println("\"");
  delay(500);
  sim800l.print(message);
  delay(500);
  sim800l.write(26); 
  delay(3000);
}

void sendAlerts(String message) {
  for (int i = 0; i < MAX_NUMBERS; i++) {
    if (registeredNumbers[i] != "") {
      sendSMS(registeredNumbers[i], message);
      delay(2000);
    }
  }
}

void makeCall() {
  for (int i = 0; i < MAX_NUMBERS; i++) {
    if (registeredNumbers[i] != "") {
      sim800l.print("ATD");
      sim800l.print(registeredNumbers[i]);
      sim800l.println(";");
      delay(20000);
      sim800l.println("ATH");
      delay(2000);
      return;
    }
  }
  Serial.println("No registered numbers to call");
}


void handleSMS(String sender, String content) {
  content.trim();
  content.toUpperCase();

  if (content.startsWith("REGISTER")) {
    String newNumber = content.substring(9); newNumber.trim();
    if (newNumber.length() < 10 || newNumber.indexOf("+") != 0) {
      sendSMS(sender, "Invalid format. Use +CCXXXXXXXXXX");
      return;
    }
    for (int i = 0; i < MAX_NUMBERS; i++) {
      if (registeredNumbers[i] == newNumber) {
        sendSMS(sender, "Number already registered");
        return;
      }
    }
    for (int i = 0; i < MAX_NUMBERS; i++) {
      if (registeredNumbers[i] == "") {
        registeredNumbers[i] = newNumber;
        writeToEEPROM(i, newNumber);
        sendSMS(sender, "Registered successfully");
        return;
      }
    }
    sendSMS(sender, "Registration full (max " + String(MAX_NUMBERS) + ")");
  }
  else if (content.startsWith("UNREGISTER")) {
    String targetNumber = content.substring(11); targetNumber.trim();
    for (int i = 0; i < MAX_NUMBERS; i++) {
      if (registeredNumbers[i] == targetNumber) {
        registeredNumbers[i] = "";
        writeToEEPROM(i, "");
        sendSMS(sender, "Number unregistered");
        return;
      }
    }
    sendSMS(sender, "Number not found");
  }
  else if (content == "LIST") {
    String list = "Registered numbers:\n";
    bool found = false;
    for (int i = 0; i < MAX_NUMBERS; i++) {
      if (registeredNumbers[i] != "") {
        list += registeredNumbers[i] + "\n";
        found = true;
      }
    }
    if (!found) list = "No numbers registered";
    sendSMS(sender, list);
  }
}


void checkIncomingSMS() {
  static bool waitingForContent = false;
  static String sender = "";
  static String line = "";

  while (sim800l.available()) {
    char c = sim800l.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line.trim();
      if (line.startsWith("+CMT:")) {
        // Extract sender number
        int firstQuote = line.indexOf('"');
        int secondQuote = line.indexOf('"', firstQuote + 1);
        if (firstQuote >= 0 && secondQuote > firstQuote)
          sender = line.substring(firstQuote + 1, secondQuote);
        else
          sender = "";
        waitingForContent = true;
      }
      else if (waitingForContent && line.length() > 0) {
        // This is the SMS content
        Serial.println("SMS from " + sender + ": " + line);
        handleSMS(sender, line);
        waitingForContent = false;
      }
      line = "";
    } else {
      line += c;
    }
  }
}


void setup() {
  pinMode(R_PHASE_PIN, INPUT_PULLUP);
  pinMode(Y_PHASE_PIN, INPUT_PULLUP);
  pinMode(B_PHASE_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  sim800l.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);

  EEPROM.begin(512);
  loadRegisteredNumbers();

  delay(2000);
  sim800l.println("AT+CMGF=1"); // Text mode
  delay(500);
  sim800l.println("AT+CNMI=2,2,0,0,0"); // Push new SMS to serial
  delay(500);

  Serial.println("Three-Phase Monitoring System Ready");
}

void loop() {
  unsigned long currentTime = millis();
  int rState = digitalRead(R_PHASE_PIN);
  int yState = digitalRead(Y_PHASE_PIN);
  int bState = digitalRead(B_PHASE_PIN);


  if (rState == LOW && yState == LOW && bState == LOW) {
    if (!completeFailureActive) {
      phaseDownTime = currentTime;
      completeFailureActive = true;
      smsSent = false;
      callMade = false;
      Serial.println("Complete power failure detected");
    }
    if (currentTime - phaseDownTime >= 10000 && !smsSent) {
      sendAlerts("CRITICAL: Complete Power Failure - All Phases Down");
      smsSent = true;
    }
    if (currentTime - phaseDownTime >= 60000 && !callMade) {
      makeCall();
      callMade = true;
    }
  }
 
  else if (rState == LOW || yState == LOW || bState == LOW) {
    if (!completeFailureActive) {
      if (phaseDownTime == 0) phaseDownTime = currentTime;
      String failedPhases = "";
      if (rState == LOW) failedPhases += "R ";
      if (yState == LOW) failedPhases += "Y ";
      if (bState == LOW) failedPhases += "B ";
      failedPhases.trim();
      if (currentTime - phaseDownTime >= 10000 && !smsSent) {
        sendAlerts("WARNING: Phase Failure - " + failedPhases + " Fuse Out");
        smsSent = true;
      }
      if (currentTime - phaseDownTime >= 60000 && !callMade) {
        makeCall();
        callMade = true;
      }
    }
  }
 
  else {
    if (phaseDownTime != 0) {
      Serial.println("Power restored. Resetting system.");
      sendAlerts("SYSTEM RECOVERED: Power restored to all phases");
    }
    phaseDownTime = 0;
    completeFailureActive = false;
    smsSent = falsefd;
    callMade = false;
  }

  checkIncomingSMS();
}

const check incoem