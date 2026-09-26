#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

// =====================================================
// DEFAULT WIFI
// Used only if no saved WiFi exists in Preferences
// =====================================================

const char* DEFAULT_SSID ="HUAWEI-2.4G-bF2k";
const char* DEFAULT_PASSWORD ="DuuBnx2S";

String wifiSSID;
String wifiPassword;

Preferences preferences;


// =====================================================
// MQTT / THINGSBOARD
// =====================================================

const char* mqtt_server = "168.195.218.199";
const int mqtt_port = 1883;

const char* mqtt_user = "mYf1TFJutCrgxYnp4K1X";
const char* mqtt_password = "";

const char* mqtt_topic = "v1/devices/me/telemetry";

WiFiClient espClient;
PubSubClient mqttClient(espClient);


// =====================================================
// DEVICE CONFIGURATION
// =====================================================

const char* deviceId = "TdsNode01";


// =====================================================
// SENSOR PINS
// =====================================================

const int tdsSensorPin = 35;
const int ledPin = 32;


// =====================================================
// MASTER ESP-NOW CONFIG
// =====================================================

uint8_t masterMac[] = {
  0xC0,
  0x49,
  0xEF,
  0x68,
  0x82,
  0x30
};

// =====================================================
// FIXED OFFLINE ESP-NOW CHANNEL
// MUST MATCH MASTER
// =====================================================

const uint8_t ESPNOW_OFFLINE_CHANNEL = 6;

bool espNowReady = false;


// =====================================================
// NODE OPERATING MODE
// =====================================================

enum NodeMode {

  MODE_ONLINE,
  MODE_OFFLINE

};

NodeMode currentMode = MODE_OFFLINE;


// =====================================================
// MANUAL OFFLINE OVERRIDE
// =====================================================

// false:
// Node automatically switches between
// WiFi and ESP-NOW.
//
// true:
// Node stays in ESP-NOW offline mode
// until "online" is entered.

bool manualOfflineMode = false;


// =====================================================
// LEGACY MQTT TEST MODE
// =====================================================

// Kept so your old CLI commands still work.
//
// mqtt test on  = same as "offline"
// mqtt test off = same as "online"

bool mqttTestMode = false;


// =====================================================
// CONNECTION TIMERS
// =====================================================

// Initial WiFi attempt

const unsigned long WIFI_CONNECT_TIMEOUT = 10000;

// How long WiFi must be missing before
// automatically switching to ESP-NOW.

const unsigned long WIFI_LOSS_TIMEOUT = 10000;

// While offline, check whether WiFi returned.

const unsigned long WIFI_RETRY_INTERVAL = 30000;

// MQTT retry while online

const unsigned long MQTT_RETRY_INTERVAL = 5000;


// =====================================================
// TIMERS
// =====================================================

unsigned long wifiLostAt = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastMQTTAttempt = 0;
unsigned long lastTelemetrySend = 0;


// =====================================================
// TELEMETRY INTERVAL
// =====================================================

const unsigned long TELEMETRY_INTERVAL = 5000;


// =====================================================
// LATEST TDS VALUES
// =====================================================

float latestTdsPpm = 0;
float latestTdsADC = 0;
float latestTdsVoltage = 0;


// =====================================================
// LOAD WIFI SETTINGS
// =====================================================

void loadWiFiSettings() {

  preferences.begin(
    "farmwiz",
    true
  );

  wifiSSID =
    preferences.getString(
      "ssid",
      DEFAULT_SSID
    );

  wifiPassword =
    preferences.getString(
      "password",
      DEFAULT_PASSWORD
    );

  preferences.end();

  Serial.println();
  Serial.println(
    "WiFi settings loaded"
  );

  Serial.print(
    "SSID: "
  );

  Serial.println(
    wifiSSID
  );
}


// =====================================================
// SAVE WIFI SETTINGS
// =====================================================

void saveWiFiSettings() {

  preferences.begin(
    "farmwiz",
    false
  );

  preferences.putString(
    "ssid",
    wifiSSID
  );

  preferences.putString(
    "password",
    wifiPassword
  );

  preferences.end();

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "WiFi settings saved"
  );

  Serial.println(
    "================================"
  );

  Serial.print(
    "SSID: "
  );

  Serial.println(
    wifiSSID
  );

  Serial.println(
    "Password: SAVED"
  );

  Serial.println();

  Serial.println(
    "Type 'restart' to use new WiFi."
  );
}


// =====================================================
// MQTT CALLBACK
// =====================================================

void mqttCallback(
  char* topic,
  byte* payload,
  unsigned int length
) {

  Serial.print(
    "Message arrived ["
  );

  Serial.print(
    topic
  );

  Serial.print(
    "] "
  );

  for (
    unsigned int i = 0;
    i < length;
    i++
  ) {

    Serial.print(
      (char)payload[i]
    );
  }

  Serial.println();
}


// =====================================================
// MQTT CONNECT
// =====================================================

