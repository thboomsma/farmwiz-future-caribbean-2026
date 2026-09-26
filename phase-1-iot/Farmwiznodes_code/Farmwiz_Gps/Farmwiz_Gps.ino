#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <TinyGPS++.h>

const char* ssid="HUAWEI-2.4G-bF2k";
const char* password="DuuBnx2S";
const char* mqtt_broker="168.195.218.199";
const int mqtt_port=1883;
const char* mqtt_topic="v1/devices/me/telemetry";
const char* mqtt_user="npjgzz0xjabfi8w8mt6g";
const char* deviceId="FarmWiz-Node02";
const char* plantId="Antroewa";
const uint8_t masterMac[6]={0xC0,0x49,0xEF,0x68,0x82,0x30};
const uint8_t OFFLINE_CHANNEL=6;
const int DHTPIN=16,DHTTYPE=DHT11,SOIL_PIN=32,SALT_PIN=34,LDR_PIN=27,BAT_ADC=33,ONEWIRE_PIN=23,POWER_CTRL=4;
DHT dht(DHTPIN,DHTTYPE); OneWire oneWire(ONEWIRE_PIN); DallasTemperature soilTemp(&oneWire);
TinyGPSPlus gps; HardwareSerial gpsSerial(2);
WiFiClient net; PubSubClient mqtt(net);
enum Mode{ONLINE,OFFLINE}; Mode mode=ONLINE; bool espNowReady=false; unsigned long lastSend=0;

void setupEspNow(){
  WiFi.mode(WIFI_STA); esp_wifi_set_promiscuous(true); esp_wifi_set_channel(OFFLINE_CHANNEL,WIFI_SECOND_CHAN_NONE); esp_wifi_set_promiscuous(false);
  if(espNowReady)return; esp_now_deinit(); delay(50); if(esp_now_init()!=ESP_OK){Serial.println("ESP-NOW init failed");return;} esp_now_peer_info_t p{}; memcpy(p.peer_addr,masterMac,6); p.channel=OFFLINE_CHANNEL; p.encrypt=false;
  if(esp_now_add_peer(&p)==ESP_OK || esp_now_is_peer_exist(masterMac)){espNowReady=true; Serial.println("ESP-NOW ready channel 6");}
}
void enterOffline(){mqtt.disconnect(); WiFi.disconnect(true); setupEspNow(); mode=OFFLINE; Serial.println("OFFLINE MODE");}
void tryOnline(){mode=ONLINE; WiFi.mode(WIFI_STA); WiFi.begin(ssid,password); unsigned long t=millis(); while(WiFi.status()!=WL_CONNECTED&&millis()-t<15000)delay(250); if(WiFi.status()!=WL_CONNECTED){enterOffline();return;} mqtt.setServer(mqtt_broker,mqtt_port); Serial.println("ONLINE WIFI READY");}
float soil(){
  uint16_t raw=analogRead(SOIL_PIN);
  uint16_t value=map(raw,2320,3443,100,0);
  if(value<50 && raw>3400)value+=10;
  return constrain(value,0,100);
}
float light(){return constrain(map(analogRead(LDR_PIN),0,2000,0,100),0,100);}
uint32_t readSalt(){
  uint16_t samples[120]; uint32_t total=0;
  for(int i=0;i<120;i++){samples[i]=analogRead(SALT_PIN);delay(2);}
  std::sort(samples,samples+120);
  for(int i=1;i<119;i++) total+=samples[i];
  uint32_t raw=total/118;
  return constrain((long)map(raw,180,3000,0,1000),0,5000);
}
float battery(){ return 95.0; }
void readGPS(){
  while(gpsSerial.available()) gps.encode(gpsSerial.read());
}
float readDhtTemp(){for(int i=0;i<5;i++){float v=dht.readTemperature();if(!isnan(v))return v;delay(500);}return 0;}
float readDhtHum(){for(int i=0;i<5;i++){float v=dht.readHumidity();if(!isnan(v))return v;delay(500);}return 0;}
void sendTelemetry(){
  readGPS(); float t=readDhtTemp(); float h=readDhtHum();
  uint16_t ldrRaw=analogRead(LDR_PIN);
  StaticJsonDocument<512> j; j["deviceId"]=deviceId; j["plantId"]=plantId; j["temp"]=t; j["hum"]=h; j["soilMoisture"]=soil(); j["soilsalt"]=readSalt(); j["light"]=light(); j["ldrADC"]=ldrRaw; j["battlife"]=battery(); j["gpsFix"]=gps.location.isValid(); if(gps.location.isValid()){j["latitude"]=gps.location.lat(); j["longitude"]=gps.location.lng();} j["timestamp"]=millis();
  char out[384]; size_t n=serializeJson(j,out,sizeof(out)); if(!n)return;
  if(mode==ONLINE){ if(!mqtt.connected())mqtt.connect(deviceId,mqtt_user,""); if(mqtt.connected()&&mqtt.publish(mqtt_topic,out))Serial.println("ThingsBoard telemetry delivered"); }
  else { if(!espNowReady)setupEspNow(); if(esp_now_send(masterMac,(uint8_t*)out,n)==ESP_OK)Serial.printf("ESP-NOW telemetry queued: %s\n",out); }
}
void setup(){Serial.begin(115200); pinMode(POWER_CTRL,OUTPUT); digitalWrite(POWER_CTRL,HIGH); dht.begin(); soilTemp.begin(); gpsSerial.begin(9600,SERIAL_8N1,13,12); delay(2000); tryOnline();}
void loop(){if(Serial.available()){String c=Serial.readStringUntil('\n');c.trim();c.toLowerCase();if(c=="offline")enterOffline();else if(c=="online")tryOnline();else if(c=="status")Serial.printf("Device=%s Mode=%s WiFi=%s MQTT=%s ESP-NOW=%s MAC=24:6F:28:9E:80:70\n",deviceId,mode==ONLINE?"ONLINE":"OFFLINE",WiFi.status()==WL_CONNECTED?"connected":"disconnected",mqtt.connected()?"connected":"disconnected",espNowReady?"ready":"off");}
  if(mode==ONLINE&&WiFi.status()!=WL_CONNECTED){enterOffline();} if(mode==ONLINE)mqtt.loop(); if(millis()-lastSend>5000){lastSend=millis();sendTelemetry();} delay(20);}
