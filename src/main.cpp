#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoWebSockets.h>

using namespace websockets;

#define HWATER_PIN  34
#define CWATER_PIN  35
#define SDATA_PIN   25
#define STORE_PIN   17
#define CLOCK_PIN   16

uint8_t SHIFTREG = 0x00;
HTTPClient http_client;
WebServer http_server;
WebsocketsClient ws_client;
static DynamicJsonDocument doc(2048);

const char* wpa_conf_path = "/wpa_supplicants.conf";
static bool wifi_sta_connection = false;
bool hot_water_signal = 0, cool_water_signal = 0;
uint32_t hot_water_occ = 0, cool_water_occ = 0;
uint8_t k_hot_water = 0, k_cool_water = 0;

String configNames[] = {
  "ssid_sta",
  "pwd_sta",
  "ssid_ap",
  "pwd_ap",
  "login_ap",
  "parol_ap",
  "device_token",
  "device_name",
  "ws_url"
};

String configValues[] = {
  "ScrollHouse_1234",
  "12345678",
  "ScrollHouse_1234",
  "",
  "Admin",
  "12345",
  "9B5QAr3JTtEr2eJbATs7f15zYpbOllz8",
  "ScrollHouse",
  "ws://85.209.88.209/ws/device/"
};

struct Switch {
  const char * name;
  uint8_t id;
  boolean stat;
  Switch(const char* name, uint8_t id, bool status) {
    this->name = name;
    this->id = id;
    this->stat = status;
  }
  String getName () {
    return String(this->name);
  }
  uint8_t getId () {
    return this->id;
  }
  bool getStatus () {
    return this->stat;
  }
  String getJsonData () {
    char temp[200];
    sprintf(temp, "{\"name\":%s,\"id\":%d,\"status\":%d}", this->name, this->id, this->stat);
    return String(temp);
  }
};


String getConfigValue (String name) {
  int index = -1;
  for (int i = 0; i < 9; i++) {
    if (configNames[i] == name) {
      index = i;
      break;
    }
  }
  if (index >= 0) return configValues[index];
  else return "";
}

void handleSetWiFi () {
  if (http_server.authenticate(getConfigValue("login_ap").c_str(), getConfigValue("parol_ap").c_str())) {
    if (http_server.args() > 0) {
      for (int i = 0; i < http_server.args(); i++) {
        if (http_server.hasArg(configNames[i])) {
          configValues[i] = http_server.arg(configNames[i]);
        }
        Serial.println(http_server.arg(i));
      }
      File f = SPIFFS.open(wpa_conf_path, "w");
      for (int i = 0; i < 9; i++) {
        f.printf("%s=%s\n", configNames[i].c_str(), configValues[i].c_str());
      }
      f.close();
      http_server.send(200, "application/json", "{\"message\":\"OK\"}");
      esp_restart();
    } else {
      http_server.send(400, "application/json", "{\"message\":\"Bad Request\"}");
    }
  } else http_server.send(401, "application/json", "{\"message\":\"Unauthorized\"}");
}

void handleGetConfig () {
  if (http_server.authenticate(getConfigValue("login_ap").c_str(), getConfigValue("parol_ap").c_str())) {
    char temp[200];
    sprintf(temp, "{\"token\":\"%s\",\"name\":\"%s\"}", getConfigValue("device_token").c_str(), getConfigValue("device_name").c_str());
    http_server.send(200, "application/json", temp);
  } else http_server.send(401, "application/json", "{\"message\":\"Unauthorized\"}");
}

void onEventsCallback(WebsocketsEvent event, String data) {
    if(event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Connnection Opened");
    } else if(event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connnection Closed");
    } else if(event == WebsocketsEvent::GotPing) {
        Serial.println("Got a Ping!");
    } else if(event == WebsocketsEvent::GotPong) {
        Serial.println("Got a Pong!");
    }
}

void readWaterSensors (bool save) {
  bool regchange = 0;
  if (digitalRead(HWATER_PIN) != hot_water_signal) {
    hot_water_signal = digitalRead(HWATER_PIN);
    k_hot_water++;
    regchange = 1;
  }
  if (k_hot_water >= 20) {
    hot_water_occ ++;
    k_hot_water = 0;
  }
  if (digitalRead(CWATER_PIN) != cool_water_signal) {
    cool_water_signal = digitalRead(CWATER_PIN);
    k_cool_water++;
    regchange = 1;
  }
  if (k_cool_water >= 20) {
    cool_water_occ ++;
    k_cool_water = 0;
  }
  if (regchange && save) {
    // Serial.printf("Hot water data: %.2f m^3\n", float(hot_water_occ + k_hot_water/20.0));
    File f = SPIFFS.open("/conf.wtr", "w");
    f.printf("%d,%d,%d,%d\n", k_hot_water, k_cool_water, hot_water_occ, cool_water_occ);
    f.close();
  }
}

