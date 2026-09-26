#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// =====================================================
// DEFAULT WIFI
// Used only if nothing has been saved in Preferences
// =====================================================

const char* DEFAULT_SSID = "HUAWEI-2.4G-bF2k";
const char* DEFAULT_PASSWORD = "DuuBnx2S";

// =====================================================
// OFFLINE / ESP-NOW CONFIGURATION
// =====================================================

// Fixed channel used by ALL nodes when normal WiFi
// is unavailable.

#define ESPNOW_OFFLINE_CHANNEL 6

// Master creates this local WiFi network only
// while in OFFLINE / ESP-NOW mode.
//
// This lets you still access the dashboard locally.

const char* OFFLINE_AP_SSID = "FarmWiz_Offline";
const char* OFFLINE_AP_PASSWORD = "farmwiz123";

// =====================================================
// WIFI SETTINGS
// =====================================================

String wifiSSID;
String wifiPassword;

// =====================================================
// PREFERENCES
// =====================================================

Preferences preferences;

// =====================================================
// WEB SERVER
// =====================================================

WebServer server(80);

// =====================================================
// MASTER MODE
// =====================================================

enum MasterMode {

  MODE_ONLINE,
  MODE_OFFLINE

};

MasterMode currentMode = MODE_OFFLINE;

// Tracks whether ESP-NOW has been initialized

bool espNowReady = false;

// Manual offline override.
//
// false:
// Master automatically manages WiFi / offline mode.
//
// true:
// Master remains offline until user types "online".

bool manualOfflineMode = false;

// =====================================================
// TIMERS
// =====================================================

// Initial WiFi connection attempt

const unsigned long WIFI_CONNECT_TIMEOUT = 15000;

// If WiFi disappears while online,
// wait this long before entering offline mode.

const unsigned long WIFI_LOSS_TIMEOUT = 10000;

// While automatically offline,
// periodically check if normal WiFi returned.

const unsigned long WIFI_RETRY_INTERVAL = 30000;

unsigned long wifiLostAt = 0;

unsigned long lastWiFiRetry = 0;

// =====================================================
// NODE STORAGE
// =====================================================

#define MAX_NODES 20

struct NodeData {

  String deviceId;

  String jsonData;

  String macAddress;

  unsigned long lastSeen;

  bool active;

};

NodeData nodes[MAX_NODES];

// =====================================================
// FIND EXISTING NODE
// =====================================================

int findNode(const String& deviceId) {

  for (int i = 0; i < MAX_NODES; i++) {

    if (
      nodes[i].active &&
      nodes[i].deviceId == deviceId
    ) {

      return i;
    }
  }

  return -1;
}

// =====================================================
// CREATE NEW NODE
// =====================================================

int createNode(const String& deviceId) {

  for (int i = 0; i < MAX_NODES; i++) {

    if (!nodes[i].active) {

      nodes[i].active = true;

      nodes[i].deviceId = deviceId;

      nodes[i].jsonData = "";

      nodes[i].macAddress = "";

      nodes[i].lastSeen = 0;

      Serial.print(
        "New node registered: "
      );

      Serial.println(
        deviceId
      );

      return i;
    }
  }

  Serial.println(
    "ERROR: Node storage full!"
  );

  return -1;
}

// =====================================================
// MAC TO STRING
// =====================================================

String macToString(
  const uint8_t* mac
) {

  char buffer[18];

  snprintf(
    buffer,
    sizeof(buffer),

    "%02X:%02X:%02X:%02X:%02X:%02X",

    mac[0],
    mac[1],
    mac[2],
    mac[3],
    mac[4],
    mac[5]
  );

  return String(buffer);
}

// =====================================================
// ESP-NOW RECEIVE CALLBACK
// ESP32 Arduino Core 2.0.6
// =====================================================

