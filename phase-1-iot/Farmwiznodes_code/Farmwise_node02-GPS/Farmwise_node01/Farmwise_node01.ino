#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <Adafruit_Sensor.h>
#include <DHT.h>

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <TinyGPS++.h>

#include "esp_sleep.h"

// ======================================================
// DEVICE
// ======================================================

const char* deviceId = "FarmWiz-Node02";
const char* plantId  = "Antroewa";

// ======================================================
// WIFI
// ======================================================

const char* ssid =
    "HUAWEI-B310-BD18";

const char* password =
    "ELHF8D5QTYF";

// ======================================================
// THINGSBOARD
// ======================================================
//
// Web dashboard:
// http://168.195.218.199:8081
//
// ESP does NOT connect to port 8081 for MQTT.
// MQTT uses port 1883.
//

const char* mqtt_broker =
    "168.195.218.199";

const int mqtt_port =
    1883;

const char* mqtt_topic =
    "v1/devices/me/telemetry";

// ThingsBoard Device Access Token
const char* mqtt_User =
    "npjgzz0xjabfi8w8mt6g";

const char* mqtt_Password =
    "";

// ======================================================
// MQTT CLIENT
// ======================================================

WiFiClient espClient;
PubSubClient client(espClient);

// ======================================================
// SENSOR PINS
// ======================================================

#define POWER_CTRL 4

#define BAT_ADC 33

#define SALT_PIN 27

#define SOIL_PIN 32

#define DHTPIN 16

#define LDR_PIN 34

#define LED_PIN 17

// ======================================================
// DHT11
// ======================================================

#define DHTTYPE DHT11

DHT dht(
    DHTPIN,
    DHTTYPE
);

// ======================================================
// DS18B20 SOIL TEMPERATURE
// ======================================================

#define SOIL_TEMP_PIN 23

OneWire oneWire(
    SOIL_TEMP_PIN
);

DallasTemperature tempSensor(
    &oneWire
);

// ======================================================
// LCD
// ======================================================

LiquidCrystal_I2C lcd(
    0x27,
    16,
    2
);

// ======================================================
// GPS
// ======================================================

const int RXPin =
    13;

const int TXPin =
    12;

const int GPSBaud =
    9600;

TinyGPSPlus gps;

HardwareSerial gpsSerial(2);

// ======================================================
// SENSOR VALUES
// ======================================================

float atmTemp = 0.0;
float atmHum = 0.0;

uint16_t soilMoisture = 0;

uint32_t soilSalt = 0;

float soilTemperature = 0.0;

float batteryLife = 0.0;

uint16_t lightLevel = 0;

// GPS
float latitude = 0.0;
float longitude = 0.0;

bool gpsFix = false;

// ======================================================
// DEEP SLEEP
// ======================================================

const uint64_t uS_TO_S_FACTOR =
    1000000ULL;

const uint64_t TIME_TO_SLEEP =
    60;

// ======================================================
// FUNCTIONS
// ======================================================

void connectWiFi();

bool connectMQTT();

void readSensors();

uint16_t readSoil();

uint32_t readSalt();

uint16_t readLight();

float readBattery();

void readGPS();

void updateLCD();

bool sendToThingsBoard();

// ======================================================
// SETUP
// ======================================================