bool reconnectMQTT() {

  // MQTT is ONLINE MODE ONLY

  if (
    currentMode != MODE_ONLINE
  ) {

    return false;
  }

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return false;
  }

  if (
    mqttClient.connected()
  ) {

    return true;
  }

  Serial.print(
    "Connecting MQTT..."
  );

  bool connected = false;

  if (
    strlen(mqtt_password) == 0
  ) {

    connected =
      mqttClient.connect(
        deviceId,
        mqtt_user,
        NULL
      );

  } else {

    connected =
      mqttClient.connect(
        deviceId,
        mqtt_user,
        mqtt_password
      );
  }

  if (connected) {

    Serial.println(
      " connected!"
    );

    digitalWrite(
      ledPin,
      HIGH
    );

    return true;

  } else {

    Serial.print(
      " failed, rc="
    );

    Serial.println(
      mqttClient.state()
    );

    return false;
  }
}


// =====================================================
// PERIODIC MQTT RECONNECT
// =====================================================

void handleMQTTReconnect() {

  if (
    currentMode != MODE_ONLINE
  ) {

    return;
  }

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return;
  }

  if (
    mqttClient.connected()
  ) {

    return;
  }

  if (
    millis() - lastMQTTAttempt <
    MQTT_RETRY_INTERVAL
  ) {

    return;
  }

  lastMQTTAttempt =
    millis();

  reconnectMQTT();
}


// =====================================================
// ESP-NOW SEND CALLBACK
// ESP32 Arduino Core 2.0.6
// =====================================================

void onDataSent(
  const uint8_t* mac_addr,
  esp_now_send_status_t status
) {

  Serial.print(
    "ESP-NOW delivery: "
  );

  if (
    status == ESP_NOW_SEND_SUCCESS
  ) {

    Serial.println(
      "SUCCESS"
    );

  } else {

    Serial.println(
      "FAILED"
    );
  }
}


// =====================================================
// STOP ESP-NOW
// =====================================================

void stopESPNow() {

  if (!espNowReady) {

    return;
  }

  esp_now_deinit();

  espNowReady =
    false;

  Serial.println(
    "ESP-NOW stopped"
  );
}


// =====================================================
// START ESP-NOW OFFLINE MODE
// =====================================================

bool setupESPNowOffline() {

  // -----------------------------------------
  // Already initialized
  // -----------------------------------------

  if (espNowReady) {

    return true;
  }

  Serial.println();
  Serial.println(
    "Starting ESP-NOW..."
  );

  // -----------------------------------------
  // Station mode required for ESP-NOW sender
  // -----------------------------------------

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setSleep(
    false
  );

  // -----------------------------------------
  // FORCE CHANNEL 6
  // -----------------------------------------

  esp_err_t channelResult =
    esp_wifi_set_channel(
      ESPNOW_OFFLINE_CHANNEL,
      WIFI_SECOND_CHAN_NONE
    );

  if (
    channelResult != ESP_OK
  ) {

    Serial.print(
      "Failed to set ESP-NOW channel. Error: "
    );

    Serial.println(
      channelResult
    );

    return false;
  }

  delay(
    100
  );

  Serial.print(
    "Sender MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  Serial.print(
    "Offline ESP-NOW Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  // -----------------------------------------
  // Initialize ESP-NOW
  // -----------------------------------------

  if (
    esp_now_init() != ESP_OK
  ) {

    Serial.println(
      "ESP-NOW initialization FAILED!"
    );

    espNowReady =
      false;

    return false;
  }

  // -----------------------------------------
  // Send callback
  // -----------------------------------------

  esp_now_register_send_cb(
    onDataSent
  );

  // -----------------------------------------
  // Configure Master peer
  // -----------------------------------------

  esp_now_peer_info_t peerInfo = {};

  memcpy(
    peerInfo.peer_addr,
    masterMac,
    6
  );

  peerInfo.channel =
    ESPNOW_OFFLINE_CHANNEL;

  peerInfo.ifidx =
    WIFI_IF_STA;

  peerInfo.encrypt =
    false;

  // -----------------------------------------
  // Remove previous peer
  // -----------------------------------------

  if (
    esp_now_is_peer_exist(
      masterMac
    )
  ) {

    esp_now_del_peer(
      masterMac
    );
  }

  // -----------------------------------------
  // Add Master
  // -----------------------------------------

  esp_err_t result =
    esp_now_add_peer(
      &peerInfo
    );

  if (
    result != ESP_OK
  ) {

    Serial.print(
      "Failed to add ESP-NOW Master. Error: "
    );

    Serial.println(
      result
    );

    esp_now_deinit();

    espNowReady =
      false;

    return false;
  }

  espNowReady =
    true;

  Serial.println();

  Serial.println(
    "ESP-NOW READY"
  );

  Serial.println(
    "Master MAC: C0:49:EF:68:82:30"
  );

  Serial.print(
    "ESP-NOW Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  return true;
}


// =====================================================
// SEND TO MASTER
// =====================================================

bool sendToMaster(
  const char* jsonData
) {

  if (
    currentMode != MODE_OFFLINE
  ) {

    Serial.println(
      "ESP-NOW blocked: Node is ONLINE"
    );

    return false;
  }

  if (!espNowReady) {

    Serial.println(
      "ESP-NOW is NOT ready"
    );

    return false;
  }

  int dataLength =
    strlen(
      jsonData
    );

  if (
    dataLength > 250
  ) {

    Serial.println(
      "ESP-NOW JSON too large!"
    );

    return false;
  }

  Serial.println();

  Serial.println(
    "Sending telemetry via ESP-NOW"
  );

  Serial.println(
    "Master: C0:49:EF:68:82:30"
  );

  Serial.print(
    "Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  Serial.print(
    "Data: "
  );

  Serial.println(
    jsonData
  );

  esp_err_t result =
    esp_now_send(
      masterMac,
      (uint8_t*)jsonData,
      dataLength
    );

  if (
    result == ESP_OK
  ) {

    Serial.println(
      "ESP-NOW packet queued"
    );

    return true;

  } else {

    Serial.print(
      "ESP-NOW send error: "
    );

    Serial.println(
      result
    );

    return false;
  }
}


// =====================================================
// CONNECT TO NORMAL WIFI
// =====================================================

bool connectToWiFi(
  unsigned long timeout
) {

  Serial.println();

  Serial.print(
    "Connecting WiFi: "
  );

  Serial.println(
    wifiSSID
  );

  // -----------------------------------------
  // ESP-NOW must be stopped before
  // returning to normal WiFi
  // -----------------------------------------

  stopESPNow();

  // -----------------------------------------
  // Station mode
  // -----------------------------------------

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setSleep(
    false
  );

  WiFi.disconnect();

  delay(
    200
  );

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );

  unsigned long start =
    millis();

  // -----------------------------------------
  // Wait for connection
  // -----------------------------------------

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < timeout
  ) {

    digitalWrite(
      ledPin,
      !digitalRead(ledPin)
    );

    delay(
      500
    );

    Serial.print(".");
  }

  // -----------------------------------------
  // Connected
  // -----------------------------------------

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    digitalWrite(
      ledPin,
      HIGH
    );

    Serial.println();

    Serial.println(
      "WiFi Connected"
    );

    Serial.print(
      "IP Address: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "Sender MAC: "
    );

    Serial.println(
      WiFi.macAddress()
    );

    Serial.print(
      "WiFi Channel: "
    );

    Serial.println(
      WiFi.channel()
    );

    Serial.print(
      "RSSI: "
    );

    Serial.print(
      WiFi.RSSI()
    );

    Serial.println(
      " dBm"
    );

    return true;
  }

  // -----------------------------------------
  // Failed
  // -----------------------------------------

  digitalWrite(
    ledPin,
    LOW
  );

  Serial.println();

  Serial.println(
    "WiFi Connection Failed"
  );

  return false;
}


