#include "esp_camera.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// =====================================================
// CAMERA MODEL
// =====================================================

#define CAMERA_MODEL_WROVER_KIT

#include "camera_pins.h"

// =====================================================
// DEVICE
// =====================================================

const char* cameraId =
    "ESP32-CAM-01";

// =====================================================
// NODE.JS CLOUD SERVER
// =====================================================
//
// This NEVER needs to change when changing Wi-Fi.
//

const char* nodeHost =
    "168.195.218.199";

const uint16_t nodePort =
    8082;

const char* nodeWebSocketPath =
    "/camera";

// =====================================================
// WIFI CONFIGURATION AP
// =====================================================
//
// If saved Wi-Fi does not work,
// this AP automatically appears.
//

const char* setupAPName =
    "FarmWiz-Camera-01";

const char* setupAPPassword =
    "FarmWiz123";

// Try saved Wi-Fi for 20 seconds
// before starting AP setup mode.

const unsigned long WIFI_CONNECT_TIMEOUT_MS =
    20000;

// =====================================================
// PREFERENCES / FLASH STORAGE
// =====================================================

Preferences preferences;

String savedSSID = "";
String savedPassword = "";

// =====================================================
// CONFIGURATION WEB SERVER
// =====================================================

WebServer configServer(80);

bool configPortalActive =
    false;

// =====================================================
// WEBSOCKET
// =====================================================

WebSocketsClient webSocket;

bool websocketConnected =
    false;

bool cloudStreamActive =
    false;

// =====================================================
// CAMERA STREAM SETTINGS
// =====================================================
//
// 100ms = theoretical maximum around 10 FPS
//

unsigned long lastFrameSend =
    0;

const unsigned long frameIntervalMs =
    100;

// =====================================================
// DEBUG STATISTICS
// =====================================================

unsigned long frameCounter =
    0;

unsigned long byteCounter =
    0;

unsigned long lastStatsPrint =
    0;

// =====================================================
// WIFI RECONNECT
// =====================================================

unsigned long lastReconnectAttempt =
    0;

const unsigned long reconnectIntervalMs =
    10000;

// =====================================================
// FUNCTION DECLARATIONS
// =====================================================

void setupCamera();

bool loadWiFiCredentials();

void saveWiFiCredentials(
    const String& ssid,
    const String& password
);

void clearWiFiCredentials();

bool connectSavedWiFi();

void startConfigPortal();

void stopConfigPortal();

void handleConfigRoot();

void handleSaveWiFi();

void handleNotFound();

String buildWiFiSetupPage();

void setupWebSocket();

void webSocketEvent(
    WStype_t type,
    uint8_t* payload,
    size_t length
);

void handleCloudStream();

void sendCameraStatus();

void printStats();

void handleSerialCommands();

void scanWiFiNetworks();

// =====================================================
// SETUP
// =====================================================