void setup() {

    Serial.begin(
        115200
    );

    delay(
        500
    );

    Serial.println();
    Serial.println(
        "=================================="
    );

    Serial.println(
        "     FARMWIZ NODE02 STARTING"
    );

    Serial.println(
        "=================================="
    );

    // ==================================================
    // PINS
    // ==================================================

    pinMode(
        POWER_CTRL,
        OUTPUT
    );

    digitalWrite(
        POWER_CTRL,
        HIGH
    );

    pinMode(
        LED_PIN,
        OUTPUT
    );

    digitalWrite(
        LED_PIN,
        LOW
    );

    pinMode(
        LDR_PIN,
        INPUT
    );

    // ==================================================
    // ADC
    // ==================================================

    analogReadResolution(
        12
    );

    analogSetAttenuation(
        ADC_11db
    );

    // ==================================================
    // I2C
    // ==================================================

    Wire.begin();

    // ==================================================
    // LCD
    // ==================================================

    lcd.init();

    lcd.backlight();

    lcd.clear();

    lcd.setCursor(
        0,
        0
    );

    lcd.print(
        "FarmWiz Node02"
    );

    lcd.setCursor(
        0,
        1
    );

    lcd.print(
        "Starting..."
    );

    Serial.println(
        "[LCD] Initialized"
    );

    delay(
        1500
    );

    // ==================================================
    // DHT
    // ==================================================

    dht.begin();

    Serial.println(
        "[DHT] Initialized"
    );

    // ==================================================
    // DS18B20
    // ==================================================

    tempSensor.begin();

    Serial.println(
        "[DS18B20] Initialized"
    );

    // ==================================================
    // GPS
    // ==================================================

    gpsSerial.begin(
        GPSBaud,
        SERIAL_8N1,
        RXPin,
        TXPin
    );

    Serial.println(
        "[GPS] Initialized"
    );

    // ==================================================
    // WIFI
    // ==================================================

    connectWiFi();

    // ==================================================
    // MQTT
    // ==================================================

    client.setServer(
        mqtt_broker,
        mqtt_port
    );

    client.setBufferSize(
        1024
    );

    // ==================================================
    // DEEP SLEEP
    // ==================================================

    esp_sleep_enable_timer_wakeup(
        TIME_TO_SLEEP *
        uS_TO_S_FACTOR
    );

    // ==================================================
    // READ + SEND
    // ==================================================

    Serial.println();
    Serial.println(
        "Starting measurement..."
    );

    readSensors();

    updateLCD();

    bool sent =
        sendToThingsBoard();

    // ==================================================
    // RESULT
    // ==================================================

    if (sent) {

        Serial.println();
        Serial.println(
            "DATA SENT SUCCESSFULLY"
        );

    } else {

        Serial.println();
        Serial.println(
            "DATA SEND FAILED"
        );
    }

    // Give MQTT time to finish.
    client.loop();

    delay(
        1000
    );

    // ==================================================
    // DEEP SLEEP
    // ==================================================

    Serial.println();
    Serial.print(
        "Sleeping for "
    );

    Serial.print(
        TIME_TO_SLEEP
    );

    Serial.println(
        " seconds..."
    );

    Serial.flush();

    esp_deep_sleep_start();
}

// ======================================================
// LOOP
// ======================================================
//
// Normally this will never run because the ESP
// goes into deep sleep at the end of setup().
//

void loop() {

    client.loop();

    delay(
        10
    );
}

// ======================================================
// WIFI
// ======================================================

void connectWiFi() {

    Serial.println();
    Serial.println(
        "=================================="
    );

    Serial.println(
        "CONNECTING TO WIFI"
    );

    Serial.println(
        "=================================="
    );

    lcd.clear();

    lcd.setCursor(
        0,
        0
    );

    lcd.print(
        "Connecting WiFi"
    );

    WiFi.mode(
        WIFI_STA
    );

    WiFi.begin(
        ssid,
        password
    );

    int attempts =
        0;

    while (
        WiFi.status() !=
            WL_CONNECTED &&
        attempts < 40
    ) {

        delay(
            500
        );

        Serial.print(
            "."
        );

        attempts++;
    }

    Serial.println();

    if (
        WiFi.status() ==
        WL_CONNECTED
    ) {

        Serial.println(
            "WiFi CONNECTED!"
        );

        Serial.print(
            "IP: "
        );

        Serial.println(
            WiFi.localIP()
        );

        Serial.print(
            "RSSI: "
        );

        Serial.println(
            WiFi.RSSI()
        );

        lcd.clear();

        lcd.setCursor(
            0,
            0
        );

        lcd.print(
            "WiFi Connected"
        );

        lcd.setCursor(
            0,
            1
        );

        lcd.print(
            WiFi.localIP()
        );

        delay(
            1000
        );

    } else {

        Serial.println(
            "WIFI CONNECTION FAILED!"
        );

        lcd.clear();

        lcd.setCursor(
            0,
            0
        );

        lcd.print(
            "WiFi Failed"
        );

        lcd.setCursor(
            0,
            1
        );

        lcd.print(
            "Check network"
        );

        delay(
            2000
        );
    }
}

