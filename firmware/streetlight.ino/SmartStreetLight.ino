
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <esp_arduino_version.h>

// ==========================================
// 1. OLED CONFIGURATION
// ==========================================

// 1.3 inch SH1106 OLED, 128x64
// SDA = GPIO 21, SCL = GPIO 22

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  U8X8_PIN_NONE
);

// ==========================================
// 2. SERVER CONFIGURATION
// ==========================================

const char* serverUrl =
  "http://192.168.0.101:5000/api/hardware/update";

const char* DEFAULT_MODE = "auto";

// First upload: true (reset old WiFi credentials)
// After WiFi configuration: change to false and upload again.
const bool RESET_WIFI_SETTINGS_ONCE = true;

// ==========================================
// 3. HARDWARE PINS
// ==========================================

const int NUM_POLES = 4;

const int ledPins[NUM_POLES] = {23, 4, 16, 17};

const int curPins[NUM_POLES] = {32, 33, 35, 39};

const bool HAS_CURRENT_SENSOR = false;

// LDR Digital Output
// HIGH = NIGHT, LOW = DAY
const int ldrPin = 34;

const int pirPin = 14;
const int irPin = 25;

const int buzzerPin = 18;

// ==========================================
// 4. SYSTEM SETTINGS
// ==========================================

const int DIM_PERCENT = 40;

const unsigned long HOLD_MS = 10000;

const unsigned long SEND_INTERVAL = 2000;

const unsigned long SERVER_TIMEOUT_MS = 10000;

const unsigned long FAULT_AFTER_MS = 4000;

const int CUR_MIN_ADC = 150;

const int PWM_FREQ = 5000;
const int PWM_RES = 8;

const unsigned long WIFI_PORTAL_TIMEOUT = 180;

// ==========================================
// 5. SYSTEM STATES
// ==========================================

struct PoleState {

  bool autoMode;
  bool on;

  int manualBr;
  int brightness;

  bool fault;

  unsigned long onSince;
};

PoleState poles[NUM_POLES];

bool isDark = false;
bool motion = false;
bool obstacle = false;

bool wifiConnected = false;
bool isLedOn = false;

String currentMode = "DAY";
String triggerReason = "NONE";

unsigned long lastDetect = 0;
unsigned long lastSendTime = 0;
unsigned long lastServerOk = 0;
unsigned long ledTimer = 0;

int serverHttpCode = 0;

// ==========================================
// 6. OLED DISPLAY FUNCTION
// ==========================================

void updateDisplay() {

  u8g2.clearBuffer();

  // Header
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(36, 12, "SASLS");

  u8g2.drawHLine(0, 16, 128);

  // WiFi status
  u8g2.setFont(u8g2_font_5x7_tr);

  u8g2.drawStr(
    0, 26,
    WiFi.status() == WL_CONNECTED ? "WiFi: CONNECTED" : "WiFi: OFFLINE"
  );

  // Server status
  u8g2.drawStr(
    0, 35,
    (millis() - lastServerOk <= SERVER_TIMEOUT_MS && lastServerOk != 0)
      ? "Server: ONLINE"
      : "Server: LOCAL AUTO"
  );

  // Day/Night
  u8g2.setFont(u8g2_font_helvB10_tr);

  u8g2.drawStr(
    0, 49,
    isDark ? "NIGHT MODE" : "DAY MODE"
  );

  // Detection screen
  if (isLedOn) {

    u8g2.setFont(u8g2_font_5x7_tr);

    u8g2.drawStr(
      0, 59,
      triggerReason.c_str()
    );

    unsigned long elapsed = millis() - ledTimer;

    int remaining = (elapsed >= HOLD_MS)
      ? 0
      : (HOLD_MS - elapsed + 999) / 1000;

    char countdown[25];

    snprintf(
      countdown,
      sizeof(countdown),
      "%ds  LED 100%%",
      remaining
    );

    u8g2.drawStr(78, 59, countdown);

  } else {

    u8g2.setFont(u8g2_font_5x7_tr);

    char brightnessText[25];

    snprintf(
      brightnessText,
      sizeof(brightnessText),
      "LED: %d%%",
      poles[0].brightness
    );

    u8g2.drawStr(0, 60, brightnessText);

    u8g2.drawStr(
      70, 60,
      motion ? "MOTION" : (obstacle ? "OBSTACLE" : "CLEAR")
    );
  }

  u8g2.sendBuffer();
}

// ==========================================
// 7. LED PWM CONTROL
// ==========================================

