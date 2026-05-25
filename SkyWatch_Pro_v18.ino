/***************************************************************************************
  SKYWATCH PRO - v17.0 (WEBSOCKET RELAY EDITION)
  -----------------------------------------------
  File layout — all four files must be in the same sketch folder:
    SkyWatch_Pro_v17.ino   ← this file
    skywatch_globals.h     ← pin defines + EEPROM addresses + extern declarations
    dashboard.h            ← PROGMEM dashboard HTML
    setup_page.h           ← AP-mode WiFi scanner setup page
    ws_tunnel.h            ← WebSocket tunnel client

  Required libraries (Arduino Library Manager):
    ✔ WebSockets           by Markus Sattler  (arduinoWebSockets)  ← NEW
    ✔ Blynk
    ✔ Adafruit GFX Library
    ✔ Adafruit SSD1306
    ✔ Adafruit BME280 Library
    ✔ Adafruit NeoPixel
    ✔ DHT sensor library   by Adafruit
    ✔ ArduinoJson          v6 or v7
    ✔ ArduinoOTA           (built-in with ESP32 Arduino core)

  Board: ESP32S3 Dev Module
  Partition: 16M Flash (3MB APP / 9.9MB FATFS)
  PSRAM: OPI PSRAM (8MB)
***************************************************************************************/

// ── Blynk (MUST be before any includes) ──────────────────────────
#define BLYNK_TEMPLATE_ID   "DoIF0X3z70iL-27SQBWneaRvXmIusDtA"
#define BLYNK_TEMPLATE_NAME "Sourya Weather"
#define BLYNK_PRINT         Serial

// ── STEP 1: globals header first — pins, EEPROM, externs ─────────
// ws_tunnel.h needs these; including here ensures they are defined
// before that header is parsed.
#include "skywatch_globals.h"

// ── STEP 2: core Arduino / ESP-IDF libraries ──────────────────────
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>
#include <Adafruit_NeoPixel.h>
#include <DHT.h>
#include <SPI.h>
#include <SD.h>
#include <FFat.h>
#include <HTTPClient.h>
#include <BlynkSimpleEsp32.h>
#include <ArduinoOTA.h>
#include "time.h"
// math.h already included via skywatch_globals.h

// ── STEP 3: project headers ───────────────────────────────────────
#include "dashboard.h"    // PROGMEM dashboard HTML
#include "setup_page.h"   // PROGMEM AP setup page HTML
#include "ws_tunnel.h"    // WebSocket tunnel (uses skywatch_globals.h externs)

// ══════════════════════════════════════════════════════════════════
//  PERIPHERAL OBJECTS
// ══════════════════════════════════════════════════════════════════
Adafruit_SSD1306   display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_BME280    bme;
DHT                dht(DHT_PIN, DHT_TYPE);
Adafruit_NeoPixel  rgb(NUM_PIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);
WebServer          server(80);

// ── Filter instances (class fully defined in skywatch_globals.h) ──
AnomalyFilter tempAI(3.0f);
AnomalyFilter presAI(3.0f);

// ══════════════════════════════════════════════════════════════════
//  GLOBAL VARIABLE DEFINITIONS
//  (declared extern in skywatch_globals.h)
// ══════════════════════════════════════════════════════════════════
float temp = 0, hum = 0, pres = 0, mslp = 0;
float wind_speed = 0, wind_gust = 0;
float dew_point = 0, cloud_base_m = 0;
float ppm_co2 = 0, dust_ug = 0, db_level = 0;
float smoothed_dust = 0;
float pressure_trend = 0;
float pressure_history[6] = {0};
int   pressure_idx = 0;
float temp_hi = -100, temp_lo = 100;
float R0 = 10.0f;
float t_off = 0, h_off = 0, p_off = 0, d_off = 0;
float w_cal = 1.0f, rl_val = 10.0f;
float v_mq = 0;
int   aqi = 0, r_pm = 0, r_mq = 0;
int   ltg_distance_km = 0;
bool  is_raining = false, is_daytime = true;
unsigned long i2c_latency = 0;
int   ai_glitches_blocked = 0;
String beaufort    = "--";
String wind_txt    = "--";
String forecast    = "--";
String active_sensor = "Booting...";
String health_status = "100% Nominal";
String ssid_name = "";
String ssid_pass = "";
String ts_key    = "";