// ======================================================
// MQTT CONNECTION
// ======================================================

bool connectMQTT() {

    if (
        client.connected()
    ) {

        return true;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    ) {

        Serial.println(
            "[MQTT] WiFi not connected"
        );

        return false;
    }

    Serial.println();
    Serial.println(
        "=================================="
    );

    Serial.println(
        "CONNECTING TO THINGSBOARD"
    );

    Serial.println(
        "=================================="
    );

    Serial.print(
        "Broker: "
    );

    Serial.println(
        mqtt_broker
    );

    Serial.print(
        "Port: "
    );

    Serial.println(
        mqtt_port
    );

    Serial.print(
        "Client ID: "
    );

    Serial.println(
        deviceId
    );

    Serial.print(
        "Topic: "
    );

    Serial.println(
        mqtt_topic
    );

    bool connected =
        client.connect(
            deviceId,
            mqtt_User,
            mqtt_Password
        );

    if (
        connected
    ) {

        Serial.println(
            "MQTT CONNECTED!"
        );

        return true;

    } else {

        Serial.print(
            "MQTT CONNECTION FAILED!"
        );

        Serial.print(
            " State = "
        );

        Serial.println(
            client.state()
        );

        return false;
    }
}

// ======================================================
// READ ALL SENSORS
// ======================================================

void readSensors() {

    Serial.println();
    Serial.println(
        "=================================="
    );

    Serial.println(
        "READING SENSORS"
    );

    Serial.println(
        "=================================="
    );

    // ==================================================
    // DHT11
    // ==================================================

    atmTemp =
        dht.readTemperature();

    atmHum =
        dht.readHumidity();

    if (
        isnan(atmTemp)
    ) {

        Serial.println(
            "[DHT] Temperature read failed"
        );

        atmTemp =
            0.0;
    }

    if (
        isnan(atmHum)
    ) {

        Serial.println(
            "[DHT] Humidity read failed"
        );

        atmHum =
            0.0;
    }

    // ==================================================
    // SOIL MOISTURE
    // ==================================================

    soilMoisture =
        readSoil();

    // ==================================================
    // SALT
    // ==================================================

    soilSalt =
        readSalt();

    // ==================================================
    // LIGHT
    // ==================================================

    lightLevel =
        readLight();

    // ==================================================
    // BATTERY
    // ==================================================

    batteryLife =
        readBattery();

    // ==================================================
    // DS18B20
    // ==================================================

    tempSensor.requestTemperatures();

    soilTemperature =
        tempSensor.getTempCByIndex(
            0
        );

    if (
        soilTemperature ==
        DEVICE_DISCONNECTED_C
    ) {

        Serial.println(
            "[DS18B20] Sensor disconnected"
        );

        soilTemperature =
            0.0;
    }

    // ==================================================
    // GPS
    // ==================================================

    readGPS();

    // ==================================================
    // PRINT EVERYTHING
    // ==================================================

    Serial.println();
    Serial.println(
        "---------- READINGS ----------"
    );

    Serial.print(
        "Air Temperature : "
    );

    Serial.print(
        atmTemp,
        1
    );

    Serial.println(
        " C"
    );

    Serial.print(
        "Humidity        : "
    );

    Serial.print(
        atmHum,
        1
    );

    Serial.println(
        " %"
    );

    Serial.print(
        "Soil Moisture   : "
    );

    Serial.print(
        soilMoisture
    );

    Serial.println(
        " %"
    );

    Serial.print(
        "Soil Salt ADC   : "
    );

    Serial.println(
        soilSalt
    );

    Serial.print(
        "Soil Temperature: "
    );

    Serial.print(
        soilTemperature,
        1
    );

    Serial.println(
        " C"
    );

    Serial.print(
        "Light           : "
    );

    Serial.print(
        lightLevel
    );

    Serial.println(
        " %"
    );

    Serial.print(
        "Battery         : "
    );

    Serial.print(
        batteryLife,
        1
    );

    Serial.println(
        " %"
    );

    Serial.print(
        "GPS Fix         : "
    );

    Serial.println(
        gpsFix
            ? "YES"
            : "NO"
    );

    Serial.print(
        "Latitude        : "
    );

    Serial.println(
        latitude,
        6
    );

    Serial.print(
        "Longitude       : "
    );

    Serial.println(
        longitude,
        6
    );

    Serial.println(
        "------------------------------"
    );
}

