/*
  Roll Chart — v1
  ----------------------------------------------------------------
  Display + touch + LittleFS + bitmap glyphs + real-font renderer +
  touch-driven interaction + JSON-off-flash + GitHub sync + tile
  menu + QR + Zero Trip + folder-aware scrolling route picker +
  Setup (sleep timeout, maps provider) + sleep/wake, all together.

  Needs THREE extra files in this SAME sketch folder: glyphs.h,
  rc_qrcode.h, rc_qrcode.c.

  ── THIS REVISION ─────────────────────────────────────────────────
  - Y-axis touch calibration nudged down slightly (dot was landing
    a bit above the actual finger position).
  - Zero Trip / QR / Sync / "no WiFi" messages now go through one
    shared full-width banner helper (showBanner()) instead of each
    screen having its own narrow box — fixes text overflowing a box
    that was sized for a different, shorter message, and closes the
    whole class of bug rather than patching one instance of it.
  - Route picker rebuilt: one level of folder browsing (matches the
    real repo structure), taller rows, scrolling (4 visible rows +
    up/down arrows that dim at the ends), pinned Back row. Labels
    are now context-relative — a route shows just its bare filename
    once you're inside its folder, since the folder itself is
    already named in the header above it.
  - WiFi is sync-scoped again (connects only when Sync Now runs,
    powers off after) — NOT because of the touch-interference theory
    from earlier (that was disproven; settle-and-lock capture was
    the real touch fix, unrelated to WiFi state). This is a
    different, good reason: "Syncing..." progress messages only
    make sense as something that happens when you tap the button,
    not silently at boot before you've seen the menu.
  - Two WiFi networks tried in sequence (home, then phone hotspot),
    8s timeout each, with progressive on-screen status — and now an
    actual ON-SCREEN failure message if both fail (previously this
    only printed to Serial, useless to a rider with no computer
    handy).
  - Setup screen is real now: Sleep Timeout and QR Maps App
    (Apple/Google), persisted across power cycles via the ESP32's
    Preferences (NVS) library. "Reset chart position" was
    deliberately left out — selecting your CURRENTLY loaded route
    again from Pick Route already resets to cue 1, so a dedicated
    button would just be a shortcut for something not missing.
  - Sleep/backlight is real now too: idle timeout dims the screen
    (GPIO21, confirmed against three independent sources for this
    board), and the first tap after sleep only wakes it — never
    advances a cue or fires a menu action, exactly like a phone
    screen waking under your thumb without also processing that
    touch as input.
  ──────────────────────────────────────────────────────────────────
*/

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "glyphs.h"
#include "rc_qrcode.h" // vendored locally — see earlier notes on the
                        // ESP32-core qrcode.h naming collision

// ---- FILL THESE IN — placeholders, not real credentials -----------------
// Two networks, tried in order: home WiFi first, then a phone hotspot.
#define WIFI_SSID_1     "YOUR_HOME_WIFI_NAME"
#define WIFI_PASSWORD_1 "YOUR_HOME_WIFI_PASSWORD"
#define WIFI_SSID_2     "YOUR_HOTSPOT_NAME"
#define WIFI_PASSWORD_2 "YOUR_HOTSPOT_PASSWORD"
#define GITHUB_OWNER    "YOUR_GITHUB_USERNAME"
#define GITHUB_REPO     "YOUR_ROUTES_REPO"
#define GITHUB_TOKEN    "YOUR_FINE_GRAINED_TOKEN"
#define GITHUB_BRANCH   "main"


// ---- touch pins (fixed — physical wiring, not calibration) --------------
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
// Calibration range (calXMin/calXMax/calYMin/calYMax) is no longer a
// compile-time constant — see globals below. It's now runtime-
// adjustable and persisted via Preferences, so each physical unit
// (yours, a buddy's) can have its own correct values instead of
// everyone inheriting whatever your specific panel needed.

// ---- backlight — confirmed GPIO21, HIGH=on, against three independent
// sources for this exact board (not guessed).
#define TFT_BL_PIN 21