// ── Additional globals (not in globals.h, only needed here) ──────
char   auth[] = "DoIF0X3z70iL-27SQBWneaRvXmIusDtA";
const float STATION_ALTITUDE = 45.0f;
const float EMA_ALPHA = 0.1f;
int   log_interval = 1;
long  gmt_offset   = 19800;       // IST UTC+5:30

volatile int  wind_clicks_now = 0;
volatile unsigned long last_int_time = 0;
int  wind_history[12] = {0};
int  wind_idx = 0;

// 1=idle 2=wifi-connecting 3=online 4=AP-mode 5=cloud-sync 6=tunnel-active
int system_state = 1;

volatile bool ota_running = false;

unsigned long last_wind_check = 0;
unsigned long last_sensor     = 0;
unsigned long last_log        = 0;
unsigned long last_wifi_check = 0;

bool wifi_was_connected   = false;
bool reboot_done_today    = false;

const String log_file     = "/weather_log.csv";
const String history_file = "/chart_hist.csv";
const char*  ntpServer    = "pool.ntp.org";

// ══════════════════════════════════════════════════════════════════
//  UTILITY
// ══════════════════════════════════════════════════════════════════
float sanitize(float val, float fallback) {
  return (isnan(val) || isinf(val)) ? fallback : val;
}

// ══════════════════════════════════════════════════════════════════
//  RGB LED
// ══════════════════════════════════════════════════════════════════
void setRGB(int r, int g, int b) {
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
}

void updateRGB() {
  unsigned long m = millis();
  rgb.setBrightness(is_daytime ? 150 : 20);

  if (ota_running)    { setRGB(0, 0, 255); return; }
  if (system_state == 4) {
    setRGB((m/200)%2==0 ? 255:0, 0, (m/200)%2==0 ? 0:255); return;
  }
  if (system_state == 2) {
    if ((m/500)%2==0) setRGB(255,100,0); else setRGB(0,0,0); return;
  }
  if (system_state == 5) { setRGB(128, 0, 128); return; }
  if (system_state == 6 && tunnelIsConnected()) { setRGB(0, 180, 120); return; }
  if (wind_gust > 60.0f) {
    if (m%500<250) setRGB(255,0,255); else setRGB(0,0,0); return;
  }
  if (is_raining) {
    if (m%1000<100) setRGB(0,255,255); else setRGB(0,0,0); return;
  }
  if (temp > 40.0f || aqi > 200) { setRGB(255, 0, 0);   return; }
  if (temp > 35.0f || aqi > 100) { setRGB(255, 180, 0); return; }
  setRGB(0, 255, 0);
}

// ══════════════════════════════════════════════════════════════════
//  WIND ISR
// ══════════════════════════════════════════════════════════════════
void IRAM_ATTR countWind() {
  unsigned long t = millis();
  if (t - last_int_time > 15) {
    wind_clicks_now++;
    last_int_time = t;
  }
}

// ══════════════════════════════════════════════════════════════════
//  SENSOR HELPERS
// ══════════════════════════════════════════════════════════════════
float readDustEMA() {
  noInterrupts();
  digitalWrite(DUST_LED_PIN, LOW);
  delayMicroseconds(280);
  int raw = analogRead(DUST_ANA_PIN);
  delayMicroseconds(40);
  digitalWrite(DUST_LED_PIN, HIGH);
  interrupts();

  float voltage = (raw > 0) ? raw * (3.3f / 4095.0f) * 1.5f : 0;
  float calc    = (0.17f * voltage - 0.1f) * 1000.0f;
  if (calc < 0) calc = 0;
  calc += d_off;
  smoothed_dust = (smoothed_dust == 0)
                ? calc
                : (EMA_ALPHA * calc + (1.0f - EMA_ALPHA) * smoothed_dust);
  r_pm = raw;
  return smoothed_dust;
}

float readMic() {
  unsigned long start = millis();
  unsigned int maxV = 0, minV = 4095;
  while (millis() - start < 50) {
    unsigned int s = (unsigned int)analogRead(MIC_PIN);
    if (s > maxV) maxV = s;
    if (s < minV) minV = s;
  }
  if (maxV - minV < 50) return 40.0f;
  return 20.0f * log10f(((maxV - minV) * 3.3f) / 4095.0f / 0.00631f) + 40.0f;
}

