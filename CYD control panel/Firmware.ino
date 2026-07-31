/*
  CYD Control Panel — v3 "Icarus"
  ----------------------------------------------------------------
  Everything built so far, all at once:
  - State machine: IDLE / MENU / KEEPALIVE / MACRO / ALERT
  - BLE HID keepalive (F12 every 2 min, configurable end hour)
  - Internet-down detector: ping 8.8.8.8 every 30s, drops to 5s on
    first miss, ALERT screen after 2 consecutive misses, gateway
    checked on first miss for diagnostics only
  - NTP clock (EST5EDT, DST-aware) on the idle screen
  - Weather via Open-Meteo (no API key needed) — refetched every
    15 minutes, shows "(stale)" if the last fetch is old
  - Dynamic macro grid (1-6 tiles) — macros are label + typed text
  - Web config page (plain HTTP, no auth, home-LAN only) to add and
    delete macros — this is the ONE part that hasn't been tested on
    real hardware yet, most likely to need debugging
  - VM ping tier — pings a list of VM IPs every few minutes, status
    logged to Serial only for now (no on-screen UI yet — lowest
    priority item per original design, "much less critical")

  Libraries needed beyond what's already installed:
  - ESP32Ping (already added for the internet-down detector)
  - WebServer, HTTPClient, WiFiClientSecure — all bundled with the
    ESP32 Arduino core, no separate install needed
*/

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <BleKeyboard.h>
#include <WiFi.h>
using fs::FS; // TFT_eSPI suppresses the core's normal global FS/File aliases
              // to avoid its own symbols colliding with them (same reason
              // fs::File is used explicitly elsewhere in this file) — but
              // WebServer.h expects the plain, unqualified FS to exist
              // globally, so it's restored here just before WebServer.h
              // is included.
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESP32Ping.h>
#include "time.h"

// ---- WiFi — fill in your real credentials ---------------------------------
#define WIFI_SSID     "YOUR_HOME_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_HOME_WIFI_PASSWORD"

// ---- touch pins (same as roll chart — confirmed working on this board) --
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ---- backlight — confirmed GPIO21, HIGH=on --------------------------------
#define TFT_BL_PIN 21