TFT_eSPI tft = TFT_eSPI();
XPT2046_Bitbang touch(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
Preferences prefs;

// ---- palette --------------------------------------------------------------
#define INK    0x18E3
#define PAPER  0xF79E
#define ACCENT 0xC186
#define AMBER  0x8AC0

// ---- route data -----------------------------------------------------------
#define MAX_CUES 40
struct Cue {
  const char* type;
  const char* text;
  const char* compound;
  const char* note;
  bool verify;
  float distFromPrev;
  float cumulative;
  float lat;
  float lon;
};
// ---- UI mode — declared FIRST among the globals, since several other
// globals below (calibReturnMode) reference it. A variable referencing
// an enum declared later in the file was exactly the ordering bug
// caught twice earlier tonight — this avoids it structurally rather
// than by remembering to order things correctly by hand.
enum AppMode { MODE_CUE, MODE_OPTIONS, MODE_PICKER, MODE_QR, MODE_SETUP, MODE_CALIBRATE };
AppMode mode = MODE_CUE;

Cue route[MAX_CUES];
int ROUTE_LEN = 0;
const char* ROUTE_NAME = "Loading...";
int idx = 0;
float offsetMi = 0;

// ---- persisted settings (loaded in setup(), saved on change) ------------
int sleepTimeoutSec = 30;
bool useGoogleMaps = false;

// ---- touch calibration — runtime + persisted (was compile-time
// constants). Defaults are your last known-good hand-calibrated
// values, used only until a real calibration is saved for THIS unit.
int calXMin = 250, calXMax = 3700, calYMin = 180, calYMax = 3800;

// ---- on-device calibration flow state -------------------------------
int calibStep = 0;          // 0 = waiting for first target, 1 = second
int calibRawX1 = 0, calibRawY1 = 0; // first tap's raw reading
AppMode calibReturnMode = MODE_CUE; // where to go after finishing —
                                     // MODE_CUE on a fresh unit's
                                     // auto-run, MODE_SETUP if launched
                                     // manually from the Setup screen
#define CAL_MARGIN_PX 100 // outward margin beyond the measured taps —
                           // same principle as every manual calibration
                           // pass tonight: a real tap lands a bit short
                           // of the true physical edge
#define CAL_PT1_X 24
#define CAL_PT1_Y 24
#define CAL_PT2_X 296
#define CAL_PT2_Y 216

// ---- sleep/wake state -----------------------------------------------------
bool asleep = false;
unsigned long lastActivityMs = 0;
bool wokeThisPress = false; // true if the CURRENT press started while
                             // asleep — suppresses both long-press and
                             // short-tap dispatch for that same press,
                             // so a wake tap never also acts as input.

// ---- touch state (kept together with the other globals up here — a
// function defined earlier in the file referencing a variable declared
// later caused a real compile error earlier tonight; consolidating
// avoids that class of bug entirely) ---------------------------------
unsigned long pressStart = 0;
bool pressActive = false;
bool longPressFired = false;
bool positionLocked = false;
int lockedXRaw = 0;
int lockedYRaw = 0;
const unsigned long LONG_PRESS_MS = 600;
const unsigned long MIN_PRESS_MS = 45;
const unsigned long SETTLE_MS = 25;

// ---- route picker: one level of folder browsing + scroll ----------------
struct PickerEntry {
  bool isFolder;
  String label; // display label, already context-relative (see header)
  String path;  // folder: name to descend into. route: local file path.
};
#define MAX_PICKER_ENTRIES 20
PickerEntry pickerEntries[MAX_PICKER_ENTRIES];
int pickerEntryCount = 0;
int pickerScroll = 0;
String currentFolder = ""; // "" = root

// JsonDocument lives for the whole program — see string-lifetime note
// from the JSON-loading stage. Never cleared after the initial load.
JsonDocument doc;

bool loadRouteFromFS(const char* path) {
  fs::File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.println("FAILED to open route file.");
    return false;
  }
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("JSON parse FAILED: ");
    Serial.println(err.c_str());
    return false;
  }

  ROUTE_NAME = doc["routeName"] | "Unnamed Route";
  JsonArray cues = doc["cues"];
  ROUTE_LEN = 0;
  for (JsonObject c : cues) {
    if (ROUTE_LEN >= MAX_CUES) {
      Serial.println("Route exceeds MAX_CUES, truncating.");
      break;
    }
    Cue &out = route[ROUTE_LEN];
    out.type = c["type"] | "straight";
    out.text = c["text"] | "(unnamed)";
    out.note = c["note"] | "";
    out.verify = c["verify"] | false;
    out.distFromPrev = c["distFromPrev"] | 0.0;
    out.cumulative = c["cumulative"] | 0.0;
    out.lat = c["lat"] | 0.0;
    out.lon = c["lon"] | 0.0;

    JsonArray comp = c["compound"];
    out.compound = (comp.size() > 0) ? (comp[0] | "") : "";

    ROUTE_LEN++;
  }
  Serial.print("Loaded ");
  Serial.print(ROUTE_LEN);
  Serial.print(" cues from ");
  Serial.println(path);
  return ROUTE_LEN > 0;
}

void loadFallbackErrorRoute() {
  route[0] = { "caution-R", "ROUTE LOAD FAILED", "", "Try Sync or Pick Route", false, 0, 0, 0, 0 };
  ROUTE_LEN = 1;
  ROUTE_NAME = "ERROR";
}

// Replaces the old built-in demo route. Shown on first boot before
// anything's ever been synced/selected, and any time nothing else is
// loaded — a neutral placeholder, not an error, using the same
// straight-ahead glyph the renderer already draws for unrecognized
// cue types.
void loadNoRouteState() {
  route[0] = { "straight", "No Route Loaded", "", "Sync or pick a route", false, 0, 0, 0, 0 };
  ROUTE_LEN = 1;
  ROUTE_NAME = "---";
}

void drawGlyph(const char* key, int cx, int cy) {
  uint16_t color = (strncmp(key, "caution", 7) == 0) ? ACCENT : INK;
  for (int i = 0; i < GLYPH_TABLE_LEN; i++) {
    if (strcmp(GLYPH_TABLE[i].key, key) == 0) {
      tft.drawBitmap(cx - GLYPH_W / 2, cy - GLYPH_H / 2,
                      GLYPH_TABLE[i].bitmap, GLYPH_W, GLYPH_H, color);
      return;
    }
  }
  tft.drawRect(cx - 16, cy - 16, 32, 32, INK);
}