float getGasPPM(float rs_ro_ratio, float a, float b_exp) {
  return a * powf(rs_ro_ratio, b_exp);
}

// ── AQI — full EPA scale, gap fixed ──────────────────────────────
int calculateAQI(float pm) {
  if (pm <= 12.0f)   return (int)map((long)pm,   0,  12,   0,  50);
  if (pm <= 35.4f)   return (int)map((long)pm,  12,  35,  51, 100);
  if (pm <= 55.4f)   return (int)map((long)pm,  35,  55, 101, 150);
  if (pm <= 150.4f)  return (int)map((long)pm,  55, 150, 151, 200);
  if (pm <= 250.4f)  return (int)map((long)pm, 150, 250, 201, 300);
  return              (int)map((long)pm,        250, 500, 301, 500);
}

String getBeaufort(float speed) {
  if (speed < 2)  return "Calm";
  if (speed < 12) return "Light";
  if (speed < 29) return "Moderate";
  if (speed < 50) return "Strong";
  if (speed < 89) return "Gale";
  return "Storm";
}

// ── Magnus dew point ─────────────────────────────────────────────
float calcDewPoint(float T, float H) {
  if (H <= 0) return T;
  float alpha = (17.27f * T) / (237.3f + T) + logf(H / 100.0f);
  return (237.3f * alpha) / (17.27f - alpha);
}

// ══════════════════════════════════════════════════════════════════
//  FORECAST
// ══════════════════════════════════════════════════════════════════
void updatePrediction() {
  pressure_history[pressure_idx] = mslp;
  pressure_idx = (pressure_idx + 1) % 6;
  float avg = 0;
  for (int i = 0; i < 6; i++) avg += pressure_history[i];
  avg /= 6.0f;
  pressure_trend = mslp - avg;

  if      (is_raining)              forecast = "PRECIPITATION";
  else if (mslp < 995 && mslp > 0)  forecast = "CYCLONE WARN";
  else if (ltg_distance_km > 0)     forecast = "STORM APPROACHING";
  else if (pressure_trend < -2.0f)  forecast = "DETERIORATING";
  else if (pressure_trend >  2.0f)  forecast = "IMPROVING";
  else                               forecast = "STABLE CLEAR";
}