// ======================================================
// SOIL MOISTURE
// ======================================================

uint16_t readSoil() {

    const int samples =
        10;

    uint32_t total =
        0;

    for (
        int i = 0;
        i < samples;
        i++
    ) {

        total +=
            analogRead(
                SOIL_PIN
            );

        delay(
            10
        );
    }

    uint16_t raw =
        total /
        samples;

    Serial.print(
        "[SOIL] Raw ADC: "
    );

    Serial.println(
        raw
    );

    // ----------------------------------------------
    // CALIBRATION
    // ----------------------------------------------
    //
    // You should eventually replace these with
    // your measured dry/wet ADC values.
    //

    const int dryValue =
        4095;

    const int wetValue =
        0;

    int percentage =
        map(
            raw,
            dryValue,
            wetValue,
            0,
            100
        );

    percentage =
        constrain(
            percentage,
            0,
            100
        );

    return
        (uint16_t)percentage;
}

// ======================================================
// SALT SENSOR
// ======================================================

uint32_t readSalt() {

    const int samples =
        50;

    uint32_t total =
        0;

    for (
        int i = 0;
        i < samples;
        i++
    ) {

        total +=
            analogRead(
                SALT_PIN
            );

        delay(
            3
        );
    }

    uint32_t average =
        total /
        samples;

    Serial.print(
        "[SALT] ADC: "
    );

    Serial.println(
        average
    );

    return average;
}

// ======================================================
// LIGHT SENSOR
// ======================================================

uint16_t readLight() {

    const int samples =
        10;

    uint32_t total =
        0;

    for (
        int i = 0;
        i < samples;
        i++
    ) {

        total +=
            analogRead(
                LDR_PIN
            );

        delay(
            5
        );
    }

    int raw =
        total /
        samples;

    Serial.print(
        "[LIGHT] Raw ADC: "
    );

    Serial.println(
        raw
    );

    // Change this calibration later
    // based on your actual LDR.

    int percentage =
        map(
            raw,
            0,
            2000,
            0,
            100
        );

    percentage =
        constrain(
            percentage,
            0,
            100
        );

    return
        (uint16_t)percentage;
}

// ======================================================
// BATTERY
// ======================================================

float readBattery() {

    // ----------------------------------------------
    // CURRENT BEHAVIOR
    // ----------------------------------------------
    //
    // Your original sketch returned a fixed 95.
    //
    // Keeping it this way until the BAT_ADC
    // voltage divider is calibrated.
    //

    return 95.0;
}

// ======================================================
// GPS
// ======================================================

void readGPS() {

    Serial.println();
    Serial.println(
        "[GPS] Looking for fix..."
    );

    gpsFix =
        false;

    // Keep previous values at zero
    // unless a valid fix is obtained.

    latitude =
        0.0;

    longitude =
        0.0;

    unsigned long startTime =
        millis();

    // Give GPS up to 3 seconds to provide data.

    while (
        millis() -
        startTime <
        3000
    ) {

        while (
            gpsSerial.available() >
            0
        ) {

            char gpsChar =
                gpsSerial.read();

            gps.encode(
                gpsChar
            );
        }

        if (
            gps.location.isValid() &&
            gps.location.isUpdated()
        ) {

            latitude =
                gps.location.lat();

            longitude =
                gps.location.lng();

            gpsFix =
                true;

            break;
        }

        delay(
            10
        );
    }

    if (
        gpsFix
    ) {

        Serial.println(
            "[GPS] FIX FOUND"
        );

        Serial.print(
            "[GPS] Latitude: "
        );

        Serial.println(
            latitude,
            6
        );

        Serial.print(
            "[GPS] Longitude: "
        );

        Serial.println(
            longitude,
            6
        );

    } else {

        Serial.println(
            "[GPS] No fix"
        );

        Serial.print(
            "[GPS] Characters processed: "
        );

        Serial.println(
            gps.charsProcessed()
        );
    }
}