void setup() {

    Serial.begin(
        115200
    );

    Serial.setDebugOutput(
        false
    );

    delay(
        1000
    );

    Serial.println();
    Serial.println(
        "=========================================="
    );

    Serial.println(
        " FARMWIZ ESP32-CAM CLOUD STREAM"
    );

    Serial.println(
        "=========================================="
    );

    // =================================================
    // CAMERA
    // =================================================

    setupCamera();

    // =================================================
    // FLASH STORAGE
    // =================================================

    preferences.begin(
        "farmwiz",
        false
    );

    // =================================================
    // LOAD WIFI
    // =================================================

    bool hasSavedWiFi =
        loadWiFiCredentials();

    // =================================================
    // CONNECT WIFI
    // =================================================

    if (
        hasSavedWiFi
    ) {

        bool connected =
            connectSavedWiFi();

        if (
            !connected
        ) {

            Serial.println();
            Serial.println(
                "[WIFI] Saved network unavailable."
            );

            Serial.println(
                "[WIFI] Starting configuration AP."
            );

            startConfigPortal();
        }

    } else {

        Serial.println(
            "[WIFI] No saved Wi-Fi credentials."
        );

        startConfigPortal();
    }

    // =================================================
    // WEBSOCKET
    // =================================================

    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

        setupWebSocket();
    }

    // =================================================
    // READY
    // =================================================

    Serial.println();
    Serial.println(
        "=========================================="
    );

    Serial.println(
        " SYSTEM READY"
    );

    Serial.println(
        "=========================================="
    );

    Serial.println();
    Serial.println(
        "Serial CLI commands:"
    );

    Serial.println(
        "help"
    );

    Serial.println(
        "status"
    );

    Serial.println(
        "wifi"
    );

    Serial.println(
        "wifi scan"
    );

    Serial.println(
        "wifi set SSID|PASSWORD"
    );

    Serial.println(
        "wifi ap"
    );

    Serial.println(
        "resetwifi"
    );

    Serial.println(
        "restart"
    );

    Serial.println();
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

    // =================================================
    // CONFIGURATION PORTAL
    // =================================================

    if (
        configPortalActive
    ) {

        configServer.handleClient();

        handleSerialCommands();

        delay(
            2
        );

        return;
    }

    // =================================================
    // WIFI DISCONNECTED
    // =================================================

    if (
        WiFi.status() !=
        WL_CONNECTED
    ) {

        websocketConnected =
            false;

        cloudStreamActive =
            false;

        unsigned long now =
            millis();

        if (
            now -
            lastReconnectAttempt >=
            reconnectIntervalMs
        ) {

            lastReconnectAttempt =
                now;

            Serial.println();
            Serial.println(
                "[WIFI] Connection lost."
            );

            Serial.println(
                "[WIFI] Trying saved network..."
            );

            bool connected =
                connectSavedWiFi();

            if (
                connected
            ) {

                Serial.println(
                    "[WIFI] Reconnected."
                );

                setupWebSocket();

            } else {

                Serial.println(
                    "[WIFI] Reconnect failed."
                );

                Serial.println(
                    "[WIFI] Starting AP configuration..."
                );

                startConfigPortal();
            }
        }

        handleSerialCommands();

        delay(
            10
        );

        return;
    }

    // =================================================
    // WEBSOCKET
    // =================================================

    webSocket.loop();

    // =================================================
    // STREAM
    // =================================================

    handleCloudStream();

    // =================================================
    // SERIAL CLI
    // =================================================

    handleSerialCommands();

    // =================================================
    // DEBUG
    // =================================================

    printStats();

    delay(
        1
    );
}

// =====================================================
// CAMERA SETUP
// =====================================================

void setupCamera() {

    Serial.println(
        "[CAMERA] Configuring camera..."
    );

    camera_config_t config;

    config.ledc_channel =
        LEDC_CHANNEL_0;

    config.ledc_timer =
        LEDC_TIMER_0;

    config.pin_d0 =
        Y2_GPIO_NUM;

    config.pin_d1 =
        Y3_GPIO_NUM;

    config.pin_d2 =
        Y4_GPIO_NUM;

    config.pin_d3 =
        Y5_GPIO_NUM;

    config.pin_d4 =
        Y6_GPIO_NUM;

    config.pin_d5 =
        Y7_GPIO_NUM;

    config.pin_d6 =
        Y8_GPIO_NUM;

    config.pin_d7 =
        Y9_GPIO_NUM;

    config.pin_xclk =
        XCLK_GPIO_NUM;

    config.pin_pclk =
        PCLK_GPIO_NUM;

    config.pin_vsync =
        VSYNC_GPIO_NUM;

    config.pin_href =
        HREF_GPIO_NUM;

    config.pin_sscb_sda =
        SIOD_GPIO_NUM;

    config.pin_sscb_scl =
        SIOC_GPIO_NUM;

    config.pin_pwdn =
        PWDN_GPIO_NUM;

    config.pin_reset =
        RESET_GPIO_NUM;

    config.xclk_freq_hz =
        20000000;

    // JPEG
    config.pixel_format =
        PIXFORMAT_JPEG;

    // Keep your existing resolution
    config.frame_size =
        FRAMESIZE_QVGA;

    config.jpeg_quality =
        12;

    // =================================================
    // PSRAM
    // =================================================

    if (
        psramFound()
    ) {

        Serial.println(
            "[CAMERA] PSRAM detected."
        );

        config.fb_location =
            CAMERA_FB_IN_PSRAM;

        config.fb_count =
            2;

        config.grab_mode =
            CAMERA_GRAB_LATEST;

    } else {

        Serial.println(
            "[CAMERA] WARNING: PSRAM not detected."
        );

        config.fb_location =
            CAMERA_FB_IN_DRAM;

        config.fb_count =
            1;

        config.grab_mode =
            CAMERA_GRAB_WHEN_EMPTY;
    }

    // =================================================
    // INITIALIZE
    // =================================================

    esp_err_t err =
        esp_camera_init(
            &config
        );

    if (
        err !=
        ESP_OK
    ) {

        Serial.printf(
            "[CAMERA] Initialization FAILED: 0x%x\n",
            err
        );

        while (
            true
        ) {

            delay(
                1000
            );
        }
    }

    Serial.println(
        "[CAMERA] Initialized successfully."
    );

    // =================================================
    // SENSOR SETTINGS
    // =================================================

    sensor_t* s =
        esp_camera_sensor_get();

    if (
        s != nullptr
    ) {

        s->set_framesize(
            s,
            FRAMESIZE_QVGA
        );

        s->set_quality(
            s,
            12
        );

        s->set_brightness(
            s,
            0
        );

        s->set_contrast(
            s,
            0
        );

        s->set_saturation(
            s,
            0
        );

        s->set_vflip(
            s,
            1
        );

        s->set_hmirror(
            s,
            0
        );

        s->set_awb_gain(
            s,
            1
        );

        s->set_wb_mode(
            s,
            0
        );

        s->set_aec2(
            s,
            1
        );
    }

    Serial.println(
        "[CAMERA] Sensor configured."
    );
}