// ══════════════════════════════════════════════════════════════════
//  HTTP HANDLERS — local WebServer
// ══════════════════════════════════════════════════════════════════
void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleData() {
  StaticJsonDocument<1024> doc;
  doc["temperature"]   = sanitize(temp,   0);
  doc["humidity"]      = sanitize(hum,    0);
  doc["pressure_mslp"] = sanitize(mslp,   0);
  doc["wind_speed"]    = sanitize(wind_speed, 0);
  doc["wind_gust"]     = sanitize(wind_gust,  0);
  doc["wind_beaufort"] = beaufort;
  doc["wind_dir"]      = wind_txt;
  doc["dew_point"]     = sanitize(dew_point,    0);
  doc["lcl"]           = sanitize(cloud_base_m, 0);
  doc["co2"]           = sanitize(ppm_co2, 400);
  doc["pm25"]          = sanitize(dust_ug, 0);
  doc["aqi"]           = aqi;
  doc["rain_status"]   = is_raining;
  doc["forecast"]      = forecast;
  doc["temp_hi"]       = sanitize(temp_hi, 0);
  doc["temp_lo"]       = sanitize(temp_lo, 0);
  doc["health_status"] = health_status;
  doc["i2c_ms"]        = i2c_latency;
  doc["wifi_rssi"]     = WiFi.RSSI();
  doc["p_trend"]       = sanitize(pressure_trend, 0);
  doc["t_mean"]        = sanitize(tempAI.currentMean,   0);
  doc["t_std"]         = sanitize(tempAI.currentStdDev, 0);
  doc["t_z"]           = sanitize(tempAI.lastZScore,    0);
  doc["p_mean"]        = sanitize(presAI.currentMean,   0);
  doc["p_std"]         = sanitize(presAI.currentStdDev, 0);
  doc["p_z"]           = sanitize(presAI.lastZScore,    0);
  doc["active_sensor"] = active_sensor;
  doc["ai_glitches"]   = ai_glitches_blocked;
  doc["db_level"]      = sanitize(db_level, 0);
  doc["ltg_dist"]      = ltg_distance_km;
  doc["tunnel_ok"]     = tunnelIsConnected();
  doc["raw_mq"]        = analogRead(MQ135_PIN);
  doc["volt_mq"]       = sanitize(v_mq, 0);
  doc["raw_pm"]        = r_pm;
  doc["raw_hspd"]      = digitalRead(HALL_SPEED);
  doc["raw_hn"]        = digitalRead(HALL_N);
  doc["raw_he"]        = digitalRead(HALL_E);
  doc["raw_hs"]        = digitalRead(HALL_S);
  doc["raw_hw"]        = digitalRead(HALL_W);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSettingsAPI() {
  StaticJsonDocument<256> doc;
  doc["ssid"]  = ssid_name;
  doc["wCal"]  = w_cal;
  doc["rlVal"] = rl_val;
  doc["tOff"]  = t_off;
  doc["hOff"]  = h_off;
  doc["pOff"]  = p_off;
  doc["dOff"]  = d_off;
  doc["key"]   = ts_key;
  String j;
  serializeJson(doc, j);
  server.send(200, "application/json", j);
}

void handleCalibrateMQ() {
  int raw  = analogRead(MQ135_PIN);
  float v  = raw * (3.3f / 4095.0f) * 1.5f;
  float rs = (v > 0) ? ((5.0f - v) / v) * rl_val : 0;
  if (rs > 100) {
    R0 = rs / 3.6f;
    EEPROM.put(ADDR_R0, R0);
    EEPROM.commit();
    server.send(200, "text/plain", "MQ135 Calibrated. R0=" + String(R0, 2));
  } else {
    server.send(500, "text/plain", "Calibration failed — RS too low. Warm up sensor.");
  }
}

void handleSave() {
  if (server.hasArg("ssid")) {
    String s = server.arg("ssid"); char b[33];
    s.toCharArray(b, 33);
    for (int i = 0; i < 32; i++) EEPROM.write(ADDR_SSID + i, b[i]);
  }
  if (server.hasArg("pass") && server.arg("pass").length() > 0) {
    String p = server.arg("pass"); char b[33];
    p.toCharArray(b, 33);
    for (int i = 0; i < 32; i++) EEPROM.write(ADDR_PASS + i, b[i]);
  }
  if (server.hasArg("wCal"))  { w_cal  = server.arg("wCal").toFloat();  EEPROM.put(ADDR_W_CAL,  w_cal);  }
  if (server.hasArg("rlVal")) { rl_val = server.arg("rlVal").toFloat(); EEPROM.put(ADDR_RL_CAL, rl_val); }
  if (server.hasArg("tOff"))  { t_off  = server.arg("tOff").toFloat();  EEPROM.put(ADDR_T_OFF,  t_off);  }
  if (server.hasArg("hOff"))  { h_off  = server.arg("hOff").toFloat();  EEPROM.put(ADDR_H_OFF,  h_off);  }
  if (server.hasArg("pOff"))  { p_off  = server.arg("pOff").toFloat();  EEPROM.put(ADDR_P_OFF,  p_off);  }
  if (server.hasArg("dOff"))  { d_off  = server.arg("dOff").toFloat();  EEPROM.put(ADDR_D_OFF,  d_off);  }
  if (server.hasArg("key")) {
    String k = server.arg("key"); char kb[21];
    k.toCharArray(kb, 21);
    for (int i = 0; i < 20; i++) EEPROM.write(ADDR_API_KEY + i, kb[i]);
  }
  EEPROM.commit();
  server.send(200, "text/plain", "Saved. Rebooting...");
  delay(1000);
  ESP.restart();
}

void handleHistory() {
  if (FFat.exists(history_file)) {
    File f = FFat.open(history_file, "r");
    if (f) { server.streamFile(f, "text/csv"); f.close(); }
    else server.send(500, "text/plain", "Open failed");
  } else {
    server.send(404, "text/plain", "No history yet");
  }
}

void handleDownload() {
  if (SD.exists(log_file)) {
    File f = SD.open(log_file, FILE_READ);
    if (f) { server.streamFile(f, "text/csv"); f.close(); }
    else server.send(500, "text/plain", "Open failed");
  } else {
    server.send(404, "text/plain", "No SD log");
  }
}

// ── WiFi scan endpoint (AP mode setup page) ───────────────────────
void handleWifiScan() {
  int n = WiFi.scanNetworks(false, false, false, 300);
  StaticJsonDocument<4096> doc;
  JsonArray arr = doc.to<JsonArray>();
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      JsonObject net = arr.createNestedObject();
      net["ssid"]    = WiFi.SSID(i);
      net["rssi"]    = WiFi.RSSI(i);
      net["channel"] = WiFi.channel(i);
      net["open"]    = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    }
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
  WiFi.scanDelete();
}

