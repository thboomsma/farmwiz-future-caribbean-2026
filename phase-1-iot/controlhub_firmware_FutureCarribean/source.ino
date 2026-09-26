/*
  USTI BEURS - ThingsBoard Integration with Full RPC Support
  FINAL USTI WORKING VERSION CODE
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_arduino_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdarg.h>
#include <stdio.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <sys/time.h>

struct CronProfile;

// ------------------- Watchdog Timer Configuration -------------------
#define WDT_TIMEOUT 10

// ------------------- Preferences -------------------
Preferences preferences;

// ------------------- RTC -------------------
RTC_DS3231 rtc;
bool rtcAvailable = false;
bool systemClockValid = false;
bool initialScheduleSyncComplete = false;
int64_t lastCronEvaluationSecond = -1;
int64_t cronProfileValidFrom[4] = {0, 0, 0, 0};
unsigned long lastNtpAttempt = 0;
const unsigned long NTP_RETRY_INTERVAL = 30000UL;
const char* CRON_TIMEZONE = "SRT3"; // Suriname Time, UTC-3 (POSIX TZ format)

void configureCronTimezone() {
  setenv("TZ", CRON_TIMEZONE, 1);
  tzset();
}

DateTime currentUtcTime() {
  if (rtcAvailable) {
    DateTime rtcTime = rtc.now();
    if (rtcTime.unixtime() > 1700000000UL) return rtcTime;
  }
  if (systemClockValid) {
    time_t epoch = time(nullptr);
    if (epoch > 1700000000) return DateTime((uint32_t)epoch);
  }
  return DateTime((uint32_t)0);
}

DateTime toCronLocalTime(const DateTime& utcTime) {
  time_t epoch = (time_t)utcTime.unixtime();
  struct tm localTime;
  localtime_r(&epoch, &localTime);
  return DateTime(localTime.tm_year + 1900, localTime.tm_mon + 1,
                  localTime.tm_mday, localTime.tm_hour, localTime.tm_min,
                  localTime.tm_sec);
}

// ------------------- LCD -------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ------------------- DHT Sensor -------------------
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ------------------- Sensor & Relay Pins -------------------
#define LDR_PIN 35
#define MOISTURE_PIN 32
#define RELAY1_PIN 25
#define RELAY2_PIN 26
#define RELAY3_PIN 27
#define RELAY4_PIN 33
const int RELAY_PINS[4] = { RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN };

// ------------------- Button Pin -------------------
const int buttonPin = 16;

// ------------------- WiFi & MQTT (ThingsBoard) -------------------
const char* DEFAULT_SSID = "HUAWEI-2.4G-bF2k";
const char* DEFAULT_PASSWORD = "DuuBnx2S";
String wifiSSID;
String wifiPassword;

//const char* ssid="HUAWEI-B310-BD18";
//const char* password="ELHF8D5QTYF";

//const char* ssid     = "Usti Agro";
//const char* password = "01agrousti";

// Increment this value for every firmware update.
const char* firmwareVersion = "2026.09.26.03";

// ---- CONTROLHUB_01 ----
const char* mqtt_broker = "168.195.218.199";
const char* mqtt_root = "";
const int   mqtt_port  = 1883;
const char* mqtt_user  = "il3xmnqwomy6nrf5ew1m";
const char* mqtt_pass  = "";
const char* deviceId   = "ControlHub_01";
String clientId = "ControlHub_01_Client_" + String(random(0xffff), HEX);

// ---- CONTROLHUB_02 ----
//const char* mqtt_broker= "168.195.218.199"; //"broker.hivemq.com"; //
//const char* mqtt_root= ""; //"farmwiznet/";
//const int   mqtt_port  = 1883;
//const char* mqtt_user  = "pzkavdqqlljtyp035cj9";
//const char* mqtt_pass  = "";
//const char* deviceId   = "ControlHub_02";
//String clientId = "ControlHub_02_Client_" + String(random(0xffff), HEX);

// Offline AP and ESP-NOW share one fixed radio channel. ESP-NOW is disabled
// whenever the control hub is associated with infrastructure Wi-Fi.
const uint8_t ESPNOW_OFFLINE_CHANNEL = 6;
const char* OFFLINE_AP_SSID = "FarmWiz_Offline";
const char* OFFLINE_AP_PASSWORD = "farmwiz123";

const unsigned long WIFI_CONNECT_TIMEOUT = 15000UL;
const unsigned long WIFI_LOSS_TIMEOUT = 10000UL;
const unsigned long WIFI_RETRY_INTERVAL = 30000UL;
const unsigned long WIFI_RETRY_TIMEOUT = 5000UL;

enum NetworkMode {
  MODE_CONNECTING,
  MODE_ONLINE,
  MODE_OFFLINE
};

NetworkMode networkMode = MODE_CONNECTING;
bool manualOfflineMode = false;
bool espNowReady = false;
bool otaReady = false;
unsigned long networkAttemptStarted = 0;
unsigned long wifiLostAt = 0;
unsigned long lastWiFiRetry = 0;
bool wifiRetryInProgress = false;

// Use regular WiFiClient (non-secure)
WiFiClient espClient;
PubSubClient client(espClient);
String mqttTopic(const char* suffix) {
  return String(mqtt_root) + suffix;
}

// ESP-NOW callbacks run on the Wi-Fi task. They only enqueue fixed-size data;
// JSON parsing and node-cache updates stay in loop() where they cannot block
// the radio callback.
#define MAX_NODES 20
#define ESPNOW_MAX_JSON 250
#define ESPNOW_QUEUE_DEPTH 8
#define NODE_STALE_TIMEOUT 30000UL

struct ReceivedPacket {
  uint8_t mac[6];
  uint16_t length;
  char json[ESPNOW_MAX_JSON + 1];
};

struct NodeData {
  char deviceId[33];
  char jsonData[ESPNOW_MAX_JSON + 1];
  char macAddress[18];
  unsigned long lastSeen;
  bool active;
};

QueueHandle_t espNowQueue = nullptr;
NodeData nodes[MAX_NODES] = {};
portMUX_TYPE nodesMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t droppedEspNowPackets = 0;
String bootResetReason = "unknown";

// ------------------- Strings / CLI -------------------
String inputString = "";

// ------------------- NTP Time Sync -------------------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 0);
unsigned long lastTimeSync = 0;
const unsigned long timeSyncInterval = 3600000UL;

// ------------------- Button / LCD state -------------------
int buttonState = HIGH;
int lastButtonState = HIGH;
unsigned long lcdOnStartTime = 0;
bool lcdIsOn = true;

// ------------------- LCD display timing -------------------
unsigned long lcdLastUpdate = 0;
int lcdDisplayStage = 0;

// ------------------- MQTT reconnect timing -------------------
unsigned long lastReconnectAttempt = 0;
const long reconnectInterval = 5000;

// ------------------- Web server -------------------
AsyncWebServer server(80);

// ------------------- Relay state array -------------------
bool relayState[4] = {false, false, false, false};

// ------------------- Manual override flags -------------------
bool manualRelay[4] = {false, false, false, false};
unsigned long manualStartTime[4] = {0, 0, 0, 0};
const unsigned long manualOverrideDuration = 30UL * 1000UL;

// ------------------- SCHEDULE STRUCTURE -------------------
struct RelaySchedule {
  int id;
  int relay;
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
  bool days[7];
  bool enabled;
  // NEW: Interval schedule fields
  bool isInterval;      // true = interval mode, false = time range mode
  int onMinutes;        // minutes ON
  int offMinutes;       // minutes OFF
  int startHourInterval; // when interval should start (optional)
  int startMinuteInterval;
  int endHourInterval;   // when interval should end (optional)
  int endMinuteInterval;
};

const int MAX_SCHEDULES_PER_RELAY = 5;
RelaySchedule schedules[4][MAX_SCHEDULES_PER_RELAY];
int scheduleCount[4] = {0, 0, 0, 0};

// ------------------- CRON PROFILES -------------------
// CRON is persisted separately from the original schedule format. This keeps
// existing installations readable while making the new UI/API authoritative
// once a relay has a CRON profile.
const size_t CRON_EXPRESSION_MAX = 96;
struct CronProfile {
  bool configured;
  int relay;
  bool enabled;
  bool hasSecondsPrecision;
  char cronOn[CRON_EXPRESSION_MAX];
  char cronOff[CRON_EXPRESSION_MAX];
};

CronProfile cronProfiles[4];

// ------------------- INTERVAL COUNTDOWN TRACKING -------------------
struct IntervalCountdown {
  int relayIndex;
  int scheduleIndex;
  bool isOn;
  int secondsRemaining;
  unsigned long lastLogTime;
};

IntervalCountdown intervalCountdowns[4] = {
  { -1, -1, false, 0, 0},
  { -1, -1, false, 0, 0},
  { -1, -1, false, 0, 0},
  { -1, -1, false, 0, 0}
};

// ------------------- Logging -------------------
static void logMessage(const char* level, const char* fmt, va_list args) {
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, args);
  Serial.print(level);
  Serial.println(buf);
}
static void logDebug(const char* fmt, ...) {
  (void)fmt; // Serial output is reserved for errors.
}
static void logInfo(const char* fmt, ...) {
  (void)fmt; // Suppress routine INFO output on Serial.
}
static void logWarn(const char* fmt, ...) {
  (void)fmt; // Suppress routine WARNING output on Serial.
}
static void logError(const char* fmt, ...) {
  va_list args; va_start(args, fmt); logMessage("[ERROR] ", fmt, args); va_end(args);
}

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external-pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "other-watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

const char* networkModeName() {
  if (networkMode == MODE_ONLINE) return "online";
  if (networkMode == MODE_OFFLINE) return "offline";
  return "connecting";
}

void loadWiFiSettings() {
  preferences.begin("farmwiz", true);
  wifiSSID = preferences.getString("ssid", DEFAULT_SSID);
  wifiPassword = preferences.getString("password", DEFAULT_PASSWORD);
  preferences.end();
  logInfo("WiFi configuration loaded for SSID: %s", wifiSSID.c_str());
}

void saveWiFiSettings() {
  preferences.begin("farmwiz", false);
  preferences.putString("ssid", wifiSSID);
  preferences.putString("password", wifiPassword);
  preferences.end();
  logInfo("WiFi configuration saved for SSID: %s", wifiSSID.c_str());
}

String macToString(const uint8_t* mac) {
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

int findNodeSlot(const char* id, const char* mac) {
  // MAC address is the physical identity. This prevents two nodes that
  // accidentally share a deviceId from overwriting each other.
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].active && strcmp(nodes[i].macAddress, mac) == 0) return i;
  }
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].active) return i;
  }
  return -1;
}

void queueEspNowPacket(const uint8_t* mac, const uint8_t* data, int len) {
  if (!espNowQueue || len <= 0 || len > ESPNOW_MAX_JSON) {
    droppedEspNowPackets++;
    return;
  }

  ReceivedPacket packet{};
  memcpy(packet.mac, mac, 6);
  packet.length = (uint16_t)len;
  memcpy(packet.json, data, len);
  packet.json[len] = '\0';
  if (xQueueSend(espNowQueue, &packet, 0) != pdTRUE) droppedEspNowPackets++;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowDataReceived(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info) {
    droppedEspNowPackets++;
    return;
  }
  queueEspNowPacket(info->src_addr, data, len);
}
#else
void onEspNowDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
  queueEspNowPacket(mac, data, len);
}
#endif

void processEspNowPackets() {
  if (!espNowQueue) return;

  ReceivedPacket packet;
  while (xQueueReceive(espNowQueue, &packet, 0) == pdTRUE) {
    StaticJsonDocument<512> parsed;
    // Parse as const input so ArduinoJson cannot insert zero-copy null
    // terminators into the receive buffer before it is cached.
    const char* jsonInput = packet.json;
    DeserializationError error = deserializeJson(parsed, jsonInput, packet.length);
    if (error) {
      logWarn("Rejected ESP-NOW JSON: %s", error.c_str());
      continue;
    }

    const char* receivedId = parsed["deviceId"];
    if (!receivedId || receivedId[0] == '\0' || strlen(receivedId) > 32) {
      logWarn("Rejected ESP-NOW packet without a valid deviceId");
      continue;
    }

    String senderMac = macToString(packet.mac);
    portENTER_CRITICAL(&nodesMux);
    int slot = findNodeSlot(receivedId, senderMac.c_str());
    if (slot >= 0) {
      snprintf(nodes[slot].deviceId, sizeof(nodes[slot].deviceId), "%s", receivedId);
      // Preserve the received ESP-NOW bytes exactly; do not depend on a
      // printf/string scan to determine payload length.
      size_t copyLength = packet.length;
      if (copyLength > ESPNOW_MAX_JSON) copyLength = ESPNOW_MAX_JSON;
      memcpy(nodes[slot].jsonData, packet.json, copyLength);
      nodes[slot].jsonData[copyLength] = '\0';
      snprintf(nodes[slot].macAddress, sizeof(nodes[slot].macAddress), "%s", senderMac.c_str());
      nodes[slot].lastSeen = millis();
      nodes[slot].active = true;
    }
    portEXIT_CRITICAL(&nodesMux);

    if (slot >= 0) logInfo("ESP-NOW node %s received from %s", receivedId, senderMac.c_str());
    else logWarn("ESP-NOW node cache is full");
  }
}

String buildNodesJson() {
  DynamicJsonDocument response(8192);
  JsonArray array = response.to<JsonArray>();
  unsigned long now = millis();

  for (int i = 0; i < MAX_NODES; i++) {
    NodeData snapshot{};
    portENTER_CRITICAL(&nodesMux);
    snapshot = nodes[i];
    portEXIT_CRITICAL(&nodesMux);
    if (!snapshot.active) continue;

    JsonObject node = array.createNestedObject();
    node["deviceId"] = snapshot.deviceId;
    node["mac"] = snapshot.macAddress;
    unsigned long ageMs = now - snapshot.lastSeen;
    node["lastSeen"] = snapshot.lastSeen;
    node["ageMs"] = ageMs;
    node["stale"] = ageMs > NODE_STALE_TIMEOUT;

    StaticJsonDocument<512> sensorDoc;
    if (!deserializeJson(sensorDoc, static_cast<const char*>(snapshot.jsonData))) {
      JsonObject data = node.createNestedObject("data");
      for (JsonPairConst item : sensorDoc.as<JsonObjectConst>()) {
        data[item.key()] = item.value();
        // Also expose fields at node level for simple clients and diagnostics.
        node[item.key()] = item.value();
      }
    }
  }

  String output;
  serializeJson(response, output);
  return output;
}

// ------------------- Helpers for schedules -------------------
bool daysEqual(const bool a[7], const bool b[7]) {
  for (int i = 0; i < 7; i++) if (a[i] != b[i]) return false;
  return true;
}
void copyDays(bool dest[7], const JsonArray& arr) {
  for (int i = 0; i < 7; i++) dest[i] = (i < (int)arr.size()) ? (bool)arr[i] : false;
}
int findScheduleIndexById(int relayIdx, int id) {
  for (int i = 0; i < scheduleCount[relayIdx]; i++) if (schedules[relayIdx][i].id == id) return i;
  return -1;
}
int findScheduleIndexByFields(int relayIdx, int sh, int sm, int eh, int em, const bool days[7]) {
  for (int i = 0; i < scheduleCount[relayIdx]; i++) {
    RelaySchedule &s = schedules[relayIdx][i];
    if (s.startHour == sh && s.startMinute == sm && s.endHour == eh && s.endMinute == em && daysEqual(s.days, days)) return i;
  }
  return -1;
}

// ------------------- NTP TIME SYNC -------------------
bool syncTimeWithNTP() {
  lastNtpAttempt = millis();
  if (WiFi.status() != WL_CONNECTED) {
    logError("NTP fallback unavailable: WiFi is not connected");
    return false;
  }
  logInfo("Syncing RTC with NTP...");
  if (!timeClient.forceUpdate()) {
    logError("NTP time synchronization failed");
    return false;
  }
  unsigned long epochTime = timeClient.getEpochTime();
  if (epochTime < 1700000000UL) {
    logError("NTP returned an invalid epoch; time was not updated");
    return false;
  }
  timeval tv = {(time_t)epochTime, 0};
  settimeofday(&tv, nullptr);
  systemClockValid = true;
  if (rtcAvailable) rtc.adjust(DateTime(epochTime));
  lastTimeSync = millis();
  initialScheduleSyncComplete = true;
  lastCronEvaluationSecond = -1;
  logInfo("RTC synced with NTP successfully!");
  return true;
}

bool initializeClockFromRTC() {
  if (!rtcAvailable) return false;
  if (rtc.lostPower()) {
    logError("RTC lost power; falling back to NTP");
    return false;
  }

  DateTime rtcTime = rtc.now();
  if (rtcTime.unixtime() < 1700000000UL) {
    logError("RTC time is invalid; falling back to NTP");
    return false;
  }

  timeval tv = {(time_t)rtcTime.unixtime(), 0};
  settimeofday(&tv, nullptr);
  systemClockValid = true;
  initialScheduleSyncComplete = true;
  lastCronEvaluationSecond = -1;
  return true;
}

// ------------------- PERSISTENCE -------------------
void saveSchedulesToPreferences() {
  preferences.begin("schedules", false);
  for (int relay = 0; relay < 4; relay++) {
    String key = "relay" + String(relay) + "_count";
    preferences.putInt(key.c_str(), scheduleCount[relay]);
    for (int i = 0; i < scheduleCount[relay]; i++) {
      key = "relay" + String(relay) + "_sch" + String(i);
      preferences.putBytes(key.c_str(), &schedules[relay][i], sizeof(RelaySchedule));
    }
  }
  preferences.end();
  logInfo("Schedules saved");
}

void loadSchedulesFromPreferences() {
  preferences.begin("schedules", true);
  bool any = false;
  for (int relay = 0; relay < 4; relay++) {
    String key = "relay" + String(relay) + "_count";
    scheduleCount[relay] = preferences.getInt(key.c_str(), 0);
    if (scheduleCount[relay] > 0) {
      any = true;
      for (int i = 0; i < scheduleCount[relay]; i++) {
        key = "relay" + String(relay) + "_sch" + String(i);
        size_t got = preferences.getBytes(key.c_str(), &schedules[relay][i], sizeof(RelaySchedule));
        if (got != sizeof(RelaySchedule)) {
          logWarn("Failed to load schedule %d for relay %d", i, relay + 1);
          scheduleCount[relay] = 0;
          break;
        }
      }
    }
  }
  preferences.end();
  if (!any) {
    logInfo("No schedules found, initializing defaults");
    for (int relay = 0; relay < 4; relay++) {
      scheduleCount[relay] = 1;
      RelaySchedule s{};
      s.id     = relay * 10 + 1;
      s.relay  = relay + 1;
      s.enabled = true;
      s.isInterval = false; // Default to time range mode
      s.onMinutes = 0;
      s.offMinutes = 0;
      s.startHourInterval = 0;
      s.startMinuteInterval = 0;
      s.endHourInterval = 0;
      s.endMinuteInterval = 0;

      if (relay == 0) { // Sprinkler
        s.startHour = 6; s.startMinute = 0; s.endHour = 6; s.endMinute = 30;
        for (int d = 0; d < 7; d++) s.days[d] = (d >= 1 && d <= 5);
      } else if (relay == 1) { // Light
        s.startHour = 6; s.startMinute = 0; s.endHour = 21; s.endMinute = 0;
        for (int d = 0; d < 7; d++) s.days[d] = true;
      } else if (relay == 2) { // Air
        s.startHour = 0; s.startMinute = 0; s.endHour = 0; s.endMinute = 15;
        for (int d = 0; d < 7; d++) s.days[d] = true;
      } else { // Irrigation pump
        s.startHour = 6; s.startMinute = 0; s.endHour = 21; s.endMinute = 0;
        for (int d = 0; d < 7; d++) s.days[d] = true;
      }
      schedules[relay][0] = s;
    }
    saveSchedulesToPreferences();
  }
}

bool parseCronNumber(const String& text, int& value) {
  String input = text;
  input.trim();
  if (input.length() == 0) return false;
  for (size_t i = 0; i < input.length(); i++) {
    if (!isDigit(input.charAt(i))) return false;
  }
  value = input.toInt();
  return true;
}

bool cronTokenValid(String token, int minValue, int maxValue) {
  token.trim();
  if (token.length() == 0) return false;

  int step = 1;
  int slash = token.indexOf('/');
  String base = token;
  if (slash >= 0) {
    if (token.indexOf('/', slash + 1) >= 0) return false;
    base = token.substring(0, slash);
    if (!parseCronNumber(token.substring(slash + 1), step) || step < 1) return false;
  }

  int start = minValue;
  int end = maxValue;
  base.trim();
  if (base == "*" || base == "?") {
    // Full field range.
  } else {
    int dash = base.indexOf('-');
    if (dash >= 0) {
      if (base.indexOf('-', dash + 1) >= 0) return false;
      if (!parseCronNumber(base.substring(0, dash), start) ||
          !parseCronNumber(base.substring(dash + 1), end)) return false;
    } else {
      if (!parseCronNumber(base, start)) return false;
      end = (slash >= 0) ? maxValue : start;
    }
  }

  return start >= minValue && end <= maxValue && start <= end;
}

bool cronFieldValid(const String& field, int minValue, int maxValue) {
  String input = field;
  input.trim();
  if (input.length() == 0) return false;

  int start = 0;
  while (start < input.length()) {
    int comma = input.indexOf(',', start);
    String token = comma < 0 ? input.substring(start) : input.substring(start, comma);
    if (!cronTokenValid(token, minValue, maxValue)) return false;
    if (comma < 0) break;
    start = comma + 1;
  }
  return true;
}

bool cronExpressionValid(const char* expression) {
  if (!expression || !*expression) return false;
  String input = expression;
  String fields[6];
  int fieldCount = 0;
  int start = 0;
  input.trim();
  while (start < input.length() && fieldCount < 6) {
    while (start < input.length() && input.charAt(start) == ' ') start++;
    if (start >= input.length()) break;
    int end = input.indexOf(' ', start);
    fields[fieldCount++] = end < 0 ? input.substring(start) : input.substring(start, end);
    start = end < 0 ? input.length() : end + 1;
  }
  if (fieldCount != 6) return false;

  const int mins[6] = {0, 0, 0, 1, 1, 0};
  const int maxs[6] = {59, 59, 23, 31, 12, 6};
  for (int i = 0; i < 6; i++) {
    if (!cronFieldValid(fields[i], mins[i], maxs[i])) return false;
  }
  return true;
}

bool cronFieldMatches(const String& field, int value, int minValue, int maxValue);

bool cronExpressionNeedsSecondPrecision(const char* expression) {
  if (!cronExpressionValid(expression)) return false;
  String input = expression;
  input.trim();
  int end = input.indexOf(' ');
  if (end < 0) return false;
  String secondsField = input.substring(0, end);
  for (int second = 1; second <= 59; second++) {
    if (cronFieldMatches(secondsField, second, 0, 59)) return true;
  }
  return false;
}

void copyCronText(char* destination, const char* source) {
  if (!destination) return;
  snprintf(destination, CRON_EXPRESSION_MAX, "%s", source ? source : "");
}

void resetCronProfile(CronProfile& profile, int relay) {
  profile.configured = false;
  profile.relay = relay;
  profile.enabled = false;
  profile.hasSecondsPrecision = false;
  profile.cronOn[0] = '\0';
  profile.cronOff[0] = '\0';
}

void saveCronProfilesToPreferences() {
  preferences.begin("cron", false);
  for (int relay = 0; relay < 4; relay++) {
    String prefix = "r" + String(relay);
    preferences.putBool((prefix + "_configured").c_str(), cronProfiles[relay].configured);
    preferences.putBool((prefix + "_enabled").c_str(), cronProfiles[relay].enabled);
    preferences.putBool((prefix + "_seconds").c_str(), cronProfiles[relay].hasSecondsPrecision);
    preferences.putString((prefix + "_on").c_str(), cronProfiles[relay].cronOn);
    preferences.putString((prefix + "_off").c_str(), cronProfiles[relay].cronOff);
  }
  preferences.end();
  logInfo("CRON profiles saved");
}

void loadCronProfilesFromPreferences() {
  preferences.begin("cron", true);
  for (int relay = 0; relay < 4; relay++) {
    resetCronProfile(cronProfiles[relay], relay + 1);
    String prefix = "r" + String(relay);
    cronProfiles[relay].configured = preferences.getBool((prefix + "_configured").c_str(), false);
    cronProfiles[relay].relay = relay + 1;
    cronProfiles[relay].enabled = preferences.getBool((prefix + "_enabled").c_str(), false);
    cronProfiles[relay].hasSecondsPrecision = preferences.getBool((prefix + "_seconds").c_str(), false);
    String on = preferences.getString((prefix + "_on").c_str(), "");
    String off = preferences.getString((prefix + "_off").c_str(), "");
    copyCronText(cronProfiles[relay].cronOn, on.c_str());
    copyCronText(cronProfiles[relay].cronOff, off.c_str());
    if (cronProfiles[relay].configured &&
        (!cronExpressionValid(cronProfiles[relay].cronOn) ||
         !cronExpressionValid(cronProfiles[relay].cronOff))) {
      logWarn("Ignoring invalid persisted CRON profile for relay %d", relay + 1);
      resetCronProfile(cronProfiles[relay], relay + 1);
    }
  }
  preferences.end();
}

void saveRelayStatesToPreferences() {
  preferences.begin("relays", false);
  for (int i = 0; i < 4; i++) {
    String key = "state" + String(i);
    preferences.putBool(key.c_str(), relayState[i]);
  }
  preferences.end();
}

void loadRelayStatesFromPreferences() {
  preferences.begin("relays", true);
  for (int i = 0; i < 4; i++) {
    String key = "state" + String(i);
    relayState[i] = preferences.getBool(key.c_str(), false);
    digitalWrite(RELAY_PINS[i], relayState[i] ? HIGH : LOW);
    logInfo("Loaded relay %d state: %s (Pin %d -> %s)",
            i + 1, relayState[i] ? "ON" : "OFF", RELAY_PINS[i],
            relayState[i] ? "HIGH" : "LOW");
  }
  preferences.end();
}

// ------------------- SEND RELAY STATE TO THINGSBOARD (INSTANT) -------------------
void sendRelayStateToThingsBoard() {
  if (!client.connected()) {
    logWarn("MQTT not connected, cannot send relay state");
    return;
  }

  StaticJsonDocument<192> doc;
  // Preserve the existing waterValve key while exposing the confirmed name.
  doc["pump"] = relayState[0];      // Relay 1 = Sprinkler = Pump
  doc["light"] = relayState[1];     // Relay 2 = Light
  doc["airPump"] = relayState[2];   // Relay 3 = Air Pump
  doc["waterValve"] = relayState[3];
  doc["irrigationPump"] = relayState[3]; // Relay 4 = Irrigation pump

  // Add timestamp
  DateTime now = currentUtcTime();
  int sh, sm, sd;
  getSurinameTime(now, sh, sm, sd);
  char dateTimeStr[25];
  snprintf(dateTimeStr, sizeof(dateTimeStr), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           now.year(), now.month(), now.day(), sh, sm, now.second());
  doc["timestamp"] = dateTimeStr;

  String jsonString;
  serializeJson(doc, jsonString);

  if (client.publish(mqttTopic( "v1/devices/me/telemetry").c_str(), jsonString.c_str())) {
    logInfo("INSTANT relay state sent to ThingsBoard: %s", jsonString.c_str());
  } else {
    logWarn("Failed to send instant relay state to ThingsBoard");
  }
}

// ------------------- RELAY -------------------
void setRelay(int relay, bool state, bool manual = false) {
  if (relay < 1 || relay > 4) {
    logError("Invalid relay number: %d", relay);
    return;
  }
  int idx = relay - 1;

  const char* name = (relay == 1) ? "sprinkler" : (relay == 2) ? "light" : (relay == 3) ? "airPump" : "irrigationPump";

  logInfo("setRelay(%d:%s, %s, manual=%s) - Pin %d",
          relay, name, state ? "ON" : "OFF", manual ? "true" : "false", RELAY_PINS[idx]);

  relayState[idx] = state;

  if (manual) {
    manualRelay[idx] = true;
    manualStartTime[idx] = millis();
    logInfo("Manual override set for Relay %d (30 seconds)", relay);
  }

  digitalWrite(RELAY_PINS[idx], state ? HIGH : LOW);

  logInfo("Relay %d pin %d set to %s (Physical: %s)",
          relay, RELAY_PINS[idx], state ? "ON" : "OFF", state ? "HIGH" : "LOW");
  saveRelayStatesToPreferences();

  // SEND INSTANT UPDATE TO THINGSBOARD whenever relay changes
  if (client.connected()) {
    sendRelayStateToThingsBoard();
  } else {
    logWarn("MQTT not connected, relay state will be sent on next scheduled update");
  }
}

bool cronTokenMatches(String token, int value, int minValue, int maxValue) {
  token.trim();
  int step = 1;
  int slash = token.indexOf('/');
  String base = token;
  if (slash >= 0) {
    step = token.substring(slash + 1).toInt();
    base = token.substring(0, slash);
  }

  int start = minValue;
  int end = maxValue;
  base.trim();
  if (base != "*" && base != "?") {
    int dash = base.indexOf('-');
    if (dash >= 0) {
      start = base.substring(0, dash).toInt();
      end = base.substring(dash + 1).toInt();
    } else {
      start = base.toInt();
      end = slash >= 0 ? maxValue : start;
    }
  }

  if (step < 1 || value < start || value > end) return false;
  return ((value - start) % step) == 0;
}

bool cronFieldMatches(const String& field, int value, int minValue, int maxValue) {
  int start = 0;
  while (start < field.length()) {
    int comma = field.indexOf(',', start);
    String token = comma < 0 ? field.substring(start) : field.substring(start, comma);
    if (cronTokenMatches(token, value, minValue, maxValue)) return true;
    if (comma < 0) break;
    start = comma + 1;
  }
  return false;
}

bool cronExpressionMatches(const char* expression, const DateTime& utcNow) {
  if (!cronExpressionValid(expression)) return false;

  // RTC/NTP stay in UTC; CRON expressions use the configured local timezone.
  DateTime localNow = toCronLocalTime(utcNow);
  String input = expression;
  String fields[6];
  int fieldCount = 0;
  int start = 0;
  while (start < input.length() && fieldCount < 6) {
    while (start < input.length() && input.charAt(start) == ' ') start++;
    if (start >= input.length()) break;
    int end = input.indexOf(' ', start);
    fields[fieldCount++] = end < 0 ? input.substring(start) : input.substring(start, end);
    start = end < 0 ? input.length() : end + 1;
  }
  if (fieldCount != 6) return false;

  const int values[6] = {
    localNow.second(), localNow.minute(), localNow.hour(),
    localNow.day(), localNow.month(), localNow.dayOfTheWeek()
  };
  const int mins[6] = {0, 0, 0, 1, 1, 0};
  const int maxs[6] = {59, 59, 23, 31, 12, 6};
  for (int i = 0; i < 6; i++) {
    if (!cronFieldMatches(fields[i], values[i], mins[i], maxs[i])) return false;
  }
  return true;
}

const unsigned long CRON_SECOND_RESOLUTION_MS = 1000UL;
const unsigned long CRON_MINUTE_RESOLUTION_MS = 60000UL;
unsigned long cronEvaluationIntervalMs = CRON_MINUTE_RESOLUTION_MS;

// The main loop must keep servicing networking, the watchdog, and sensors.
// This sets the CRON evaluation cadence without delaying that loop.
void updateCronResolution() {
  cronEvaluationIntervalMs = CRON_MINUTE_RESOLUTION_MS;
  for (int relay = 0; relay < 4; relay++) {
    const CronProfile& profile = cronProfiles[relay];
    if (profile.configured && profile.enabled &&
        (profile.hasSecondsPrecision ||
         cronExpressionNeedsSecondPrecision(profile.cronOn) ||
         cronExpressionNeedsSecondPrecision(profile.cronOff))) {
      cronEvaluationIntervalMs = CRON_SECOND_RESOLUTION_MS;
      return;
    }
  }
}

void evaluateCronProfileNow(int relayIndex, const DateTime& now) {
  if (relayIndex < 0 || relayIndex >= 4 || !initialScheduleSyncComplete) return;
  if (!rtcAvailable && !systemClockValid) return;
  CronProfile& profile = cronProfiles[relayIndex];
  if (!profile.configured || manualRelay[relayIndex]) return;

  bool onEvent = profile.enabled && cronExpressionMatches(profile.cronOn, now);
  bool offEvent = !profile.enabled || cronExpressionMatches(profile.cronOff, now);
  if (onEvent && !offEvent) setRelay(relayIndex + 1, true);
  else if (offEvent && !onEvent) setRelay(relayIndex + 1, false);
}

void checkCronSchedules() {
  if (!initialScheduleSyncComplete) return;
  if (!rtcAvailable && !systemClockValid) return;
  DateTime now = currentUtcTime();
  int64_t currentSecond = (int64_t)now.unixtime();
  const int64_t stepSeconds = cronEvaluationIntervalMs == CRON_SECOND_RESOLUTION_MS ? 1 : 60;
  const int64_t latestTick = currentSecond - (currentSecond % stepSeconds);
  if (lastCronEvaluationSecond >= latestTick) return;

  int64_t firstTick = lastCronEvaluationSecond < 0
    ? latestTick
    : ((lastCronEvaluationSecond / stepSeconds) + 1) * stepSeconds;

  // Recover missed ticks after a temporarily busy loop. Bound recovery so an
  // RTC jump or long outage cannot monopolize the device's main loop.
  const int64_t maxCatchUpTicks = 3600;
  if (latestTick - firstTick >= maxCatchUpTicks * stepSeconds) {
    firstTick = latestTick - (maxCatchUpTicks - 1) * stepSeconds;
  }

  int64_t lastEventSecond[4] = {-1, -1, -1, -1};
  bool lastEventState[4] = {false, false, false, false};
  for (int64_t tick = firstTick; tick <= latestTick; tick += stepSeconds) {
    DateTime tickTime((uint32_t)tick);
    for (int relay = 0; relay < 4; relay++) {
      CronProfile& profile = cronProfiles[relay];
      if (!profile.configured || manualRelay[relay] || tick < cronProfileValidFrom[relay]) continue;

      bool onEvent = profile.enabled && cronExpressionMatches(profile.cronOn, tickTime);
      bool offEvent = !profile.enabled || cronExpressionMatches(profile.cronOff, tickTime);
      if (onEvent != offEvent) {
        lastEventSecond[relay] = tick;
        lastEventState[relay] = onEvent;
      }
    }
  }

  lastCronEvaluationSecond = latestTick;
  for (int relay = 0; relay < 4; relay++) {
    if (lastEventSecond[relay] >= 0 && !manualRelay[relay]) {
      setRelay(relay + 1, lastEventState[relay]);
    }
  }
  updateCronResolution();
}

// ------------------- TIMEZONE -------------------
void getSurinameTime(DateTime utcTime, int &surinameHour, int &surinameMinute, int &surinameDay) {
  DateTime localTime = toCronLocalTime(utcTime);
  surinameHour = localTime.hour();
  surinameMinute = localTime.minute();
  surinameDay = localTime.dayOfTheWeek();
}

// ------------------- INTERVAL TRACKING -------------------
struct IntervalState {
  bool isOn;
  unsigned long cycleStartTime;
  int currentCycleOnMinutes;
  int currentCycleOffMinutes;
};

IntervalState intervalStates[4] = {
  {false, 0, 0, 0},
  {false, 0, 0, 0},
  {false, 0, 0, 0},
  {false, 0, 0, 0}
};

// ------------------- SCHEDULE CHECK WITH INTERVAL SUPPORT -------------------
void checkSchedules(int relayOnly = 0) {
  if (!initialScheduleSyncComplete) return;
  DateTime now = currentUtcTime();
  int sh, sm, sd;
  getSurinameTime(now, sh, sm, sd);
  int currentTotal = sh * 60 + sm;

  logInfo("=== CHECKING SCHEDULES at %02d:%02d (Day %d) ===", sh, sm, sd);

  for (int relay = 0; relay < 4; relay++) {
    if (relayOnly > 0 && relay != relayOnly - 1) continue;
    if (cronProfiles[relay].configured) continue;
    if (manualRelay[relay]) {
      logDebug("Relay %d: Manual override active", relay + 1);
      continue;
    }

    bool shouldBeOn = false;
    bool hasIntervalSchedule = false;
    String activeSchedule = "None";

    for (int i = 0; i < scheduleCount[relay]; i++) {
      RelaySchedule &s = schedules[relay][i];
      if (!s.enabled) continue;
      if (!s.days[sd]) continue;

      if (s.isInterval) {
        // INTERVAL SCHEDULE
        hasIntervalSchedule = true;

        // Check if current time is within the interval's active window (if defined)
        bool inWindow = true;
        if (s.startHourInterval != 0 || s.startMinuteInterval != 0 || s.endHourInterval != 0 || s.endMinuteInterval != 0) {
          int startTotal = s.startHourInterval * 60 + s.startMinuteInterval;
          int endTotal = s.endHourInterval * 60 + s.endMinuteInterval;

          if (startTotal < endTotal) {
            inWindow = (currentTotal >= startTotal && currentTotal < endTotal);
          } else {
            // Overnight interval
            inWindow = (currentTotal >= startTotal || currentTotal < endTotal);
          }
        }

        if (inWindow && s.onMinutes > 0 && s.offMinutes > 0) {
          // Calculate interval cycle
          int totalCycleMinutes = s.onMinutes + s.offMinutes;
          int minutesSinceMidnight = sh * 60 + sm;

          // Get the start time of the interval window
          int windowStartMinutes = s.startHourInterval * 60 + s.startMinuteInterval;

          // Calculate minutes since window start (handle overnight)
          int minutesSinceWindowStart;
          if (windowStartMinutes <= minutesSinceMidnight) {
            minutesSinceWindowStart = minutesSinceMidnight - windowStartMinutes;
          } else {
            minutesSinceWindowStart = (24 * 60 - windowStartMinutes) + minutesSinceMidnight;
          }

          int cyclePosition = minutesSinceWindowStart % totalCycleMinutes;

          // Calculate time remaining in current cycle phase (in seconds)
          int secondsSinceMidnight = sh * 3600 + sm * 60 + now.second();
          int windowStartSeconds = s.startHourInterval * 3600 + s.startMinuteInterval * 60;
          int secondsSinceWindowStart;
          if (windowStartSeconds <= secondsSinceMidnight) {
            secondsSinceWindowStart = secondsSinceMidnight - windowStartSeconds;
          } else {
            secondsSinceWindowStart = (24 * 3600 - windowStartSeconds) + secondsSinceMidnight;
          }

          int totalCycleSeconds = totalCycleMinutes * 60;
          int cyclePositionSeconds = secondsSinceWindowStart % totalCycleSeconds;
          int onSeconds = s.onMinutes * 60;

          int remainingSeconds;
          String phase;

          if (cyclePositionSeconds < onSeconds) {
            shouldBeOn = true;
            remainingSeconds = onSeconds - cyclePositionSeconds;
            phase = "ON";
            activeSchedule = String(i + 1) + " (Interval: " + String(s.onMinutes) + "m ON / " + String(s.offMinutes) + "m OFF)";

            // Log countdown every 10 seconds
            if (millis() - intervalCountdowns[relay].lastLogTime > 10000) {
              logInfo("Relay %d [%s] - %d:%02d remaining in ON phase (Cycle: %d/%d min)",
                      relay + 1, phase.c_str(), remainingSeconds / 60, remainingSeconds % 60,
                      s.onMinutes, s.offMinutes);
              intervalCountdowns[relay].lastLogTime = millis();
              intervalCountdowns[relay].isOn = true;
              intervalCountdowns[relay].secondsRemaining = remainingSeconds;
            }
          } else {
            shouldBeOn = false;
            remainingSeconds = totalCycleSeconds - cyclePositionSeconds;
            phase = "OFF";
            activeSchedule = String(i + 1) + " (Interval: " + String(s.onMinutes) + "m ON / " + String(s.offMinutes) + "m OFF - OFF phase)";

            // Log countdown every 10 seconds
            if (millis() - intervalCountdowns[relay].lastLogTime > 10000) {
              logInfo("Relay %d [%s] - %d:%02d remaining in OFF phase (Next ON in %d:%02d)",
                      relay + 1, phase.c_str(), remainingSeconds / 60, remainingSeconds % 60,
                      remainingSeconds / 60, remainingSeconds % 60);
              intervalCountdowns[relay].lastLogTime = millis();
              intervalCountdowns[relay].isOn = false;
              intervalCountdowns[relay].secondsRemaining = remainingSeconds;
            }
          }

          logInfo("Relay %d: Interval schedule active - cycle position %d/%d min, ON=%d, OFF=%d",
                  relay + 1, cyclePosition, totalCycleMinutes, s.onMinutes, s.offMinutes);
          break;
        }
      } else {
        // TIME RANGE SCHEDULE (original behavior)
        int st = s.startHour * 60 + s.startMinute;
        int et = s.endHour * 60   + s.endMinute;

        if (currentTotal >= st && currentTotal < et) {
          shouldBeOn = true;
          activeSchedule = String(i + 1) + " (" + String(s.startHour) + ":" + String(s.startMinute) + "-" + String(s.endHour) + ":" + String(s.endMinute) + ")";
          break;
        }
      }
    }

    // If no schedule matched and we're not in an interval, turn off
    if (!hasIntervalSchedule && !shouldBeOn) {
      // Check if any time range schedule would turn it on
      for (int i = 0; i < scheduleCount[relay]; i++) {
        RelaySchedule &s = schedules[relay][i];
        if (!s.enabled || s.isInterval) continue;
        if (!s.days[sd]) continue;

        int st = s.startHour * 60 + s.startMinute;
        int et = s.endHour * 60   + s.endMinute;
        if (currentTotal >= st && currentTotal < et) {
          shouldBeOn = true;
          activeSchedule = String(i + 1) + " (" + String(s.startHour) + ":" + String(s.startMinute) + "-" + String(s.endHour) + ":" + String(s.endMinute) + ")";
          break;
        }
      }
    }

    logInfo("Relay %d: Schedule says %s (Active: %s)", relay + 1, shouldBeOn ? "ON" : "OFF", activeSchedule.c_str());
    logInfo("Relay %d: Current state is %s", relay + 1, relayState[relay] ? "ON" : "OFF");

    if (relayState[relay] != shouldBeOn) {
      logInfo("*** CHANGING Relay %d from %s to %s ***",
              relay + 1, relayState[relay] ? "ON" : "OFF", shouldBeOn ? "ON" : "OFF");
      setRelay(relay + 1, shouldBeOn);
    }
  }
  logInfo("=== END SCHEDULE CHECK ===");
}

// ------------------- NETWORK/OTA -------------------
void stopESPNow() {
  if (!espNowReady) return;
  esp_now_deinit();
  espNowReady = false;
  if (espNowQueue) xQueueReset(espNowQueue);
  logInfo("ESP-NOW receiver stopped");
}

bool startESPNow() {
  if (espNowReady) return true;
  if (esp_now_init() != ESP_OK) {
    logError("ESP-NOW initialization failed");
    return false;
  }
  if (esp_now_register_recv_cb(onEspNowDataReceived) != ESP_OK) {
    logError("ESP-NOW receive callback registration failed");
    esp_now_deinit();
    return false;
  }
  espNowReady = true;
  logInfo("ESP-NOW receiver ready on channel %u", ESPNOW_OFFLINE_CHANNEL);
  return true;
}

void startWiFiAttempt(bool recoveryAttempt) {
  stopESPNow();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(50);
  logInfo("Connecting to infrastructure WiFi: %s", wifiSSID.c_str());
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  networkMode = MODE_CONNECTING;
  wifiRetryInProgress = recoveryAttempt;
  networkAttemptStarted = millis();
}

void enterOnlineMode() {
  stopESPNow();
  WiFi.softAPdisconnect(true);
  networkMode = MODE_ONLINE;
  wifiRetryInProgress = false;
  wifiLostAt = 0;
  logInfo("Network mode ONLINE: IP=%s channel=%d RSSI=%d",
          WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
  if (!otaReady) setupOTA();
}

void enterOfflineMode() {
  if (client.connected()) client.disconnect();
  stopESPNow();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  bool apReady = WiFi.softAP(OFFLINE_AP_SSID, OFFLINE_AP_PASSWORD, ESPNOW_OFFLINE_CHANNEL);
  if (!apReady) logError("Failed to start offline AP");
  else logInfo("Offline AP ready: SSID=%s IP=%s channel=%u",
                 OFFLINE_AP_SSID, WiFi.softAPIP().toString().c_str(), ESPNOW_OFFLINE_CHANNEL);
  startESPNow();
  networkMode = MODE_OFFLINE;
  wifiRetryInProgress = false;
  wifiLostAt = 0;
  lastWiFiRetry = millis();
}

void handleNetworkMode() {
  unsigned long now = millis();

  if (manualOfflineMode) {
    if (networkMode != MODE_OFFLINE) enterOfflineMode();
    return;
  }

  if (networkMode == MODE_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      enterOnlineMode();
      return;
    }
    unsigned long timeout = wifiRetryInProgress ? WIFI_RETRY_TIMEOUT : WIFI_CONNECT_TIMEOUT;
    if (now - networkAttemptStarted >= timeout) {
      logWarn("Infrastructure WiFi unavailable; entering fixed-channel offline mode");
      enterOfflineMode();
    }
    return;
  }

  if (networkMode == MODE_ONLINE) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiLostAt = 0;
      return;
    }
    if (wifiLostAt == 0) {
      wifiLostAt = now;
      logWarn("Infrastructure WiFi lost; allowing %lu ms for recovery", WIFI_LOSS_TIMEOUT);
    } else if (now - wifiLostAt >= WIFI_LOSS_TIMEOUT) {
      logWarn("Infrastructure WiFi did not recover; enabling offline AP and ESP-NOW");
      enterOfflineMode();
    }
    return;
  }

  if (networkMode == MODE_OFFLINE && now - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {
    logInfo("Probing for infrastructure WiFi recovery");
    startWiFiAttempt(true);
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(deviceId);
  ArduinoOTA
  .onStart([]() {
    const char* t = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    logInfo("OTA start (%s)", t);
  })
  .onEnd([]() {
    logInfo("OTA end");
  })
  .onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset();
    (void)progress;
    (void)total;
  })
  .onError([](ota_error_t error) {
    logError("OTA Error[%u]", (unsigned)error);
  });
  ArduinoOTA.begin();
  otaReady = true;
  logInfo("OTA Ready, IP: %s", WiFi.localIP().toString().c_str());
}

bool applyCronProfile(int relay, bool enabled, bool hasSecondsPrecision,
                      const char* cronOn, const char* cronOff, String& error) {
  if (relay < 1 || relay > 4) {
    error = "relay must be between 1 and 4";
    return false;
  }
  if (!cronExpressionValid(cronOn) || !cronExpressionValid(cronOff)) {
    error = "cronOn and cronOff must be valid six-field CRON expressions";
    return false;
  }

  CronProfile& profile = cronProfiles[relay - 1];
  profile.configured = true;
  profile.relay = relay;
  profile.enabled = enabled;
  profile.hasSecondsPrecision = hasSecondsPrecision;
  copyCronText(profile.cronOn, cronOn);
  copyCronText(profile.cronOff, cronOff);
  saveCronProfilesToPreferences();
  updateCronResolution();
  DateTime updateTime = currentUtcTime();
  cronProfileValidFrom[relay - 1] = initialScheduleSyncComplete
    ? (int64_t)updateTime.unixtime() + 1
    : 0;
  evaluateCronProfileNow(relay - 1, updateTime);
  error = "";
  return true;
}

void addCronProfileToJson(JsonObject target, const CronProfile& profile) {
  target["relay"] = profile.relay;
  target["configured"] = profile.configured;
  target["enabled"] = profile.enabled;
  target["hasSecondsPrecision"] = profile.hasSecondsPrecision;
  target["cronOn"] = profile.cronOn;
  target["cronOff"] = profile.cronOff;
}

bool readCapabilitiesJson(String& output) {
  output = "";
  if (!SPIFFS.exists("/capabilities.json")) return false;

  File file = SPIFFS.open("/capabilities.json", "r");
  if (!file) return false;

  output = file.readString();
  file.close();
  return output.length() > 0;
}

bool sendCapabilitiesMQTT() {
  if (!client.connected()) {
    logWarn("MQTT not connected, cannot send capabilities");
    return false;
  }

  String manifest;
  if (!readCapabilitiesJson(manifest)) {
    logError("Capabilities manifest is missing or empty");
    return false;
  }

  DynamicJsonDocument validationDoc(16384);
  DeserializationError error = deserializeJson(validationDoc, manifest);
  if (error || !validationDoc.is<JsonObject>()) {
    logError("Capabilities manifest is invalid JSON");
    return false;
  }

  if (!client.publish(mqttTopic( "v1/devices/me/telemetry").c_str(), manifest.c_str())) {
    logWarn("MQTT capabilities publish failed");
    return false;
  }

  logInfo("Capabilities published to upstream MQTT telemetry");
  return true;
}
// ------------------- MQTT CALLBACK -------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  logInfo("MQTT Message arrived on topic: %s", topic);

  char message[length + 1];
  for (unsigned int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';

  logDebug("Message: %s", message);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (!error) {
    // Handle ThingsBoard RPC request
    if (doc.containsKey("method")) {
      String method = doc["method"].as<String>();
      int requestId = 0;
      String requestIdStr = "";

      // Extract request ID from topic (v1/devices/me/rpc/request/18)
      String topicStr = String(topic);
      int lastSlash = topicStr.lastIndexOf('/');
      if (lastSlash != -1) {
        requestIdStr = topicStr.substring(lastSlash + 1);
        requestId = requestIdStr.toInt();
      }

      bool state = false;
      int relayNum = 0;
      String relayKey = "";

      // LOG THE METHOD NAME FOR DEBUGGING
      logInfo("RPC Method received: '%s'", method.c_str());
      if (doc.containsKey("params")) {
        logInfo("Params value: %s", doc["params"].as<String>().c_str());
      }

      // Handle different method names from ThingsBoard
if (method == "getCapabilities") {
        String manifest;
        DynamicJsonDocument responseDoc(16384);
        bool ok = readCapabilitiesJson(manifest);

        if (ok) {
          DeserializationError manifestError = deserializeJson(responseDoc, manifest);
          ok = !manifestError;
        }

        if (ok) {
          responseDoc["id"] = requestId;
          responseDoc["result"] = "SUCCESS";
          responseDoc["deviceId"] = deviceId;
        } else {
          responseDoc.clear();
          responseDoc["id"] = requestId;
          responseDoc["result"] = "ERROR";
          responseDoc["deviceId"] = deviceId;
          responseDoc["error"] = "capabilities_manifest_missing_or_invalid";
        }

        String responseMsg;
        serializeJson(responseDoc, responseMsg);
        String responseTopic = mqttTopic( "v1/devices/me/rpc/response/") + requestIdStr;
        bool published = client.publish(responseTopic.c_str(), responseMsg.c_str());
        logInfo("getCapabilities RPC %s", published ? "sent" : "publish failed");
        return;
      }
      else if (method == "setCron") {
        JsonObject params = doc["params"].as<JsonObject>();
        int cronRelay = params["relay"] | 0;
        bool enabled = params["enabled"] | false;
        bool hasSecondsPrecision = params["hasSecondsPrecision"] | false;
        const char* cronOn = params["cronOn"] | "";
        const char* cronOff = params["cronOff"] | "";
        String applyError;
        bool ok = applyCronProfile(cronRelay, enabled, hasSecondsPrecision,
                                   cronOn, cronOff, applyError);

        StaticJsonDocument<768> responseDoc;
        responseDoc["id"] = requestId;
        responseDoc["result"] = ok ? "SUCCESS" : "ERROR";
        responseDoc["deviceId"] = deviceId;
        if (ok) {
          JsonObject profile = responseDoc.createNestedObject("profile");
          addCronProfileToJson(profile, cronProfiles[cronRelay - 1]);
        } else {
          responseDoc["error"] = applyError;
        }

        String responseMsg;
        serializeJson(responseDoc, responseMsg);
        // TB MQTT receiver
        String responseTopic = mqttTopic( "v1/devices/me/rpc/response/") + requestIdStr;
        client.publish(responseTopic.c_str(), responseMsg.c_str());
        logInfo("MQTT setCron response: %s", responseMsg.c_str());
        return;
      }
      else if (method == "getCron") {
        JsonVariant relayValue = doc["params"]["relay"];
        int requestedRelay = 0;
        if (relayValue.is<int>()) requestedRelay = relayValue.as<int>();
        else {
          String relayText = relayValue | "all";
          if (relayText != "all") requestedRelay = relayText.toInt();
        }

        DynamicJsonDocument responseDoc(3072);
        responseDoc["id"] = requestId;
        responseDoc["result"] = "SUCCESS";
        responseDoc["deviceId"] = deviceId;
        JsonArray profiles = responseDoc.createNestedArray("profiles");
        int count = 0;
        for (int relay = 0; relay < 4; relay++) {
          if (requestedRelay >= 1 && requestedRelay <= 4 && requestedRelay != relay + 1) continue;
          JsonObject profile = profiles.createNestedObject();
          addCronProfileToJson(profile, cronProfiles[relay]);
          count++;
        }
        responseDoc["count"] = count;

        String responseMsg;
        serializeJson(responseDoc, responseMsg);
        String responseTopic = mqttTopic( "v1/devices/me/rpc/response/") + requestIdStr;
        client.publish(responseTopic.c_str(), responseMsg.c_str());
        logInfo("MQTT getCron response: %s", responseMsg.c_str());
        return;
      }
      else if (method == "setPump") {
        relayNum = 1; // Relay 1 = Sprinkler = Pump
        relayKey = "pump";
        state = doc["params"] | false;
        logInfo("Mapping setPump -> Relay 1");
      }
      else if (method == "setLight") {
        relayNum = 2; // Relay 2 = Light
        relayKey = "light";
        state = doc["params"] | false;
        logInfo("Mapping setLight -> Relay 2");
      }
      else if (method == "setAirPump") {
        relayNum = 3; // Relay 3 = Air Pump
        relayKey = "airPump";
        state = doc["params"] | false;
        logInfo("Mapping setAirPump -> Relay 3");
      }
      else if (method == "setWaterValve" || method == "setMirror" || method == "setIrrigationPump") {
        relayNum = 4; // Relay 4 = Irrigation pump; legacy aliases retained
        relayKey = (method == "setIrrigationPump") ? "irrigationPump" : "waterValve";
        state = doc["params"] | false;
        logInfo("Mapping setWaterValve -> Relay 4");
      }
      else if (method == "setRelay") {
        // Fallback for generic setRelay with relay parameter
        if (doc.containsKey("params")) {
          JsonObject params = doc["params"];
          String relayName = params["relay"] | "";
          state = params["state"] | false;

          if (relayName == "pump" || relayName == "sprinkler") {
            relayNum = 1;
            relayKey = "pump";
          }
          else if (relayName == "light") {
            relayNum = 2;
            relayKey = "light";
          }
          else if (relayName == "airPump") {
            relayNum = 3;
            relayKey = "airPump";
          }
          else if (relayName == "waterValve" || relayName == "mirror" || relayName == "irrigationPump") {
            relayNum = 4;
            relayKey = "irrigationPump";
          }
          logInfo("Mapping setRelay -> Relay %d", relayNum);
        }
      }

      // Execute the relay command if we found a valid relay
      if (relayNum >= 1 && relayNum <= 4) {
        logInfo("ThingsBoard RPC: %s -> Relay %d %s",
                method.c_str(), relayNum, state ? "ON" : "OFF");

        setRelay(relayNum, state, true);

        // Send RPC response back to ThingsBoard with the current state
        // This helps the Control widget update its state
        StaticJsonDocument<256> responseDoc;
        responseDoc["id"] = requestId;
        responseDoc["result"] = "SUCCESS";
        responseDoc["state"] = state;  // Send the state back
        responseDoc[relayKey] = state; // Send the specific relay state

        String responseMsg;
        serializeJson(responseDoc, responseMsg);

        String responseTopic = mqttTopic( "v1/devices/me/rpc/response/") + requestIdStr;
        if (client.publish(responseTopic.c_str(), responseMsg.c_str())) {
          logDebug("RPC response sent successfully: %s", responseMsg.c_str());
        } else {
          logWarn("Failed to send RPC response");
        }

        // Send telemetry update immediately
        sendRelayStateToThingsBoard();
      } else {
        logWarn("Unknown method: %s", method.c_str());
      }
    }
  } else {
    logError("Failed to parse MQTT JSON: %s", error.c_str());
  }
}

// ------------------- MQTT RECONNECT (NON-BLOCKING) -------------------
void reconnectMQTT() {
  // Don't block - just attempt connection if time has passed
  if (client.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastReconnectAttempt < reconnectInterval) return;
  lastReconnectAttempt = now;

  logInfo("Attempting MQTT connection to ThingsBoard...");


  if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
    //logInfo("MQTT Connected to ThingsBoard!");
    client.subscribe(mqttTopic( "v1/devices/me/rpc/request/+").c_str());

    // Publish online status
    StaticJsonDocument<128> statusDoc;
    statusDoc["deviceId"] = deviceId;
    statusDoc["status"] = "online";
    String statusMsg;
    serializeJson(statusDoc, statusMsg);
    client.publish(mqttTopic( "v1/devices/me/telemetry").c_str(), statusMsg.c_str());
  } else {
    logError("MQTT connection failed, rc=%d. Will retry in %d ms",
             client.state(), reconnectInterval);
  }
}

// ------------------- DEBUG -------------------
void debugTimeLogging() {
  DateTime now = currentUtcTime();
  int sh, sm, sd; getSurinameTime(now, sh, sm, sd);
  logInfo("UTC %04d-%02d-%02d %02d:%02d:%02d | SRM %02d:%02d DOW=%d",
          now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second(), sh, sm, sd);
}

// ------------------- CLI -------------------
#if 0 // User requested that serial output contain errors only.
void printCLIHelp() {
  Serial.println("\nFarmWiz Control Hub CLI");
  Serial.println("  help                 Show commands");
  Serial.println("  status               Complete hub status");
  Serial.println("  read                 Read local sensors");
  Serial.println("  relays               Show relay and override states");
  Serial.println("  nodes                List received ESP-NOW nodes");
  Serial.println("  send capabilities    Send Capabilities (MQTT)");
  Serial.println("  wifi | mqtt | espnow Show radio/cloud details");
  Serial.println("  ip | time | synctime Show network/time information");
  Serial.println("  on <1-4>             Turn relay on with manual override");
  Serial.println("  off <1-4>            Turn relay off with manual override");
  Serial.println("  release <1-4>        Return relay to schedules");
  Serial.println("  set ssid <SSID>      Stage infrastructure SSID");
  Serial.println("  set password <PASS>  Stage infrastructure password");
  Serial.println("  save                 Persist staged WiFi settings");
  Serial.println("  offline              Diagnostic fixed-channel override");
  Serial.println("  online               Resume automatic WiFi connection");
  Serial.println("  restart | reboot     Restart the hub");
  Serial.println("  wdt test confirm     Deliberately prove watchdog recovery");
}

void printRelayStatus() {
  const char* names[4] = {"sprinkler", "light", "airPump", "irrigationPump"};
  for (int i = 0; i < 4; i++) {
    Serial.printf("R%d %-16s %s | manual=%s | GPIO=%d\n", i + 1, names[i],
                  relayState[i] ? "ON" : "OFF", manualRelay[i] ? "yes" : "no", RELAY_PINS[i]);
  }
}

void printNodeStatus() {
  bool found = false;
  unsigned long now = millis();
  for (int i = 0; i < MAX_NODES; i++) {
    NodeData snapshot{};
    portENTER_CRITICAL(&nodesMux);
    snapshot = nodes[i];
    portEXIT_CRITICAL(&nodesMux);
    if (!snapshot.active) continue;
    found = true;
    unsigned long age = now - snapshot.lastSeen;
    Serial.printf("[%d] %s | %s | age=%lus | %s\n", i, snapshot.deviceId,
                  snapshot.macAddress, age / 1000UL,
                  age > NODE_STALE_TIMEOUT ? "STALE" : "LIVE");
    Serial.printf("    %s\n", snapshot.jsonData);
  }
  if (!found) Serial.println("No ESP-NOW nodes received yet.");
}

void printNetworkStatus() {
  Serial.printf("Mode: %s%s\n", networkModeName(), manualOfflineMode ? " (manual override)" : "");
  Serial.printf("Master MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("Configured SSID: %s\n", wifiSSID.c_str());
  Serial.printf("WiFi: %s | MQTT: %s | ESP-NOW: %s\n",
                WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                client.connected() ? "connected" : "disconnected",
                espNowReady ? "ready" : "off");
  if (networkMode == MODE_OFFLINE) {
    Serial.printf("Offline AP: %s | IP: %s | channel: %u\n", OFFLINE_AP_SSID,
                  WiFi.softAPIP().toString().c_str(), ESPNOW_OFFLINE_CHANNEL);
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP: %s | channel: %d | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
  }
}

void printHubStatus() {
  float t = dht.readTemperature() || 0;
  float h = dht.readHumidity() || 0;
  int ldr = constrain(map(analogRead(LDR_PIN), 0, 2944, 0, 100), 0, 100) || 0;
  Serial.println("\n========== FARMWIZ CONTROL HUB ==========");
  Serial.printf("Device: %s | reset: %s | uptime: %lus\n", deviceId,
                bootResetReason.c_str(), millis() / 1000UL);
  Serial.printf("Temperature: %.1f C | humidity: %.1f %% | light: %d %% | soil: 50 %% (placeholder)\n",
                t, h, ldr);
  printNetworkStatus();
  printRelayStatus();
  Serial.printf("ESP-NOW dropped packets: %lu\n", (unsigned long)droppedEspNowPackets);
  Serial.println("Watchdog: armed after setup, 10 second timeout");
  Serial.println("==========================================");
}
#else
void printCLIHelp() {}
void printRelayStatus() {}
void printNodeStatus() {}
void printNetworkStatus() {}
void printHubStatus() {}
#endif

void processCLICommand(String command) {
  command.trim();
  if (command.length() == 0) return;
  String lower = command;
  lower.toLowerCase();

  if (lower == "help") printCLIHelp();
  else if (lower == "status") printHubStatus();
  else if (lower == "read") {
    float t = dht.readTemperature() || 0;
    float h = dht.readHumidity() || 0;
    int ldr = constrain(map(analogRead(LDR_PIN), 0, 2944, 0, 100), 0, 100) || 0;
    //Serial.printf("Temperature=%.1fC Humidity=%.1f%% Light=%d%% Soil=50%%(placeholder)\n", t, h, ldr);
  }
  else if (lower == "relays") printRelayStatus();
  else if (lower == "nodes") printNodeStatus();
  else if (lower == "send capabilities") sendCapabilitiesMQTT();
  else if (lower == "wifi" || lower == "mqtt" || lower == "espnow" || lower == "ip") printNetworkStatus();
  else if (lower == "time") debugTimeLogging();
  else if (lower == "synctime") {
    if (WiFi.status() == WL_CONNECTED) syncTimeWithNTP(); else logError("No infrastructure WiFi");
  }
  else if (lower.startsWith("on ")) setRelay(lower.substring(3).toInt(), true, true);
  else if (lower.startsWith("off ")) setRelay(lower.substring(4).toInt(), false, true);
  else if (lower.startsWith("release ")) {
    int relay = lower.substring(8).toInt();
    if (relay >= 1 && relay <= 4) {
      manualRelay[relay - 1] = false;
      //logInfo("Relay %d manual override released", relay);
      checkSchedules();
    } else logError("Relay must be 1-4");
  }
  else if (lower.startsWith("set ssid ")) {
    wifiSSID = command.substring(9); wifiSSID.trim();
//    logInfo("Staged SSID: %s (use 'save', then 'online')", wifiSSID.c_str());
  }
  else if (lower.startsWith("set password ")) {
    wifiPassword = command.substring(13); wifiPassword.trim();
//    logInfo("WiFi password staged (use 'save', then 'online')");
  }
  else if (lower == "save") saveWiFiSettings();
  else if (lower == "offline") {
    manualOfflineMode = true;
    enterOfflineMode();
    logInfo("Manual offline diagnostic override enabled; type 'online' to exit");
  }
  else if (lower == "online") {
    manualOfflineMode = false;
    startWiFiAttempt(false);
    logInfo("Automatic infrastructure WiFi connection resumed");
  }
  else if (lower == "restart" || lower == "reboot") {
    logWarn("Restarting control hub");
    delay(200);
    ESP.restart();
  }
  else if (lower == "wdt test confirm") {
    logWarn("Watchdog test armed; loop will stall and should reset within %d seconds", WDT_TIMEOUT);
    while (true) delay(100);
  }
  else logWarn("Unknown command: %s (type 'help')", command.c_str());
}

void handleCLI() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      processCLICommand(inputString);
      inputString = "";
    } else if (inputString.length() < 200) {
      inputString += c;
    }
  }
}

// ------------------- GLOBAL JSON BUFFERS -------------------
StaticJsonDocument<4096> docSchedules;
StaticJsonDocument<2048> docSetSchedule;
StaticJsonDocument<1024> docDelete;
StaticJsonDocument<2048> docCron;

// ------------------- SETUP -------------------
void setup() {
  configureCronTimezone();
  Serial.begin(115200);
  //Serial.setTimeout(100);
  bootResetReason = resetReasonName(esp_reset_reason());
  logInfo("System starting...");
  logInfo("Reset reason: %s", bootResetReason.c_str());

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("FarmWiz Starting");

  // SPIFFS
  lcd.setCursor(0, 1);
  lcd.print("Mounting SPIFFS...");
  if (!SPIFFS.begin(true)) {
    logError("SPIFFS mount failed");
    lcd.clear();
    lcd.print("SPIFFS FAILED!");
    lcd.setCursor(0, 1);
    lcd.print("Upload data folder");
    // Don't block - continue without SPIFFS
    // while(1) { delay(1000); Serial.println("SPIFFS mount failed - upload data folder using 'ESP32 Sketch Data Upload'"); }
  } else {
    logInfo("SPIFFS mounted successfully");
    lcd.setCursor(0, 1);
    lcd.print("SPIFFS OK       ");
  }

  inputString.reserve(200);
  espNowQueue = xQueueCreate(ESPNOW_QUEUE_DEPTH, sizeof(ReceivedPacket));
  if (!espNowQueue) logError("Could not allocate ESP-NOW receive queue");
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  dht.begin();

  // Relays
  logInfo("Setting relay pin modes...");
  for (int i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
    logInfo("Relay %d pin %d set to OUTPUT", i + 1, RELAY_PINS[i]);
  }

  loadRelayStatesFromPreferences();

  pinMode(LDR_PIN, INPUT);
  pinMode(MOISTURE_PIN, INPUT);
  pinMode(buttonPin, INPUT_PULLUP);

  // Begin WiFi asynchronously. Local control and the watchdog are never held
  // inside a long connection loop.
  loadWiFiSettings();
  startWiFiAttempt(false);

  timeClient.begin();
  timeClient.setTimeOffset(0);

  // MQTT with ThingsBoard
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(mqttCallback);
  client.setKeepAlive(60);
  client.setBufferSize(1024);
  client.setSocketTimeout(3);

  // Don't block setup - let MQTT connect in the background
  // if (WiFi.status() == WL_CONNECTED) {
  //   reconnectMQTT(); // REMOVED - non-blocking now
  // }

  // RTC - don't block if not found
  rtcAvailable = rtc.begin();
  if (!rtcAvailable) {
    logError("Couldn't find RTC");
    lcd.clear();
    lcd.print("RTC NOT FOUND!");
    lcd.setCursor(0, 1);
    lcd.print("Using NTP only  ");
    // Continue without blocking - we'll try NTP later
  } else initializeClockFromRTC();

  loadSchedulesFromPreferences();
  loadCronProfilesFromPreferences();
  updateCronResolution();

  // -------- Web Server Routes ----------
  server.on("/list", HTTP_GET, [](AsyncWebServerRequest * request) {
    String output = "SPIFFS files:\n";
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while (file) {
      output += String(file.name()) + " (" + String(file.size()) + " bytes)\n";
      file = root.openNextFile();
    }
    request->send(200, "text/plain", output);
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (SPIFFS.exists("/index.html")) {
      request->send(SPIFFS, "/index.html", "text/html");
    } else {
      String html = "<html><head><title>FarmWiz Control</title><meta charset='UTF-8'></head>";
      html += "<body style='font-family: Arial; text-align: center; margin: 50px;'>";
      html += "<h1>🌱 FarmWiz Environmental Control</h1>";
      html += "<p style='color: red; font-weight: bold;'>SPIFFS files missing!</p>";
      html += "<p>Upload the data folder using:<br><strong>Tools → ESP32 Sketch Data Upload</strong></p>";
      html += "<p><a href='/list'>Check SPIFFS files</a> | <a href='/data'>Sensor Data</a></p>";
      html += "</body></html>";
      request->send(200, "text/html", html);
    }
  });

  server.serveStatic("/", SPIFFS, "/")
  .setDefaultFile("index.html")
  .setCacheControl("no-cache");

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest * request) {
    float temp = dht.readTemperature() || 0;
    float hum  = dht.readHumidity() || 0;
    int rawLDR = analogRead(LDR_PIN) || 0;
    int lightPercent = constrain(map(rawLDR, 0, 2944, 0, 100), 0, 100) || 0;
    int soilMoisturePercent = 50;

    StaticJsonDocument<768> response;
    if (isnan(temp)) temp = 0;
    if (isnan(hum))  hum  = 0;

    response["deviceId"] = deviceId;
    response["firmwareVersion"] = firmwareVersion;
    response["temp"] = temp;
    response["hum"]  = hum;
    response["soilMoisture"] = soilMoisturePercent;
    response["ldr"]  = lightPercent;
    response["uptimeMs"] = millis();
    response["resetReason"] = bootResetReason;

    JsonObject status = response.createNestedObject("relay");
    status["relay1"] = relayState[0];
    status["relay2"] = relayState[1];
    status["relay3"] = relayState[2];
    status["relay4"] = relayState[3];

    int nodeCount = 0;
    portENTER_CRITICAL(&nodesMux);
    for (int i = 0; i < MAX_NODES; i++) if (nodes[i].active) nodeCount++;
    portEXIT_CRITICAL(&nodesMux);

    JsonObject network = response.createNestedObject("network");
    network["mode"] = networkModeName();
    network["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    network["mqttConnected"] = client.connected();
    network["espNowReady"] = espNowReady;
    network["channel"] = networkMode == MODE_OFFLINE ? ESPNOW_OFFLINE_CHANNEL : WiFi.channel();
    network["ip"] = networkMode == MODE_OFFLINE
                    ? WiFi.softAPIP().toString()
                    : WiFi.localIP().toString();
    network["masterMac"] = WiFi.macAddress();
    network["nodeCount"] = nodeCount;
    network["droppedPackets"] = droppedEspNowPackets;

    String jsonStr; serializeJson(response, jsonStr);
    request->send(200, "application/json", jsonStr);
  });

  server.on("/capabilities", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (!SPIFFS.exists("/capabilities.json")) {
      request->send(404, "application/json",
                    R"({"ok":false,"error":"capabilities_manifest_missing"})");
      return;
    }
    request->send(SPIFFS, "/capabilities.json", "application/json");
  });
  server.on("/api/nodes", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "application/json", buildNodesJson());
  });

  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (request->hasParam("relay")) {
      int relay = request->getParam("relay")->value().toInt();
      if (relay >= 1 && relay <= 4) {
        setRelay(relay, !relayState[relay - 1], true);
        request->send(200, "text/plain", relayState[relay - 1] ? "ON" : "OFF");
        return;
      }
    }
    request->send(400, "text/plain", "Invalid relay number");
  });

  server.on("/setSchedule", HTTP_POST,
  [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "Schedule received");
  },
  NULL,
  [](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t, size_t) {
    docSetSchedule.clear();
    auto err = deserializeJson(docSetSchedule, data, len);
    if (err) {
      logError("setSchedule parse: %s", err.c_str());
      return;
    }

    bool changedRelay[4] = {false, false, false, false};
    if (docSetSchedule.containsKey("relay") && docSetSchedule["relay"].is<JsonArray>()) {
      JsonArray relayArray = docSetSchedule["relay"].as<JsonArray>();
      for (JsonObject r : relayArray) {
        int relayNum = r["relay"] | 0;
        if (relayNum < 1 || relayNum > 4) continue;
        int relayIdx = relayNum - 1;

        // Check if this is an interval schedule
        bool isInterval = r["isInterval"] | false;

        if (isInterval) {
          // INTERVAL SCHEDULE
          int onMinutes = r["onMinutes"] | 0;
          int offMinutes = r["offMinutes"] | 0;
          int sh = r["startHourInterval"] | 0;
          int sm = r["startMinuteInterval"] | 0;
          int eh = r["endHourInterval"] | 0;
          int em = r["endMinuteInterval"] | 0;
          bool en = r["enabled"] | true;

          bool daysA[7] = {false, false, false, false, false, false, false};
          if (r.containsKey("days") && r["days"].is<JsonArray>()) {
            copyDays(daysA, r["days"].as<JsonArray>());
          }

          int idx = -1;
          if (r.containsKey("id") && r["id"].is<int>()) {
            idx = findScheduleIndexById(relayIdx, r["id"].as<int>());
          }

          if (idx < 0) {
            if (scheduleCount[relayIdx] >= MAX_SCHEDULES_PER_RELAY) {
              logWarn("No slot for relay %d", relayNum);
              continue;
            }
            idx = scheduleCount[relayIdx]++;
          }

          RelaySchedule &dst = schedules[relayIdx][idx];
          dst.relay = relayNum;
          dst.isInterval = true;
          dst.onMinutes = onMinutes;
          dst.offMinutes = offMinutes;
          dst.startHourInterval = sh;
          dst.startMinuteInterval = sm;
          dst.endHourInterval = eh;
          dst.endMinuteInterval = em;
          dst.enabled = en;
          for (int d = 0; d < 7; d++) dst.days[d] = daysA[d];
          changedRelay[relayIdx] = true;

          if (r.containsKey("id") && r["id"].is<int>()) dst.id = r["id"].as<int>();
          else if (dst.id == 0) dst.id = relayNum * 100 + idx + (millis() % 100);

          logInfo("Added INTERVAL schedule for Relay %d: %dm ON / %dm OFF", relayNum, onMinutes, offMinutes);
        } else {
          // TIME RANGE SCHEDULE (original)
          int sh = r["startHour"]   | 0;
          int sm = r["startMinute"] | 0;
          int eh = r["endHour"]     | 0;
          int em = r["endMinute"]   | 0;
          bool en = r["enabled"]     | true;

          bool daysA[7] = {false, false, false, false, false, false, false};
          if (r.containsKey("days") && r["days"].is<JsonArray>()) {
            copyDays(daysA, r["days"].as<JsonArray>());
          }

          int idx = -1;
          if (r.containsKey("id") && r["id"].is<int>()) {
            idx = findScheduleIndexById(relayIdx, r["id"].as<int>());
          }
          if (idx < 0) idx = findScheduleIndexByFields(relayIdx, sh, sm, eh, em, daysA);

          if (idx < 0) {
            if (scheduleCount[relayIdx] >= MAX_SCHEDULES_PER_RELAY) {
              logWarn("No slot for relay %d", relayNum);
              continue;
            }
            idx = scheduleCount[relayIdx]++;
          }

          RelaySchedule &dst = schedules[relayIdx][idx];
          dst.relay = relayNum;
          dst.isInterval = false;
          dst.startHour = sh; dst.startMinute = sm; dst.endHour = eh; dst.endMinute = em;
          dst.enabled = en;
          for (int d = 0; d < 7; d++) dst.days[d] = daysA[d];
          changedRelay[relayIdx] = true;

          if (r.containsKey("id") && r["id"].is<int>()) dst.id = r["id"].as<int>();
          else if (dst.id == 0) dst.id = relayNum * 100 + idx + (millis() % 100);

          logInfo("Added TIME RANGE schedule for Relay %d: %02d:%02d - %02d:%02d",
                  relayNum, sh, sm, eh, em);
        }
      }
    }
    saveSchedulesToPreferences();
    for (int relay = 0; relay < 4; relay++) {
      if (changedRelay[relay]) checkSchedules(relay + 1);
    }
  }
           );

  server.on("/api/schedules", HTTP_POST,
  [](AsyncWebServerRequest * request) {},
  NULL,
  [](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t, size_t) {
    docCron.clear();
    DeserializationError err = deserializeJson(docCron, data, len);
    if (err) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
      return;
    }

    int relay = docCron["relay"] | 0;
    bool enabled = docCron["enabled"] | false;
    bool hasSecondsPrecision = docCron["hasSecondsPrecision"] | false;
    const char* cronOn = docCron["cronOn"] | "";
    const char* cronOff = docCron["cronOff"] | "";
    String applyError;
    if (!applyCronProfile(relay, enabled, hasSecondsPrecision, cronOn, cronOff, applyError)) {
      StaticJsonDocument<256> response;
      response["ok"] = false;
      response["error"] = applyError;
      String json;
      serializeJson(response, json);
      request->send(400, "application/json", json);
      return;
    }

    CronProfile& profile = cronProfiles[relay - 1];
    StaticJsonDocument<256> response;
    response["ok"] = true;
    response["relay"] = relay;
    response["enabled"] = profile.enabled;
    response["cronOn"] = profile.cronOn;
    response["cronOff"] = profile.cronOff;
    String json;
    serializeJson(response, json);
    request->send(200, "application/json", json);
  }
           );

  server.on("/getSchedules", HTTP_GET, [](AsyncWebServerRequest * request) {
    docSchedules.clear();
    JsonArray out = docSchedules.to<JsonArray>();
    for (int relay = 0; relay < 4; relay++) {
      CronProfile& profile = cronProfiles[relay];
      if (!profile.configured) continue;
      JsonObject r = out.createNestedObject();
      r["id"] = relay * 100 + 1;
      r["relay"] = profile.relay;
      r["enabled"] = profile.enabled;
      r["hasSecondsPrecision"] = profile.hasSecondsPrecision;
      r["cronOn"] = profile.cronOn;
      r["cronOff"] = profile.cronOff;
    }
    String jsonStr; serializeJson(docSchedules, jsonStr);
    request->send(200, "application/json", jsonStr);
  });

  server.on("/deleteSchedule", HTTP_POST,
  [](AsyncWebServerRequest * request) {},
  NULL,
  [](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t, size_t) {
    docDelete.clear();
    auto err = deserializeJson(docDelete, data, len);
    if (err) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
      return;
    }

    int relayNum = docDelete["relay"] | 0;
    int sh = docDelete["startHour"]   | -1;
    int sm = docDelete["startMinute"] | -1;
    int eh = docDelete["endHour"]     | -1;
    int em = docDelete["endMinute"]   | -1;

    bool haveDays = docDelete.containsKey("days") && docDelete["days"].is<JsonArray>();
    bool daysA[7] = {false, false, false, false, false, false, false};
    if (haveDays) copyDays(daysA, docDelete["days"].as<JsonArray>());

    bool haveId = docDelete["id"].is<int>();
    int  idNum  = haveId ? docDelete["id"].as<int>() : -1;

    bool deleted = false;
    int deletedRelayIndex = -1;
    int rStart = 0, rEnd = 3;
    if (relayNum >= 1 && relayNum <= 4) {
      rStart = rEnd = relayNum - 1;
    }

    for (int r = rStart; r <= rEnd && !deleted; r++) {
      for (int i = 0; i < scheduleCount[r]; i++) {
        RelaySchedule &s = schedules[r][i];
        bool match = false;
        if (haveId && s.id == idNum) match = true;
        else {
          bool ok = true;
          if (sh >= 0 && s.startHour   != sh) ok = false;
          if (sm >= 0 && s.startMinute != sm) ok = false;
          if (eh >= 0 && s.endHour     != eh) ok = false;
          if (em >= 0 && s.endMinute   != em) ok = false;
          if (haveDays && !daysEqual(s.days, daysA)) ok = false;
          if (relayNum >= 1 && relayNum <= 4 && s.relay != relayNum) ok = false;
          match = ok;
        }
        if (match) {
          for (int j = i; j < scheduleCount[r] - 1; j++) schedules[r][j] = schedules[r][j + 1];
          scheduleCount[r]--;
          deleted = true;
          deletedRelayIndex = r;
          break;
        }
      }
    }
    if (deleted) {
      saveSchedulesToPreferences();
      if (deletedRelayIndex >= 0) checkSchedules(deletedRelayIndex + 1);
      request->send(200, "application/json", "{\"ok\":true}");
    }
    else         {
      request->send(404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
    }
  }
           );

  server.on("/forceCheck", HTTP_GET, [](AsyncWebServerRequest * request) {
    checkSchedules();
    request->send(200, "text/plain", "Schedule check completed");
  });

  server.on("/syncTime", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (WiFi.status() == WL_CONNECTED) {
      if (syncTimeWithNTP()) {
        request->send(200, "text/plain", "Time synced with NTP successfully!");
      } else {
        request->send(500, "text/plain", "NTP time synchronization failed");
      }
    }
    else                               {
      request->send(500, "text/plain", "Cannot sync time - WiFi not connected");
    }
  });

  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "Rebooting...");
    delay(200);
    ESP.restart();
  });
  server.on("/ota_status", HTTP_GET, [](AsyncWebServerRequest * request) {
    String s = String("OTA Hostname: ") + deviceId + "\nIP: " + WiFi.localIP().toString() + "\n";
    request->send(200, "text/plain", s);
  });

  server.onNotFound([](AsyncWebServerRequest * request) {
    if (SPIFFS.exists("/index.html")) {
      request->send(SPIFFS, "/index.html", "text/html");
    } else {
      request->send(404, "text/plain", "File not found - SPIFFS not properly loaded");
    }
  });

  server.begin();

  randomSeed(analogRead(0));
  logInfo("System initialized");
  lcd.clear();
  lcd.print("FarmWiz Ready!");
  lcd.setCursor(0, 1);
  lcd.print("IP: ");
  if (networkMode == MODE_ONLINE && WiFi.status() == WL_CONNECTED) {
    lcd.print(WiFi.localIP().toString().c_str());
  } else if (networkMode == MODE_OFFLINE) {
    lcd.print(WiFi.softAPIP().toString().c_str());
  } else {
    lcd.print("Connecting...");
  }
  delay(1000);
  if (initialScheduleSyncComplete) {
    checkSchedules();
    checkCronSchedules();
  }

  // Arm the task watchdog only after all startup work is complete. The old
  // firmware armed it before a 30-second WiFi wait, causing reset loops when
  // the network was unavailable.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtConfig);
#else
  esp_task_wdt_init(WDT_TIMEOUT, true);
#endif
  esp_task_wdt_add(NULL);
  logInfo("Watchdog armed after startup with %d second timeout", WDT_TIMEOUT);
}

// ------------------- LOOP -------------------
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 2UL * 60UL * 1000UL;
unsigned long lastDebugTime = 0;
const unsigned long debugInterval = 30000UL;
unsigned long lastScheduleCheck = 0;
const unsigned long scheduleCheckInterval = 60000UL;

// ###TB Make these variables global
float temp;
float hum;
int rawLDR;
int lightPercent;
int soilMoisturePercent;

void loop() {

  esp_task_wdt_reset();

  handleCLI();

  handleNetworkMode();

  processEspNowPackets();

  if (networkMode == MODE_ONLINE && otaReady) ArduinoOTA.handle();

  if (networkMode == MODE_ONLINE && WiFi.status() == WL_CONNECTED) {
    bool initialSyncDue = !initialScheduleSyncComplete &&
      (lastNtpAttempt == 0 || millis() - lastNtpAttempt >= NTP_RETRY_INTERVAL);
    bool periodicFallbackSyncDue = initialScheduleSyncComplete && !rtcAvailable &&
      millis() - lastTimeSync >= timeSyncInterval &&
      (lastNtpAttempt == 0 || millis() - lastNtpAttempt >= NTP_RETRY_INTERVAL);
    if (initialSyncDue || periodicFallbackSyncDue) {
      if (syncTimeWithNTP() && initialSyncDue) {
        checkSchedules();
        lastScheduleCheck = millis();
      }
    }
  }

  checkCronSchedules();

  // Non-blocking MQTT reconnect
  if (networkMode == MODE_ONLINE && !client.connected() && WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
  }
  if (networkMode == MODE_ONLINE && client.connected()) client.loop();

  buttonState = digitalRead(buttonPin);
  if (buttonState == LOW && lastButtonState == HIGH) {
    lcdIsOn = !lcdIsOn;
    if (lcdIsOn) {
      lcd.backlight();
      lcdOnStartTime = millis();
      logDebug("LCD ON");
    }
    else {
      lcd.noBacklight();
      logDebug("LCD OFF");
    }
  }
  lastButtonState = buttonState;

  if (lcdIsOn && (millis() - lcdOnStartTime > 60000)) {
    lcd.noBacklight();
    lcdIsOn = false;
    logDebug("LCD auto-off");
  }

  if (millis() - lastDebugTime >= debugInterval) {
    debugTimeLogging();
    lastDebugTime = millis();
  }

  if (millis() - lastScheduleCheck >= scheduleCheckInterval) {
    checkSchedules();
    lastScheduleCheck = millis();
  }

  // MQTT - ThingsBoard format (scheduled update every 2 minutes)
  if (networkMode == MODE_ONLINE && WiFi.status() == WL_CONNECTED && client.connected() && millis() - lastSendTime >= sendInterval) {

    // ###TB moved here for now to unblock the loop
    temp = dht.readTemperature() || 0;
    hum = dht.readHumidity() || 0;
    rawLDR = analogRead(LDR_PIN) || 0;
    lightPercent = constrain(map(rawLDR, 0, 2944, 0, 100), 0, 100) || 0;
    soilMoisturePercent = 50;

    StaticJsonDocument<256> doc;
    if (!isnan(temp) && !isnan(hum)) {
      // Preserve legacy waterValve telemetry and add the confirmed name.
      doc["pump"] = relayState[0];      // Relay 1 = Sprinkler = Pump
      doc["light"] = relayState[1];     // Relay 2 = Light
      doc["airPump"] = relayState[2];   // Relay 3 = Air Pump
      doc["waterValve"] = relayState[3];
      doc["irrigationPump"] = relayState[3];

      doc["temperature"] = temp;
      doc["humidity"] = hum;
      doc["lightLevel"] = lightPercent;
      doc["soilMoisture"] = soilMoisturePercent;

      DateTime now = currentUtcTime();
      int sh, sm, sd; getSurinameTime(now, sh, sm, sd);
      char dateTimeStr[25];
      snprintf(dateTimeStr, sizeof(dateTimeStr), "%04d-%02d-%02dT%02d:%02d:%02dZ",
               now.year(), now.month(), now.day(), sh, sm, now.second());
      doc["timestamp"] = dateTimeStr;

      String jsonString;
      serializeJson(doc, jsonString);

      if (client.publish(mqttTopic( "v1/devices/me/telemetry").c_str(), jsonString.c_str())) {
        lastSendTime = millis();
        logDebug("Scheduled ThingsBoard telemetry sent: %s", jsonString.c_str());
      } else {
        logWarn("MQTT publish failed");
      }
    } else {
      logWarn("DHT read failed, skip MQTT");
    }
  }

  if (lcdIsOn && millis() - lcdLastUpdate > 2000) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Temp:"); lcd.print(temp, 1); lcd.print("C");
    lcd.setCursor(0, 1);
    if (lcdDisplayStage == 0) {
      lcd.print("Hum:");
      lcd.print(hum, 1);
      lcd.print("%");
    }
    else if (lcdDisplayStage == 1) {
      lcd.print("Light:");
      lcd.print(lightPercent);
      lcd.print("%");
    }
    else if (lcdDisplayStage == 2) {
      lcd.print("Soil:");
      lcd.print(soilMoisturePercent);
      lcd.print("%");
    }
    lcdDisplayStage = (lcdDisplayStage + 1) % 3;
    lcdLastUpdate = millis();
  }

  for (int i = 0; i < 4; i++) {
    if (manualRelay[i] && millis() - manualStartTime[i] > manualOverrideDuration) {
      manualRelay[i] = false;
      logInfo("Manual override expired for Relay %d", i + 1);
      checkSchedules();
    }
  }

  delay(10);
}