// =====================================================
// LOAD SAVED WIFI
// =====================================================

bool loadWiFiCredentials() {

    savedSSID =
        preferences.getString(
            "ssid",
            ""
        );

    savedPassword =
        preferences.getString(
            "password",
            ""
        );

    if (
        savedSSID.length() ==
        0
    ) {

        return false;
    }

    Serial.print(
        "[WIFI] Saved SSID: "
    );

    Serial.println(
        savedSSID
    );

    return true;
}

// =====================================================
// SAVE WIFI
// =====================================================

void saveWiFiCredentials(
    const String& ssid,
    const String& password
) {

    preferences.putString(
        "ssid",
        ssid
    );

    preferences.putString(
        "password",
        password
    );

    savedSSID =
        ssid;

    savedPassword =
        password;

    Serial.println(
        "[WIFI] Credentials saved to flash."
    );
}

// =====================================================
// CLEAR WIFI
// =====================================================

void clearWiFiCredentials() {

    preferences.remove(
        "ssid"
    );

    preferences.remove(
        "password"
    );

    savedSSID =
        "";

    savedPassword =
        "";

    Serial.println(
        "[WIFI] Saved Wi-Fi credentials erased."
    );
}

// =====================================================
// CONNECT TO SAVED WIFI
// =====================================================

bool connectSavedWiFi() {

    if (
        savedSSID.length() ==
        0
    ) {

        Serial.println(
            "[WIFI] No saved credentials."
        );

        return false;
    }

    Serial.println();
    Serial.println(
        "=========================================="
    );

    Serial.print(
        "[WIFI] Connecting to: "
    );

    Serial.println(
        savedSSID
    );

    WiFi.mode(
        WIFI_STA
    );

    // Important for ESP32 camera streaming
    WiFi.setSleep(
        false
    );

    WiFi.disconnect(
        true
    );

    delay(
        300
    );

    WiFi.begin(
        savedSSID.c_str(),
        savedPassword.c_str()
    );

    unsigned long startTime =
        millis();

    while (
        WiFi.status() !=
        WL_CONNECTED &&
        millis() -
        startTime <
        WIFI_CONNECT_TIMEOUT_MS
    ) {

        Serial.print(
            "."
        );

        delay(
            500
        );
    }

    Serial.println();

    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

        Serial.println(
            "[WIFI] CONNECTED!"
        );

        Serial.print(
            "[WIFI] SSID: "
        );

        Serial.println(
            WiFi.SSID()
        );

        Serial.print(
            "[WIFI] IP: "
        );

        Serial.println(
            WiFi.localIP()
        );

        Serial.print(
            "[WIFI] RSSI: "
        );

        Serial.print(
            WiFi.RSSI()
        );

        Serial.println(
            " dBm"
        );

        Serial.println(
            "=========================================="
        );

        return true;
    }

    Serial.println(
        "[WIFI] Connection FAILED."
    );

    return false;
}

// =====================================================
// START AP CONFIGURATION
// =====================================================