// ── AP setup page root ────────────────────────────────────────────
void handleAPRoot() {
  server.send(200, "text/html", ap_setup_html);
}

// ══════════════════════════════════════════════════════════════════
//  CLOUD / SD LOGGING
// ══════════════════════════════════════════════════════════════════
void logDataNow(unsigned long now) {
  Serial.println("\n[CLOUD] Log/Sync cycle starting...");
  int prev_state = system_state;
  system_state = 5; updateRGB();

  // SD
  File f = SD.open(log_file, FILE_APPEND);
  if (f) {
    f.printf("%lu,%.1f,%.0f,%.1f,%.1f,%d,%.0f,%.0f,%d,%.1f,%d\n",
             now, temp, hum, mslp, wind_speed,
             (int)is_raining, ppm_co2, dust_ug,
             ai_glitches_blocked, db_level, ltg_distance_km);
    f.close();
    Serial.println("[LOCAL] SD updated.");
  }

  // FFat chart history
  File hist = FFat.open(history_file, "a");
  if (hist) {
    struct tm ti; char tStr[9];
    if (getLocalTime(&ti, 10)) { strftime(tStr, 9, "%H:%M:%S", &ti); }
    else {
      unsigned long s = millis()/1000;
      sprintf(tStr, "%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);
    }
    hist.printf("%s,%.1f,%.0f,%.0f,%.1f,%d,%.0f,%.0f,%d,%.1f,%d\n",
                tStr, temp, hum, mslp, wind_speed,
                (int)is_raining, ppm_co2, dust_ug,
                ai_glitches_blocked, db_level, ltg_distance_km);
    hist.close();
  }

  // Cloud
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.virtualWrite(V1, sanitize(temp, 0));
    Blynk.virtualWrite(V2, sanitize(hum,  0));
    Blynk.virtualWrite(V3, sanitize(mslp, 0));
    Blynk.virtualWrite(V4, sanitize(wind_speed, 0));
    Blynk.virtualWrite(V6, sanitize(ppm_co2, 400));
    Blynk.virtualWrite(V7, aqi);

    if (ts_key.length() > 5 && millis() > 16000) {
      HTTPClient http;
      String url = "http://api.thingspeak.com/update?api_key=" + ts_key
                 + "&field1=" + String(sanitize(temp,0),1)
                 + "&field2=" + String((int)sanitize(hum,0))
                 + "&field3=" + String(sanitize(mslp,0),1)
                 + "&field4=" + String(sanitize(wind_speed,0),1)
                 + "&field5=" + String((int)sanitize(dust_ug,0))
                 + "&field6=" + String((int)sanitize(ppm_co2,400))
                 + "&field7=" + String(is_raining ? 1 : 0)
                 + "&field8=" + String(ai_glitches_blocked);
      http.begin(url);
      int code = http.GET();
      Serial.printf("[CLOUD] ThingSpeak: %d\n", code);
      http.end();
    } else if (millis() <= 16000) {
      Serial.println("[CLOUD] ThingSpeak skip — boot guard.");
    }
  }

  system_state = prev_state;
  last_log = millis();
}

