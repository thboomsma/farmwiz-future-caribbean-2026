#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>


//const char* ssid     = "HUAWEI-2.4G-bF2k";
//const char* password = "DuuBnx2S";

const char* ssid="HUAWEI-2.4G-bF2k";
const char* password="DuuBnx2S";

//const char* ssid     = "Usti Agro";
//const char* password = "01agrousti";
// -------- MQTT ----------
const char* mqtt_server = "168.195.218.199";
const int mqtt_port = 1883;
const char* mqtt_user = "tAoVO6OVZ0VuUjHhOSZY";
const char* mqtt_password = "";  

const char* mqtt_topic = "v1/devices/me/telemetry";
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// -------- Sensor Pins ----------
const int pHSensorPin = 34;       // GPIO 34 for pH (Updated to match your working pin)
     // GPIO 35 for Turbidity (Swapped so pins don't conflict)
const int ledPin = 32;            // GPIO 32 for LED indicator  <-- ADDED

// -------- Device Configuration ----------
const char* deviceId = "PhTurbNode01";
const uint8_t masterMac[6] = {0xC0,0x49,0xEF,0x68,0x82,0x30};
const uint8_t ESPNOW_CHANNEL = 6;
enum NodeMode { MODE_ONLINE, MODE_OFFLINE };
NodeMode nodeMode = MODE_ONLINE;
bool manualOffline = false;
bool espNowReady = false;
unsigned long lastWifiAttempt = 0;

// ========== CALIBRATION CONSTANTS ==========
// pH Dual-Slope Calibration Constants
float neutralVoltage = 2.511;     // Measured voltage at pH 6.86
float acidSlope = 0.271;          // Slope for pH below 6.86 (tested with 4.01)
float baseSlope = 0.186;          // Slope for pH above 6.86 (tested with 9.18)



// ========== CALIBRATION FUNCTIONS ==========


// -------- Function: Connect to WiFi ----------
bool setup_wifi() {
  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    digitalWrite(ledPin, !digitalRead(ledPin));  // <-- ADDED: Toggle LED while connecting
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(ledPin, HIGH);  // <-- ADDED: LED on when connected
    Serial.println();
    Serial.println("WiFi Connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    digitalWrite(ledPin, LOW);  // <-- ADDED: LED off if failed
    Serial.println();
    Serial.println("WiFi Connection Failed! Restarting...");
    delay(2000);
    return false;
  }
  return true;
}

void stopEspNow() { if (espNowReady) { esp_now_deinit(); espNowReady = false; } }

bool setupEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, masterMac, 6); peer.channel = ESPNOW_CHANNEL; peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK && !esp_now_is_peer_exist(masterMac)) return false;
  espNowReady = true; return true;
}

void enterOffline() {
  manualOffline = true; nodeMode = MODE_OFFLINE;
  mqttClient.disconnect(); WiFi.disconnect(true); delay(100);
  if (setupEspNow()) Serial.println("OFFLINE ESP-NOW READY channel 6");
}

void enterOnline() {
  manualOffline = false; stopEspNow(); nodeMode = MODE_ONLINE;
  if (setup_wifi()) { Serial.println("ONLINE WIFI READY"); reconnectMQTT(); }
}

// -------- MQTT Callback ----------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

// -------- Function: Connect to MQTT Broker ----------
void reconnectMQTT() {
  int retryCount = 0;
  while (!mqttClient.connected() && retryCount < 10) {
    Serial.print("Connecting MQTT... Attempt ");
    Serial.print(retryCount + 1);
    Serial.print("/10");
    
    digitalWrite(ledPin, !digitalRead(ledPin));  // <-- ADDED: Toggle LED while connecting
    delay(100);  // <-- ADDED: Small delay for visual effect
    
    bool connected;
    if (strlen(mqtt_password) == 0) {
      connected = mqttClient.connect(deviceId, mqtt_user, NULL);
    } else {
      connected = mqttClient.connect(deviceId, mqtt_user, mqtt_password);
    }
    
    if (connected) {
      digitalWrite(ledPin, HIGH);  // <-- ADDED: LED on when connected
      Serial.println(" connected!");
    } else {
      digitalWrite(ledPin, LOW);  // <-- ADDED: LED off if failed
      Serial.print(" failed, rc=");
      Serial.print(mqttClient.state());
      retryCount++;
      if (retryCount < 10) {
        Serial.println(" retrying in 5 seconds");
        delay(5000);
      }
    }
  }
  
  if (!mqttClient.connected()) Serial.println("MQTT unavailable; will retry without rebooting");
}