void startConfigPortal() {

    if (
        configPortalActive
    ) {

        return;
    }

    // Stop camera streaming
    cloudStreamActive =
        false;

    websocketConnected =
        false;

    Serial.println();
    Serial.println(
        "=========================================="
    );

    Serial.println(
        " WIFI CONFIGURATION MODE"
    );

    Serial.println(
        "=========================================="
    );

    // =================================================
    // DISCONNECT STATION
    // =================================================

    WiFi.disconnect(
        true
    );

    delay(
        200
    );

    // =================================================
    // START AP
    // =================================================

    WiFi.mode(
        WIFI_AP
    );

    bool apStarted =
        WiFi.softAP(
            setupAPName,
            setupAPPassword
        );

    if (
        !apStarted
    ) {

        Serial.println(
            "[AP] Failed to start AP."
        );

        return;
    }

    configPortalActive =
        true;

    Serial.print(
        "[AP] Network: "
    );

    Serial.println(
        setupAPName
    );

    Serial.print(
        "[AP] Password: "
    );

    Serial.println(
        setupAPPassword
    );

    Serial.print(
        "[AP] IP: "
    );

    Serial.println(
        WiFi.softAPIP()
    );

    // =================================================
    // WEB ROUTES
    // =================================================

    configServer.on(
        "/",
        HTTP_GET,
        handleConfigRoot
    );

    configServer.on(
        "/save",
        HTTP_POST,
        handleSaveWiFi
    );

    configServer.onNotFound(
        handleNotFound
    );

    configServer.begin();

    Serial.println(
        "[AP] Configuration web server started."
    );

    Serial.println();
    Serial.println(
        "Connect phone/laptop to:"
    );

    Serial.println(
        setupAPName
    );

    Serial.println();

    Serial.println(
        "Password:"
    );

    Serial.println(
        setupAPPassword
    );

    Serial.println();

    Serial.println(
        "Then open:"
    );

    Serial.println(
        "http://192.168.4.1"
    );

    Serial.println(
        "=========================================="
    );
}

// =====================================================
// STOP AP PORTAL
// =====================================================

void stopConfigPortal() {

    if (
        !configPortalActive
    ) {

        return;
    }

    configServer.stop();

    WiFi.softAPdisconnect(
        true
    );

    configPortalActive =
        false;
}

// =====================================================
// CONFIG ROOT
// =====================================================

void handleConfigRoot() {

    String page =
        buildWiFiSetupPage();

    configServer.send(
        200,
        "text/html",
        page
    );
}

// =====================================================
// WIFI SETUP WEB PAGE
// =====================================================

String buildWiFiSetupPage() {

    // =================================================
    // SCAN WIFI
    // =================================================

    int networkCount =
        WiFi.scanNetworks();

    String networkOptions =
        "";

    if (
        networkCount <=
        0
    ) {

        networkOptions +=
            "<option value=''>No networks found</option>";

    } else {

        for (
            int i = 0;
            i < networkCount;
            i++
        ) {

            String networkName =
                WiFi.SSID(i);

            int signal =
                WiFi.RSSI(i);

            networkOptions +=
                "<option value='" +
                networkName +
                "'>";

            networkOptions +=
                networkName;

            networkOptions +=
                " (";

            networkOptions +=
                String(
                    signal
                );

            networkOptions +=
                " dBm)";

            networkOptions +=
                "</option>";
        }
    }

    // =================================================
    // HTML
    // =================================================

    String html =
        R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta
    name="viewport"
    content="width=device-width, initial-scale=1">

<title>
FarmWiz Camera Setup
</title>

<style>

body {
    font-family: Arial, sans-serif;
    background: #f3f5f7;
    margin: 0;
    padding: 20px;
}

.card {
    max-width: 450px;
    margin: 40px auto;
    background: white;
    padding: 25px;
    border-radius: 14px;
    box-shadow: 0 4px 20px rgba(0,0,0,.12);
}

.logo {
    font-size: 42px;
    text-align: center;
}

h1 {
    text-align: center;
    font-size: 24px;
}

.subtitle {
    text-align: center;
    color: #666;
    margin-bottom: 25px;
}

label {
    font-weight: 600;
    display: block;
    margin-top: 18px;
    margin-bottom: 7px;
}

select,
input {
    width: 100%;
    box-sizing: border-box;
    padding: 12px;
    border: 1px solid #ccc;
    border-radius: 7px;
    font-size: 16px;
}

button {
    width: 100%;
    margin-top: 25px;
    padding: 13px;
    background: #2e7d32;
    color: white;
    border: none;
    border-radius: 7px;
    font-size: 16px;
    font-weight: 600;
    cursor: pointer;
}

.note {
    font-size: 12px;
    color: #777;
    margin-top: 18px;
    text-align: center;
}

</style>

</head>

<body>

<div class="card">

<div class="logo">
📷
</div>

<h1>
FarmWiz Camera
</h1>

<div class="subtitle">
Wi-Fi Configuration
</div>

<form
    method="POST"
    action="/save">

<label>
Wi-Fi Network
</label>

<select
    name="ssid"
    required>
)rawliteral";

    html +=
        networkOptions;

    html +=
        R"rawliteral(