// =====================================================
// ENTER ONLINE MODE
// =====================================================

void enterOnlineMode() {

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "TDS NODE MODE: ONLINE"
  );

  Serial.println(
    "WiFi available."
  );

  Serial.println(
    "Telemetry destination: ThingsBoard MQTT"
  );

  Serial.println(
    "ESP-NOW: OFF"
  );

  Serial.println(
    "========================================"
  );

  stopESPNow();

  currentMode =
    MODE_ONLINE;

  wifiLostAt =
    0;

  mqttTestMode =
    false;

  lastMQTTAttempt =
    0;

  digitalWrite(
    ledPin,
    HIGH
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    reconnectMQTT();
  }
}


// =====================================================
// ENTER OFFLINE MODE
// =====================================================

void enterOfflineMode() {

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "TDS NODE MODE: OFFLINE"
  );

  Serial.println(
    "Normal WiFi unavailable / disabled."
  );

  Serial.println(
    "Switching to ESP-NOW."
  );

  Serial.print(
    "Fixed Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  Serial.println(
    "========================================"
  );

  // -----------------------------------------
  // Disconnect MQTT
  // -----------------------------------------

  if (
    mqttClient.connected()
  ) {

    mqttClient.disconnect();
  }

  // -----------------------------------------
  // Stop previous ESP-NOW
  // -----------------------------------------

  stopESPNow();

  // -----------------------------------------
  // Disconnect normal WiFi
  // -----------------------------------------

  WiFi.disconnect(
    true
  );

  delay(
    300
  );

  // -----------------------------------------
  // Return radio to station mode
  // -----------------------------------------

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setSleep(
    false
  );

  delay(
    100
  );

  // -----------------------------------------
  // Force channel 6 and start ESP-NOW
  // -----------------------------------------

  setupESPNowOffline();

  currentMode =
    MODE_OFFLINE;

  lastWiFiRetry =
    millis();

  digitalWrite(
    ledPin,
    LOW
  );

  Serial.println();

  Serial.println(
    "OFFLINE SYSTEM READY"
  );

  Serial.println(
    "Telemetry will be sent to Master over ESP-NOW."
  );

  if (
    manualOfflineMode
  ) {

    Serial.println();

    Serial.println(
      "Manual Offline Override: ON"
    );

    Serial.println(
      "Type 'online' to reconnect to WiFi."
    );
  }
}


// =====================================================
// AUTOMATIC MODE MANAGEMENT
// =====================================================