void relayWrite(uint8_t relay_pin, bool relay_stat) {
  if (relay_pin < 1) relay_pin = 1;
  if (relay_stat) SHIFTREG = SHIFTREG | (1 << (relay_pin - 1));
  else SHIFTREG = SHIFTREG & (0xFF ^ (1 << (relay_pin - 1))); 
  // Serial.println(SHIFTREG, BIN);
  digitalWrite(STORE_PIN, 0);
  shiftOut(SDATA_PIN, CLOCK_PIN, MSBFIRST, SHIFTREG);
  digitalWrite(STORE_PIN, 1);
}

void setup() {
  pinMode(HWATER_PIN, INPUT_PULLUP);
  pinMode(CWATER_PIN, INPUT_PULLUP);
  pinMode(SDATA_PIN, OUTPUT);
  pinMode(STORE_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  Serial.begin(115200);
  if(!SPIFFS.begin(true)){
      Serial.println("SPIFFS Mount Failed");
      return;
  }
  Serial.println("\n\nConfiguration: ");
  if (SPIFFS.exists(wpa_conf_path)) {
    File f = SPIFFS.open(wpa_conf_path, "r");
    int k = 0;
    while (f.available()) {
      String str = f.readStringUntil('\n');
      int equalIndex = str.indexOf("=");
      configNames[k] = str.substring(0, equalIndex);
      configValues[k] = str.substring(equalIndex + 1);
      Serial.printf("%s=%s\n", configNames[k].c_str(), configValues[k].c_str());
      k++;
    }
  } else {
    for (int i = 0; i < 9; i++) {
      Serial.printf("%s=%s\n", configNames[i].c_str(), configValues[i].c_str());
    }
  }
  Serial.printf("\n\nConnect to:\tSSID=%s Password=%s\n", getConfigValue("ssid_sta").c_str(), getConfigValue("pwd_sta").c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(getConfigValue("ssid_sta").c_str(), getConfigValue("pwd_sta").c_str());
  Serial.print("Wait for Wifi connection: ...");
  uint8_t kw = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wifi_sta_connection = true;
    kw++;
    if (kw >= 20) {
      wifi_sta_connection = false;
      Serial.println(" close");
      break;
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP); 
    WiFi.softAP(getConfigValue("ssid_ap").c_str(), getConfigValue("pwd_ap").length() >= 8 ? getConfigValue("pwd_ap").c_str() : NULL);
    Serial.printf("\n\nWiFi hotspot started name: %s , pwd: %s\n\n", getConfigValue("ssid_ap").c_str(), getConfigValue("pwd_ap").c_str());
    http_server.on("/set-config", handleSetWiFi);
    http_server.on("/get-config", handleGetConfig);
    http_server.begin();
    Serial.printf("\nAuthorization: Login=%s Parol=%s\n",getConfigValue("login_ap").c_str(), getConfigValue("parol_ap").c_str());
  }
  
  if (wifi_sta_connection) {
    Serial.println("\n\nBegin websocket client");
    ws_client.onMessage([&](WebsocketsMessage message) {
        Serial.println(message.data());
        if (!deserializeJson(doc, message.data())) {
          Serial.println((const char*) doc[0]);
          // JsonObject obj = doc.as<JsonObject>();
          // if (obj[String("enabled")] == false) {
          // } else if (obj[String("enabled")] == true) {
          // }
        }
      }
    );
    ws_client.onEvent(onEventsCallback);
    Serial.println("WebSocket connecting...");
    if (ws_client.connect(getConfigValue("ws_url") + getConfigValue("device_token"))) {
      Serial.println("WebSocket is connected...");
    } else {
      Serial.println("WebSocket connection is failed!!!");
    }
  }
}
uint32_t btn_time = 0;
bool btn_stat = 0;
void loop() {
  if (!digitalRead(0) && !btn_stat) {
    btn_time = millis();
    btn_stat = 1;
  }
  if (digitalRead(0)) btn_stat = 0;
  if (btn_stat) {
    if (millis() - btn_time >= 5000) {
      Serial.println("Factory reset");
      SPIFFS.format();
      esp_restart();
    }
  }
  http_server.handleClient();
  if (wifi_sta_connection) {
    if (ws_client.available()) ws_client.poll();
  }
  delay(50);
}