void setPoleBrightness(int i, int pct) {

  pct = constrain(pct, 0, 100);

  poles[i].brightness = pct;

  int pwmValue = map(pct, 0, 100, 0, 255);

#if ESP_ARDUINO_VERSION_MAJOR >= 3

  ledcWrite(ledPins[i], pwmValue);

#else

  ledcWrite(i, pwmValue);

#endif
}

// ==========================================
// 8. SENSOR READING
// ==========================================

void readSensors() {

  // Digital LDR
  isDark = (digitalRead(ldrPin) == HIGH);

  currentMode = isDark ? "NIGHT" : "DAY";

  // PIR
  motion = (digitalRead(pirPin) == HIGH);

  // IR: LOW means obstacle detected
  obstacle = (digitalRead(irPin) == LOW);

  if (motion || obstacle) {

    lastDetect = millis();
  }

  // Detection timer
  bool detected =
    (lastDetect != 0) &&
    (millis() - lastDetect < HOLD_MS);

  isLedOn = isDark && detected;

  // Detection reason
  if (isLedOn) {

    if (motion && obstacle) {

      triggerReason = "BOTH DETECTED";

    } else if (motion) {

      triggerReason = "MOTION DETECTED";

    } else {

      triggerReason = "IR DETECTED";
    }

  } else {

    triggerReason = "NONE";
  }
}

// ==========================================
// 9. BUZZER
// ==========================================

void beepBuzzer() {

  digitalWrite(buzzerPin, HIGH);

  delay(100);

  digitalWrite(buzzerPin, LOW);
}

// ==========================================
// 10. AUTOMATIC LIGHT CONTROL
// ==========================================

void updatePoles() {

  bool serverLost =
    (lastServerOk == 0) ||
    (millis() - lastServerOk > SERVER_TIMEOUT_MS);

  bool detected =
    (lastDetect != 0) &&
    (millis() - lastDetect < HOLD_MS);

  for (int i = 0; i < NUM_POLES; i++) {

    PoleState &p = poles[i];

    int target = 0;

    // AUTO MODE OR SERVER LOST
    if (p.autoMode || serverLost) {

      if (!isDark) {

        target = 0;

      } else if (detected) {

        target = 100;

      } else {

        target = DIM_PERCENT;
      }

    }

    // MANUAL MODE
    else {

      target = p.on ? p.manualBr : 0;
    }

    setPoleBrightness(i, target);

    // Current sensor fault detection
    if (HAS_CURRENT_SENSOR && p.brightness >= 20) {

      if (p.onSince == 0) {

        p.onSince = millis();
      }

      p.fault =
        (analogRead(curPins[i]) < CUR_MIN_ADC) &&
        (millis() - p.onSince > FAULT_AFTER_MS);

    } else {

      p.onSince = 0;
      p.fault = false;
    }
  }
}

// ==========================================
// 11. SERVER COMMUNICATION
// ==========================================

void syncWithServer() {

  if (WiFi.status() != WL_CONNECTED) {

    return;
  }

  HTTPClient http;

  http.begin(serverUrl);

  http.setTimeout(3000);

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  StaticJsonDocument<768> doc;

  doc["id"] = 1;

  doc["ldr"] = isDark ? "NIGHT" : "DAY";

  doc["motion"] = motion;

  doc["obstacle"] = obstacle;

  doc["rssi"] = WiFi.RSSI();

  JsonArray arr = doc.createNestedArray("poles");

  for (int i = 0; i < NUM_POLES; i++) {

    JsonObject o = arr.createNestedObject();

    o["id"] = i + 1;

    o["brightness"] = poles[i].brightness;

    o["status"] =
      poles[i].brightness > 0 ? "ON" : "OFF";

    o["mode"] =
      poles[i].autoMode ? "auto" : "manual";

    o["fault"] = poles[i].fault;
  }

  String body;

  serializeJson(doc, body);

  int code = http.POST(body);

  serverHttpCode = code;

  if (code >= 200 && code < 300) {

    String response = http.getString();

    StaticJsonDocument<1024> res;

    DeserializationError err =
      deserializeJson(res, response);

    if (!err) {

      lastServerOk = millis();

      const char* globalMode =
        res["mode"] | DEFAULT_MODE;

      JsonArray lights =
        res["lights"].as<JsonArray>();

      if (!lights.isNull()) {

        for (JsonObject light : lights) {

          int id = light["id"] | 0;

          if (id < 1 || id > NUM_POLES) {

            continue;
          }

          PoleState &p = poles[id - 1];

          const char* mode =
            light["mode"] | globalMode;

          p.autoMode =
            (strcmp(mode, "auto") == 0);

          p.on =
            (strcmp(light["status"] | "OFF", "ON") == 0);

          p.manualBr =
            constrain(
              (int)(light["brightness"] | 100),
              0,
              100
            );
        }
      }

      Serial.println("Server Sync: SUCCESS");

      Serial.print("HTTP Code: ");
      Serial.println(code);

      updatePoles();

    } else {

      Serial.println("JSON Parsing Error");
    }

  } else {

    Serial.print("HTTP Error: ");
    Serial.println(code);
  }

  http.end();
}