// ======================================================
// LCD
// ======================================================

void updateLCD() {

    lcd.clear();

    // ==================================================
    // LINE 1
    //
    // Example:
    //
    // T:28.5 H:76%
    // ==================================================

    lcd.setCursor(
        0,
        0
    );

    lcd.print(
        "T:"
    );

    lcd.print(
        atmTemp,
        1
    );

    lcd.print(
        " H:"
    );

    lcd.print(
        atmHum,
        0
    );

    lcd.print(
        "%"
    );

    // ==================================================
    // LINE 2
    //
    // Example:
    //
    // S:65 ST:27.4
    // ==================================================

    lcd.setCursor(
        0,
        1
    );

    lcd.print(
        "S:"
    );

    lcd.print(
        soilMoisture
    );

    lcd.print(
        " ST:"
    );

    lcd.print(
        soilTemperature,
        1
    );

    Serial.println();
    Serial.println(
        "[LCD] Sensor values displayed"
    );
}

// ======================================================
// SEND FLAT JSON TO THINGSBOARD
// ======================================================

bool sendToThingsBoard() {

    Serial.println();
    Serial.println(
        "=================================="
    );

    Serial.println(
        "THINGSBOARD TELEMETRY"
    );

    Serial.println(
        "=================================="
    );

    // ==================================================
    // CONNECT MQTT
    // ==================================================

    if (
        !connectMQTT()
    ) {

        Serial.println(
            "Cannot send telemetry."
        );

        return false;
    }

    // ==================================================
    // STRICTLY FLAT JSON
    // ==================================================
    //
    // IMPORTANT:
    //
    // There are NO nested objects.
    // There are NO arrays.
    //
    // Every telemetry key exists directly
    // at the top level.
    //

    StaticJsonDocument<512> doc;

    doc["deviceId"] =
        deviceId;

    doc["plantId"] =
        plantId;

    doc["atmtemp"] =
        atmTemp;

    doc["atmhum"] =
        atmHum;

    doc["soilmoisture"] =
        soilMoisture;

    doc["soilsalt"] =
        soilSalt;

    doc["soiltemp"] =
        soilTemperature;

    doc["battlife"] =
        batteryLife;

    doc["light"] =
        lightLevel;

    // GPS is ALSO flat.
    //
    // We send the keys even if there is no fix.
    // Without a fix they will be 0.

    doc["latitude"] =
        latitude;

    doc["longitude"] =
        longitude;

    // Optional GPS fix indicator.
    //
    // 1 = GPS fix
    // 0 = no GPS fix

    doc["gpsfix"] =
        gpsFix ? 1 : 0;

    // ==================================================
    // SERIALIZE
    // ==================================================

    String payload;

    serializeJson(
        doc,
        payload
    );

    // ==================================================
    // SHOW EXACT JSON
    // ==================================================

    Serial.println();
    Serial.println(
        "FLAT JSON:"
    );

    Serial.println(
        payload
    );

    Serial.println();

    Serial.print(
        "Topic: "
    );

    Serial.println(
        mqtt_topic
    );

    // ==================================================
    // PUBLISH
    // ==================================================

    bool published =
        client.publish(
            mqtt_topic,
            payload.c_str()
        );

    // Process MQTT immediately.
    client.loop();

    delay(
        500
    );

    if (
        published
    ) {

        Serial.println();
        Serial.println(
            ">>> TELEMETRY PUBLISHED <<<"
        );

        return true;

    } else {

        Serial.println();
        Serial.println(
            ">>> MQTT PUBLISH FAILED <<<"
        );

        Serial.print(
            "MQTT state: "
        );

        Serial.println(
            client.state()
        );

        return false;
    }
}