</select>

<label>
Wi-Fi Password
</label>

<input
    name="password"
    type="password"
    placeholder="Enter Wi-Fi password">

<button
    type="submit">
Save & Connect
</button>

</form>

<div class="note">
The camera will restart after saving.
</div>

</div>

</body>

</html>
)rawliteral";

    WiFi.scanDelete();

    return html;
}

// =====================================================
// SAVE WIFI FROM AP WEB PAGE
// =====================================================

void handleSaveWiFi() {

    if (
        !configServer.hasArg(
            "ssid"
        )
    ) {

        configServer.send(
            400,
            "text/plain",
            "SSID missing"
        );

        return;
    }

    String newSSID =
        configServer.arg(
            "ssid"
        );

    String newPassword =
        configServer.arg(
            "password"
        );

    newSSID.trim();

    if (
        newSSID.length() ==
        0
    ) {

        configServer.send(
            400,
            "text/plain",
            "Invalid SSID"
        );

        return;
    }

    // =================================================
    // SAVE
    // =================================================

    saveWiFiCredentials(
        newSSID,
        newPassword
    );

    // =================================================
    // RESPONSE
    // =================================================

    String html =
        R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta
    name="viewport"
    content="width=device-width,initial-scale=1">

<style>

body {
    font-family: Arial;
    text-align: center;
    padding: 40px;
}

h2 {
    color: #2e7d32;
}

</style>

</head>

<body>

<h2>
Wi-Fi Saved!
</h2>

<p>
FarmWiz Camera is restarting...
</p>

<p>
Reconnect your phone to your normal Wi-Fi network.
</p>

</body>

</html>
)rawliteral";

    configServer.send(
        200,
        "text/html",
        html
    );

    Serial.println();
    Serial.println(
        "[WIFI] New credentials received from AP page."
    );

    Serial.print(
        "[WIFI] SSID: "
    );

    Serial.println(
        newSSID
    );

    delay(
        1500
    );

    ESP.restart();
}

// =====================================================
// CONFIG NOT FOUND
// =====================================================

void handleNotFound() {

    configServer.sendHeader(
        "Location",
        "/"
    );

    configServer.send(
        302,
        "text/plain",
        ""
    );
}

// =====================================================
// SERIAL WIFI SCANNER
// =====================================================

void scanWiFiNetworks() {

    Serial.println();
    Serial.println(
        "=========================================="
    );

    Serial.println(
        " SCANNING WIFI NETWORKS"
    );

    Serial.println(
        "=========================================="
    );

    int networkCount =
        WiFi.scanNetworks();

    if (
        networkCount <=
        0
    ) {

        Serial.println(
            "No Wi-Fi networks found."
        );

        WiFi.scanDelete();

        return;
    }

    Serial.print(
        "Networks found: "
    );

    Serial.println(
        networkCount
    );

    Serial.println();

    for (
        int i = 0;
        i < networkCount;
        i++
    ) {

        Serial.print(
            i + 1
        );

        Serial.print(
            ". "
        );

        Serial.print(
            WiFi.SSID(i)
        );

        Serial.print(
            " | "
        );

        Serial.print(
            WiFi.RSSI(i)
        );

        Serial.print(
            " dBm"
        );

        if (
            WiFi.encryptionType(i) ==
            WIFI_AUTH_OPEN
        ) {

            Serial.print(
                " | OPEN"
            );

        } else {

            Serial.print(
                " | SECURED"
            );
        }

        Serial.println();
    }

    WiFi.scanDelete();

    Serial.println();
    Serial.println(
        "=========================================="
    );
}

// =====================================================
// SERIAL CLI
// =====================================================