void onDataRecv(
  const uint8_t* mac,
  const uint8_t* incomingData,
  int len
) {

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "ESP-NOW DATA RECEIVED"
  );

  Serial.println(
    "================================"
  );

  String senderMAC =
    macToString(mac);

  Serial.print(
    "Sender MAC: "
  );

  Serial.println(
    senderMAC
  );

  // -----------------------------------------
  // Convert incoming bytes to String
  // -----------------------------------------

  String incomingJSON;

  incomingJSON.reserve(
    len + 1
  );

  for (
    int i = 0;
    i < len;
    i++
  ) {

    incomingJSON +=
      (char)incomingData[i];
  }

  Serial.print(
    "JSON: "
  );

  Serial.println(
    incomingJSON
  );

  // -----------------------------------------
  // Parse JSON
  // -----------------------------------------

  StaticJsonDocument<512> doc;

  DeserializationError error =
    deserializeJson(
      doc,
      incomingJSON
    );

  if (error) {

    Serial.print(
      "JSON parse failed: "
    );

    Serial.println(
      error.c_str()
    );

    return;
  }

  // -----------------------------------------
  // Get device ID
  // -----------------------------------------

  const char* id =
    doc["deviceId"];

  if (id == nullptr) {

    Serial.println(
      "ERROR: No deviceId found"
    );

    return;
  }

  String deviceId =
    String(id);

  Serial.print(
    "Device ID: "
  );

  Serial.println(
    deviceId
  );

  // -----------------------------------------
  // Find/create node
  // -----------------------------------------

  int nodeIndex =
    findNode(
      deviceId
    );

  if (nodeIndex == -1) {

    nodeIndex =
      createNode(
        deviceId
      );
  }

  if (nodeIndex == -1) {

    return;
  }

  // -----------------------------------------
  // Store latest packet
  // -----------------------------------------

  nodes[nodeIndex].jsonData =
    incomingJSON;

  nodes[nodeIndex].macAddress =
    senderMAC;

  nodes[nodeIndex].lastSeen =
    millis();

  Serial.print(
    "Stored node index: "
  );

  Serial.println(
    nodeIndex
  );

  Serial.println(
    "================================"
  );
}

// =====================================================
// API - RETURN NODE DATA
// =====================================================

void handleNodeAPI() {

  DynamicJsonDocument response(
    8192
  );

  JsonArray array =
    response.to<JsonArray>();

  for (
    int i = 0;
    i < MAX_NODES;
    i++
  ) {

    if (!nodes[i].active) {

      continue;
    }

    JsonObject node =
      array.createNestedObject();

    node["deviceId"] =
      nodes[i].deviceId;

    node["mac"] =
      nodes[i].macAddress;

    node["lastSeen"] =
      nodes[i].lastSeen;

    DynamicJsonDocument sensorDoc(
      1024
    );

    DeserializationError error =
      deserializeJson(
        sensorDoc,
        nodes[i].jsonData
      );

    if (!error) {

      JsonObject sensorData =
        node.createNestedObject(
          "data"
        );

      for (
        JsonPair kv :
        sensorDoc.as<JsonObject>()
      ) {

        sensorData[kv.key()] =
          kv.value();
      }
    }
  }

  String output;

  serializeJson(
    response,
    output
  );

  server.send(
    200,
    "application/json",
    output
  );
}

// =====================================================
// SERVE index.html AT ROOT
// =====================================================

void handleRoot() {

  if (
    !SPIFFS.exists(
      "/index.html"
    )
  ) {

    server.send(
      404,
      "text/plain",
      "index.html not found in SPIFFS"
    );

    return;
  }

  File file =
    SPIFFS.open(
      "/index.html",
      "r"
    );

  if (!file) {

    server.send(
      500,
      "text/plain",
      "Failed to open index.html"
    );

    return;
  }

  server.streamFile(
    file,
    "text/html"
  );

  file.close();
}

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
    "WiFi settings loaded."
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
    "=============================="
  );

  Serial.println(
    "WiFi settings saved."
  );

  Serial.println(
    "=============================="
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
    "Type 'restart' to reconnect using the new WiFi."
  );
}

// =====================================================
// TRY NORMAL WIFI
// =====================================================