// ==========================================
// 12. WIFI CONNECTION
// ==========================================

void connectWiFi() {

  WiFi.mode(WIFI_STA);

  WiFiManager wm;

  if (RESET_WIFI_SETTINGS_ONCE) {

    Serial.println("Resetting saved WiFi...");

    wm.resetSettings();
  }

  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT);

  Serial.println("Starting WiFi connection...");

  bool result =
    wm.autoConnect("Smart_Street_Light_ESP32");

  if (result && WiFi.status() == WL_CONNECTED) {

    wifiConnected = true;

    Serial.println("WiFi Connected!");

    Serial.print("ESP32 IP: ");

    Serial.println(WiFi.localIP());

  } else {

    wifiConnected = false;

    Serial.println("WiFi Failed!");

    Serial.println("LOCAL AUTO MODE");

    WiFi.disconnect(true);

    WiFi.mode(WIFI_OFF);
  }
}

// ==========================================
// 13. SERIAL MONITOR STATUS
// ==========================================

void printStatus() {

  Serial.println("------------------------------");

  Serial.print("Light: ");
  Serial.println(currentMode);

  Serial.print("PIR: ");
  Serial.println(motion ? "MOTION DETECTED" : "CLEAR");

  Serial.print("IR: ");
  Serial.println(obstacle ? "OBSTACLE DETECTED" : "CLEAR");

  Serial.print("LED Brightness: ");
  Serial.print(poles[0].brightness);
  Serial.println("%");

  Serial.print("WiFi: ");

  Serial.println(
    WiFi.status() == WL_CONNECTED
      ? "CONNECTED"
      : "OFFLINE"
  );

  Serial.print("Server HTTP: ");
  Serial.println(serverHttpCode);

  Serial.println("------------------------------");
}

// ==========================================
// 14. SETUP
// ==========================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println("SMART STREET LIGHT SYSTEM");

  // OLED initialization
  Wire.begin(21, 22);

  u8g2.begin();

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_helvB10_tr);

  u8g2.drawStr(36, 25, "SASLS");

  u8g2.setFont(u8g2_font_profont12_mf);

  u8g2.drawStr(20, 45, "System Starting...");

  u8g2.sendBuffer();

  delay(1500);

  // Sensor initialization
  pinMode(ldrPin, INPUT);

  pinMode(pirPin, INPUT);

  pinMode(irPin, INPUT_PULLUP);

  pinMode(buzzerPin, OUTPUT);

  digitalWrite(buzzerPin, LOW);

  analogReadResolution(12);

  // LED initialization
  for (int i = 0; i < NUM_POLES; i++) {

#if ESP_ARDUINO_VERSION_MAJOR >= 3

    ledcAttach(
      ledPins[i],
      PWM_FREQ,
      PWM_RES
    );

#else

    ledcSetup(i, PWM_FREQ, PWM_RES);

    ledcAttachPin(ledPins[i], i);

#endif

    poles[i] = {
      strcmp(DEFAULT_MODE, "auto") == 0,
      false,
      100,
      0,
      false,
      0
    };

    setPoleBrightness(i, 0);
  }

  // WiFi connection
  connectWiFi();

  // Initial sensor reading
  readSensors();

  updatePoles();

  updateDisplay();

  Serial.println("System Ready!");
}

// ==========================================
// 15. MAIN LOOP
// ==========================================

void loop() {

  readSensors();

  updatePoles();

  // Beep once when a new detection starts
  static bool previousDetected = false;

  bool currentDetected = isLedOn;

  if (currentDetected && !previousDetected) {

    beepBuzzer();
  }

  previousDetected = currentDetected;

  // Server synchronization
  if (millis() - lastSendTime >= SEND_INTERVAL) {

    if (WiFi.status() == WL_CONNECTED) {

      syncWithServer();
    }

    lastSendTime = millis();
  }

  // OLED refresh
  static unsigned long lastDisplayUpdate = 0;

  if (millis() - lastDisplayUpdate >= 250) {

    updateDisplay();

    lastDisplayUpdate = millis();
  }

  // Serial output
  static unsigned long lastSerialUpdate = 0;

  if (millis() - lastSerialUpdate >= 2000) {

    printStatus();

    lastSerialUpdate = millis();
  }

  delay(20);
}