void handleConnectionMode() {

  // =================================================
  // MANUAL OFFLINE
  // =================================================

  if (
    manualOfflineMode
  ) {

    return;
  }

  // =================================================
  // ONLINE MODE
  // =================================================

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    // -----------------------------------------
    // WiFi still exists
    // -----------------------------------------

    if (
      WiFi.status() ==
      WL_CONNECTED
    ) {

      wifiLostAt =
        0;

      return;
    }

    // -----------------------------------------
    // WiFi just disappeared
    // -----------------------------------------

    if (
      wifiLostAt == 0
    ) {

      wifiLostAt =
        millis();

      Serial.println();

      Serial.println(
        "WiFi connection lost..."
      );

      Serial.println(
        "Waiting before entering ESP-NOW fallback..."
      );
    }

    // -----------------------------------------
    // WiFi did not recover
    // -----------------------------------------

    if (
      millis() - wifiLostAt >=
      WIFI_LOSS_TIMEOUT
    ) {

      Serial.println();

      Serial.println(
        "WiFi unavailable."
      );

      Serial.println(
        "Switching to ESP-NOW Channel 6."
      );

      enterOfflineMode();
    }

    return;
  }

  // =================================================
  // OFFLINE MODE
  // =================================================

  if (
    currentMode ==
    MODE_OFFLINE
  ) {

    // -----------------------------------------
    // Wait before testing WiFi again
    // -----------------------------------------

    if (
      millis() - lastWiFiRetry <
      WIFI_RETRY_INTERVAL
    ) {

      return;
    }

    lastWiFiRetry =
      millis();

    Serial.println();

    Serial.println(
      "Checking if normal WiFi has returned..."
    );

    // -----------------------------------------
    // Stop ESP-NOW temporarily
    // -----------------------------------------

    stopESPNow();

    // -----------------------------------------
    // Try normal WiFi for 5 seconds
    // -----------------------------------------

    bool connected =
      connectToWiFi(
        5000
      );

    // -----------------------------------------
    // WiFi returned
    // -----------------------------------------

    if (connected) {

      Serial.println();

      Serial.println(
        "Normal WiFi restored!"
      );

      enterOnlineMode();
    }

    // -----------------------------------------
    // WiFi still unavailable
    // -----------------------------------------

    else {

      Serial.println();

      Serial.println(
        "Normal WiFi still unavailable."
      );

      Serial.println(
        "Returning to ESP-NOW Channel 6."
      );

      enterOfflineMode();
    }
  }
}


// =====================================================
// READ TDS
// =====================================================

void readTDS() {

  unsigned long int tdsAvgValue = 0;

  int tdsBuffer[10];

  int tempTds;

  // -----------------------------------------
  // Take 10 samples
  // -----------------------------------------

  for (
    int i = 0;
    i < 10;
    i++
  ) {

    tdsBuffer[i] =
      analogRead(
        tdsSensorPin
      );

    delay(
      30
    );
  }

  // -----------------------------------------
  // Sort samples
  // -----------------------------------------

  for (
    int i = 0;
    i < 9;
    i++
  ) {

    for (
      int j = i + 1;
      j < 10;
      j++
    ) {

      if (
        tdsBuffer[i] >
        tdsBuffer[j]
      ) {

        tempTds =
          tdsBuffer[i];

        tdsBuffer[i] =
          tdsBuffer[j];

        tdsBuffer[j] =
          tempTds;
      }
    }
  }

  // -----------------------------------------
  // Average middle 6
  // -----------------------------------------

  for (
    int i = 2;
    i < 8;
    i++
  ) {

    tdsAvgValue +=
      tdsBuffer[i];
  }

  latestTdsADC =
    (float)tdsAvgValue /
    6.0;

  latestTdsVoltage =
    latestTdsADC *
    (
      3.3 /
      4095.0
    );

  // -----------------------------------------
  // ORIGINAL TDS FORMULA
  // -----------------------------------------

  latestTdsPpm =
    (
      133.42 *
      pow(
        latestTdsVoltage,
        3
      )

      -

      255.86 *
      (
        latestTdsVoltage *
        latestTdsVoltage
      )

      +

      857.39 *
      latestTdsVoltage
    )

    * 0.5;

  if (
    latestTdsPpm < 0
  ) {

    latestTdsPpm =
      0;
  }
}


// =====================================================
// CLI HELP
// =====================================================

void printHelp() {

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "FarmWiz TDS Node CLI"
  );

  Serial.println(
    "========================================"
  );

  Serial.println(
    "help"
  );

  Serial.println(
    "  Show commands"
  );

  Serial.println();

  Serial.println(
    "status"
  );

  Serial.println(
    "  Show complete node status"
  );

  Serial.println();

  Serial.println(
    "read"
  );

  Serial.println(
    "  Show latest TDS reading"
  );

  Serial.println();

  Serial.println(
    "wifi"
  );

  Serial.println(
    "  Show WiFi information"
  );

  Serial.println();

  Serial.println(
    "mqtt"
  );

  Serial.println(
    "  Show MQTT information"
  );

  Serial.println();

  Serial.println(
    "espnow"
  );

  Serial.println(
    "  Show ESP-NOW information"
  );

  Serial.println();

  Serial.println(
    "master"
  );

  Serial.println(
    "  Show configured master MAC and offline channel"
  );

  Serial.println();

  Serial.println(
    "ip"
  );

  Serial.println(
    "  Show the current online IP or offline radio state"
  );

  Serial.println();

  Serial.println(
    "offline"
  );

  Serial.println(
    "  Force ESP-NOW offline mode on Channel 6"
  );

  Serial.println();

  Serial.println(
    "online"
  );

  Serial.println(
    "  Stop ESP-NOW and reconnect to normal WiFi"
  );

  Serial.println();

  Serial.println(
    "set ssid <SSID>"
  );

  Serial.println(
    "  Change WiFi SSID"
  );

  Serial.println();

  Serial.println(
    "set password <PASSWORD>"
  );

  Serial.println(
    "  Change WiFi password"
  );

  Serial.println();

  Serial.println(
    "save"
  );

  Serial.println(
    "  Save WiFi settings permanently"
  );

  Serial.println();

  Serial.println(
    "mqtt test on"
  );

  Serial.println(
    "  Legacy alias for OFFLINE mode"
  );

  Serial.println();

  Serial.println(
    "mqtt test off"
  );

  Serial.println(
    "  Legacy alias for ONLINE mode"
  );

  Serial.println();

  Serial.println(
    "restart | reboot"
  );

  Serial.println(
    "  Restart node"
  );

  Serial.println();

  Serial.println(
    "========================================"
  );
}