void handleSerialCommands() {

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
        command.length() ==
        0
    ) {

        return;
    }

    Serial.println();
    Serial.print(
        "[CLI] "
    );

    Serial.println(
        command
    );

    // =================================================
    // HELP
    // =================================================

    if (
        command.equalsIgnoreCase(
            "help"
        )
    ) {

        Serial.println();
        Serial.println(
            "=========================================="
        );

        Serial.println(
            " FARMWIZ CAMERA CLI"
        );

        Serial.println(
            "=========================================="
        );

        Serial.println();

        Serial.println(
            "status"
        );

        Serial.println(
            "  Show camera/network status"
        );

        Serial.println();

        Serial.println(
            "wifi"
        );

        Serial.println(
            "  Show saved Wi-Fi information"
        );

        Serial.println();

        Serial.println(
            "wifi scan"
        );

        Serial.println(
            "  Scan nearby Wi-Fi networks"
        );

        Serial.println();

        Serial.println(
            "wifi set SSID|PASSWORD"
        );

        Serial.println(
            "  Save new Wi-Fi and restart"
        );

        Serial.println();

        Serial.println(
            "Example:"
        );

        Serial.println(
            "wifi set IoTLabSu|MyPassword123"
        );

        Serial.println();

        Serial.println(
            "wifi ap"
        );

        Serial.println(
            "  Clear saved Wi-Fi and start AP setup"
        );

        Serial.println();

        Serial.println(
            "resetwifi"
        );

        Serial.println(
            "  Same as wifi ap"
        );

        Serial.println();

        Serial.println(
            "restart"
        );

        Serial.println(
            "  Restart ESP32"
        );

        Serial.println();

        Serial.println(
            "=========================================="
        );

        return;
    }

    // =================================================
    // STATUS
    // =================================================

    if (
        command.equalsIgnoreCase(
            "status"
        )
    ) {

        Serial.println();
        Serial.println(
            "--------- STATUS ---------"
        );

        Serial.print(
            "WiFi: "
        );

        Serial.println(
            WiFi.status() ==
            WL_CONNECTED
                ? "CONNECTED"
                : "DISCONNECTED"
        );

        Serial.print(
            "Saved SSID: "
        );

        if (
            savedSSID.length() >
            0
        ) {

            Serial.println(
                savedSSID
            );

        } else {

            Serial.println(
                "NONE"
            );
        }

        Serial.print(
            "Current SSID: "
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

        if (
            WiFi.status() ==
            WL_CONNECTED
        ) {

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
            "Config portal: "
        );

        Serial.println(
            configPortalActive
                ? "YES"
                : "NO"
        );

        Serial.print(
            "Node WebSocket: "
        );

        Serial.println(
            websocketConnected
                ? "CONNECTED"
                : "DISCONNECTED"
        );

        Serial.print(
            "Streaming: "
        );

        Serial.println(
            cloudStreamActive
                ? "YES"
                : "NO"
        );

        Serial.println(
            "--------------------------"
        );

        return;
    }

    // =================================================
    // WIFI INFO
    // =================================================

    if (
        command.equalsIgnoreCase(
            "wifi"
        )
    ) {

        Serial.println();
        Serial.println(
            "--------- WIFI ---------"
        );

        Serial.print(
            "Saved SSID: "
        );

        if (
            savedSSID.length() >
            0
        ) {

            Serial.println(
                savedSSID
            );

        } else {

            Serial.println(
                "NONE"
            );
        }

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

        Serial.println(
            "------------------------"
        );

        return;
    }

    // =================================================
    // WIFI SCAN
    // =================================================

    if (
        command.equalsIgnoreCase(
            "wifi scan"
        )
    ) {

        scanWiFiNetworks();

        return;
    }

    // =================================================
    // WIFI SET
    // =================================================
    //
    // Format:
    //
    // wifi set SSID|PASSWORD
    //
    // Example:
    //
    // wifi set IoTLabSu|MyPassword123
    //

    if (
        command.startsWith(
            "wifi set "
        )
    ) {

        String credentials =
            command.substring(
                9
            );

        credentials.trim();

        int separator =
            credentials.indexOf(
                '|'
            );

        if (
            separator ==
            -1
        ) {

            Serial.println();
            Serial.println(
                "[WIFI] Invalid format."
            );

            Serial.println(
                "Use:"
            );

            Serial.println(
                "wifi set SSID|PASSWORD"
            );

            return;
        }

        String newSSID =
            credentials.substring(
                0,
                separator
            );

        String newPassword =
            credentials.substring(
                separator + 1
            );

        newSSID.trim();

        if (
            newSSID.length() ==
            0
        ) {

            Serial.println(
                "[WIFI] SSID cannot be empty."
            );

            return;
        }

        Serial.println();
        Serial.println(
            "=========================================="
        );

        Serial.println(
            " NEW WIFI CONFIGURATION"
        );

        Serial.println(
            "=========================================="
        );

        Serial.print(
            "SSID: "
        );

        Serial.println(
            newSSID
        );

        Serial.print(
            "Password length: "
        );

        Serial.println(
            newPassword.length()
        );

        // Save using same Preferences
        // as the AP portal.
        saveWiFiCredentials(
            newSSID,
            newPassword
        );

        Serial.println();
        Serial.println(
            "[WIFI] New credentials saved."
        );

        Serial.println(
            "[WIFI] Restarting..."
        );

        delay(
            1000
        );

        ESP.restart();

        return;
    }

    // =================================================
    // FORCE AP MODE
    // =================================================

    if (
        command.equalsIgnoreCase(
            "wifi ap"
        ) ||
        command.equalsIgnoreCase(
            "resetwifi"
        )
    ) {

        Serial.println();
        Serial.println(
            "=========================================="
        );

        Serial.println(
            " RESETTING WIFI"
        );

        Serial.println(
            "=========================================="
        );

        clearWiFiCredentials();

        Serial.println(
            "[WIFI] Restarting into AP setup mode..."
        );

        Serial.println();
        Serial.print(
            "AP: "
        );

        Serial.println(
            setupAPName
        );

        Serial.print(
            "Password: "
        );

        Serial.println(
            setupAPPassword
        );

        Serial.println(
            "URL: http://192.168.4.1"
        );

        delay(
            1500
        );

        ESP.restart();

        return;
    }

    // =================================================
    // RESTART
    // =================================================

    if (
        command.equalsIgnoreCase(
            "restart"
        )
    ) {

        Serial.println(
            "[SYSTEM] Restarting..."
        );

        delay(
            1000
        );

        ESP.restart();

        return;
    }

    // =================================================
    // UNKNOWN COMMAND
    // =================================================

    Serial.println();
    Serial.print(
        "[CLI] Unknown command: "
    );

    Serial.println(
        command
    );

    Serial.println(
        "Type 'help' for available commands."
    );
}