TFT_eSPI tft = TFT_eSPI();
XPT2046_Bitbang touch(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
BleKeyboard bleKeyboard("CYD Control Panel", "Nick's Workshop", 100);
WebServer webServer(80);

// ---- palette ---------------------------------------------------------------
#define INK    0x18E3
#define PAPER  0xF79E
#define ACCENT 0xC186
#define ALERT_RED 0xC000
#define GRAY   0xC618
// Computed at runtime via tft.color565() in setup() — TFT_eSPI's color565
// isn't usable as a compile-time #define, and this way avoids hand-rolled
// 565 bit math getting a shade wrong.
uint16_t GREEN_COLOR;
uint16_t YELLOW_COLOR;
uint16_t GRAD_TOP_COLOR;
uint16_t GRAD_BOT_COLOR;

// ---- touch calibration — carried over from the roll chart panel ---------
int calXMin = 250, calXMax = 3700, calYMin = 180, calYMax = 3800;

// ---- touch state -------------------------------------------------------------
unsigned long pressStart = 0;
bool pressActive = false;
bool positionLocked = false;
int lockedXRaw = 0;
int lockedYRaw = 0;
bool wokeThisPress = false;
const unsigned long MIN_PRESS_MS = 45;
const unsigned long SETTLE_MS = 25;

bool asleep = false;
unsigned long lastActivityMs = 0;

// ---- idle screen change-tracking — avoids redrawing every second when
// nothing shown on screen actually changed, which was the source of the
// flicker (a full-screen gradient repaint 60x/minute when the clock
// itself only changes once a minute).
int lastDrawnMinute = -1;
bool lastDrawnKeepalive = false;
bool lastDrawnVmDown = false;
bool lastDrawnWeatherFetched = false;
float lastDrawnWeatherTemp = -999;
String lastDrawnWeatherDesc = "";
bool lastDrawnWeatherStale = false;

// ============================================================
// App state
// ============================================================
enum AppMode { MODE_IDLE, MODE_MENU, MODE_KEEPALIVE, MODE_MACRO, MODE_VMS, MODE_ALERT };
AppMode mode = MODE_IDLE;
AppMode preAlertMode = MODE_IDLE;

const unsigned long MENU_TIMEOUT_MS = 60000; // 1 minute
unsigned long menuEnteredMs = 0;

bool internetDown = false;

// ---- keepalive state --------------------------------------------------------
bool keepaliveActive = false;   // always boots false, not persisted
int keepaliveEndHour = 17;      // default 5 PM — persisted
unsigned long lastF12Ms = 0;
const unsigned long F12_INTERVAL_MS = 120000; // 2 minutes

// ---- macro storage -----------------------------------------------------------
struct MacroButton {
  String label;
  String text;
};
const int MAX_MACROS = 6;
std::vector<MacroButton> macros;

const char* CONFIG_PATH = "/config.json";

// ---- NTP / time --------------------------------------------------------------
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";
const char* TZ_STRING = "EST5EDT,M3.2.0,M11.1.0/2";
bool ntpEverSynced = false;

// ---- internet-down ping detector ---------------------------------------------
IPAddress pingTarget(8, 8, 8, 8);
unsigned long lastPingMs = 0;
const unsigned long PING_INTERVAL_NORMAL_MS = 30000;
const unsigned long PING_INTERVAL_SUSPECT_MS = 5000;
int consecutiveMisses = 0;
const int MISSES_BEFORE_ALERT = 2;
bool suspectDown = false;

// ---- weather — now fetched from a local VM running a PowerShell
// companion script (weather_fetch.ps1) on a schedule, rather than
// hitting Open-Meteo's HTTPS API directly. Sidesteps the TLS
// handshake heap cost entirely: this is a plain local HTTP GET.
// Update this to your actual VM's IP/hostname and IIS path.
#define WEATHER_URL "http://192.168.1.100/weather/weather.json"
const unsigned long WEATHER_FETCH_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 min
unsigned long lastWeatherFetchMs = 0;
unsigned long lastWeatherSuccessMs = 0;
bool weatherFetchAttempted = false; // drives the retry throttle — set on
                                     // every attempt, success or failure
bool weatherEverFetched = false;    // true only after a SUCCESSFUL fetch —
                                     // drawIdle() uses this to decide
                                     // whether to show real data
float weatherTempF = 0;
String weatherDesc = "";

// ---- VM ping tier — now configurable via the web page, same pattern
// as macros. Capped at 4 rather than 6 (macros' cap) so the status
// screen never needs scrolling — 4 rows fit the 320x240 panel cleanly
// below the header and above the Back bar; more would need a scroll UI
// we haven't built.
const int MAX_VMS = 4;
std::vector<String> vmIps;
std::vector<bool> vmUp;
const unsigned long VM_PING_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 min
unsigned long lastVmPingMs = 0;
bool vmPingAttempted = false; // same immediate-first-check pattern as weather
bool anyVmDown = false;

// ============================================================
// Forward declarations
// ============================================================
void goToMode(AppMode newMode);
void drawIdle();
void drawMenu();
void drawKeepaliveScreen();
void drawMacroScreen();
void drawAlertScreen();
void handleIdleTap(int sx, int sy);
void handleMenuTap(int sx, int sy);
void handleKeepaliveTap(int sx, int sy);
void handleMacroTap(int sx, int sy);
void drawVmScreen();
void handleVmTap(int sx, int sy);
void drawIdleBackground();
void loadConfig();
void saveConfig();
void toggleKeepalive();
void adjustEndHour(int delta);
void handleKeepaliveTimer();
void checkMenuTimeout();
void checkInternetStatus();
void checkInternetPing();
void connectWiFi();
void fireMacro(int i);
void checkWeather();
void fetchWeather();
String weatherCodeToText(int code);
void checkVmPings();
void setupWebServer();
void handleWebRoot();
void handleWebAdd();
void handleWebDelete();
void handleWebVmAdd();
void handleWebVmDelete();

// ============================================================
// Setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("=== CYD Control Panel v3 ===");

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS.begin() FAILED");
  }
  loadConfig();

  tft.init();
  tft.setRotation(1);
  GREEN_COLOR = tft.color565(46, 160, 67);
  YELLOW_COLOR = tft.color565(235, 180, 40);
  GRAD_TOP_COLOR = tft.color565(58, 70, 110);   // deep slate blue
  GRAD_BOT_COLOR = tft.color565(247, 228, 214); // matches PAPER's cream tone
  touch.begin();
  lastActivityMs = millis();

  connectWiFi();
  configTzTime(TZ_STRING, NTP_SERVER1, NTP_SERVER2);
  setupWebServer();

  Serial.println("Starting BLE keyboard...");
  bleKeyboard.begin();

  goToMode(MODE_IDLE);
}