// =====================================================
// CLI STATUS
// =====================================================

void printStatus() {

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "TDS NODE STATUS"
  );

  Serial.println(
    "========================================"
  );

  Serial.print(
    "Device ID: "
  );

  Serial.println(
    deviceId
  );

  // -----------------------------------------
  // Operating Mode
  // -----------------------------------------

  Serial.print(
    "Mode: "
  );

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    Serial.println(
      "ONLINE / MQTT"
    );

  } else {

    Serial.println(
      "OFFLINE / ESP-NOW"
    );
  }

  Serial.print(
    "Manual Offline Override: "
  );

  Serial.println(
    manualOfflineMode
      ? "ON"
      : "OFF"
  );

  // -----------------------------------------
  // WiFi
  // -----------------------------------------

  Serial.print(
    "WiFi: "
  );

  Serial.println(
    WiFi.status() == WL_CONNECTED
      ? "CONNECTED"
      : "DISCONNECTED"
  );

  Serial.print(
    "Configured SSID: "
  );

  Serial.println(
    wifiSSID
  );

  if (
    currentMode ==
    MODE_ONLINE &&
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "WiFi Channel: "
    );

    Serial.println(
      WiFi.channel()
    );

    Serial.print(
      "RSSI: "
    );

    Serial.print(
      WiFi.RSSI()
    );

    Serial.println(
      " dBm"
    );
  }

  // -----------------------------------------
  // MAC
  // -----------------------------------------

  Serial.print(
    "MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  // -----------------------------------------
  // MQTT
  // -----------------------------------------

  Serial.print(
    "MQTT: "
  );

  Serial.println(
    mqttClient.connected()
      ? "CONNECTED"
      : "DISCONNECTED"
  );

  // -----------------------------------------
  // ESP-NOW
  // -----------------------------------------

  Serial.print(
    "ESP-NOW: "
  );

  Serial.println(
    espNowReady
      ? "READY"
      : "OFF"
  );

  Serial.println(
    "Master MAC: C0:49:EF:68:82:30"
  );

  Serial.print(
    "Offline Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  // -----------------------------------------
  // TDS
  // -----------------------------------------

  Serial.print(
    "TDS: "
  );

  Serial.print(
    latestTdsPpm,
    1
  );

  Serial.println(
    " ppm"
  );

  // -----------------------------------------
  // Uptime
  // -----------------------------------------

  Serial.print(
    "Uptime: "
  );

  Serial.print(
    millis() / 1000
  );

  Serial.println(
    " sec"
  );

  Serial.println(
    "========================================"
  );
}


// =====================================================
// CLI WIFI INFO
// =====================================================

void printWiFiInfo() {

  Serial.println();

  Serial.println(
    "========== WIFI =========="
  );

  Serial.print(
    "Configured SSID: "
  );

  Serial.println(
    wifiSSID
  );

  Serial.print(
    "Node Mode: "
  );

  Serial.println(
    currentMode == MODE_ONLINE
      ? "ONLINE"
      : "OFFLINE"
  );

  Serial.print(
    "Status: "
  );

  Serial.println(
    WiFi.status() == WL_CONNECTED
      ? "CONNECTED"
      : "DISCONNECTED"
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "Connected SSID: "
    );

    Serial.println(
      WiFi.SSID()
    );

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "Channel: "
    );

    Serial.println(
      WiFi.channel()
    );

    Serial.print(
      "RSSI: "
    );

    Serial.print(
      WiFi.RSSI()
    );

    Serial.println(
      " dBm"
    );
  }

  Serial.print(
    "MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  if (
    currentMode ==
    MODE_OFFLINE
  ) {

    Serial.print(
      "Offline Channel: "
    );

    Serial.println(
      ESPNOW_OFFLINE_CHANNEL
    );
  }

  Serial.println(
    "=========================="
  );
}


// =====================================================
// CLI MQTT INFO
// =====================================================