// =====================================================
// WEBSOCKET SETUP
// =====================================================

void setupWebSocket() {

    Serial.println();
    Serial.println(
        "[WS] Starting WebSocket client..."
    );

    Serial.print(
        "[WS] Server: ws://"
    );

    Serial.print(
        nodeHost
    );

    Serial.print(
        ":"
    );

    Serial.print(
        nodePort
    );

    Serial.println(
        nodeWebSocketPath
    );

    webSocket.begin(
        nodeHost,
        nodePort,
        nodeWebSocketPath
    );

    webSocket.onEvent(
        webSocketEvent
    );

    webSocket.setReconnectInterval(
        5000
    );

    webSocket.enableHeartbeat(
        15000,
        3000,
        2
    );
}

// =====================================================
// WEBSOCKET EVENTS
// =====================================================

void webSocketEvent(
    WStype_t type,
    uint8_t* payload,
    size_t length
) {

    switch (
        type
    ) {

        // =============================================
        // DISCONNECTED
        // =============================================

        case WStype_DISCONNECTED:

            websocketConnected =
                false;

            cloudStreamActive =
                false;

            Serial.println(
                "[WS] Disconnected from Node.js."
            );

            break;

        // =============================================
        // CONNECTED
        // =============================================

        case WStype_CONNECTED:

            websocketConnected =
                true;

            Serial.println(
                "[WS] Connected to Node.js!"
            );

            sendCameraStatus();

            break;

        // =============================================
        // TEXT COMMAND
        // =============================================

        case WStype_TEXT: {

            String message =
                String(
                    (char*)payload
                ).substring(
                    0,
                    length
                );

            Serial.print(
                "[WS] Command received: "
            );

            Serial.println(
                message
            );

            StaticJsonDocument<256>
                doc;

            DeserializationError error =
                deserializeJson(
                    doc,
                    message
                );

            if (
                error
            ) {

                Serial.print(
                    "[WS] JSON error: "
                );

                Serial.println(
                    error.c_str()
                );

                return;
            }

            const char* command =
                doc[
                    "command"
                ];

            if (
                command ==
                nullptr
            ) {

                Serial.println(
                    "[WS] No command field."
                );

                return;
            }

            // =========================================
            // START STREAM
            // =========================================

            if (
                strcmp(
                    command,
                    "start_stream"
                ) ==
                0
            ) {

                cloudStreamActive =
                    true;

                lastFrameSend =
                    0;

                Serial.println();
                Serial.println(
                    "=============================="
                );

                Serial.println(
                    " CLOUD STREAM STARTED"
                );

                Serial.println(
                    "=============================="
                );

                sendCameraStatus();
            }

            // =========================================
            // STOP STREAM
            // =========================================

            else if (
                strcmp(
                    command,
                    "stop_stream"
                ) ==
                0
            ) {

                cloudStreamActive =
                    false;

                Serial.println();
                Serial.println(
                    "=============================="
                );

                Serial.println(
                    " CLOUD STREAM STOPPED"
                );

                Serial.println(
                    "=============================="
                );

                sendCameraStatus();
            }

            // =========================================
            // STATUS
            // =========================================

            else if (
                strcmp(
                    command,
                    "status"
                ) ==
                0
            ) {

                sendCameraStatus();
            }

            else {

                Serial.print(
                    "[WS] Unknown command: "
                );

                Serial.println(
                    command
                );
            }

            break;
        }

        // =============================================
        // ERROR
        // =============================================

        case WStype_ERROR:

            Serial.println(
                "[WS] WebSocket error."
            );

            break;

        // =============================================
        // PONG
        // =============================================

        case WStype_PONG:

            break;

        default:

            break;
    }
}