void loop() {
  webServer.handleClient();

  auto p = touch.getTouch();
  bool touching = p.zRaw > 200;

  if (touching) lastActivityMs = millis();

  if (touching && pressActive && !positionLocked &&
      millis() - pressStart >= SETTLE_MS) {
    lockedXRaw = p.xRaw;
    lockedYRaw = p.yRaw;
    positionLocked = true;
  }

  if (touching && !pressActive) {
    pressActive = true;
    positionLocked = false;
    pressStart = millis();
    wokeThisPress = asleep;
    if (asleep) {
      asleep = false;
      digitalWrite(TFT_BL_PIN, HIGH);
    }
  } else if (!touching && pressActive) {
    unsigned long heldFor = millis() - pressStart;
    pressActive = false;
    if (!wokeThisPress && heldFor >= MIN_PRESS_MS) {
      int sx = constrain(map(lockedXRaw, calXMin, calXMax, 0, 320), 0, 319);
      int sy = constrain(map(lockedYRaw, calYMin, calYMax, 0, 240), 0, 239);

      switch (mode) {
        case MODE_IDLE:      handleIdleTap(sx, sy); break;
        case MODE_MENU:      handleMenuTap(sx, sy); break;
        case MODE_KEEPALIVE: handleKeepaliveTap(sx, sy); break;
        case MODE_MACRO:     handleMacroTap(sx, sy); break;
        case MODE_VMS:       handleVmTap(sx, sy); break;
        case MODE_ALERT:     break;
      }
    }
  }

  checkMenuTimeout();
  checkInternetPing();
  checkInternetStatus();
  handleKeepaliveTimer();
  checkWeather();
  checkVmPings();

  static unsigned long lastClockCheck = 0;
  if (mode == MODE_IDLE && millis() - lastClockCheck >= 1000) {
    lastClockCheck = millis();

    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    int currentMinute = (t->tm_year > 100) ? t->tm_min : -1;
    bool currentStale = (millis() - lastWeatherSuccessMs) > (2 * WEATHER_FETCH_INTERVAL_MS);

    bool changed = (currentMinute != lastDrawnMinute) ||
                   (keepaliveActive != lastDrawnKeepalive) ||
                   (anyVmDown != lastDrawnVmDown) ||
                   (weatherEverFetched != lastDrawnWeatherFetched) ||
                   (weatherTempF != lastDrawnWeatherTemp) ||
                   (weatherDesc != lastDrawnWeatherDesc) ||
                   (currentStale != lastDrawnWeatherStale);

    if (changed) {
      drawIdle(); // also updates the lastDrawn* snapshot at its end
    }
  }
}

// ============================================================
// WiFi
// ============================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed — ping checks will keep retrying in background.");
  }
}

// ============================================================
// Web config server — macros only, for now
// ============================================================
void setupWebServer() {
  webServer.on("/", HTTP_GET, handleWebRoot);
  webServer.on("/add", HTTP_POST, handleWebAdd);
  webServer.on("/delete", HTTP_POST, handleWebDelete);
  webServer.on("/vmadd", HTTP_POST, handleWebVmAdd);
  webServer.on("/vmdelete", HTTP_POST, handleWebVmDelete);
  webServer.begin();
  Serial.println("Web config server started on port 80");
}