// -------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(ledPin, OUTPUT);  // <-- ADDED: Initialize LED pin
  digitalWrite(ledPin, LOW);  // <-- ADDED: Start with LED off
  
  Serial.println("\n========================================");
  Serial.println("Water Quality Sensor Node (ESP32)");
  Serial.println("========================================\n");
  
  analogReadResolution(12); // 12-bit resolution (0-4095)
  
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  
  delay(1000);
  if (setup_wifi()) reconnectMQTT();
  else enterOffline();
  
  Serial.println("\nSetup complete! Starting main loop...\n");
}

// -------- Main Loop ----------
void loop() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n'); command.trim(); command.toLowerCase();
    if (command == "offline") enterOffline();
    else if (command == "online") enterOnline();
    else if (command == "status") {
      Serial.printf("Device=%s Mode=%s WiFi=%s MQTT=%s ESP-NOW=%s Master=C0:49:EF:68:82:30 Channel=6\n",
        deviceId, nodeMode == MODE_OFFLINE ? "OFFLINE" : "ONLINE",
        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
        mqttClient.connected() ? "connected" : "disconnected",
        espNowReady ? "ready" : "off");
    }
  }
  if (nodeMode == MODE_ONLINE) {
    if (WiFi.status() != WL_CONNECTED) { enterOffline(); }
    else { if (!mqttClient.connected()) reconnectMQTT(); mqttClient.loop(); }
  }
  
  // --- Read pH from GPIO 34 (Dual-Slope Calibration & Filtering) ---
  unsigned long int phAvgValue = 0;
  int phBuffer[10], tempPh;
  
  for (int i = 0; i < 10; i++) {
    phBuffer[i] = analogRead(pHSensorPin);
    delay(30);
  }
  
  // Sort pH samples
  for (int i = 0; i < 9; i++) {
    for (int j = i + 1; j < 10; j++) {
      if (phBuffer[i] > phBuffer[j]) {
        tempPh = phBuffer[i];
        phBuffer[i] = phBuffer[j];
        phBuffer[j] = tempPh;
      }
    }
  }
  
  // Average center 6 pH samples
  for (int i = 2; i < 8; i++) {
    phAvgValue += phBuffer[i];
  }
  float phAvgADC = (float)phAvgValue / 6.0;
  float phVoltage = phAvgADC * (3.3 / 4095.0);
  
  float pHValue = 0.0;
  if (phVoltage >= neutralVoltage) {
    pHValue = 6.86 - ((phVoltage - neutralVoltage) / acidSlope);
  } else {
    pHValue = 6.86 + ((neutralVoltage - phVoltage) / baseSlope);
  }

  // Clamp pH to safe limits
  if (pHValue < 0) pHValue = 0;
  if (pHValue > 14) pHValue = 14;

  
  // Create JSON payload
  StaticJsonDocument<512> json;
  
  json["deviceId"] = deviceId;
  json["timestamp"] = millis();
  
  json["pH"] = pHValue;
  
  json["phADC"] = phAvgADC;
  
  // Water quality assessment
  String waterQuality = "GOOD";
  if (pHValue < 6.0 || pHValue > 8.0) waterQuality = "FAIR";
  if (pHValue < 5.0 || pHValue > 9.0) waterQuality = "POOR";
  json["waterQuality"] = waterQuality;
  
  char buffer[512];
  serializeJson(json, buffer);
  
  if (nodeMode == MODE_OFFLINE) {
    if (!espNowReady) setupEspNow();
    esp_now_send(masterMac, (uint8_t*)buffer, strlen(buffer));
    Serial.printf("ESP-NOW sent: %s\n", buffer);
    digitalWrite(ledPin, LOW); delay(50); digitalWrite(ledPin, HIGH);
  } else if (mqttClient.connected()) {
    if (mqttClient.publish(mqtt_topic, buffer)) {
      Serial.println("✅ MQTT Sent successfully");
      Serial.printf("   pH: %.2f (Voltage: %.3fV)\n", pHValue, phVoltage);
      digitalWrite(ledPin, LOW);  // <-- ADDED: Turn LED OFF
      delay(50);  // <-- ADDED: Short pulse
      digitalWrite(ledPin, HIGH);  // <-- ADDED: Turn LED back ON
    } else {
      Serial.println("❌ Failed to publish MQTT message");
      // <-- ADDED: Quick flash pattern for error
      digitalWrite(ledPin, LOW);
      delay(200);
      digitalWrite(ledPin, HIGH);
      delay(200);
      digitalWrite(ledPin, LOW);
      delay(200);
      digitalWrite(ledPin, HIGH);
    }
  } else {
    Serial.println("⚠️ MQTT not connected, attempting reconnect...");
    reconnectMQTT();
  }
  
  Serial.println();
  delay(5000);
}