// ══════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== SkyWatch Pro v17.0 (WebSocket Relay Edition) ===");

  // RGB init
  rgb.begin(); rgb.setBrightness(150); setRGB(255, 100, 0);

  // EEPROM
  EEPROM.begin(EEPROM_SIZE);
  char b[33];
  for (int i=0;i<32;i++) b[i]=EEPROM.read(ADDR_SSID+i); b[32]='\0';
  ssid_name=String(b); ssid_name.trim();
  for (int i=0;i<32;i++) b[i]=EEPROM.read(ADDR_PASS+i); b[32]='\0';
  ssid_pass=String(b); ssid_pass.trim();
  char k[21];
  for (int i=0;i<20;i++) k[i]=EEPROM.read(ADDR_API_KEY+i); k[20]='\0';
  ts_key=String(k); ts_key.trim();
  EEPROM.get(ADDR_R0,     R0);
  EEPROM.get(ADDR_W_CAL,  w_cal);
  EEPROM.get(ADDR_RL_CAL, rl_val);
  EEPROM.get(ADDR_T_OFF,  t_off);
  EEPROM.get(ADDR_H_OFF,  h_off);
  EEPROM.get(ADDR_P_OFF,  p_off);
  EEPROM.get(ADDR_D_OFF,  d_off);

  // I2C + sensors
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeout(100);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, true);
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0); display.println("SkyWatch v17.0");
  display.println("Booting..."); display.display();

  dht.begin();
  if (!bme.begin(0x76, &Wire)) bme.begin(0x77, &Wire);

  // Storage
  SD.begin(SD_CS_PIN);
  FFat.begin(true);

  // GPIO
  pinMode(DUST_LED_PIN, OUTPUT);
  pinMode(RAIN_PIN,     INPUT_PULLUP);
  pinMode(RAIN_PWR_PIN, OUTPUT);
  pinMode(HALL_SPEED,   INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_SPEED), countWind, FALLING);
  pinMode(HALL_N, INPUT_PULLUP); pinMode(HALL_E, INPUT_PULLUP);
  pinMode(HALL_S, INPUT_PULLUP); pinMode(HALL_W, INPUT_PULLUP);
  pinMode(MIC_PIN, INPUT);

  // WiFi
  Serial.println("[BOOT] Connecting to WiFi...");
  system_state = 2;
  WiFi.disconnect(); WiFi.mode(WIFI_STA);
  WiFi.begin(ssid_name.c_str(), ssid_pass.c_str());

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 20) {
    delay(500); timeout++; setRGB(255,100,0); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    // ── AP fallback with scanner ──────────────────────────────
    Serial.println("[BOOT] WiFi failed — AP mode.");
    WiFi.mode(WIFI_AP_STA);   // AP+STA so we can still scan
    WiFi.softAP("SkyWatch_Setup", "12345678");
    system_state = 4;
    setRGB(255, 0, 0);
    Serial.printf("[AP] IP: %s\n", WiFi.softAPIP().toString().c_str());

    // AP routes only
    server.on("/",         handleAPRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/api/scan", handleWifiScan);
    server.begin();

    display.clearDisplay();
    display.setTextSize(1); display.setCursor(0,0);
    display.println("AP MODE");
    display.println("SSID: SkyWatch_Setup");
    display.println("Pass: 12345678");
    display.println("Open: 192.168.4.1");
    display.display();

    // In AP mode we just run the server — no sensors, no tunnel
    while (true) {
      server.handleClient();
      updateRGB();
      yield();
    }
    // never returns

  } else {
    // ── Online path ────────────────────────────────────────────
    Serial.printf("[BOOT] WiFi OK — IP: %s\n", WiFi.localIP().toString().c_str());
    wifi_was_connected = true;
    system_state = 3; setRGB(0,255,0);

    // NTP
    configTime(gmt_offset, 0, ntpServer);
    struct tm t;
    if (getLocalTime(&t, 5000)) Serial.println("[TIME] NTP OK.");

    // Blynk
    Blynk.config(auth); Blynk.connect();

    // mDNS
    MDNS.begin("skywatch");

    // ArduinoOTA
    ArduinoOTA.setHostname("SkyWatch-Roof");
    ArduinoOTA.onStart([]() {
      ota_running = true;
      detachInterrupt(digitalPinToInterrupt(HALL_SPEED));
      setRGB(0,0,255);
      display.clearDisplay();
      display.setTextSize(2); display.setCursor(0,10); display.println("OTA UPDATE");
      display.setTextSize(1); display.setCursor(0,40); display.println("Receiving...");
      display.display();
      Serial.println("\n[OTA] Starting...");
    });
    ArduinoOTA.onEnd([]() {
      display.clearDisplay(); display.setTextSize(2);
      display.setCursor(0,20); display.println("SUCCESS!");
      display.display();
      Serial.println("[OTA] Done. Rebooting...");
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
      Serial.printf("[OTA] %u%%\r", (p/(t/100)));
    });
    ArduinoOTA.onError([](ota_error_t e) {
      Serial.printf("[OTA] Error[%u]\n", e); ESP.restart();
    });
    ArduinoOTA.begin();

    // WebSocket tunnel
    tunnelBegin();
    system_state = 6;

    // Local WebServer routes
    server.on("/",           handleRoot);
    server.on("/data",       handleData);
    server.on("/save",  HTTP_POST, handleSave);
    server.on("/api/set",    handleSettingsAPI);
    server.on("/api/cal_mq", handleCalibrateMQ);
    server.on("/api/scan",   handleWifiScan);
    server.on("/history",    handleHistory);
    server.on("/download",   handleDownload);
    server.begin();
    Serial.println("[HTTP] Server started.");
  }

  // Heap report
  Serial.printf("[MEM] Free heap:  %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  Serial.printf("[MEM] Free PSRAM: %lu bytes\n", (unsigned long)ESP.getFreePsram());

  Serial.println("[BOOT] Sensor warm-up (5s)...");
  delay(5000);
  Serial.println("[BOOT] Ready.\n");
}

// ══════════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════════
void loop() {

  ArduinoOTA.handle();   // always first
  tunnelLoop();          // lightweight — only active when bytes arrive
  if (ota_running) return;

  server.handleClient();
  if (WiFi.status() == WL_CONNECTED) Blynk.run();

  // ── WiFi watchdog (every 10s) ─────────────────────────────────
  unsigned long now = millis();
  if (now - last_wifi_check >= 10000) {
    last_wifi_check = now;
    bool cur = (WiFi.status() == WL_CONNECTED);
    if (!cur && wifi_was_connected) {
      Serial.println("[WIFI] Lost connection.");
      wifi_was_connected = false; system_state = 2;
    }
    if (cur && !wifi_was_connected) {
      Serial.println("[WIFI] Reconnected — restarting tunnel.");
      wifi_was_connected = true;
      tunnelBegin();
      system_state = 6;
    }
  }

  updateRGB();

  // ── Scheduled 3AM reboot (debounced) ─────────────────────────
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    if (timeinfo.tm_hour == 3 && timeinfo.tm_min == 0 && !reboot_done_today) {
      reboot_done_today = true;
      Serial.println("[SYS] 3AM maintenance reboot.");
      delay(500); ESP.restart();
    }
    if (timeinfo.tm_hour != 3) reboot_done_today = false;

    // Midnight daily stats reset
    if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0 && timeinfo.tm_sec < 3) {
      temp_hi = -100; temp_lo = 100; wind_gust = 0;
      Serial.println("[SYS] Daily stats reset.");
    }
    is_daytime = (timeinfo.tm_hour >= 6 && timeinfo.tm_hour < 19);
  }

  // ── Wind (every 5s) ───────────────────────────────────────────
  if (now - last_wind_check >= 5000) {
  detachInterrupt(digitalPinToInterrupt(HALL_SPEED));

  noInterrupts();
  int clicks = wind_clicks_now;
  wind_clicks_now = 0;
  interrupts();

  // Sanity clamp
  // 500 RPM = 129 km/h → clicks in 5s at 500 RPM = 500/60*5 = 41.7
  // Allow up to 45 clicks before clamping
  if (clicks > 45) {
    Serial.printf("[WIND] Spike clamped: %d clicks\n", clicks);
    clicks = 45;
  }

  // ── Correct physics-based conversion ─────────────────────────
  // 1 pulse per revolution assumed.
  // RPM = (clicks / 5s) × 60
  // Wind (km/h) = RPM × 0.258   (from your calibration table)
  // Combined:    clicks × (60/5) × 0.258 = clicks × 3.096
  const float KMH_PER_CLICK = 3.096f;   // for 1 pulse/rev
  // If your anemometer gives 2 pulses/rev, change to 1.548f

  float instant_kmh = (clicks * KMH_PER_CLICK) * w_cal;

  // ── Gust: highest instant reading this session ────────────────
  if (instant_kmh > wind_gust) wind_gust = instant_kmh;

  // ── Rolling 60s average (12 slots × 5s) ──────────────────────
  wind_history[wind_idx] = clicks;
  wind_idx = (wind_idx + 1) % 12;

  int tot = 0;
  for (int i = 0; i < 12; i++) tot += wind_history[i];
  // tot = total clicks over last 60s
  // RPM_avg = (tot / 60s) × 60 = tot (clicks/s avg × 60)
  // Wind_avg = tot × (1/60) × 60 × 0.258 = tot × 0.258 / 60 × 60
  // Simplified: wind_speed = (tot / 12) × KMH_PER_CLICK × w_cal
  wind_speed = ((float)tot / 12.0f) * KMH_PER_CLICK * w_cal;

  beaufort = getBeaufort(wind_speed);

  // Wind direction
  int n=digitalRead(HALL_N), e=digitalRead(HALL_E);
  int s=digitalRead(HALL_S), w=digitalRead(HALL_W);
  if      (n==0) wind_txt="N"; else if (e==0) wind_txt="E";
  else if (s==0) wind_txt="S"; else if (w==0) wind_txt="W";
  else           wind_txt="--";

  attachInterrupt(digitalPinToInterrupt(HALL_SPEED), countWind, FALLING);
  last_wind_check = now;
}

  // ── Sensor read (every 2s) ────────────────────────────────────
  if (now - last_sensor >= 2000) {
    unsigned long st = millis();
    float bt = bme.readTemperature();
    float bp = bme.readPressure() / 100.0f;
    float bh = bme.readHumidity();
    float dt = dht.readTemperature();
    float dh = dht.readHumidity();
    i2c_latency = millis() - st;

    // Temp + humidity
    if (!isnan(bt) && bt != 0.0f) {
      active_sensor = "BME280";
      if (!tempAI.isGlitch(bt + t_off)) temp = bt + t_off;
      else ai_glitches_blocked++;
      hum = (!isnan(bh) && bh > 0) ? bh + h_off : dh + h_off;
    } else if (!isnan(dt)) {
      active_sensor = "DHT11";
      if (!tempAI.isGlitch(dt + t_off)) temp = dt + t_off;
      else ai_glitches_blocked++;
      hum = dh + h_off;
    }

    // Pressure + MSLP
    if (!isnan(bp) && bp > 0) {
      if (!presAI.isGlitch(bp + p_off)) {
        pres = bp + p_off;
        mslp = pres * powf(1.0f - (0.0065f * STATION_ALTITUDE) /
               (temp + (0.0065f * STATION_ALTITUDE) + 273.15f), -5.257f);
      } else {
        ai_glitches_blocked++;
      }
    }

    if (temp > temp_hi) temp_hi = temp;
    if (temp < temp_lo) temp_lo = temp;

    dew_point    = calcDewPoint(temp, hum);
    cloud_base_m = (temp - dew_point) * 125.0f;

    // Rain
    digitalWrite(RAIN_PWR_PIN, HIGH); delay(10);
    is_raining = (digitalRead(RAIN_PIN) == LOW);
    digitalWrite(RAIN_PWR_PIN, LOW);

    // Dust + AQI
    dust_ug = readDustEMA();
    aqi     = calculateAQI(dust_ug);

    // Acoustic
    db_level = readMic();

    // CO2 (corrected compensation)
    if (now > 10000) {
      r_mq = analogRead(MQ135_PIN);
      v_mq = r_mq * (3.3f / 4095.0f) * 1.5f;
      float rs = (v_mq > 0) ? ((5.0f - v_mq) / v_mq) * rl_val : 0;
      float tc = 1.0f - (temp - 20.0f) * 0.02f;
      float hc = 1.0f + (hum  - 33.0f) * 0.01f;
      float rc = (tc > 0 && hc > 0) ? rs / (tc * hc) : rs;
      ppm_co2  = getGasPPM(rc / R0, 16393.0f, -2.862f);
      if (ppm_co2 < 400) ppm_co2 = 400;
    }

    updatePrediction();

    // OLED
    display.clearDisplay();
    display.setTextSize(2); display.setCursor(0,0);
    display.printf("%.1fC", temp);
    display.setTextSize(1); display.setCursor(0,20);
    display.printf("H:%.0f%% P:%.0f", hum, mslp);
    display.setCursor(0,30);
    display.printf("W:%.1f %s", wind_speed, wind_txt.c_str());
    display.setCursor(0,40);
    display.printf("%s", forecast.c_str());
    display.setCursor(0,54);
    display.printf("TUN:%s AQI:%d",
                   tunnelIsConnected() ? "OK" : "--", aqi);
    display.display();

    last_sensor = now;
  }

  // ── Log + sync ────────────────────────────────────────────────
  if (now > 10000 && (now - last_log) >= (unsigned long)(log_interval * 60000UL)) {
    logDataNow(now);
  }
}
