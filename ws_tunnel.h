#ifndef WS_TUNNEL_H
#define WS_TUNNEL_H

/***************************************************************************************
  ws_tunnel.h — SkyWatch Pro WebSocket Tunnel Client
  ---------------------------------------------------
  Maintains a persistent outbound WSS connection to the relay server.
  Receives HTTP requests forwarded by the relay, dispatches them to the
  same logic used by the local WebServer, and returns responses.

  REQUIRES:
    - skywatch_globals.h  included BEFORE this file
    - "WebSockets" by Markus Sattler  (arduinoWebSockets) in Library Manager
    - ArduinoJson (already in sketch)

  CONFIGURATION:
    Edit TUNNEL_HOST and TUNNEL_SECRET below to match your deployed relay.
***************************************************************************************/

#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <FFat.h>
#include <SD.h>
#include <EEPROM.h>
#include <WiFi.h>

// ── Tunnel configuration ──────────────────────────────────────────
#define TUNNEL_HOST          "your-relay-name.onrender.com"  // ← change after deploy
#define TUNNEL_PORT          443
#define TUNNEL_PATH          "/"
#define TUNNEL_SECRET        "changeme_strong_secret"         // ← match relay env var
#define TUNNEL_USE_SSL       true
#define TUNNEL_RECONNECT_MS  5000

// ── State ─────────────────────────────────────────────────────────
static WebSocketsClient  _tunnelWS;
static bool              _tunnelConnected   = false;
static unsigned long     _tunnelConnectedAt = 0;
static int               _tunnelReconnects  = 0;

// ── Response capture buffers ──────────────────────────────────────
static String  _tresp_body   = "";
static int     _tresp_status = 200;
static String  _tresp_ctype  = "application/json";

// ── Send a completed response back to relay ───────────────────────
static void _tunnelSendResponse(const String& reqId, int status,
                                const String& ctype, const String& body) {
  // Envelope: {"type":"http_response","id":"...","status":NNN,"ctype":"...","body":"..."}
  // We use ArduinoJson to safely escape the body regardless of content type.
  // Allocate on heap because HTML body can be ~35KB; PSRAM on S3 handles this fine.
  const size_t envSize = body.length() + 256;
  DynamicJsonDocument env(envSize);
  env["type"]   = "http_response";
  env["id"]     = reqId;
  env["status"] = status;
  env["ctype"]  = ctype;
  env["body"]   = body;

  String out;
  out.reserve(envSize);
  serializeJson(env, out);

  if (_tunnelWS.sendTXT(out)) {
    Serial.printf("[TUNNEL] Response sent for %s — %d bytes, status %d\n",
                  reqId.c_str(), out.length(), status);
  } else {
    Serial.println("[TUNNEL] Send failed — socket not ready.");
  }
}

