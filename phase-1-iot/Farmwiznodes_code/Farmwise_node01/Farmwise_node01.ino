//USTI CODE HERE BEURS

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <Wire.h>
#include <BH1750.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_sleep.h"
#include <time.h>
#include <algorithm>

const uint64_t uS_TO_S_FACTOR = 1000000ULL;
const uint64_t TIME_TO_SLEEP = 60;  // 2 minutes for testing

const int SENSOR_PIN = 23;
OneWire oneWire(SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);

float tempCelsius;

const char* deviceId = "FarmBuddy_Sensor_Node01";
const char* plantId = "SLA";

// ========== LOCAL MQTT BROKER CONFIGURATION (Non-Secure) ==========
const char* mqtt_broker = "168.195.218.199";  // Change to your local broker IP
const int mqtt_port = 1883;                  // Standard MQTT port (non-SSL)
const char* mqtt_user = "7z8ec5oDR5BCqh3Y1gIg";//"t9il3uq7276ubj46o7nh";
const char* mqtt_password = "";                // Empty if no authentication

//const char* mqtt_topic = "farmwiz/node01/sensors/data";
const char* mqtt_topic = "v1/devices/me/telemetry";
const char* mqtt_topic_subscribe = "farmwiz/node01/sensors/data";
const char* gpioTopic = "farmwiz/node01/gpio/control";

// WiFi Credentials
//const char* ssid     ="HUAWEI-2.4G-bF2k";
//const char* password = "DuuBnx2S";

const char* ssid="HUAWEI-2.4G-bF2k";
const char* password="DuuBnx2S";

//const char* ssid     = "Usti Agro";
//const char* password = "01agrousti";

// AP Variables
const char* ssid_ap = "FarmWise-Sensor1";
const char* password_ap = "12345678";

// ========== USE REGULAR WiFiClient (Non-Secure) ==========
WiFiClient espClient;
PubSubClient client(espClient);

// NTP Server for accurate timestamps
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;  // Suriname UTC-3
const int daylightOffset_sec = 0;

// Store last valid readings
float lastValidTemp = 25.0;
float lastValidHum = 70.0;

// Function to get ISO timestamp
String getISOTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return "";
  }
  
  char isoString[30];
  strftime(isoString, sizeof(isoString), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(isoString);
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Received message on topic: ");
  Serial.println(topic);
  
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println("Message: " + message);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }
  
  if (doc.containsKey("pin") && doc.containsKey("command")) {
    int pin = doc["pin"];
    String command = doc["command"].as<String>();
    Serial.printf("GPIO Control: pin=%d, command=%s\n", pin, command.c_str());
  }
}

AsyncWebServer server(80);

#define BOARD_ID 1
#define POWER_CTRL          4
#define BAT_ADC             33
#define SALT_PIN            34
#define SOIL_PIN            32
#define I2C_SDA             25
#define I2C_SCL             26
#define DHTPIN 16
#define LDR_PIN 27
#define MIN_LDR 0
#define MAX_LDR 2000

float temp = 0;
float hum = 0;

BH1750 lightMeter(0x23);

#define DHTTYPE    DHT11
DHT dht(DHTPIN, DHTTYPE);

uint8_t broadcastAddress[] = {0xC0, 0x49, 0xEF, 0x68, 0x82, 0x30};
enum NodeMode { MODE_ONLINE, MODE_OFFLINE };
NodeMode nodeMode = MODE_ONLINE;
bool espNowReady = false;
const uint8_t ESPNOW_CHANNEL = 6;

typedef struct struct_message {
  int id;
  float temp;
  float hum;
  float soil;
  float salt;
  float light;
  float batt;
  int readingId;
} struct_message;

struct_message myData;

unsigned long previousMillis = 0;
const long interval = 1000;
unsigned int readingId = 0;

constexpr char WIFI_SSID[] = "HUAWEI-2.4G-bF2k";