void printMQTTInfo() {

  Serial.println();

  Serial.println(
    "========== MQTT =========="
  );

  Serial.print(
    "Server: "
  );

  Serial.println(
    mqtt_server
  );

  Serial.print(
    "Port: "
  );

  Serial.println(
    mqtt_port
  );

  Serial.print(
    "Status: "
  );

  Serial.println(
    mqttClient.connected()
      ? "CONNECTED"
      : "DISCONNECTED"
  );

  Serial.print(
    "Node Mode: "
  );

  Serial.println(
    currentMode == MODE_ONLINE
      ? "ONLINE / MQTT ENABLED"
      : "OFFLINE / MQTT DISABLED"
  );

  Serial.println(
    "=========================="
  );
}


// =====================================================
// CLI ESP-NOW INFO
// =====================================================

void printESPNowInfo() {

  Serial.println();

  Serial.println(
    "========= ESP-NOW ========="
  );

  Serial.print(
    "Status: "
  );

  Serial.println(
    espNowReady
      ? "READY"
      : "OFF"
  );

  Serial.println(
    "Master MAC: C0:49:EF:68:82:30"
  );

  Serial.print(
    "Offline Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  Serial.print(
    "Node Mode: "
  );

  Serial.println(
    currentMode == MODE_OFFLINE
      ? "OFFLINE / ESP-NOW ACTIVE"
      : "ONLINE / ESP-NOW DISABLED"
  );

  Serial.print(
    "Manual Override: "
  );

  Serial.println(
    manualOfflineMode
      ? "ON"
      : "OFF"
  );

  Serial.println(
    "==========================="
  );
}


// =====================================================
// CLI SENSOR READ
// =====================================================

void printSensorReading() {

  Serial.println();

  Serial.println(
    "========== TDS =========="
  );

  Serial.print(
    "TDS: "
  );

  Serial.print(
    latestTdsPpm,
    1
  );

  Serial.println(
    " ppm"
  );

  Serial.print(
    "ADC: "
  );

  Serial.println(
    latestTdsADC,
    0
  );

  Serial.print(
    "Voltage: "
  );

  Serial.print(
    latestTdsVoltage,
    3
  );

  Serial.println(
    " V"
  );

  Serial.println(
    "========================="
  );
}


// =====================================================
// FORCE OFFLINE FROM CLI
// =====================================================

void forceOfflineCLI() {

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "FORCING OFFLINE MODE"
  );

  Serial.println(
    "========================================"
  );

  Serial.println(
    "Normal WiFi will be disconnected."
  );

  Serial.println(
    "MQTT will be stopped."
  );

  Serial.print(
    "ESP-NOW Channel "
  );

  Serial.print(
    ESPNOW_OFFLINE_CHANNEL
  );

  Serial.println(
    " will be enabled."
  );

  Serial.println();

  Serial.println(
    "Automatic WiFi reconnect is disabled."
  );

  Serial.println(
    "Type 'online' to return to WiFi."
  );

  manualOfflineMode =
    true;

  mqttTestMode =
    true;

  enterOfflineMode();
}


// =====================================================
// FORCE ONLINE FROM CLI
// =====================================================

void forceOnlineCLI() {

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "TRYING ONLINE MODE"
  );

  Serial.println(
    "========================================"
  );

  manualOfflineMode =
    false;

  mqttTestMode =
    false;

  // -----------------------------------------
  // Stop offline ESP-NOW
  // -----------------------------------------

  stopESPNow();

  // -----------------------------------------
  // Try normal WiFi
  // -----------------------------------------

  bool connected =
    connectToWiFi(
      WIFI_CONNECT_TIMEOUT
    );

  if (connected) {

    enterOnlineMode();

    Serial.println();

    Serial.println(
      "ONLINE MODE SUCCESSFUL"
    );

    Serial.println(
      "Telemetry destination: ThingsBoard"
    );

  } else {

    Serial.println();

    Serial.println(
      "Configured WiFi is unavailable."
    );

    Serial.println(
      "Returning to automatic ESP-NOW fallback."
    );

    enterOfflineMode();
  }
}


// =====================================================
// CLI HANDLER
// =====================================================