bool connectToWiFi(
  unsigned long timeout
) {

  Serial.println();

  Serial.print(
    "Connecting to WiFi: "
  );

  Serial.println(
    wifiSSID
  );

  // Stop offline AP if active

  WiFi.softAPdisconnect(
    true
  );

  WiFi.mode(
    WIFI_STA
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

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < timeout
  ) {

    delay(
      500
    );

    Serial.print(".");
  }

  // -----------------------------------------
  // WiFi connected
  // -----------------------------------------

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    WiFi.setSleep(
      false
    );

    Serial.println();

    Serial.println(
      "WiFi connected!"
    );

    Serial.print(
      "Local IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "Master MAC: "
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
  // WiFi unavailable
  // -----------------------------------------

  Serial.println();

  Serial.println(
    "WiFi network unavailable."
  );

  return false;
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
// START ESP-NOW RECEIVER
// =====================================================

void setupESPNow() {

  if (espNowReady) {

    return;
  }

  Serial.println();

  Serial.println(
    "Starting ESP-NOW..."
  );

  if (
    esp_now_init() != ESP_OK
  ) {

    Serial.println(
      "ESP-NOW initialization FAILED!"
    );

    espNowReady =
      false;

    return;
  }

  esp_now_register_recv_cb(
    onDataRecv
  );

  espNowReady =
    true;

  Serial.println(
    "ESP-NOW receiver ready"
  );

  Serial.print(
    "ESP-NOW Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );
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
    "MASTER MODE: ONLINE"
  );

  Serial.println(
    "Normal WiFi network is available."
  );

  Serial.println(
    "ESP-NOW fallback not required."
  );

  Serial.println(
    "========================================"
  );

  // ESP-NOW only used offline

  stopESPNow();

  currentMode =
    MODE_ONLINE;

  wifiLostAt =
    0;

  Serial.print(
    "Dashboard: http://"
  );

  Serial.println(
    WiFi.localIP()
  );
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
    "MASTER MODE: OFFLINE"
  );

  Serial.println(
    "Using ESP-NOW fallback."
  );

  Serial.print(
    "ESP-NOW Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  Serial.println(
    "========================================"
  );

  // -----------------------------------------
  // Stop existing ESP-NOW
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
  // AP + STA MODE
  //
  // The AP gives us the local dashboard.
  //
  // Because the AP is on channel 6,
  // ESP-NOW also operates on channel 6.
  // -----------------------------------------

  WiFi.mode(
    WIFI_AP_STA
  );

  WiFi.setSleep(
    false
  );

  bool apStarted =
    WiFi.softAP(
      OFFLINE_AP_SSID,
      OFFLINE_AP_PASSWORD,
      ESPNOW_OFFLINE_CHANNEL
    );

  if (apStarted) {

    Serial.println();

    Serial.println(
      "Offline WiFi started"
    );

    Serial.print(
      "SSID: "
    );

    Serial.println(
      OFFLINE_AP_SSID
    );

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.softAPIP()
    );

    Serial.print(
      "Channel: "
    );

    Serial.println(
      ESPNOW_OFFLINE_CHANNEL
    );

  } else {

    Serial.println();

    Serial.println(
      "WARNING: Failed to start offline AP!"
    );
  }

  delay(
    300
  );

  // -----------------------------------------
  // Start ESP-NOW
  // -----------------------------------------

  setupESPNow();

  currentMode =
    MODE_OFFLINE;

  lastWiFiRetry =
    millis();

  Serial.println();

  Serial.println(
    "OFFLINE SYSTEM READY"
  );

  Serial.println(
    "Sensor nodes must use ESP-NOW Channel 6."
  );

  Serial.print(
    "Offline Dashboard: http://"
  );

  Serial.println(
    WiFi.softAPIP()
  );

  if (manualOfflineMode) {

    Serial.println();

    Serial.println(
      "Manual Offline Override: ON"
    );

    Serial.println(
      "Master will remain offline until 'online' is entered."
    );
  }
}

// =====================================================
// SPIFFS SETUP
// =====================================================

void setupSPIFFS() {

  Serial.println();

  Serial.println(
    "Mounting SPIFFS..."
  );

  if (
    !SPIFFS.begin(true)
  ) {

    Serial.println(
      "SPIFFS mount FAILED!"
    );

    return;
  }

  Serial.println(
    "SPIFFS mounted successfully"
  );

  if (
    SPIFFS.exists(
      "/index.html"
    )
  ) {

    Serial.println(
      "index.html found"
    );

  } else {

    Serial.println(
      "WARNING: index.html NOT found"
    );
  }
}

// =====================================================
// WEB SERVER SETUP
// =====================================================

void setupWebServer() {

  // -----------------------------------------
  // Root dashboard
  // -----------------------------------------

  server.on(
    "/",
    HTTP_GET,
    handleRoot
  );

  // -----------------------------------------
  // Node API
  // -----------------------------------------

  server.on(
    "/api/nodes",
    HTTP_GET,
    handleNodeAPI
  );

  // -----------------------------------------
  // 404
  // -----------------------------------------

  server.onNotFound([]() {

    server.send(
      404,
      "text/plain",
      "404 - Not Found"
    );

  });

  server.begin();

  Serial.println(
    "Web server started"
  );
}