int32_t getWiFiChannel(const char *ssid) {
  if (int32_t n = WiFi.scanNetworks()) {
    for (uint8_t i = 0; i < n; i++) {
      if (!strcmp(ssid, WiFi.SSID(i).c_str())) {
        return WiFi.channel(i);
      }
    }
  }
  return 0;
}

// ========== DHT Temperature with retries and fallback ==========
float readDHTTemperature() {
  int retries = 5;  // Increased retries
  for (int i = 0; i < retries; i++) {
    float temp = dht.readTemperature();
    if (!isnan(temp) && temp > 0 && temp < 60) {
      lastValidTemp = temp;
      Serial.printf("✓ Temperature read: %.1f°C\n", temp);
      return temp;
    }
    Serial.printf("  Attempt %d/%d - Temperature failed (got: %f)\n", i+1, retries, temp);
    delay(500);  // Longer delay between retries
  }
  Serial.printf("✗ Failed to read temperature after %d attempts, using last valid: %.1f°C\n", retries, lastValidTemp);
  return lastValidTemp;
}

// ========== DHT Humidity with retries and fallback ==========
float readDHTHumidity() {
  int retries = 5;  // Increased retries
  for (int i = 0; i < retries; i++) {
    float hum = dht.readHumidity();
    if (!isnan(hum) && hum >= 0 && hum <= 100) {
      lastValidHum = hum;
      Serial.printf("✓ Humidity read: %.1f%%\n", hum);
      return hum;
    }
    Serial.printf("  Attempt %d/%d - Humidity failed (got: %f)\n", i+1, retries, hum);
    delay(500);  // Longer delay between retries
  }
  Serial.printf("✗ Failed to read humidity after %d attempts, using last valid: %.1f%%\n", retries, lastValidHum);
  return lastValidHum;
}

// ========== DIAGNOSTIC FUNCTION TO TEST DHT11 ==========
void testDHT11() {
  Serial.println("\n========================================");
  Serial.println("DHT11 DIAGNOSTIC TEST");
  Serial.println("========================================");
  Serial.printf("DHT11 connected to GPIO: %d\n", DHTPIN);
  Serial.println("Checking sensor...\n");
  
  delay(1000);
  
  // Try multiple reads to see if sensor responds at all
  for (int test = 0; test < 5; test++) {
    Serial.printf("Test %d/5: ", test+1);
    
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    
    if (isnan(h) || isnan(t)) {
      Serial.println("❌ FAILED - No response from sensor");
      Serial.println("   Possible issues:");
      Serial.println("   1. Check wiring - DHT11 data pin to GPIO 16");
      Serial.println("   2. Missing 4.7k-10k ohm pull-up resistor on data line");
      Serial.println("   3. Sensor not powered (3.3V or 5V)");
      Serial.println("   4. Faulty sensor");
    } else {
      Serial.printf("✅ SUCCESS - Temp: %.1f°C, Humidity: %.1f%%\n", t, h);
      break;
    }
    delay(1000);
  }
  
  Serial.println("========================================\n");
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nLast Packet Send Status:\t");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Run DHT11 diagnostic test first
  testDHT11();
  
  pinMode(LDR_PIN, INPUT);
  analogReadResolution(12);

  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  Serial.println("Setup ESP32 to sleep for " + String(TIME_TO_SLEEP) + " Seconds");

  if (!SPIFFS.begin()) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  Serial.println("\n[*] Creating AP");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid_ap, password_ap);
  Serial.print("[+] AP Created with IP Gateway ");
  Serial.println(WiFi.softAPIP());

  WiFi.begin(ssid, password);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(1000);
    Serial.print(".");
    wifiAttempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("Got IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
  }

  // Initialize NTP for accurate timestamps
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  Serial.print("Waiting for NTP time sync...");
  struct tm timeinfo;
  int ntpAttempts = 0;
  while (!getLocalTime(&timeinfo) && ntpAttempts < 10) {
    Serial.print(".");
    delay(500);
    ntpAttempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    Serial.println(" Done!");
    Serial.println("Current time: " + getISOTimestamp());
  } else {
    Serial.println(" NTP sync failed");
  }

  // ========== SETUP LOCAL MQTT BROKER (Non-Secure) ==========
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);

  connectMQTT();

  // Web Server Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, "/index.html");
  });
  server.on("/temperature", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send_P(200, "text/plain", String(temp).c_str());
  });
  server.on("/humidity", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send_P(200, "text/plain", String(hum).c_str());
  });

  server.begin();
  Serial.println("HTTP server started");

  Wire.begin(I2C_SDA, I2C_SCL);

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println(F("BH1750 Advanced begin"));
  }

  dht.begin();
  tempSensor.begin();

  pinMode(POWER_CTRL, OUTPUT);
  digitalWrite(POWER_CTRL, 1);

  // ESP-NOW Setup (only if no WiFi)
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No internet connection, setting up ESP-NOW");
    
    int32_t channel = ESPNOW_CHANNEL;
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() != ESP_OK) {
      Serial.println("Error initializing ESP-NOW");
      return;
    }

    esp_now_register_send_cb(OnDataSent);
    esp_now_peer_info_t peerInfo;
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add peer");
      return;
    }
    espNowReady = true;
    Serial.println("ESP-NOW initialized on fixed channel 6");
    nodeMode = MODE_OFFLINE;
  }
}