// ── Route dispatcher ──────────────────────────────────────────────
static void _tunnelDispatch(const String& reqId, const String& method,
                            const String& path,   const String& body) {

  Serial.printf("[TUNNEL] Dispatch: %s %s\n", method.c_str(), path.c_str());

  _tresp_body   = "";
  _tresp_status = 200;
  _tresp_ctype  = "application/json";

  // ── GET / ── serve dashboard HTML ────────────────────────────
  if (path == "/" || path == "") {
    extern const char index_html[];
    _tresp_status = 200;
    _tresp_ctype  = "text/html";
    _tresp_body   = String(index_html);   // PROGMEM → String; fine with PSRAM

  // ── GET /data ─────────────────────────────────────────────────
  } else if (path == "/data") {
    StaticJsonDocument<1024> doc;
    doc["temperature"]   = temp;
    doc["humidity"]      = hum;
    doc["pressure_mslp"] = mslp;
    doc["wind_speed"]    = wind_speed;
    doc["wind_gust"]     = wind_gust;
    doc["wind_beaufort"] = beaufort;
    doc["wind_dir"]      = wind_txt;
    doc["dew_point"]     = dew_point;
    doc["lcl"]           = cloud_base_m;
    doc["co2"]           = ppm_co2;
    doc["pm25"]          = dust_ug;
    doc["aqi"]           = aqi;
    doc["rain_status"]   = is_raining;
    doc["forecast"]      = forecast;
    doc["temp_hi"]       = temp_hi;
    doc["temp_lo"]       = temp_lo;
    doc["i2c_ms"]        = i2c_latency;
    doc["wifi_rssi"]     = WiFi.RSSI();
    doc["p_trend"]       = pressure_trend;
    doc["t_mean"]        = tempAI.currentMean;
    doc["t_std"]         = tempAI.currentStdDev;
    doc["t_z"]           = tempAI.lastZScore;
    doc["p_mean"]        = presAI.currentMean;
    doc["p_std"]         = presAI.currentStdDev;
    doc["p_z"]           = presAI.lastZScore;
    doc["active_sensor"] = active_sensor;
    doc["ai_glitches"]   = ai_glitches_blocked;
    doc["db_level"]      = db_level;
    doc["ltg_dist"]      = ltg_distance_km;
    doc["tunnel_ok"]     = _tunnelConnected;
    doc["raw_mq"]        = analogRead(MQ135_PIN);
    doc["volt_mq"]       = v_mq;
    doc["raw_pm"]        = r_pm;
    doc["raw_hspd"]      = digitalRead(HALL_SPEED);
    doc["raw_hn"]        = digitalRead(HALL_N);
    doc["raw_he"]        = digitalRead(HALL_E);
    doc["raw_hs"]        = digitalRead(HALL_S);
    doc["raw_hw"]        = digitalRead(HALL_W);
    serializeJson(doc, _tresp_body);

  // ── GET /api/set ──────────────────────────────────────────────
  } else if (path == "/api/set") {
    StaticJsonDocument<256> doc;
    doc["ssid"]  = ssid_name;
    doc["wCal"]  = w_cal;
    doc["rlVal"] = rl_val;
    doc["tOff"]  = t_off;
    doc["hOff"]  = h_off;
    doc["pOff"]  = p_off;
    doc["dOff"]  = d_off;
    doc["key"]   = ts_key;
    serializeJson(doc, _tresp_body);

  // ── POST /save ────────────────────────────────────────────────
  } else if (path == "/save" && method == "POST") {
    // URL-encoded body parser
    auto getParam = [&](const String& key) -> String {
      String search = key + "=";
      int idx = body.indexOf(search);
      if (idx < 0) return "";
      int start = idx + search.length();
      int end   = body.indexOf('&', start);
      String val = (end < 0) ? body.substring(start) : body.substring(start, end);
      val.replace("+", " ");
      // Basic percent-decode for common chars
      val.replace("%40","@"); val.replace("%21","!");
      val.replace("%23","#"); val.replace("%24","$");
      val.replace("%25","%"); val.replace("%26","&");
      return val;
    };

    bool changed = false;
    char b[33];

    String s = getParam("ssid");
    if (s.length() > 0) {
      s.toCharArray(b, 33);
      for (int i = 0; i < 32; i++) EEPROM.write(ADDR_SSID + i, b[i]);
      changed = true;
    }
    String p = getParam("pass");
    if (p.length() > 0) {
      p.toCharArray(b, 33);
      for (int i = 0; i < 32; i++) EEPROM.write(ADDR_PASS + i, b[i]);
      changed = true;
    }
    String wc  = getParam("wCal");  if (wc.length())  { w_cal  = wc.toFloat();  EEPROM.put(ADDR_W_CAL,  w_cal);  changed = true; }
    String rlv = getParam("rlVal"); if (rlv.length()) { rl_val = rlv.toFloat(); EEPROM.put(ADDR_RL_CAL, rl_val); changed = true; }
    String tov = getParam("tOff");  if (tov.length()) { t_off  = tov.toFloat(); EEPROM.put(ADDR_T_OFF,  t_off);  changed = true; }
    String hov = getParam("hOff");  if (hov.length()) { h_off  = hov.toFloat(); EEPROM.put(ADDR_H_OFF,  h_off);  changed = true; }
    String pov = getParam("pOff");  if (pov.length()) { p_off  = pov.toFloat(); EEPROM.put(ADDR_P_OFF,  p_off);  changed = true; }
    String dov = getParam("dOff");  if (dov.length()) { d_off  = dov.toFloat(); EEPROM.put(ADDR_D_OFF,  d_off);  changed = true; }
    String k   = getParam("key");
    if (k.length() > 0) {
      char kb[21]; k.toCharArray(kb, 21);
      for (int i = 0; i < 20; i++) EEPROM.write(ADDR_API_KEY + i, kb[i]);
      changed = true;
    }
    if (changed) EEPROM.commit();

    _tresp_status = 200;
    _tresp_ctype  = "text/plain";
    _tresp_body   = "Saved. Rebooting...";
    _tunnelSendResponse(reqId, _tresp_status, _tresp_ctype, _tresp_body);
    delay(1500);
    ESP.restart();
    return;  // never reached

  // ── GET /api/cal_mq ───────────────────────────────────────────
  } else if (path == "/api/cal_mq") {
    int raw   = analogRead(MQ135_PIN);
    float v   = raw * (3.3f / 4095.0f) * 1.5f;
    float rs  = (v > 0) ? ((5.0f - v) / v) * rl_val : 0;
    if (rs > 100) {
      R0 = rs / 3.6f;
      EEPROM.put(ADDR_R0, R0);
      EEPROM.commit();
      _tresp_ctype  = "text/plain";
      _tresp_body   = "MQ135 Calibrated. R0=" + String(R0, 2);
    } else {
      _tresp_status = 500;
      _tresp_ctype  = "text/plain";
      _tresp_body   = "Calibration failed — RS too low. Warm up sensor first.";
    }

  // ── GET /history ──────────────────────────────────────────────
  } else if (path == "/history") {
    if (FFat.exists("/chart_hist.csv")) {
      File f = FFat.open("/chart_hist.csv", "r");
      if (f) {
        _tresp_body = "";
        while (f.available()) _tresp_body += (char)f.read();
        f.close();
        _tresp_ctype = "text/csv";
      } else {
        _tresp_status = 500;
        _tresp_ctype  = "text/plain";
        _tresp_body   = "Failed to open history file";
      }
    } else {
      _tresp_status = 404;
      _tresp_ctype  = "text/plain";
      _tresp_body   = "No history yet";
    }

  // ── GET /download ─────────────────────────────────────────────
  } else if (path == "/download") {
    if (SD.exists("/weather_log.csv")) {
      File f = SD.open("/weather_log.csv", FILE_READ);
      if (f) {
        _tresp_body = "";
        while (f.available()) _tresp_body += (char)f.read();
        f.close();
        _tresp_ctype = "text/csv";
      } else {
        _tresp_status = 500;
        _tresp_ctype  = "text/plain";
        _tresp_body   = "Failed to open SD log";
      }
    } else {
      _tresp_status = 404;
      _tresp_ctype  = "text/plain";
      _tresp_body   = "No SD log file";
    }

  // ── 404 ───────────────────────────────────────────────────────
  } else {
    _tresp_status = 404;
    _tresp_ctype  = "text/plain";
    _tresp_body   = "Not found: " + path;
  }

  _tunnelSendResponse(reqId, _tresp_status, _tresp_ctype, _tresp_body);
}