const GFXfont* ROAD_FONT_LADDER[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
const int ROAD_FONT_COUNT = 3;

int wrapAndPrint(const char* text, int x, int y, int maxWidth) {
  for (int fi = 0; fi < ROAD_FONT_COUNT; fi++) {
    tft.setFreeFont(ROAD_FONT_LADDER[fi]);
    char buf[128];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* words[20];
    int wc = 0;
    char* tok = strtok(buf, " ");
    while (tok && wc < 20) { words[wc++] = tok; tok = strtok(nullptr, " "); }

    String lines[2];
    int lineCount = 0;
    String cur = "";
    bool overflowed = false;
    for (int i = 0; i < wc; i++) {
      String trial = cur.length() ? cur + " " + words[i] : String(words[i]);
      if (tft.textWidth(trial) > maxWidth && cur.length()) {
        if (lineCount >= 2) { overflowed = true; break; }
        lines[lineCount++] = cur;
        cur = words[i];
      } else {
        cur = trial;
      }
    }
    if (!overflowed && cur.length()) {
      if (lineCount < 2) lines[lineCount++] = cur;
      else overflowed = true;
    }
    if (overflowed && fi < ROAD_FONT_COUNT - 1) continue;

    int lineH = tft.fontHeight() + 4;
    tft.setTextDatum(TL_DATUM);
    for (int i = 0; i < lineCount; i++) {
      tft.drawString(lines[i], x, y + i * lineH);
    }
    return lineCount;
  }
  return 0;
}

void drawCue(int i) {
  Cue &c = route[i];
  tft.fillScreen(PAPER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextWrap(false, false);

  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(ROUTE_NAME, 6, 5);
  char counter[8];
  snprintf(counter, sizeof(counter), "%d/%d", i + 1, ROUTE_LEN);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(counter, 314, 5);
  tft.setTextDatum(TL_DATUM);

  tft.drawRect(8, 34, 96, 96, INK);
  tft.drawRect(9, 35, 94, 94, INK);
  drawGlyph(c.type, 56, 82);

  int textTop = 44;
  if (c.verify) {
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(ACCENT, PAPER);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("VERIFY", 10, 136);
  }

  tft.setTextColor(INK, PAPER);
  int lines = wrapAndPrint(c.text, 112, textTop, 196);

  int y = textTop + lines * (tft.fontHeight() + 4) + 6;
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(TL_DATUM);
  if (strlen(c.compound) > 0) {
    tft.setTextColor(INK, PAPER);
    String compoundLine = "> " + String(c.compound);
    tft.drawString(compoundLine, 112, y);
    y += tft.fontHeight() + 4;
  }

  if (strlen(c.note) > 0) {
    tft.setTextColor(AMBER, PAPER);
    tft.drawString(c.note, 112, y);
  }

  if (i + 1 < ROUTE_LEN) {
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(INK, PAPER);
    tft.setTextDatum(TL_DATUM);
    char hint[24];
    snprintf(hint, sizeof(hint), "next +%.1f", route[i + 1].distFromPrev);
    tft.drawString(hint, 8, 202);
  }

  float rel = c.cumulative - offsetMi;
  char mi[10];
  snprintf(mi, sizeof(mi), "%.1f", rel);
  const int numberTopY = 182;
  tft.setTextDatum(TR_DATUM);

  tft.setFreeFont(&FreeSansBold9pt7b);
  int miLabelH = tft.fontHeight();
  tft.setTextColor(INK, PAPER);
  tft.drawString("mi", 314, numberTopY - miLabelH - 2);

  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.drawString(mi, 314, numberTopY);
  tft.setTextDatum(TL_DATUM);

  Serial.print("Showing cue ");
  Serial.print(i + 1);
  Serial.print("/");
  Serial.print(ROUTE_LEN);
  Serial.print(": ");
  Serial.println(c.text);
}

void edgeFlash() {
  tft.invertDisplay(true);
  delay(90);
  tft.invertDisplay(false);
  delay(80);
  tft.invertDisplay(true);
  delay(90);
  tft.invertDisplay(false);
}

// ---- shared full-width banner — replaces the various narrow
// hand-sized message boxes that were prone to text overflowing them
// (the original bug report this revision fixes). Two overloads
// instead of a default argument, to avoid any uncertainty about how
// Arduino's automatic function-prototype generation handles default
// parameters.
void showBanner(const char* line1, const char* line2) {
  int h = line2 ? 70 : 50;
  int y = 120 - h / 2;
  tft.fillRect(0, y, 320, h, INK);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(MC_DATUM);
  if (line2) {
    tft.drawString(line1, 160, y + 24);
    tft.drawString(line2, 160, y + 48);
  } else {
    tft.drawString(line1, 160, y + h / 2);
  }
  tft.setTextDatum(TL_DATUM);
}
void showBanner(const char* line1) {
  showBanner(line1, nullptr);
}

// ---- WiFi: two networks tried in sequence, sync-scoped (connects
// only when actually syncing, powers off after — see header comment
// for why this changed back from the always-on boot-time version).
bool tryWiFiNetwork(const char* ssid, const char* password, unsigned long timeoutMs) {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  // Fully reset the driver's internal state before each attempt. A
  // failed/timed-out PREVIOUS attempt can leave the ESP32's WiFi
  // driver still internally "connecting" even after our own timeout
  // gives up waiting on it — the next WiFi.begin() then gets
  // rejected at the driver level ("sta is connecting, cannot set
  // config") before it ever really tries the new network. This is a
  // well-documented ESP32 driver quirk, not specific to this SSID —
  // confirmed against multiple independent reports, same fix each
  // time: disconnect + brief settle before every begin().
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  return false;
}

bool connectWiFiWithFallback() {
  showBanner("Trying home WiFi...");
  if (tryWiFiNetwork(WIFI_SSID_1, WIFI_PASSWORD_1, 8000)) return true;

  showBanner("Trying hotspot...");
  if (tryWiFiNetwork(WIFI_SSID_2, WIFI_PASSWORD_2, 8000)) return true;

  Serial.println("Both WiFi networks failed.");
  return false;
}

void shutdownWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi powered off.");
}

String urlEncodePath(const String &path) {
  String out;
  for (size_t i = 0; i < path.length(); i++) {
    char c = path[i];
    bool safe = isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' ||
                c == '~' || c == '/';
    if (safe) {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

String localFileNameFor(const String &remotePath) {
  int slash = remotePath.lastIndexOf('/');
  String base = (slash >= 0) ? remotePath.substring(slash + 1) : remotePath;
  String out = "/route_";
  for (size_t i = 0; i < base.length(); i++) {
    char c = base[i];
    out += (c == ' ') ? '_' : c;
  }
  return out;
}

#define MANIFEST_PATH "/sync_manifest.json"

bool loadManifest(JsonDocument &manifest) {
  if (!LittleFS.exists(MANIFEST_PATH)) {
    Serial.println("No existing manifest — first sync.");
    return false;
  }
  fs::File f = LittleFS.open(MANIFEST_PATH, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(manifest, f);
  f.close();
  if (err) {
    Serial.print("Manifest parse failed, treating as empty: ");
    Serial.println(err.c_str());
    return false;
  }
  return true;
}

void saveManifest(JsonDocument &manifest) {
  fs::File f = LittleFS.open(MANIFEST_PATH, "w");
  if (!f) {
    Serial.println("FAILED to open manifest for writing.");
    return;
  }
  serializeJson(manifest, f);
  f.close();
}

bool downloadFile(const String &remotePath, const String &localPath) {
  String downloadUrl;
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    String metaUrl = String("https://api.github.com/repos/") + GITHUB_OWNER +
                      "/" + GITHUB_REPO + "/contents/" + urlEncodePath(remotePath) +
                      "?ref=" + GITHUB_BRANCH;
    http.begin(client, metaUrl);
    http.addHeader("Accept", "application/vnd.github+json");
    http.addHeader("Authorization", String("Bearer ") + GITHUB_TOKEN);
    http.addHeader("User-Agent", "RollChart-ESP32");
    int code = http.GET();
    if (code != 200) {
      Serial.print("  Contents API failed: ");
      Serial.print(code);
      Serial.print(" (");
      Serial.print(HTTPClient::errorToString(code));
      Serial.println(")");
      http.end();
      return false;
    }
    JsonDocument metaDoc;
    DeserializationError err = deserializeJson(metaDoc, http.getString());
    http.end();
    if (err) {
      Serial.print("  Contents API JSON parse failed: ");
      Serial.println(err.c_str());
      return false;
    }
    downloadUrl = String((const char*)(metaDoc["download_url"] | ""));
    if (downloadUrl.length() == 0) {
      Serial.println("  No download_url in response — skipping.");
      return false;
    }
  }

  Serial.print("  Free heap before download: ");
  Serial.println(ESP.getFreeHeap());

  WiFiClientSecure client2;
  client2.setInsecure();
  HTTPClient http2;
  http2.begin(client2, downloadUrl);
  int code2 = http2.GET();
  if (code2 != 200) {
    Serial.print("  Signed download failed: ");
    Serial.print(code2);
    Serial.print(" (");
    Serial.print(HTTPClient::errorToString(code2));
    Serial.println(")");
    http2.end();
    return false;
  }
  String content = http2.getString();
  http2.end();

  fs::File f = LittleFS.open(localPath, "w");
  if (!f) {
    Serial.println("  FAILED to open local file for writing.");
    return false;
  }
  f.print(content);
  f.close();
  return true;
}

void syncRoutes() {
  if (!connectWiFiWithFallback()) {
    showBanner("No WiFi found", "Check networks & retry");
    delay(1800);
    shutdownWiFi();
    return;
  }

  showBanner("Syncing routes...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String treeUrl = String("https://api.github.com/repos/") + GITHUB_OWNER +
                    "/" + GITHUB_REPO + "/git/trees/" + GITHUB_BRANCH + "?recursive=1";
  http.begin(client, treeUrl);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("Authorization", String("Bearer ") + GITHUB_TOKEN);
  http.addHeader("User-Agent", "RollChart-ESP32");
  int code = http.GET();
  if (code != 200) {
    Serial.print("Tree fetch failed: HTTP ");
    Serial.print(code);
    Serial.print(" (");
    Serial.print(HTTPClient::errorToString(code));
    Serial.println(")");
    showBanner("Sync failed", "Network error");
    delay(1500);
    http.end();
    shutdownWiFi();
    return;
  }
  JsonDocument treeDoc;
  DeserializationError err = deserializeJson(treeDoc, http.getString());
  http.end();
  if (err) {
    Serial.print("Tree JSON parse failed: ");
    Serial.println(err.c_str());
    showBanner("Sync failed", "Bad response");
    delay(1500);
    shutdownWiFi();
    return;
  }

  JsonDocument oldManifest;
  loadManifest(oldManifest);
  JsonDocument newManifest;

  int added = 0, updated = 0, unchanged = 0, failed = 0, deleted = 0;

  for (JsonObject entry : treeDoc["tree"].as<JsonArray>()) {
    const char* type = entry["type"] | "";
    if (strcmp(type, "blob") != 0) continue;
    String path = String((const char*)(entry["path"] | ""));
    if (!path.endsWith(".json")) continue;

    String sha = String((const char*)(entry["sha"] | ""));
    String localPath = localFileNameFor(path);

    const char* oldSha = oldManifest[path]["sha"] | "";
    bool hasOld = oldManifest[path]["sha"].is<const char*>();

    if (hasOld && sha == String(oldSha)) {
      newManifest[path]["sha"] = sha;
      newManifest[path]["local"] = localPath;
      unchanged++;
      continue;
    }

    Serial.print(hasOld ? "Updating: " : "New: ");
    Serial.println(path);
    if (downloadFile(path, localPath)) {
      newManifest[path]["sha"] = sha;
      newManifest[path]["local"] = localPath;
      if (hasOld) updated++; else added++;
    } else {
      failed++;
      if (hasOld) {
        newManifest[path]["sha"] = oldSha;
        newManifest[path]["local"] = oldManifest[path]["local"] | localPath;
      }
    }
  }

  for (JsonPair kv : oldManifest.as<JsonObject>()) {
    if (newManifest[kv.key()].isNull()) {
      const char* localPath = kv.value()["local"] | "";
      if (strlen(localPath) > 0 && LittleFS.exists(localPath)) {
        LittleFS.remove(localPath);
        Serial.print("Deleted (removed from repo): ");
        Serial.println(kv.key().c_str());
        deleted++;
      }
    }
  }

  saveManifest(newManifest);

  Serial.println("--- Sync complete ---");
  Serial.print("Added: "); Serial.println(added);
  Serial.print("Updated: "); Serial.println(updated);
  Serial.print("Unchanged: "); Serial.println(unchanged);
  Serial.print("Deleted: "); Serial.println(deleted);
  Serial.print("Failed: "); Serial.println(failed);

  // On-device result, not a pointer to Serial — this is the actual
  // outcome, using the same counts just logged above.
  char summary[40];
  if (added + updated + deleted == 0) {
    snprintf(summary, sizeof(summary), "Up to date");
  } else {
    snprintf(summary, sizeof(summary), "New:%d  Updated:%d  Removed:%d", added, updated, deleted);
  }
  showBanner(summary);
  delay(1500);

  shutdownWiFi();
}

// ---- generic row hit-test, reused by both Setup and (indirectly) the
// route picker's row band.
int hitRow(int y, int top, int rowH, int count) {
  if (y < top) return -1;
  int row = (y - top) / rowH;
  if (row < 0 || row >= count) return -1;
  return row;
}

// ---- route picker: builds the list for whatever folder we're
// currently looking at. Root shows folders first, then any routes
// sitting directly at root. One level in, shows just the routes in
// that folder — no deeper nesting supported (matches the real repo
// structure; revisit if that ever changes).
void buildPickerList() {
  pickerEntryCount = 0;
  pickerScroll = 0;

  JsonDocument manifest;
  loadManifest(manifest);

  if (currentFolder.length() == 0) {
    String seenFolders[MAX_PICKER_ENTRIES];
    int seenFolderCount = 0;
    for (JsonPair kv : manifest.as<JsonObject>()) {
      String remotePath = String(kv.key().c_str());
      int slash = remotePath.indexOf('/');
      if (slash < 0) continue;
      String folderName = remotePath.substring(0, slash);
      bool seen = false;
      for (int i = 0; i < seenFolderCount; i++) if (seenFolders[i] == folderName) { seen = true; break; }
      if (seen) continue;
      if (seenFolderCount < MAX_PICKER_ENTRIES) seenFolders[seenFolderCount++] = folderName;
      if (pickerEntryCount < MAX_PICKER_ENTRIES) {
        pickerEntries[pickerEntryCount++] = { true, folderName, folderName };
      }
    }
    for (JsonPair kv : manifest.as<JsonObject>()) {
      String remotePath = String(kv.key().c_str());
      const char* localPath = kv.value()["local"] | "";
      if (strlen(localPath) == 0) continue;
      if (remotePath.indexOf('/') >= 0) continue;
      String label = remotePath;
      if (label.endsWith(".json")) label = label.substring(0, label.length() - 5);
      if (pickerEntryCount < MAX_PICKER_ENTRIES) {
        pickerEntries[pickerEntryCount++] = { false, label, String(localPath) };
      }
    }
  } else {
    String prefix = currentFolder + "/";
    for (JsonPair kv : manifest.as<JsonObject>()) {
      String remotePath = String(kv.key().c_str());
      const char* localPath = kv.value()["local"] | "";
      if (strlen(localPath) == 0) continue;
      if (!remotePath.startsWith(prefix)) continue;
      String rest = remotePath.substring(prefix.length());
      if (rest.indexOf('/') >= 0) continue; // deeper nesting not supported
      String label = rest;
      if (label.endsWith(".json")) label = label.substring(0, label.length() - 5);
      if (pickerEntryCount < MAX_PICKER_ENTRIES) {
        pickerEntries[pickerEntryCount++] = { false, label, String(localPath) };
      }
    }
  }
}

#define PICKER_VISIBLE_ROWS 4
#define PICKER_ROW_H 42
#define PICKER_ROW_TOP 26
#define PICKER_ARROW_W 36
#define PICKER_LIST_W (320 - PICKER_ARROW_W)
#define PICKER_BACK_Y (PICKER_ROW_TOP + PICKER_VISIBLE_ROWS * PICKER_ROW_H + 4)
#define PICKER_BACK_H 36
#define GRAY 0xC618

void drawRoutePicker() {
  tft.fillScreen(PAPER);
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  String header = "Routes: /" + currentFolder;
  tft.drawString(header, 6, 5);

  tft.setFreeFont(&FreeSansBold9pt7b);
  for (int i = 0; i < PICKER_VISIBLE_ROWS; i++) {
    int entryIdx = pickerScroll + i;
    int y = PICKER_ROW_TOP + i * PICKER_ROW_H;
    if (entryIdx >= pickerEntryCount) continue;
    bool isFolder = pickerEntries[entryIdx].isFolder;
    // Folders get a shaded fill so they're visually distinct from
    // routes at a glance — no text marker needed.
    if (isFolder) {
      tft.fillRect(6, y, PICKER_LIST_W - 10, PICKER_ROW_H - 6, GRAY);
    }
    tft.drawRect(6, y, PICKER_LIST_W - 10, PICKER_ROW_H - 6, INK);
    tft.setTextColor(INK, isFolder ? GRAY : PAPER);
    tft.setTextDatum(ML_DATUM);
    String label = pickerEntries[entryIdx].label;
    int maxW = PICKER_LIST_W - 24;
    if (tft.textWidth(label) > maxW) {
      while (label.length() > 3 && tft.textWidth(label + "...") > maxW) {
        label = label.substring(0, label.length() - 1);
      }
      label += "...";
    }
    tft.drawString(label, 14, y + (PICKER_ROW_H - 6) / 2);
  }

  bool canUp = pickerScroll > 0;
  bool canDown = (pickerScroll + PICKER_VISIBLE_ROWS) < pickerEntryCount;
  int arrowX = PICKER_LIST_W;
  int upH = (PICKER_VISIBLE_ROWS * PICKER_ROW_H) / 2;
  int downY = PICKER_ROW_TOP + upH;
  int downH = PICKER_VISIBLE_ROWS * PICKER_ROW_H - upH;
  tft.fillRect(arrowX, PICKER_ROW_TOP, PICKER_ARROW_W, upH, canUp ? INK : GRAY);
  tft.fillRect(arrowX, downY, PICKER_ARROW_W, downH, canDown ? INK : GRAY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(PAPER, canUp ? INK : GRAY);
  tft.drawString("^", arrowX + PICKER_ARROW_W / 2, PICKER_ROW_TOP + upH / 2);
  tft.setTextColor(PAPER, canDown ? INK : GRAY);
  tft.drawString("v", arrowX + PICKER_ARROW_W / 2, downY + downH / 2);

  tft.drawRect(6, PICKER_BACK_Y, 320 - 12, PICKER_BACK_H, ACCENT);
  tft.setTextColor(ACCENT, PAPER);
  tft.drawString("Back", 160, PICKER_BACK_Y + PICKER_BACK_H / 2);
  tft.setTextDatum(TL_DATUM);
}

void handlePickerTap(int sx, int sy) {
  Serial.print("Picker tap: sx=");
  Serial.print(sx);
  Serial.print(" sy=");
  Serial.println(sy);

  if (sy >= PICKER_BACK_Y && sy < PICKER_BACK_Y + PICKER_BACK_H) {
    if (currentFolder.length() == 0) {
      mode = MODE_OPTIONS;
      drawOptionsMenu();
    } else {
      currentFolder = "";
      buildPickerList();
      drawRoutePicker();
    }
    return;
  }

  if (sx >= PICKER_LIST_W) {
    int upH = (PICKER_VISIBLE_ROWS * PICKER_ROW_H) / 2;
    if (sy < PICKER_ROW_TOP + upH) {
      if (pickerScroll > 0) { pickerScroll--; drawRoutePicker(); }
    } else if (sy < PICKER_ROW_TOP + PICKER_VISIBLE_ROWS * PICKER_ROW_H) {
      if (pickerScroll + PICKER_VISIBLE_ROWS < pickerEntryCount) { pickerScroll++; drawRoutePicker(); }
    }
    return;
  }

  if (sy >= PICKER_ROW_TOP && sy < PICKER_ROW_TOP + PICKER_VISIBLE_ROWS * PICKER_ROW_H) {
    int rowInView = (sy - PICKER_ROW_TOP) / PICKER_ROW_H;
    int entryIdx = pickerScroll + rowInView;
    if (entryIdx < pickerEntryCount) {
      PickerEntry &e = pickerEntries[entryIdx];
      if (e.isFolder) {
        currentFolder = e.path;
        buildPickerList();
        drawRoutePicker();
      } else {
        Serial.print("Loading route: ");
        Serial.println(e.path);
        if (loadRouteFromFS(e.path.c_str())) {
          idx = 0;
          mode = MODE_CUE;
          drawCue(idx);
        } else {
          Serial.println("Load failed — falling back to error cue.");
          loadFallbackErrorRoute();
          mode = MODE_CUE;
          drawCue(idx);
        }
      }
    }
  }
}

// ---- options menu: 3x2 grid of square-ish tiles.
#define GRID_COLS 3
#define GRID_ROWS 2
#define GRID_MARGIN 6
#define GRID_GAP 6
#define TILE_W ((320 - 2 * GRID_MARGIN - (GRID_COLS - 1) * GRID_GAP) / GRID_COLS)
#define TILE_H ((240 - 2 * GRID_MARGIN - (GRID_ROWS - 1) * GRID_GAP) / GRID_ROWS)

const char* MENU_LABELS[GRID_COLS * GRID_ROWS] = {
  "Sync Now", "Pick Route", "QR Code",
  "Zero Trip", "Setup",     "Back"
};

void drawTileLabel(const char* label, int cx, int cy, int maxW) {
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(MC_DATUM);
  if (tft.textWidth(label) <= maxW) {
    tft.drawString(label, cx, cy);
    return;
  }
  String s(label);
  int splitAt = s.indexOf(' ');
  if (splitAt < 0) { tft.drawString(label, cx, cy); return; }
  String line1 = s.substring(0, splitAt);
  String line2 = s.substring(splitAt + 1);
  int lh = tft.fontHeight() + 2;
  tft.drawString(line1, cx, cy - lh / 2);
  tft.drawString(line2, cx, cy + lh / 2);
}

void drawOptionsMenu() {
  tft.fillScreen(PAPER);
  for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
    int col = i % GRID_COLS, row = i / GRID_COLS;
    int x = GRID_MARGIN + col * (TILE_W + GRID_GAP);
    int y = GRID_MARGIN + row * (TILE_H + GRID_GAP);
    tft.fillRect(x, y, TILE_W, TILE_H, INK);
    tft.setTextColor(PAPER, INK);
    drawTileLabel(MENU_LABELS[i], x + TILE_W / 2, y + TILE_H / 2, TILE_W - 10);
  }
  tft.setTextDatum(TL_DATUM);
}

int hitGrid(int sx, int sy) {
  if (sx < GRID_MARGIN || sy < GRID_MARGIN) return -1;
  int col = (sx - GRID_MARGIN) / (TILE_W + GRID_GAP);
  int row = (sy - GRID_MARGIN) / (TILE_H + GRID_GAP);
  if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) return -1;
  return row * GRID_COLS + col;
}

void runSyncWithOverlay() {
  syncRoutes(); // shows its own progress + result banners throughout
  drawOptionsMenu();
}

void doZeroTrip() {
  offsetMi = route[idx].cumulative;
  showBanner("Trip zeroed", "Reset bike meter too");
  delay(1200);
  mode = MODE_CUE;
  drawCue(idx);
}

#define QR_VERSION 6
void showQRCode() {
  Cue &c = route[idx];
  char url[100];
  int n;
  if (useGoogleMaps) {
    n = snprintf(url, sizeof(url), "https://www.google.com/maps/dir/?api=1&destination=%.6f,%.6f", c.lat, c.lon);
  } else {
    n = snprintf(url, sizeof(url), "https://maps.apple.com/?daddr=%.6f,%.6f", c.lat, c.lon);
  }
  if (n < 0 || n >= (int)sizeof(url)) {
    Serial.println("QR: URL too long, aborting (shouldn't happen for lat/lon).");
    return;
  }

  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(QR_VERSION)];
  qrcode_initText(&qrcode, qrcodeData, QR_VERSION, ECC_LOW, url);

  tft.fillScreen(PAPER);
  int scale = 4;
  int side = qrcode.size * scale;
  int ox = (320 - side) / 2;
  int oy = (240 - side) / 2 - 10;

  for (uint8_t qy = 0; qy < qrcode.size; qy++) {
    for (uint8_t qx = 0; qx < qrcode.size; qx++) {
      uint16_t color = qrcode_getModule(&qrcode, qx, qy) ? INK : PAPER;
      tft.fillRect(ox + qx * scale, oy + qy * scale, scale, scale, color);
    }
  }

  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(INK, PAPER);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Scan for directions - tap to close", 160, oy + side + 16);
  tft.setTextDatum(TL_DATUM);

  Serial.print("QR encoded (");
  Serial.print(useGoogleMaps ? "Google" : "Apple");
  Serial.print("): ");
  Serial.println(url);
  mode = MODE_QR;
}

// ---- Setup: sleep timeout + QR maps provider, persisted via NVS
// (Preferences) so they survive a power cycle. "Reset chart position"
// deliberately omitted — re-selecting your current route from Pick
// Route already does this, so a dedicated button would duplicate an
// existing path rather than fill a real gap.
#define SETUP_ROW_H 44
#define SETUP_ROW_TOP 30

void drawSetupMenu() {
  tft.fillScreen(PAPER);
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Setup", 6, 5);

  tft.setFreeFont(&FreeSansBold9pt7b);
  char line0[32];
  snprintf(line0, sizeof(line0), "Sleep: %ds", sleepTimeoutSec);
  char line1[32];
  snprintf(line1, sizeof(line1), "Maps: %s", useGoogleMaps ? "Google" : "Apple");
  const char* labels[4] = { line0, line1, "Calibrate Touch", "Back" };
  for (int i = 0; i < 4; i++) {
    int y = SETUP_ROW_TOP + i * SETUP_ROW_H;
    uint16_t c = (i == 3) ? ACCENT : INK;
    tft.drawRect(10, y, 300, SETUP_ROW_H - 6, c);
    tft.setTextColor(c, PAPER);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(labels[i], 20, y + (SETUP_ROW_H - 6) / 2);
  }
  tft.setTextDatum(TL_DATUM);
}

// ---- on-device touch calibration: tap two diagonal targets, raw
// readings recorded directly (NOT mapped through calXMin/etc — those
// are exactly what's being determined, so calibration mode
// deliberately bypasses the normal coordinate pipeline entirely
// rather than depending on it). A fixed outward margin is applied
// after the two taps, same principle as every manual calibration
// pass tonight: a real tap consistently lands a bit short of the
// true physical edge.
void drawCalibrateScreen(int step) {
  tft.fillScreen(PAPER);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(INK, PAPER);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(step == 0 ? "Tap the target (1 of 2)" : "Tap the target (2 of 2)", 160, 120);
  tft.setTextDatum(TL_DATUM);

  int tx = (step == 0) ? CAL_PT1_X : CAL_PT2_X;
  int ty = (step == 0) ? CAL_PT1_Y : CAL_PT2_Y;
  tft.drawCircle(tx, ty, 12, ACCENT);
  tft.drawLine(tx - 16, ty, tx + 16, ty, ACCENT);
  tft.drawLine(tx, ty - 16, tx, ty + 16, ACCENT);
}

void handleCalibrateTap() {
  if (calibStep == 0) {
    calibRawX1 = lockedXRaw;
    calibRawY1 = lockedYRaw;
    calibStep = 1;
    drawCalibrateScreen(1);
    return;
  }

  int rawX2 = lockedXRaw;
  int rawY2 = lockedYRaw;

  calXMin = min(calibRawX1, rawX2) - CAL_MARGIN_PX;
  calXMax = max(calibRawX1, rawX2) + CAL_MARGIN_PX;
  calYMin = min(calibRawY1, rawY2) - CAL_MARGIN_PX;
  calYMax = max(calibRawY1, rawY2) + CAL_MARGIN_PX;

  prefs.putInt("calXMin", calXMin);
  prefs.putInt("calXMax", calXMax);
  prefs.putInt("calYMin", calYMin);
  prefs.putInt("calYMax", calYMax);
  prefs.putBool("calDone", true);

  Serial.print("Calibration saved: X[");
  Serial.print(calXMin); Serial.print(","); Serial.print(calXMax);
  Serial.print("]  Y[");
  Serial.print(calYMin); Serial.print(","); Serial.print(calYMax);
  Serial.println("]");

  showBanner("Calibration saved");
  delay(1200);
  calibStep = 0;
  mode = calibReturnMode;
  if (mode == MODE_SETUP) drawSetupMenu();
  else drawCue(idx);
}

void handleSetupTap(int sy) {
  int row = hitRow(sy, SETUP_ROW_TOP, SETUP_ROW_H, 4);
  Serial.print("Setup tap row=");
  Serial.println(row);
  if (row == 0) {
    const int STEPS[] = { 15, 30, 60, 120, 300 };
    int cur = 0;
    for (int i = 0; i < 5; i++) if (STEPS[i] == sleepTimeoutSec) cur = i;
    sleepTimeoutSec = STEPS[(cur + 1) % 5];
    prefs.putInt("sleepSec", sleepTimeoutSec);
    drawSetupMenu();
  } else if (row == 1) {
    useGoogleMaps = !useGoogleMaps;
    prefs.putBool("useGoogle", useGoogleMaps);
    drawSetupMenu();
  } else if (row == 2) {
    calibReturnMode = MODE_SETUP;
    calibStep = 0;
    mode = MODE_CALIBRATE;
    drawCalibrateScreen(0);
  } else if (row == 3) {
    mode = MODE_OPTIONS;
    drawOptionsMenu();
  }
}

void handleOptionsTap(int sx, int sy) {
  int tile = hitGrid(sx, sy);
  Serial.print("Options tap: sx=");
  Serial.print(sx);
  Serial.print(" sy=");
  Serial.print(sy);
  Serial.print(" -> tile=");
  Serial.println(tile);
  switch (tile) {
    case 0: runSyncWithOverlay(); break;
    case 1: currentFolder = ""; buildPickerList(); mode = MODE_PICKER; drawRoutePicker(); break;
    case 2: showQRCode(); break;
    case 3: doZeroTrip(); break;
    case 4: mode = MODE_SETUP; drawSetupMenu(); break;
    case 5: mode = MODE_CUE; drawCue(idx); break;
    default: break;
  }
}

void handleLongPress() {
  if (mode == MODE_CUE) {
    mode = MODE_OPTIONS;
    drawOptionsMenu();
  } else {
    mode = MODE_CUE;
    drawCue(idx);
  }
}

void nextCue() {
  if (idx < ROUTE_LEN - 1) {
    idx++;
    drawCue(idx);
  } else {
    Serial.println("Already at last cue -> edge flash");
    edgeFlash();
  }
}

void prevCue() {
  if (idx > 0) {
    idx--;
    drawCue(idx);
  } else {
    Serial.println("Already at first cue -> edge flash");
    edgeFlash();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Roll Chart v1 ===");

  prefs.begin("rollchart", false);
  sleepTimeoutSec = prefs.getInt("sleepSec", 30);
  useGoogleMaps = prefs.getBool("useGoogle", false);
  bool calDone = prefs.getBool("calDone", false);
  calXMin = prefs.getInt("calXMin", calXMin);
  calXMax = prefs.getInt("calXMax", calXMax);
  calYMin = prefs.getInt("calYMin", calYMin);
  calYMax = prefs.getInt("calYMax", calYMax);
  Serial.print("Loaded settings: sleep=");
  Serial.print(sleepTimeoutSec);
  Serial.print("s  maps=");
  Serial.print(useGoogleMaps ? "Google" : "Apple");
  Serial.print("  calDone=");
  Serial.println(calDone ? "yes" : "no");

  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS.begin() FAILED — check Partition Scheme.");
    loadFallbackErrorRoute();
  } else {
    loadNoRouteState(); // no demo route to seed/load anymore — starts
                         // here until Sync + Pick Route loads something
  }

  tft.init();
  tft.setRotation(1);
  touch.begin();
  lastActivityMs = millis();

  if (!calDone) {
    // Genuinely fresh unit — no saved calibration at all. Goes
    // straight into the calibration flow instead of trusting
    // whatever numbers happen to be compiled in for a screen that
    // was never actually calibrated for THIS panel.
    calibReturnMode = MODE_CUE;
    calibStep = 0;
    mode = MODE_CALIBRATE;
    drawCalibrateScreen(0);
  } else {
    drawCue(idx);
  }

  // WiFi stays OFF until Sync Now is tapped — see syncRoutes().
}

void loop() {
  auto p = touch.getTouch();
  bool touching = p.zRaw > 200;

  if (touching) lastActivityMs = millis();

  // Auto-sleep: only when no press is currently active, so the
  // backlight never cuts out from under a finger mid-tap.
  if (!asleep && !pressActive &&
      millis() - lastActivityMs > (unsigned long)sleepTimeoutSec * 1000UL) {
    asleep = true;
    digitalWrite(TFT_BL_PIN, LOW);
    Serial.println("Sleeping (backlight off).");
  }

  if (touching && pressActive && !positionLocked &&
      millis() - pressStart >= SETTLE_MS) {
    lockedXRaw = p.xRaw;
    lockedYRaw = p.yRaw;
    positionLocked = true;
  }

  if (touching && !pressActive) {
    pressActive = true;
    longPressFired = false;
    positionLocked = false;
    pressStart = millis();
    wokeThisPress = asleep;
    if (asleep) {
      asleep = false;
      digitalWrite(TFT_BL_PIN, HIGH);
      Serial.println("Wake tap (no action).");
    }
  } else if (touching && pressActive) {
    if (!wokeThisPress && !longPressFired && millis() - pressStart >= LONG_PRESS_MS) {
      longPressFired = true;
      handleLongPress();
    }
  } else if (!touching && pressActive) {
    unsigned long heldFor = millis() - pressStart;
    pressActive = false;
    if (!wokeThisPress && !longPressFired && heldFor >= MIN_PRESS_MS) {
      int sx = constrain(map(lockedXRaw, calXMin, calXMax, 0, 320), 0, 319);
      int sy = constrain(map(lockedYRaw, calYMin, calYMax, 0, 240), 0, 239);

      if (mode == MODE_CUE) {
        float fx = sx / 320.0;
        if (fx < 0.33) {
          Serial.println("Zone tap: LEFT (prev)");
          prevCue();
        } else {
          Serial.println("Zone tap: RIGHT (next)");
          nextCue();
        }
      } else if (mode == MODE_OPTIONS || mode == MODE_PICKER || mode == MODE_SETUP) {
        tft.fillCircle(sx, sy, 5, ACCENT);
        tft.drawCircle(sx, sy, 8, ACCENT);
        delay(180);
        if (mode == MODE_OPTIONS) handleOptionsTap(sx, sy);
        else if (mode == MODE_PICKER) handlePickerTap(sx, sy);
        else handleSetupTap(sy);
      } else if (mode == MODE_QR) {
        mode = MODE_CUE;
        drawCue(idx);
      } else if (mode == MODE_CALIBRATE) {
        // No crosshair-at-computed-position here on purpose — sx/sy
        // above are meaningless during calibration (they're computed
        // from the very values this mode is in the middle of
        // determining). handleCalibrateTap() reads lockedXRaw/
        // lockedYRaw directly instead.
        handleCalibrateTap();
      }
    }
  }
}