void connectMQTT() {
  int attempts = 0;
  while (!client.connected() && attempts < 5) {
    Serial.print("Connecting to MQTT Broker at ");
    Serial.print(mqtt_broker);
    Serial.print(":");
    Serial.print(mqtt_port);
    Serial.print("...");
    
    if (client.connect(deviceId, mqtt_user, mqtt_password)) {
      Serial.println(" connected!");
      client.subscribe(gpioTopic);
      client.subscribe(mqtt_topic_subscribe);
      return;
    } else {
      Serial.print(" failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
      attempts++;
    }
  }
}

void enableOfflineMode() {
  client.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (!espNowReady) {
    if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init failed"); return; }
    esp_now_register_send_cb(OnDataSent);
    esp_now_peer_info_t peerInfo{};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK && !esp_now_is_peer_exist(broadcastAddress)) {
      Serial.println("ESP-NOW peer setup failed"); return;
    }
    espNowReady = true;
  }
  nodeMode = MODE_OFFLINE;
  Serial.println("OFFLINE MODE: ESP-NOW channel 6 ready");
}

uint16_t readSoil() {
  uint16_t soilpin = analogRead(SOIL_PIN);
  uint16_t drySoilValue = 3443;
  uint16_t waterSoilValue = 2320;
  uint16_t soil = map(soilpin, waterSoilValue, drySoilValue, 100, 0);
  soil = constrain(soil, 0, 100);
  return soil;
}

uint16_t readLight() {
  int ldrValue = analogRead(LDR_PIN);
  uint16_t lightIntensity = map(ldrValue, MIN_LDR, MAX_LDR, 0, 100);
  lightIntensity = constrain(lightIntensity, 0, 100);
  return lightIntensity;
}