void handleCLI() {

  if (
    !Serial.available()
  ) {

    return;
  }

  String command =
    Serial.readStringUntil(
      '\n'
    );

  command.trim();

  if (
    command.length() == 0
  ) {

    return;
  }

  String lowerCommand =
    command;

  lowerCommand.toLowerCase();

  // =================================================
  // HELP
  // =================================================

  if (
    lowerCommand == "help"
  ) {

    printHelp();
  }

  // =================================================
  // STATUS
  // =================================================

  else if (
    lowerCommand == "status"
  ) {

    printStatus();
  }

  // =================================================
  // READ
  // =================================================

  else if (
    lowerCommand == "read"
  ) {

    printSensorReading();
  }

  // =================================================
  // WIFI
  // =================================================

  else if (
    lowerCommand == "wifi"
  ) {

    printWiFiInfo();
  }

  // =================================================
  // MQTT
  // =================================================

  else if (
    lowerCommand == "mqtt"
  ) {

    printMQTTInfo();
  }

  // =================================================
  // ESP-NOW
  // =================================================

  else if (
    lowerCommand == "espnow"
  ) {

    printESPNowInfo();
  }

  else if (
    lowerCommand == "master"
  ) {

    printESPNowInfo();
  }

  else if (
    lowerCommand == "ip"
  ) {

    if (currentMode == MODE_ONLINE && WiFi.status() == WL_CONNECTED) {
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.print("Offline ESP-NOW only | Channel: ");
      Serial.println(ESPNOW_OFFLINE_CHANNEL);
    }
  }

  // =================================================
  // OFFLINE
  // =================================================

  else if (
    lowerCommand == "offline"
  ) {

    forceOfflineCLI();
  }

  // =================================================
  // ONLINE
  // =================================================

  else if (
    lowerCommand == "online"
  ) {

    forceOnlineCLI();
  }

  // =================================================
  // SET SSID
  // =================================================

  else if (
    lowerCommand.startsWith(
      "set ssid "
    )
  ) {

    String newSSID =
      command.substring(
        9
      );

    newSSID.trim();

    if (
      newSSID.length() == 0
    ) {

      Serial.println(
        "SSID cannot be empty."
      );

      return;
    }

    wifiSSID =
      newSSID;

    Serial.println();

    Serial.print(
      "New SSID: "
    );

    Serial.println(
      wifiSSID
    );

    Serial.println(
      "Type 'save' to store permanently."
    );
  }

  // =================================================
  // SET PASSWORD
  // =================================================

  else if (
    lowerCommand.startsWith(
      "set password "
    )
  ) {

    String newPassword =
      command.substring(
        13
      );

    newPassword.trim();

    wifiPassword =
      newPassword;

    Serial.println();

    Serial.println(
      "Password updated in memory."
    );

    Serial.println(
      "Type 'save' to store permanently."
    );
  }

  // =================================================
  // SAVE
  // =================================================

  else if (
    lowerCommand == "save"
  ) {

    saveWiFiSettings();
  }

  // =================================================
  // LEGACY MQTT TEST ON
  // Same as OFFLINE
  // =================================================

  else if (
    lowerCommand == "mqtt test on"
  ) {

    Serial.println();

    Serial.println(
      "MQTT TEST ON now uses full OFFLINE mode."
    );

    forceOfflineCLI();
  }

  // =================================================
  // LEGACY MQTT TEST OFF
  // Same as ONLINE
  // =================================================

  else if (
    lowerCommand == "mqtt test off"
  ) {

    Serial.println();

    Serial.println(
      "MQTT TEST OFF now returns to ONLINE mode."
    );

    forceOnlineCLI();
  }

  // =================================================
  // RESTART
  // =================================================

  else if (
    lowerCommand == "restart" ||
    lowerCommand == "reboot"
  ) {

    Serial.println();

    Serial.println(
      "Restarting TDS node..."
    );

    delay(
      500
    );

    ESP.restart();
  }

  // =================================================
  // UNKNOWN
  // =================================================

  else {

    Serial.println();

    Serial.print(
      "Unknown command: "
    );

    Serial.println(
      command
    );

    Serial.println(
      "Type 'help' for commands."
    );
  }
}


// =====================================================
// SEND TELEMETRY
// =====================================================