// ── WebSocket event callback ──────────────────────────────────────
static void _tunnelWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      _tunnelConnected   = true;
      _tunnelConnectedAt = millis();
      _tunnelReconnects++;
      Serial.printf("[TUNNEL] Connected to relay (attempt %d)\n", _tunnelReconnects);
      _tunnelWS.sendTXT("{\"type\":\"hello\",\"node\":\"SkyWatch-Roof\",\"fw\":\"v17.0\"}");
      break;

    case WStype_DISCONNECTED:
      _tunnelConnected = false;
      Serial.println("[TUNNEL] Disconnected. Auto-retry in " + String(TUNNEL_RECONNECT_MS/1000) + "s...");
      break;

    case WStype_TEXT: {
      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) {
        Serial.printf("[TUNNEL] JSON parse error: %s\n", err.c_str());
        break;
      }
      const char* msgType = doc["type"] | "";

      if (strcmp(msgType, "http_request") == 0) {
        String reqId  = doc["id"]     | "";
        String method = doc["method"] | "GET";
        String path   = doc["path"]   | "/";
        String body   = doc["body"]   | "";
        _tunnelDispatch(reqId, method, path, body);

      } else if (strcmp(msgType, "ping") == 0) {
        _tunnelWS.sendTXT("{\"type\":\"pong\"}");
      }
      break;
    }

    case WStype_PING:
      // arduinoWebSockets handles PONG automatically
      break;

    case WStype_ERROR:
      _tunnelConnected = false;
      Serial.println("[TUNNEL] Socket error.");
      break;

    default:
      break;
  }
}

// ── Public API ────────────────────────────────────────────────────

/**
 * Call once in setup() AFTER WiFi is confirmed WL_CONNECTED.
 */
void tunnelBegin() {
  Serial.printf("[TUNNEL] Starting — host: %s port: %d\n", TUNNEL_HOST, TUNNEL_PORT);

  if (TUNNEL_USE_SSL) {
    _tunnelWS.beginSSL(TUNNEL_HOST, TUNNEL_PORT, TUNNEL_PATH);
  } else {
    _tunnelWS.begin(TUNNEL_HOST, TUNNEL_PORT, TUNNEL_PATH);
  }

  _tunnelWS.setExtraHeaders(("x-tunnel-secret: " + String(TUNNEL_SECRET)).c_str());
  _tunnelWS.onEvent(_tunnelWsEvent);
  _tunnelWS.setReconnectInterval(TUNNEL_RECONNECT_MS);
  // Heartbeat: ping every 20s, expect pong within 8s, disconnect after 2 misses
  _tunnelWS.enableHeartbeat(20000, 8000, 2);
}

/**
 * Call every loop() iteration — lightweight poll.
 */
void tunnelLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  _tunnelWS.loop();
}

/**
 * Returns true when relay connection is active.
 */
bool tunnelIsConnected() {
  return _tunnelConnected;
}

#endif // WS_TUNNEL_H