// =====================================================
// MASTER CONNECTION HANDLER
// =====================================================

void handleMasterConnection() {

  // =================================================
  // MANUAL OFFLINE MODE
  // =================================================

  if (
    manualOfflineMode
  ) {

    // Do nothing.
    //
    // Keep:
    // FarmWiz_Offline AP
    // ESP-NOW channel 6
    //
    // until user types "online".

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
    // WiFi still connected
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
    // WiFi disappeared
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
        "Waiting for recovery..."
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
        "WiFi did not recover."
      );

      Serial.println(
        "Entering ESP-NOW offline mode."
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
    // Wait before checking normal WiFi
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
    // Stop ESP-NOW before changing radio mode
    // -----------------------------------------

    stopESPNow();

    // -----------------------------------------
    // Stop offline AP
    // -----------------------------------------

    WiFi.softAPdisconnect(
      true
    );

    // -----------------------------------------
    // Station mode
    // -----------------------------------------

    WiFi.mode(
      WIFI_STA
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

    // Try normal WiFi for 5 seconds

    while (
      WiFi.status() != WL_CONNECTED &&
      millis() - start < 5000
    ) {

      delay(
        250
      );
    }

    // -----------------------------------------
    // WiFi restored
    // -----------------------------------------

    if (
      WiFi.status() ==
      WL_CONNECTED
    ) {

      WiFi.setSleep(
        false
      );

      Serial.println();

      Serial.println(
        "Normal WiFi restored!"
      );

      Serial.print(
        "SSID: "
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
// CLI HELP
// =====================================================

void printHelp() {

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "FarmWiz Master CLI"
  );

  Serial.println(
    "================================"
  );

  Serial.println(
    "help"
  );

  Serial.println(
    "  Show this command list"
  );

  Serial.println();

  Serial.println(
    "status"
  );

  Serial.println(
    "  Show master status"
  );

  Serial.println();

  Serial.println(
    "nodes"
  );

  Serial.println(
    "  List received sensor nodes"
  );

  Serial.println();

  Serial.println(
    "wifi"
  );

  Serial.println(
    "  Show current WiFi information"
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
    "ip"
  );

  Serial.println(
    "  Show dashboard IP"
  );

  Serial.println();

  Serial.println(
    "offline"
  );

  Serial.println(
    "  Force master into ESP-NOW offline mode"
  );

  Serial.println();

  Serial.println(
    "online"
  );

  Serial.println(
    "  Exit forced offline mode and retry normal WiFi"
  );

  Serial.println();

  Serial.println(
    "set ssid <SSID>"
  );

  Serial.println(
    "  Change WiFi SSID in memory"
  );

  Serial.println();

  Serial.println(
    "set password <PASSWORD>"
  );

  Serial.println(
    "  Change WiFi password in memory"
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
    "restart"
  );

  Serial.println(
    "  Restart master ESP32"
  );

  Serial.println();

  Serial.println(
    "================================"
  );
}

// =====================================================
// CLI STATUS
// =====================================================

void printStatus() {

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "MASTER STATUS"
  );

  Serial.println(
    "================================"
  );

  Serial.print(
    "Mode: "
  );

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    Serial.println(
      "ONLINE / WIFI"
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

  Serial.print(
    "Configured SSID: "
  );

  Serial.println(
    wifiSSID
  );

  Serial.print(
    "WiFi: "
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "CONNECTED"
    );

  } else {

    Serial.println(
      "DISCONNECTED"
    );
  }

  // -----------------------------------------
  // ONLINE INFO
  // -----------------------------------------

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    Serial.print(
      "SSID: "
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
      "Router Channel: "
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
  // OFFLINE INFO
  // -----------------------------------------

  else {

    Serial.print(
      "Offline AP: "
    );

    Serial.println(
      OFFLINE_AP_SSID
    );

    Serial.print(
      "Offline IP: "
    );

    Serial.println(
      WiFi.softAPIP()
    );

    Serial.print(
      "ESP-NOW Channel: "
    );

    Serial.println(
      ESPNOW_OFFLINE_CHANNEL
    );
  }

  Serial.print(
    "Master MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  Serial.print(
    "ESP-NOW: "
  );

  Serial.println(
    espNowReady
      ? "READY"
      : "OFF"
  );

  Serial.print(
    "Uptime: "
  );

  Serial.print(
    millis() / 1000
  );

  Serial.println(
    " sec"
  );

  int nodeCount =
    0;

  for (
    int i = 0;
    i < MAX_NODES;
    i++
  ) {

    if (
      nodes[i].active
    ) {

      nodeCount++;
    }
  }

  Serial.print(
    "Sensor Nodes: "
  );

  Serial.println(
    nodeCount
  );

  Serial.println(
    "================================"
  );
}

// =====================================================
// CLI LIST NODES
// =====================================================

void printNodes() {

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "SENSOR NODES"
  );

  Serial.println(
    "================================"
  );

  bool found =
    false;

  for (
    int i = 0;
    i < MAX_NODES;
    i++
  ) {

    if (
      !nodes[i].active
    ) {

      continue;
    }

    found =
      true;

    Serial.print(
      "["
    );

    Serial.print(
      i
    );

    Serial.print(
      "] "
    );

    Serial.print(
      nodes[i].deviceId
    );

    Serial.print(
      " | MAC: "
    );

    Serial.print(
      nodes[i].macAddress
    );

    Serial.print(
      " | Last Seen: "
    );

    unsigned long secondsAgo =
      (
        millis() -
        nodes[i].lastSeen
      ) / 1000;

    Serial.print(
      secondsAgo
    );

    Serial.println(
      " sec ago"
    );

    Serial.print(
      "    Data: "
    );

    Serial.println(
      nodes[i].jsonData
    );
  }

  if (!found) {

    Serial.println(
      "No sensor nodes received yet."
    );
  }

  Serial.println(
    "================================"
  );
}

// =====================================================
// CLI WIFI INFO
// =====================================================

void printWiFiInfo() {

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "WIFI"
  );

  Serial.println(
    "================================"
  );

  Serial.print(
    "Configured SSID: "
  );

  Serial.println(
    wifiSSID
  );

  Serial.print(
    "Mode: "
  );

  Serial.println(
    currentMode == MODE_ONLINE
      ? "NORMAL WIFI"
      : "OFFLINE FALLBACK"
  );

  // -----------------------------------------
  // ONLINE
  // -----------------------------------------

  if (
    currentMode ==
    MODE_ONLINE
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

  // -----------------------------------------
  // OFFLINE
  // -----------------------------------------

  else {

    Serial.print(
      "Offline SSID: "
    );

    Serial.println(
      OFFLINE_AP_SSID
    );

    Serial.print(
      "Offline IP: "
    );

    Serial.println(
      WiFi.softAPIP()
    );

    Serial.print(
      "Offline Channel: "
    );

    Serial.println(
      ESPNOW_OFFLINE_CHANNEL
    );
  }

  Serial.println(
    "================================"
  );
}

// =====================================================
// CLI ESP-NOW INFO
// =====================================================

void printESPNowInfo() {

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "ESP-NOW"
  );

  Serial.println(
    "================================"
  );

  Serial.print(
    "Status: "
  );

  Serial.println(
    espNowReady
      ? "READY"
      : "OFF"
  );

  Serial.print(
    "Master MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  Serial.print(
    "Offline Channel: "
  );

  Serial.println(
    ESPNOW_OFFLINE_CHANNEL
  );

  Serial.print(
    "Manual Override: "
  );

  Serial.println(
    manualOfflineMode
      ? "ON"
      : "OFF"
  );

  Serial.println();

  Serial.println(
    "ESP-NOW is used only in offline mode."
  );

  Serial.println(
    "================================"
  );
}

// =====================================================
// CLI HANDLER
// =====================================================

void handleCLI() {

  if (!Serial.available()) {

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

  // Lowercase copy used only for matching

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
  // NODES
  // =================================================

  else if (
    lowerCommand == "nodes"
  ) {

    printNodes();
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
  // IP
  // =================================================

  else if (
    lowerCommand == "ip"
  ) {

    if (
      currentMode ==
      MODE_ONLINE
    ) {

      Serial.print(
        "Dashboard: http://"
      );

      Serial.println(
        WiFi.localIP()
      );

    } else {

      Serial.print(
        "Offline Dashboard: http://"
      );

      Serial.println(
        WiFi.softAPIP()
      );
    }
  }

  // =================================================
  // ESP-NOW
  // =================================================

  else if (
    lowerCommand == "espnow"
  ) {

    printESPNowInfo();
  }

  // =================================================
  // FORCE OFFLINE MODE
  // =================================================

  else if (
    lowerCommand == "offline"
  ) {

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
      "ESP-NOW Channel 6 will be enabled."
    );

    Serial.println(
      "Automatic WiFi reconnect is disabled."
    );

    Serial.println();

    Serial.println(
      "Type 'online' to return to normal mode."
    );

    manualOfflineMode =
      true;

    enterOfflineMode();
  }

  // =================================================
  // RETURN TO ONLINE MODE
  // =================================================

  else if (
    lowerCommand == "online"
  ) {

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

    Serial.println(
      "Stopping ESP-NOW and trying normal WiFi..."
    );

    // Disable manual override

    manualOfflineMode =
      false;

    // Stop ESP-NOW

    stopESPNow();

    // Stop offline AP

    WiFi.softAPdisconnect(
      true
    );

    delay(
      300
    );

    bool connected =
      connectToWiFi(
        WIFI_CONNECT_TIMEOUT
      );

    // -----------------------------------------
    // Online successful
    // -----------------------------------------

    if (connected) {

      enterOnlineMode();

      Serial.println();

      Serial.println(
        "ONLINE MODE SUCCESSFUL"
      );

      Serial.print(
        "Dashboard: http://"
      );

      Serial.println(
        WiFi.localIP()
      );
    }

    // -----------------------------------------
    // WiFi unavailable
    // -----------------------------------------

    else {

      Serial.println();

      Serial.println(
        "Configured WiFi is not available."
      );

      Serial.println(
        "Returning to automatic ESP-NOW offline mode."
      );

      enterOfflineMode();
    }
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

    Serial.println(
      "SSID updated in memory."
    );

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
      "WiFi password updated in memory."
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
  // RESTART
  // =================================================

  else if (
    lowerCommand == "restart"
  ) {

    Serial.println();

    Serial.println(
      "Restarting FarmWiz Master..."
    );

    delay(
      500
    );

    ESP.restart();
  }

  // =================================================
  // UNKNOWN COMMAND
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

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "FarmWiz Master Gateway"
  );

  Serial.println(
    "WiFi Dashboard + ESP-NOW Offline Mode"
  );

  Serial.println(
    "========================================"
  );

  // =================================================
  // SPIFFS
  // =================================================

  setupSPIFFS();

  // =================================================
  // LOAD WIFI SETTINGS
  // =================================================

  loadWiFiSettings();

  // =================================================
  // TRY NORMAL WIFI
  // =================================================

  bool wifiConnected =
    connectToWiFi(
      WIFI_CONNECT_TIMEOUT
    );

  // =================================================
  // SELECT MASTER MODE
  // =================================================

  if (
    wifiConnected
  ) {

    enterOnlineMode();

  } else {

    enterOfflineMode();
  }

  // =================================================
  // START WEB SERVER
  // =================================================

  setupWebServer();

  // =================================================
  // READY
  // =================================================

  Serial.println();

  Serial.println(
    "========================================"
  );

  Serial.println(
    "MASTER READY"
  );

  Serial.println(
    "========================================"
  );

  Serial.print(
    "Master MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  // -----------------------------------------
  // ONLINE
  // -----------------------------------------

  if (
    currentMode ==
    MODE_ONLINE
  ) {

    Serial.println(
      "Mode: ONLINE"
    );

    Serial.print(
      "Dashboard: http://"
    );

    Serial.println(
      WiFi.localIP()
    );
  }

  // -----------------------------------------
  // OFFLINE
  // -----------------------------------------

  else {

    Serial.println(
      "Mode: OFFLINE ESP-NOW"
    );

    Serial.print(
      "ESP-NOW Channel: "
    );

    Serial.println(
      ESPNOW_OFFLINE_CHANNEL
    );

    Serial.print(
      "Offline WiFi: "
    );

    Serial.println(
      OFFLINE_AP_SSID
    );

    Serial.print(
      "Dashboard: http://"
    );

    Serial.println(
      WiFi.softAPIP()
    );
  }

  Serial.println();

  Serial.println(
    "Type 'help' for CLI commands."
  );

  Serial.println(
    "========================================"
  );
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  // =================================================
  // CLI
  // =================================================

  handleCLI();

  // =================================================
  // DASHBOARD
  // =================================================

  server.handleClient();

  // =================================================
  // MANAGE WIFI / ESP-NOW MODE
  // =================================================

  handleMasterConnection();

  delay(
    2
  );
}