void handleWebRoot() {
  String html = "<html><head><title>CYD Control Panel</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 12px;}"
                "input,textarea{width:100%;padding:8px;margin:4px 0;box-sizing:border-box;font-family:inherit;}"
                "button{padding:8px 16px;margin-top:6px;}"
                ".macro{border:1px solid #ccc;padding:8px;margin:8px 0;border-radius:6px;}"
                "</style></head><body>";
  html += "<h2>Macros (" + String(macros.size()) + "/" + String(MAX_MACROS) + ")</h2>";

  for (int i = 0; i < (int)macros.size(); i++) {
    html += "<div class='macro'><b>" + macros[i].label + "</b><br>";
    html += "<code>" + macros[i].text + "</code><br>";
    html += "<form method='POST' action='/delete' style='display:inline'>";
    html += "<input type='hidden' name='index' value='" + String(i) + "'>";
    html += "<button type='submit'>Delete</button></form></div>";
  }

  if ((int)macros.size() < MAX_MACROS) {
    html += "<h3>Add Macro</h3>";
    html += "<form method='POST' action='/add'>";
    html += "Label: <input name='label' maxlength='16' required><br>";
    html += "Text to send: <textarea name='text' maxlength='100' rows='3' required></textarea><br>";
    html += "<small>Press Enter inside the box for a literal Enter keystroke in the macro.</small><br>";
    html += "<button type='submit'>Add</button></form>";
  } else {
    html += "<p>Max macros reached (" + String(MAX_MACROS) + "). Delete one to add another.</p>";
  }

  html += "<h2>VMs to monitor (" + String(vmIps.size()) + "/" + String(MAX_VMS) + ")</h2>";
  for (int i = 0; i < (int)vmIps.size(); i++) {
    html += "<div class='macro'>" + vmIps[i] + " - " + (vmUp[i] ? "up" : "<b>DOWN</b>");
    html += "<form method='POST' action='/vmdelete' style='display:inline'>";
    html += "<input type='hidden' name='index' value='" + String(i) + "'>";
    html += "<button type='submit'>Delete</button></form></div>";
  }
  if ((int)vmIps.size() < MAX_VMS) {
    html += "<form method='POST' action='/vmadd'>";
    html += "IP address: <input name='ip' maxlength='39' required><br>";
    html += "<button type='submit'>Add</button></form>";
  } else {
    html += "<p>Max VMs reached (" + String(MAX_VMS) + "). Delete one to add another.</p>";
  }

  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleWebAdd() {
  if ((int)macros.size() >= MAX_MACROS) {
    webServer.send(400, "text/plain", "Max macros reached");
    return;
  }
  if (!webServer.hasArg("label") || !webServer.hasArg("text")) {
    webServer.send(400, "text/plain", "Missing fields");
    return;
  }
  MacroButton mb;
  mb.label = webServer.arg("label");
  mb.text = webServer.arg("text");
  macros.push_back(mb);
  saveConfig();
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleWebDelete() {
  if (!webServer.hasArg("index")) {
    webServer.send(400, "text/plain", "Missing index");
    return;
  }
  int idx = webServer.arg("index").toInt();
  if (idx >= 0 && idx < (int)macros.size()) {
    macros.erase(macros.begin() + idx);
    saveConfig();
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleWebVmAdd() {
  if ((int)vmIps.size() >= MAX_VMS) {
    webServer.send(400, "text/plain", "Max VMs reached");
    return;
  }
  if (!webServer.hasArg("ip")) {
    webServer.send(400, "text/plain", "Missing IP");
    return;
  }
  vmIps.push_back(webServer.arg("ip"));
  vmUp.push_back(true); // assume up until the next ping check
  saveConfig();
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleWebVmDelete() {
  if (!webServer.hasArg("index")) {
    webServer.send(400, "text/plain", "Missing index");
    return;
  }
  int idx = webServer.arg("index").toInt();
  if (idx >= 0 && idx < (int)vmIps.size()) {
    vmIps.erase(vmIps.begin() + idx);
    vmUp.erase(vmUp.begin() + idx);
    saveConfig();
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

// ============================================================
// Mode transitions
// ============================================================
void goToMode(AppMode newMode) {
  if (newMode == MODE_ALERT && mode != MODE_ALERT) {
    preAlertMode = mode;
  }
  mode = newMode;
  menuEnteredMs = millis();

  switch (newMode) {
    case MODE_IDLE:      drawIdle(); break;
    case MODE_MENU:      drawMenu(); break;
    case MODE_KEEPALIVE: drawKeepaliveScreen(); break;
    case MODE_MACRO:     drawMacroScreen(); break;
    case MODE_VMS:        drawVmScreen(); break;
    case MODE_ALERT:     drawAlertScreen(); break;
  }
  Serial.printf("Mode -> %d\n", newMode);
}

void checkMenuTimeout() {
  if (mode == MODE_MENU || mode == MODE_KEEPALIVE || mode == MODE_MACRO || mode == MODE_VMS) {
    if (millis() - menuEnteredMs > MENU_TIMEOUT_MS) {
      goToMode(MODE_IDLE);
    }
  }
}

void checkInternetStatus() {
  if (internetDown && mode != MODE_ALERT) {
    goToMode(MODE_ALERT);
  } else if (!internetDown && mode == MODE_ALERT) {
    goToMode(preAlertMode);
  }
}

// ============================================================
// Internet-down ping detector
// ============================================================
void checkInternetPing() {
  unsigned long interval = suspectDown ? PING_INTERVAL_SUSPECT_MS : PING_INTERVAL_NORMAL_MS;
  if (millis() - lastPingMs < interval) return;
  lastPingMs = millis();

  bool ok = Ping.ping(pingTarget, 1);

  if (ok) {
    if (consecutiveMisses > 0 || internetDown) {
      Serial.println("Ping OK — clearing alert");
    }
    consecutiveMisses = 0;
    suspectDown = false;
    internetDown = false;
  } else {
    consecutiveMisses++;
    suspectDown = true;
    Serial.printf("Ping miss #%d\n", consecutiveMisses);

    if (consecutiveMisses == 1) {
      IPAddress gw = WiFi.gatewayIP();
      bool gwOk = Ping.ping(gw, 1);
      Serial.println(gwOk ? "Gateway OK — likely upstream/ISP issue"
                           : "Gateway unreachable — likely local WiFi/router issue");
    }

    if (consecutiveMisses >= MISSES_BEFORE_ALERT) {
      internetDown = true;
    }
  }
}

// ============================================================
// Weather (Open-Meteo)
// ============================================================
String weatherCodeToText(int code) {
  if (code == 0) return "Clear";
  if (code == 1) return "Mostly Clear";
  if (code == 2) return "Partly Cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 55) return "Drizzle";
  if (code >= 61 && code <= 65) return "Rain";
  if (code >= 71 && code <= 75) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather fetch skipped — no WiFi");
    return;
  }

  HTTPClient http;
  http.begin(WEATHER_URL); // plain http:// — no WiFiClientSecure needed,
                            // no TLS handshake, no heap cost
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Weather fetch failed: HTTP %d\n", code);
    http.end();
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    Serial.printf("Weather JSON parse failed: %s\n", err.c_str());
    return;
  }

  weatherTempF = doc["current"]["temperature_2m"] | 0.0;
  int code_wmo = doc["current"]["weather_code"] | -1;
  weatherDesc = weatherCodeToText(code_wmo);
  weatherEverFetched = true;
  lastWeatherSuccessMs = millis();
  Serial.printf("Weather: %.0fF %s\n", weatherTempF, weatherDesc.c_str());
}

void checkWeather() {
  if (weatherFetchAttempted && millis() - lastWeatherFetchMs < WEATHER_FETCH_INTERVAL_MS) return;
  weatherFetchAttempted = true;
  lastWeatherFetchMs = millis();
  fetchWeather();
}

// ============================================================
// VM ping tier — Serial-logged only for now, no UI yet
// ============================================================
void checkVmPings() {
  if (vmPingAttempted && millis() - lastVmPingMs < VM_PING_INTERVAL_MS) return;
  vmPingAttempted = true;
  lastVmPingMs = millis();

  anyVmDown = false;
  for (int i = 0; i < (int)vmIps.size(); i++) {
    IPAddress vmIp;
    bool ok = false;
    if (vmIp.fromString(vmIps[i])) {
      ok = Ping.ping(vmIp, 1);
    }
    vmUp[i] = ok;
    if (!ok) anyVmDown = true;
    Serial.printf("VM %s: %s\n", vmIps[i].c_str(), ok ? "up" : "DOWN");
  }

  // Refresh whichever screen is currently showing this info
  if (mode == MODE_IDLE) drawIdle();
  if (mode == MODE_VMS) drawVmScreen();
}

// ============================================================
// Config persistence
// ============================================================
void loadConfig() {
  keepaliveActive = false;
  keepaliveEndHour = 17;
  macros.clear();
  vmIps.clear();

  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("No config file, using defaults");
    vmUp.assign(vmIps.size(), true);
    return;
  }
  fs::File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    Serial.println("Config exists but failed to open, using defaults");
    vmUp.assign(vmIps.size(), true);
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("Config parse failed (%s), using defaults\n", err.c_str());
    vmUp.assign(vmIps.size(), true);
    return;
  }
  keepaliveEndHour = doc["keepaliveEndHour"] | 17;

  JsonArray macroArr = doc["macros"];
  for (JsonObject m : macroArr) {
    if ((int)macros.size() >= MAX_MACROS) break;
    MacroButton mb;
    mb.label = m["label"].as<String>();
    mb.text = m["text"].as<String>();
    macros.push_back(mb);
  }

  JsonArray vmArr = doc["vms"];
  for (JsonVariant v : vmArr) {
    if ((int)vmIps.size() >= MAX_VMS) break;
    vmIps.push_back(v.as<String>());
  }
  vmUp.assign(vmIps.size(), true); // assume up until the first ping check

  Serial.printf("Config loaded: endHour=%d, %d macros, %d VMs\n",
                keepaliveEndHour, macros.size(), vmIps.size());
}

void saveConfig() {
  JsonDocument doc;
  doc["keepaliveEndHour"] = keepaliveEndHour;
  JsonArray macroArr = doc["macros"].to<JsonArray>();
  for (auto &mb : macros) {
    JsonObject m = macroArr.add<JsonObject>();
    m["label"] = mb.label;
    m["text"] = mb.text;
  }
  JsonArray vmArr = doc["vms"].to<JsonArray>();
  for (auto &ip : vmIps) {
    vmArr.add(ip);
  }
  fs::File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println("Failed to open config file for writing");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("Config saved");
}

// ============================================================
// Keepalive logic
// ============================================================
void toggleKeepalive() {
  keepaliveActive = !keepaliveActive;
  if (keepaliveActive) {
    lastF12Ms = millis();
  }
  goToMode(MODE_IDLE);
}

void adjustEndHour(int delta) {
  keepaliveEndHour = (keepaliveEndHour + delta + 24) % 24;
  saveConfig();
  drawKeepaliveScreen();
  menuEnteredMs = millis();
}

void handleKeepaliveTimer() {
  if (!keepaliveActive) return;
  if (!ntpEverSynced) return;

  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  if (t->tm_hour >= keepaliveEndHour) {
    keepaliveActive = false;
    if (mode == MODE_IDLE) drawIdle();
    return;
  }

  if (millis() - lastF12Ms >= F12_INTERVAL_MS) {
    lastF12Ms = millis();
    if (bleKeyboard.isConnected()) {
      bleKeyboard.write(KEY_F12);
      Serial.println("Keepalive: sent F12");
    } else {
      Serial.println("Keepalive: BLE not connected, skipped F12");
    }
  }
}

// ============================================================
// Screens
// ============================================================
void drawIdleBackground() {
  // Simple vertical gradient, computed fresh each redraw — cheap enough
  // at 240 horizontal lines that it doesn't need caching to a buffer.
  uint8_t r1 = (GRAD_TOP_COLOR >> 11) & 0x1F, g1 = (GRAD_TOP_COLOR >> 5) & 0x3F, b1 = GRAD_TOP_COLOR & 0x1F;
  uint8_t r2 = (GRAD_BOT_COLOR >> 11) & 0x1F, g2 = (GRAD_BOT_COLOR >> 5) & 0x3F, b2 = GRAD_BOT_COLOR & 0x1F;
  for (int y = 0; y < 240; y++) {
    float t = (float)y / 239.0;
    uint8_t r = r1 + (int)((r2 - r1) * t);
    uint8_t g = g1 + (int)((g2 - g1) * t);
    uint8_t b = b1 + (int)((b2 - b1) * t);
    uint16_t c = (r << 11) | (g << 5) | b;
    tft.drawFastHLine(0, y, 320, c);
  }
}

void drawIdle() {
  drawIdleBackground();

  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char timeBuf[8];
  if (t->tm_year > 100) {
    ntpEverSynced = true;
    int hour12 = t->tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(timeBuf, sizeof(timeBuf), "%d:%02d", hour12, t->tm_min);
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "--:--");
  }

  // Clock: 24pt bold is the largest font TFT_eSPI bundles by default, so
  // going bigger still means scaling it — setTextSize(2) pixel-doubles
  // the glyphs. Slightly blockier than a native larger font would be,
  // but there isn't one bundled; a custom .vlw smooth font would be the
  // next step up if this doesn't look good enough on real hardware.
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextSize(2);
  tft.setTextColor(INK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(timeBuf, 160, 80);
  tft.setTextSize(1); // reset — everything else on this screen wants 1x

  tft.setFreeFont(&FreeSansBold18pt7b);
  if (weatherEverFetched) {
    char wbuf[40];
    bool stale = (millis() - lastWeatherSuccessMs) > (2 * WEATHER_FETCH_INTERVAL_MS);
    snprintf(wbuf, sizeof(wbuf), "%.0f%cF  %s%s", weatherTempF, 176, weatherDesc.c_str(),
             stale ? " (stale)" : "");
    tft.drawString(wbuf, 160, 155);
  } else {
    tft.drawString("Weather: --", 160, 155);
  }

  if (anyVmDown) {
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(YELLOW_COLOR);
    tft.drawString("VM offline - check menu", 160, 185);
  }

  if (keepaliveActive) {
    tft.fillCircle(300, 20, 6, GREEN_COLOR);
  }
  tft.setTextDatum(TL_DATUM);

  // Snapshot what was just drawn, so loop()'s periodic check can tell
  // whether a redraw is actually needed next time around.
  lastDrawnMinute = t->tm_year > 100 ? t->tm_min : -1;
  lastDrawnKeepalive = keepaliveActive;
  lastDrawnVmDown = anyVmDown;
  lastDrawnWeatherFetched = weatherEverFetched;
  lastDrawnWeatherTemp = weatherTempF;
  lastDrawnWeatherDesc = weatherDesc;
  lastDrawnWeatherStale = (millis() - lastWeatherSuccessMs) > (2 * WEATHER_FETCH_INTERVAL_MS);
}

void handleIdleTap(int sx, int sy) {
  goToMode(MODE_MENU);
}

#define MENU_TILE_GAP 8
#define MENU_TILE_W ((320 - 4 * MENU_TILE_GAP) / 3)
#define MENU_TILE_H 80
#define MENU_TILE_TOP 40

void drawMenu() {
  tft.fillScreen(PAPER);
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Menu", 6, 5);

  const char* labels[3] = { "Keepalive", "Macros", "VMs" };
  tft.setTextColor(INK, PAPER);
  tft.setTextDatum(MC_DATUM);
  for (int i = 0; i < 3; i++) {
    int x = MENU_TILE_GAP + i * (MENU_TILE_W + MENU_TILE_GAP);
    tft.drawRect(x, MENU_TILE_TOP, MENU_TILE_W, MENU_TILE_H, INK);
    tft.drawString(labels[i], x + MENU_TILE_W / 2, MENU_TILE_TOP + MENU_TILE_H / 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void handleMenuTap(int sx, int sy) {
  if (sy < MENU_TILE_TOP || sy >= MENU_TILE_TOP + MENU_TILE_H) return;
  int col = sx / (MENU_TILE_W + MENU_TILE_GAP);
  if (col == 0) goToMode(MODE_KEEPALIVE);
  else if (col == 1) goToMode(MODE_MACRO);
  else if (col == 2) goToMode(MODE_VMS);
}

void drawKeepaliveScreen() {
  tft.fillScreen(PAPER);
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Keepalive", 6, 5);

  tft.fillRect(70, 40, 180, 60, keepaliveActive ? ALERT_RED : INK);
  tft.setTextColor(PAPER, keepaliveActive ? ALERT_RED : INK);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(keepaliveActive ? "STOP" : "START", 160, 70);

  int displayHour = keepaliveEndHour % 12;
  if (displayHour == 0) displayHour = 12;
  const char* ampm = keepaliveEndHour < 12 ? "AM" : "PM";
  char buf[16];
  snprintf(buf, sizeof(buf), "Ends %d %s", displayHour, ampm);

  tft.fillRect(20, 130, 50, 50, INK);
  tft.setTextColor(PAPER, INK);
  tft.drawString("-", 45, 155);

  tft.setTextColor(INK, PAPER);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString(buf, 160, 155);

  tft.fillRect(250, 130, 50, 50, INK);
  tft.setTextColor(PAPER, INK);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString("+", 275, 155);

  tft.setTextDatum(TL_DATUM);
}

void handleKeepaliveTap(int sx, int sy) {
  if (sy >= 40 && sy < 100 && sx >= 70 && sx < 250) {
    toggleKeepalive();
    return;
  }
  if (sy >= 130 && sy < 180) {
    if (sx >= 20 && sx < 70) {
      adjustEndHour(-1);
      return;
    }
    if (sx >= 250 && sx < 300) {
      adjustEndHour(1);
      return;
    }
  }
}

#define MACRO_COLS 3
#define MACRO_GRID_TOP 26
#define MACRO_TILE_H 78
#define MACRO_TILE_GAP 6
#define MACRO_BACK_H 36

void drawMacroScreen() {
  tft.fillScreen(PAPER);
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Macros", 6, 5);

  int count = macros.size();
  if (count == 0) {
    tft.setTextColor(INK, PAPER);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("No macros yet", 160, 100);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString("Add some on the web config page", 160, 125);
    tft.setTextDatum(TL_DATUM);
  } else {
    int tileW = (320 - (MACRO_COLS + 1) * MACRO_TILE_GAP) / MACRO_COLS;
    tft.setFreeFont(&FreeSansBold9pt7b);
    for (int i = 0; i < count; i++) {
      int col = i % MACRO_COLS;
      int row = i / MACRO_COLS;
      int x = MACRO_TILE_GAP + col * (tileW + MACRO_TILE_GAP);
      int y = MACRO_GRID_TOP + row * (MACRO_TILE_H + MACRO_TILE_GAP);
      tft.drawRect(x, y, tileW, MACRO_TILE_H, INK);
      tft.setTextColor(INK, PAPER);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(macros[i].label, x + tileW / 2, y + MACRO_TILE_H / 2);
    }
    tft.setTextDatum(TL_DATUM);
  }

  int backY = 240 - MACRO_BACK_H;
  tft.drawRect(6, backY, 320 - 12, MACRO_BACK_H, ACCENT);
  tft.setTextColor(ACCENT, PAPER);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.drawString("Back", 160, backY + MACRO_BACK_H / 2);
  tft.setTextDatum(TL_DATUM);
}

void fireMacro(int i) {
  if (i < 0 || i >= (int)macros.size()) return;
  Serial.printf("Firing macro %d: %s\n", i, macros[i].label.c_str());
  if (!bleKeyboard.isConnected()) {
    Serial.println("Macro fire skipped — BLE not connected");
    return;
  }
  // Sending the whole string via print() fires each character's
  // press/release back-to-back with no gap — under BLE notification
  // timing (especially with WiFi/web server also active) a release
  // can get delayed or dropped, and the host reads that as a held
  // key and starts auto-repeating. A small delay between characters
  // gives each one time to actually land before the next goes out.
  String text = macros[i].text;
  for (unsigned int c = 0; c < text.length(); c++) {
    bleKeyboard.print(text[c]);
    delay(40);
  }
}

void handleMacroTap(int sx, int sy) {
  int backY = 240 - MACRO_BACK_H;
  if (sy >= backY) {
    goToMode(MODE_MENU);
    return;
  }

  int count = macros.size();
  if (count == 0) return;

  int tileW = (320 - (MACRO_COLS + 1) * MACRO_TILE_GAP) / MACRO_COLS;
  if (sy < MACRO_GRID_TOP) return;
  int row = (sy - MACRO_GRID_TOP) / (MACRO_TILE_H + MACRO_TILE_GAP);
  int col = sx / (tileW + MACRO_TILE_GAP);
  int idx = row * MACRO_COLS + col;
  if (idx >= 0 && idx < count) {
    fireMacro(idx);
    menuEnteredMs = millis();
  }
}

#define VM_ROW_H 44
#define VM_ROW_TOP 30
#define VM_BACK_H 36

void drawVmScreen() {
  tft.fillScreen(PAPER);
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("VM Status", 6, 5);

  if (vmIps.empty()) {
    tft.setTextColor(INK, PAPER);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("No VMs configured", 160, 100);
    tft.setFreeFont(&FreeSans9pt7b);
    tft.drawString("Add some on the web config page", 160, 125);
    tft.setTextDatum(TL_DATUM);
  } else {
    tft.setFreeFont(&FreeSansBold9pt7b);
    for (int i = 0; i < (int)vmIps.size(); i++) {
      int y = VM_ROW_TOP + i * VM_ROW_H;
      int dotCy = y + (VM_ROW_H - 6) / 2;
      tft.fillCircle(28, dotCy, 8, vmUp[i] ? GREEN_COLOR : ALERT_RED);
      tft.setTextColor(INK, PAPER);
      tft.setTextDatum(ML_DATUM);
      String label = vmIps[i] + (vmUp[i] ? "  up" : "  DOWN");
      tft.drawString(label, 48, dotCy);
    }
    tft.setTextDatum(TL_DATUM);
  }

  int backY = 240 - VM_BACK_H;
  tft.drawRect(6, backY, 320 - 12, VM_BACK_H, ACCENT);
  tft.setTextColor(ACCENT, PAPER);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.drawString("Back", 160, backY + VM_BACK_H / 2);
  tft.setTextDatum(TL_DATUM);
}

void handleVmTap(int sx, int sy) {
  int backY = 240 - VM_BACK_H;
  if (sy >= backY) {
    goToMode(MODE_MENU);
  }
}

void drawAlertScreen() {
  tft.fillScreen(ALERT_RED);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextColor(PAPER, ALERT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("INTERNET DOWN", 160, 110);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString(keepaliveActive ? "Keepalive still running" : "", 160, 150);
  tft.setTextDatum(TL_DATUM);
}