// =====================================================
// SEND CAMERA STATUS
// =====================================================

void sendCameraStatus() {

    if (
        !websocketConnected
    ) {

        return;
    }

    StaticJsonDocument<256>
        doc;

    doc["type"] =
        "camera_status";

    doc["cameraId"] =
        cameraId;

    doc["streaming"] =
        cloudStreamActive;

    doc["ip"] =
        WiFi.localIP().toString();

    doc["rssi"] =
        WiFi.RSSI();

    doc["freeHeap"] =
        ESP.getFreeHeap();

    doc["ssid"] =
        WiFi.SSID();

    String json;

    serializeJson(
        doc,
        json
    );

    webSocket.sendTXT(
        json
    );
}

// =====================================================
// CLOUD STREAM
// =====================================================

void handleCloudStream() {

    if (
        !cloudStreamActive
    ) {

        return;
    }

    if (
        !websocketConnected
    ) {

        return;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    ) {

        return;
    }

    unsigned long now =
        millis();

    if (
        now -
        lastFrameSend <
        frameIntervalMs
    ) {

        return;
    }

    lastFrameSend =
        now;

    // =================================================
    // CAPTURE
    // =================================================

    camera_fb_t* fb =
        esp_camera_fb_get();

    if (
        !fb
    ) {

        Serial.println(
            "[STREAM] Capture failed."
        );

        return;
    }

    // =================================================
    // VERIFY JPEG
    // =================================================

    if (
        fb->format !=
        PIXFORMAT_JPEG
    ) {

        Serial.println(
            "[STREAM] Frame is not JPEG."
        );

        esp_camera_fb_return(
            fb
        );

        return;
    }

    // =================================================
    // SEND RAW JPEG
    // =================================================

    bool sent =
        webSocket.sendBIN(
            fb->buf,
            fb->len
        );

    if (
        sent
    ) {

        frameCounter++;

        byteCounter +=
            fb->len;

    } else {

        Serial.println(
            "[STREAM] WebSocket send failed."
        );
    }

    // ALWAYS return frame buffer
    esp_camera_fb_return(
        fb
    );
}

// =====================================================
// DEBUG STATUS
// =====================================================

void printStats() {

    unsigned long now =
        millis();

    if (
        now -
        lastStatsPrint <
        5000
    ) {

        return;
    }

    lastStatsPrint =
        now;

    Serial.println();
    Serial.println(
        "--------- STATUS ---------"
    );

    Serial.print(
        "WiFi: "
    );

    Serial.println(
        WiFi.status() ==
        WL_CONNECTED
            ? "CONNECTED"
            : "DISCONNECTED"
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
        "Node WS: "
    );

    Serial.println(
        websocketConnected
            ? "CONNECTED"
            : "DISCONNECTED"
    );

    Serial.print(
        "Streaming: "
    );

    Serial.println(
        cloudStreamActive
            ? "YES"
            : "NO"
    );

    Serial.print(
        "Frames sent: "
    );

    Serial.println(
        frameCounter
    );

    Serial.print(
        "Data sent: "
    );

    Serial.print(
        byteCounter /
        1024.0
    );

    Serial.println(
        " KB"
    );

    Serial.print(
        "Free heap: "
    );

    Serial.println(
        ESP.getFreeHeap()
    );

    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

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

    Serial.println(
        "--------------------------"
    );
}