void sendTelemetry() {

  // -----------------------------------------
  // Read sensor
  // -----------------------------------------

  readTDS();

  // -----------------------------------------
  // Build telemetry JSON
  // SAME JSON used for MQTT and ESP-NOW
  // -----------------------------------------

  StaticJsonDocument<256> json;

  json["deviceId"] =
    deviceId;

  json["timestamp"] =
    millis();

  json["tds_ppm"] =
    latestTdsPpm;

  json["tdsADC"] =
    latestTdsADC;

  json["tdsVoltage"] =
    latestTdsVoltage;

  char buffer[256];

  // Always serialize with an explicit bound so the exact payload sent over
  // MQTT and ESP-NOW is null-terminated and cannot be silently truncated.
  size_t serializedLength = serializeJson(json, buffer, sizeof(buffer));
  if (serializedLength == 0 || serializedLength >= sizeof(buffer)) {
    Serial.println("Telemetry JSON serialization failed or was truncated");
    return;
  }

  // -----------------------------------------
  // Print reading
  // -----------------------------------------

  Serial.println();

  Serial.println(
    "----------------------------------------"
  );

  Serial.printf(
    "TDS: %.1f ppm | Voltage: %.3f V | ADC: %.0f\n",
    latestTdsPpm,
    latestTdsVoltage,
    latestTdsADC
  );

  // =================================================
  // ONLINE MODE
  // MQTT / THINGSBOARD ONLY
  // =================================================

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    Serial.println(
      "Mode: ONLINE"
    );

    // -----------------------------------------
    // WiFi must exist
    // -----------------------------------------

    if (
      WiFi.status() !=
      WL_CONNECTED
    ) {

      Serial.println(
        "WiFi unavailable."
      );

      Serial.println(
        "Waiting for offline mode switch..."
      );

      Serial.println(
        "----------------------------------------"
      );

      return;
    }

    // -----------------------------------------
    // Ensure MQTT connection
    // -----------------------------------------

    if (
      !mqttClient.connected()
    ) {

      reconnectMQTT();
    }

    // -----------------------------------------
    // Publish
    // -----------------------------------------

    if (
      mqttClient.connected()
    ) {

      bool published =
        mqttClient.publish(
          mqtt_topic,
          buffer
        );

      if (published) {

        Serial.println(
          "MQTT Sent Successfully"
        );

        Serial.println(
          "Destination: ThingsBoard"
        );

        digitalWrite(
          ledPin,
          LOW
        );

        delay(
          50
        );

        digitalWrite(
          ledPin,
          HIGH
        );

      } else {

        Serial.println(
          "MQTT publish FAILED"
        );

        Serial.println(
          "WiFi is still available."
        );

        Serial.println(
          "ESP-NOW will NOT be used."
        );

        Serial.println(
          "MQTT will retry."
        );
      }

    } else {

      Serial.println(
        "MQTT unavailable."
      );

      Serial.println(
        "WiFi is still available."
      );

      Serial.println(
        "ESP-NOW will NOT be used."
      );

      Serial.println(
        "MQTT will retry."
      );
    }
  }

  // =================================================
  // OFFLINE MODE
  // ESP-NOW ONLY
  // =================================================

  else {

    Serial.println(
      "Mode: OFFLINE"
    );

    Serial.println(
      "Destination: FarmWiz Master"
    );

    Serial.print(
      "ESP-NOW Channel: "
    );

    Serial.println(
      ESPNOW_OFFLINE_CHANNEL
    );

    if (!espNowReady) {

      Serial.println(
        "ESP-NOW not ready. Reinitializing..."
      );

      setupESPNowOffline();
    }

    bool espNowSent =
      sendToMaster(
        buffer
      );

    // Offline telemetry has no MQTT status indication. Give the local
    // status LED a short pulse for each ESP-NOW send attempt.
    digitalWrite(ledPin, LOW);
    delay(50);
    digitalWrite(ledPin, HIGH);

    if (espNowSent) {

      Serial.println(
        "ESP-NOW packet queued"
      );

    } else {

      Serial.println(
        "WARNING: ESP-NOW send failed"
      );
    }
  }

  Serial.println(
    "----------------------------------------"
  );

  Serial.println();
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(
    115200
  );

  Serial.setTimeout(
    100
  );

  delay(
    1000
  );

  // -----------------------------------------
  // LED
  // -----------------------------------------

  pinMode(
    ledPin,
    OUTPUT
  );

  digitalWrite(
    ledPin,
    LOW
  );

  // -----------------------------------------
  // TDS ADC
  // -----------------------------------------

  analogReadResolution(
    12
  );

  // -----------------------------------------
  // Startup banner
  // -----------------------------------------

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "FarmWiz TDS Sensor Node"
  );

  Serial.println(
    "WiFi MQTT + ESP-NOW Offline Fallback"
  );

  Serial.print(
    "Device: "
  );

  Serial.println(
    deviceId
  );

  Serial.print(
    "Offline ESP-NOW Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  Serial.println(
    "========================================"
  );

  // -----------------------------------------
  // Load saved WiFi
  // -----------------------------------------

  loadWiFiSettings();

  // -----------------------------------------
  // MQTT configuration
  // -----------------------------------------

  mqttClient.setServer(
    mqtt_server,
    mqtt_port
  );

  mqttClient.setCallback(
    mqttCallback
  );

  mqttClient.setKeepAlive(
    60
  );

  // -----------------------------------------
  // Try normal WiFi FIRST
  // -----------------------------------------

  bool wifiConnected =
    connectToWiFi(
      WIFI_CONNECT_TIMEOUT
    );

  // -----------------------------------------
  // Select operating mode
  // -----------------------------------------

  if (wifiConnected) {

    enterOnlineMode();

  } else {

    enterOfflineMode();
  }

  // -----------------------------------------
  // READY
  // -----------------------------------------

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "NODE READY"
  );

  Serial.println(
    "========================================"
  );

  Serial.print(
    "Mode: "
  );

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    Serial.println(
      "ONLINE / MQTT"
    );

    Serial.print(
      "WiFi: "
    );

    Serial.println(
      WiFi.SSID()
    );

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

  } else {

    Serial.println(
      "OFFLINE / ESP-NOW"
    );

    Serial.print(
      "ESP-NOW Channel: "
    );

    Serial.println(
      ESPNOW_OFFLINE_CHANNEL
    );
  }

  Serial.println();

  Serial.println(
    "Type 'help' for CLI commands."
  );

  Serial.println(
    "========================================"
  );

  Serial.println();

  // Allow first telemetry packet immediately

  lastTelemetrySend =
    millis() -
    TELEMETRY_INTERVAL;
}


// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  // =================================================
  // CLI
  // =================================================

  handleCLI();

  // =================================================
  // AUTOMATIC WIFI / ESP-NOW MODE MANAGEMENT
  // =================================================

  handleConnectionMode();

  // =================================================
  // MQTT MANAGEMENT
  // =================================================

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    handleMQTTReconnect();

    if (
      mqttClient.connected()
    ) {

      mqttClient.loop();
    }
  }

  // =================================================
  // TELEMETRY EVERY 5 SECONDS
  // =================================================

  if (
    millis() - lastTelemetrySend >=
    TELEMETRY_INTERVAL
  ) {

    lastTelemetrySend =
      millis();

    sendTelemetry();
  }

  delay(
    2
  );
}