// ========== DEBUG SALT / PPM FUNCTION ==========
uint16_t readSaltPPM() {
  uint8_t samples = 60;
  uint32_t raw_adc = 0;
  uint16_t array[60];

  for (int i = 0; i < samples; i++) {
    array[i] = analogRead(SALT_PIN);
    delay(1);
  }

  std::sort(array, array + samples);
  for (int i = 1; i < samples - 1; i++) {
    raw_adc += array[i];
  }
  raw_adc /= (samples - 2);

  // DEBUG: Print raw ADC value
  Serial.printf("🔍 RAW ADC Reading: %u\n", raw_adc);

  // --- CALIBRATION MAPPING ---
  // Replace these values with your actual readings after testing
  uint16_t distilledWaterADC = 180;  // UPDATE THIS after testing in distilled water
  uint16_t highSaltADC = 3000;        // UPDATE THIS after testing in salt water

  // DEBUG: Show what values are being used
  Serial.printf("📊 Using: Distilled=%d, HighSalt=%d\n", distilledWaterADC, highSaltADC);

  // Check if raw_adc is outside the mapping range
  if (raw_adc < distilledWaterADC) {
    Serial.printf("⚠️ Raw ADC (%u) is LOWER than distilled water baseline (%d)\n", raw_adc, distilledWaterADC);
    Serial.println("   Try updating distilledWaterADC to a LOWER value");
  } else if (raw_adc > highSaltADC) {
    Serial.printf("⚠️ Raw ADC (%u) is HIGHER than high salt baseline (%d)\n", raw_adc, highSaltADC);
    Serial.println("   Try updating highSaltADC to a HIGHER value");
  }

  uint16_t calibratedPPM = map(raw_adc, distilledWaterADC, highSaltADC, 0, 1000);
  calibratedPPM = constrain(calibratedPPM, 0, 5000);

  Serial.printf("📈 Calibrated PPM: %u\n", calibratedPPM);
  Serial.println("---");

  return calibratedPPM;
}

float readBattery() {
  uint16_t volt = analogRead(BAT_ADC);
  float battery_voltage = (volt / 4095.0) * 2.0 * 3.3 * 1.1;
  return battery_voltage;
}

unsigned long lastMqttTime = millis();
const unsigned long Mqtt_INTERVAL_MS = 3000;

void loop() {
  digitalWrite(POWER_CTRL, 1);
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n'); command.trim(); command.toLowerCase();
    if (command == "offline") enableOfflineMode();
  }
  if (nodeMode == MODE_ONLINE && WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) connectMQTT();
    client.loop();
  }
  if (millis() - lastMqttTime > Mqtt_INTERVAL_MS) {
    sendSensorData();
    lastMqttTime = millis();
  }

  digitalWrite(POWER_CTRL, 1);
}

// ========== SEND SENSOR DATA (WITH CALIBRATED SALT/PPM) ==========
void sendSensorData() {
  // Read all sensors
  float dhtTemp = readDHTTemperature();
  float dhtHum = readDHTHumidity();
  uint16_t soil = readSoil();
  uint16_t saltPPM = readSaltPPM(); // Use calibrated PPM function
  float batt = readBattery();
  uint16_t light = readLight();

  tempSensor.requestTemperatures();
  float soilTemp = tempSensor.getTempCByIndex(0);
  delay(500);

  // Get ISO timestamp
  String isoTime = getISOTimestamp();
  
  // Create JSON document
  StaticJsonDocument<1024> doc;
  doc["deviceId"] = deviceId;
  doc["plantId"] = plantId;
  doc["temp"] = dhtTemp;
  doc["hum"] = dhtHum;
  doc["soilMoisture"] = soil;
  doc["soilsalt"] = saltPPM; // Sends the calibrated PPM value
  doc["battlife"] = batt;
  doc["soiltemp"] = soilTemp;
  doc["light"] = light;
  doc["timestamp"] = isoTime;
  doc["datetime"] = isoTime;

  char out[1024];
  serializeJson(doc, out);
  
  Serial.println("=== Publishing Sensor Data ===");
  Serial.printf("Temp: %.1f°C, Hum: %.1f%%, Soil: %d%%, Salt/PPM: %u, Light: %d%%, SoilTemp: %.1f°C\n", 
                dhtTemp, dhtHum, soil, saltPPM, light, soilTemp);

  if (nodeMode == MODE_OFFLINE && espNowReady) {
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)out, strlen(out));
    Serial.printf("ESP-NOW %s: %s\n", result == ESP_OK ? "queued" : "failed", out);
  } else if (client.connected() && client.publish(mqtt_topic, out)) {
    Serial.println("✅ Data delivered to MQTT Broker");
  } else {
    Serial.println("❌ Failed to publish");
  }
}
