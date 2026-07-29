/*
  Roll Chart — WiFi + GitHub API Connectivity Test
  ----------------------------------------------------------------
  Purpose: prove WiFi connects and the GitHub Contents API responds
  correctly, in ISOLATION from the settings menu / on-device WiFi
  entry / actual file sync logic — all of which are separate, later
  stages. This bundles enough new things already (WiFi, HTTPS/TLS,
  GitHub's auth header format, a new JSON response shape) that
  layering UI on top of it before it's proven would make any bug
  hard to isolate.

  Trigger: long-press now runs the connectivity test instead of the
  old stub message — reuses the exact same proven long-press
  detection, just pointed at something real.

  Route rendering/loading/touch below is UNCHANGED from the JSON
  test. Needs glyphs.h in the SAME sketch folder.

  ── CREDENTIALS: FILL THESE IN YOURSELF, DON'T SHARE THEM ────────────
  WiFi and GitHub credentials below are placeholders. Fill in your
  own before flashing. Once the settings-menu stage exists, WiFi
  becomes on-device entry and only the GitHub repo/token stay as
  compile-time constants (per the earlier design decision) — this
  stage hardcodes WiFi too, temporarily, just to isolate the network
  layer from the not-yet-built credential-entry UI.

  ── VERIFIED AGAINST CURRENT GITHUB DOCS (NOT GUESSED) ────────────────
  - Auth header: "Authorization: Bearer <token>" — current format for
    fine-grained personal access tokens.
  - "User-Agent" header is REQUIRED — GitHub rejects requests without
    one outright. Easy to miss, confirmed directly from their docs.
  - The X-GitHub-Api-Version header is optional (defaults to latest
    if omitted) — deliberately left out rather than guessing a
    version string that might be stale.
  - TLS: uses WiFiClientSecure::setInsecure(), which skips certificate
    validation. That's a real security tradeoff (no protection against
    a MITM on a hostile network) — a reasonable choice for a hobby
    device pulling non-sensitive ride routes over trusted home WiFi,
    but worth knowing it's a deliberate shortcut, not an oversight.
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
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "glyphs.h"
#include "rc_qrcode.h" // ricmoo's QRCode library, vendored locally as
                        // rc_qrcode.h/.c in this sketch folder — NOT the
                        // Library Manager install. ESP32's core bundles
                        // its own internal component also named
                        // qrcode.h (used for Espressif's WiFi-
                        // provisioning examples), and <angle-bracket>
                        // search order was picking that one instead of
                        // the installed library — a documented, known
                        // collision (see ricmoo/QRCode issue #35).
                        // "double-quote" includes are guaranteed by the
                        // C/C++ standard to check the sketch's own
                        // folder first, which sidesteps the ambiguity
                        // entirely rather than fighting search order.

// ---- FILL THESE IN — placeholders, not real credentials -----------------
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#define GITHUB_OWNER  ""
#define GITHUB_REPO   ""
#define GITHUB_TOKEN  ""
#define GITHUB_BRANCH "main" 


// ---- touch pins + calibration (confirmed working) -----------------------
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define TS_MINX 250
#define TS_MAXX 3700
// Y range widened based on real edge-tap data (349 top / 3934 bottom
// — both OUTSIDE the old 380/3750 range, which is exactly why a
// center-screen tap was landing on the top menu button instead of
// the middle one). Small margin applied beyond the measured taps,
// same convention as the original X calibration.
#define TS_MINY 330
#define TS_MAXY 3950

TFT_eSPI tft = TFT_eSPI();
XPT2046_Bitbang touch(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);

// ---- palette --------------------------------------------------------------
#define INK    0x18E3
#define PAPER  0xF79E
#define ACCENT 0xC186
#define AMBER  0x8AC0

// ---- route data: now loaded at runtime, not compile-time constants ------
#define MAX_CUES 40
struct Cue {
  const char* type;
  const char* text;
  const char* compound;
  const char* note;
  bool verify;
  float distFromPrev;
  float cumulative;
  float lat;   // parsed but unused by the renderer yet — needed for the
  float lon;   // later QR / zero-trip stage, harmless to carry now
};
Cue route[MAX_CUES];
int ROUTE_LEN = 0;
const char* ROUTE_NAME = "Loading...";
int idx = 0;
float offsetMi = 0;

// ---- touch state — moved here (was previously declared right before
// setup(), which is TOO LATE): handleOptionsTap()/handlePickerTap() are
// defined earlier in the file and reference these directly. Arduino
// auto-generates forward declarations for FUNCTIONS but never for
// global variables, so the compiler needs to have already seen these
// declarations by the time it reaches any function body that uses
// them. Consolidating all globals together up here avoids this class
// of ordering bug entirely, rather than relying on remembering where
// in the file is "early enough."
unsigned long pressStart = 0;
bool pressActive = false;
bool longPressFired = false;
bool positionLocked = false; // NEW: replaces continuous-overwrite capture
int lockedXRaw = 0;
int lockedYRaw = 0; // was lastTouchXRaw/lastTouchYRaw — renamed because
                     // the capture STRATEGY changed, not just the name.
                     // See loop() comment for why.
const unsigned long LONG_PRESS_MS = 600;
const unsigned long MIN_PRESS_MS = 45; // phantom-touch filter
const unsigned long SETTLE_MS = 25; // let the resistive panel's voltage
                                     // stabilize after initial contact
                                     // before trusting a reading

// ---- UI mode: long-press now opens a real menu instead of directly
// triggering sync, per the earlier design decision. Touch dispatch in
// loop() branches on this.
enum AppMode { MODE_CUE, MODE_OPTIONS, MODE_PICKER, MODE_QR, MODE_WIFI_SCAN };
AppMode mode = MODE_CUE;

// Route picker's cached list — built once when entering MODE_PICKER,
// not re-read from the manifest on every touch check.
#define MAX_ROUTE_ENTRIES 4
String pickerLabels[MAX_ROUTE_ENTRIES + 1]; // +1 for the always-present demo route
String pickerPaths[MAX_ROUTE_ENTRIES + 1];
int pickerCount = 0;

// JsonDocument lives for the whole program — see string-lifetime note
// in the header comment above. Never cleared after the initial load.
JsonDocument doc;

const char* DEMO_JSON = R"JSON(
{
  "routeName": "Demo - Danbury Loop",
  "totalMiles": 34.13,
  "cues": [
    { "seq":1, "type":"depart", "text":"NC-8 N / NC-89 W", "distFromPrev":0.00, "cumulative":0.00, "lat":36.4097, "lon":-80.2061 },
    { "seq":2, "type":"left", "text":"NC-268 W", "distFromPrev":6.69, "cumulative":6.69, "lat":36.4419, "lon":-80.3138 },
    { "seq":3, "type":"right", "text":"NC-66 S", "compound":["then quick left onto NC-66 S"], "note":"staggered crossroads", "distFromPrev":3.83, "cumulative":10.52, "lat":36.4653, "lon":-80.3771 },
    { "seq":4, "type":"straight", "text":"NC-268 W", "verify":true, "distFromPrev":0.06, "cumulative":10.58, "lat":36.4655, "lon":-80.3779 },
    { "seq":5, "type":"caution-R", "text":"120 deg right curve", "distFromPrev":4.10, "cumulative":14.68, "lat":36.4922, "lon":-80.4433 },
    { "seq":6, "type":"ramp", "text":"US-52 N", "note":"gas at ramp", "distFromPrev":6.10, "cumulative":20.78, "lat":36.4488, "lon":-80.6003 },
    { "seq":7, "type":"exit", "text":"Exit 18: I-74 W", "distFromPrev":4.35, "cumulative":25.13, "lat":36.4991, "lon":-80.6221 },
    { "seq":8, "type":"sharp-left", "text":"NC-18 S", "distFromPrev":6.90, "cumulative":32.03, "lat":36.5203, "lon":-80.7415 },
    { "seq":9, "type":"arrive", "text":"Arrive", "distFromPrev":2.10, "cumulative":34.13, "lat":36.5305, "lon":-80.7622 }
  ]
}
)JSON";

void seedDemoFileIfMissing(const char* path) {
  if (LittleFS.exists(path)) {
    Serial.println("Route file already exists, using it as-is.");
    return;
  }
  Serial.println("No route file found — writing built-in demo route.");
  fs::File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.println("FAILED to create demo route file.");
    return;
  }
  f.print(DEMO_JSON);
  f.close();
}

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

// A route that failed to load entirely shouldn't leave the device
// hung or showing garbage — this gives it one clearly-labeled error
// cue instead, using the SAME Cue struct/renderer as everything else.
void loadFallbackErrorRoute() {
  route[0] = { "caution-R", "ROUTE LOAD FAILED", "", "check Serial Monitor", false, 0, 0, 0, 0 };
  ROUTE_LEN = 1;
  ROUTE_NAME = "ERROR";
}

void drawGlyph(const char* key, int cx, int cy) {
  // Bitmaps are pure shape data, no color baked in — caution glyphs
  // stay red (ACCENT) to match the warning convention used everywhere
  // else in this project (web tools, simulators); everything else
  // draws in the standard ink color.
  uint16_t color = (strncmp(key, "caution", 7) == 0) ? ACCENT : INK;
  for (int i = 0; i < GLYPH_TABLE_LEN; i++) {
    if (strcmp(GLYPH_TABLE[i].key, key) == 0) {
      tft.drawBitmap(cx - GLYPH_W / 2, cy - GLYPH_H / 2,
                      GLYPH_TABLE[i].bitmap, GLYPH_W, GLYPH_H, color);
      return;
    }
  }
  // Unknown glyph key (typo in route data, or a type not yet added to
  // glyphs.h) — draw a visible placeholder box rather than silently
  // showing nothing, so a bad key is obvious instead of a mystery.
  tft.drawRect(cx - 16, cy - 16, 32, 32, INK);
}

// ---- text wrapping, using real font metrics ------------------------------
// tries a ladder of font sizes (largest first), same idea as before but
// driven by actual font objects instead of integer setTextSize() steps
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
    if (overflowed && fi < ROAD_FONT_COUNT - 1) continue; // try smaller font

    int lineH = tft.fontHeight() + 4;
    tft.setTextDatum(TL_DATUM);
    for (int i = 0; i < lineCount; i++) {
      tft.drawString(lines[i], x, y + i * lineH);
    }
    return lineCount;
  }
  return 0;
}

// ---- main cue draw --------------------------------------------------------
void drawCue(int i) {
  Cue &c = route[i];
  tft.fillScreen(PAPER);
  tft.setTextDatum(TL_DATUM);
  tft.setTextWrap(false, false);

  // top bar
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

  // glyph tile
  tft.drawRect(8, 34, 96, 96, INK);
  tft.drawRect(9, 35, 94, 94, INK); // slightly bolder tile border
  drawGlyph(c.type, 56, 82);

  // VERIFY flag
  int textTop = 44;
  if (c.verify) {
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(ACCENT, PAPER);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("VERIFY", 10, 136);
  }

  // road text, wrapped, real fonts, shrink-to-fit
  tft.setTextColor(INK, PAPER);
  int lines = wrapAndPrint(c.text, 112, textTop, 196);

  // compound sub-line
  int y = textTop + lines * (tft.fontHeight() + 4) + 6;
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextDatum(TL_DATUM);
  if (strlen(c.compound) > 0) {
    tft.setTextColor(INK, PAPER);
    String compoundLine = "> " + String(c.compound);
    tft.drawString(compoundLine, 112, y);
    y += tft.fontHeight() + 4;
  }

  // note line (amber)
  if (strlen(c.note) > 0) {
    tft.setTextColor(AMBER, PAPER);
    tft.drawString(c.note, 112, y);
  }

  // next-leg hint, bottom-left — bumped to a real (bigger, clearer) font
  if (i + 1 < ROUTE_LEN) {
    tft.setFreeFont(&FreeSansBold9pt7b);
    tft.setTextColor(INK, PAPER);
    tft.setTextDatum(TL_DATUM);
    char hint[24];
    snprintf(hint, sizeof(hint), "next +%.1f", route[i + 1].distFromPrev);
    tft.drawString(hint, 8, 202);
  }

  // big mileage, bottom-right — right-aligned via text datum. "mi" label
  // is placed ABOVE the number using the label font's real measured
  // height, not a guessed pixel offset — that guess is what caused the
  // overlap before.
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

// ---- edge flash: brief full-screen color invert, a genuine TFT_eSPI
// built-in (invertDisplay), not a manual redraw trick. Signals "you're
// already at the end of the chart" without adding a page.
void edgeFlash() {
  tft.invertDisplay(true);
  delay(90);
  tft.invertDisplay(false);
  delay(80);
  tft.invertDisplay(true);
  delay(90);
  tft.invertDisplay(false);
}

// ---- WiFi connect, with a visible timeout rather than hanging forever ---
bool connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("WiFi connect FAILED (15s timeout).");
    return false;
  }
}

// ---- percent-encode a path for use in a URL. Real repo testing
// turned up a filename with spaces in it — this handles that and
// other unsafe characters generically rather than special-casing
// just spaces.
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

// ---- local filename for a synced route: flattened to LittleFS root
// with a prefix, regardless of which GitHub subfolder it came from.
// Deliberately NOT mirroring folder structure onto LittleFS — that
// would need directory auto-creation behavior I haven't verified,
// and flattening sidesteps the question entirely. Spaces (seen in
// real testing) are sanitized to underscores.
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

// manifest maps remote path -> {sha, local} so re-syncing only
// downloads what actually changed, and so deletions can be detected
// (a manifest entry whose remote path no longer appears in the repo
// listing means that file was removed upstream).
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

// ---- downloads one file via GitHub's documented two-step path:
// Contents API (authenticated) to get a short-lived signed
// download_url, then a second, UNAUTHENTICATED request to that URL
// for the actual raw bytes. This is GitHub's own recommended pattern
// for fetching file content — deliberately not using
// raw.githubusercontent.com directly, since recent community reports
// say that domain silently ignores auth tokens on private repos
// (returns 404 with no clear explanation) rather than a real, sourced
// answer I could verify.
bool downloadFile(const String &remotePath, const String &localPath) {
  String downloadUrl;

  // Scoped block: forces `client`/`http` to fully destruct and release
  // their TLS buffers before the second connection below is opened.
  // Two WiFiClientSecure objects alive at once is a well-known way to
  // run an ESP32 low enough on RAM that the second TLS handshake
  // fails outright — this avoids that regardless of whether it's the
  // actual cause here.
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
  } // client/http destructed here, TLS memory released

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

// ---- the actual sync: fetch the full repo tree, diff against the
// local manifest, download anything new/changed, delete anything
// that disappeared from the repo (full mirror, per earlier design
// decision — not additive-only).
void syncRoutes() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi, can't sync.");
    return;
  }

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
    http.end();
    return;
  }
  JsonDocument treeDoc;
  DeserializationError err = deserializeJson(treeDoc, http.getString());
  http.end();
  if (err) {
    Serial.print("Tree JSON parse failed: ");
    Serial.println(err.c_str());
    return;
  }

  JsonDocument oldManifest;
  loadManifest(oldManifest);
  JsonDocument newManifest; // rebuilt fresh each sync, see comment below

  int added = 0, updated = 0, unchanged = 0, failed = 0, deleted = 0;

  for (JsonObject entry : treeDoc["tree"].as<JsonArray>()) {
    const char* type = entry["type"] | "";
    if (strcmp(type, "blob") != 0) continue;
    String path = String((const char*)(entry["path"] | ""));
    if (!path.endsWith(".json")) continue; // README.md etc. — not route files

    String sha = String((const char*)(entry["sha"] | ""));
    String localPath = localFileNameFor(path);

    const char* oldSha = oldManifest[path]["sha"] | "";
    bool hasOld = oldManifest[path]["sha"].is<const char*>();

    if (hasOld && sha == String(oldSha)) {
      // unchanged — still record it in the new manifest so it isn't
      // mistaken for "removed" in the deletion pass below
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
      // keep the OLD entry if download failed, so a transient network
      // blip doesn't orphan an otherwise-fine local file
      if (hasOld) {
        newManifest[path]["sha"] = oldSha;
        newManifest[path]["local"] = oldManifest[path]["local"] | localPath;
      }
    }
  }

  // deletion pass: anything in the OLD manifest not carried into the
  // NEW one was removed from the repo — delete the local file too
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
}

// ---- builds the route picker's list: the demo route (always present
// as a safe fallback) plus everything currently in the sync manifest.
// Read once when entering the picker, not per-touch.
void buildRouteList() {
  pickerCount = 0;
  pickerLabels[pickerCount] = "Demo Route";
  pickerPaths[pickerCount] = "/demo_route.json";
  pickerCount++;

  JsonDocument manifest;
  loadManifest(manifest);
  int skipped = 0;
  for (JsonPair kv : manifest.as<JsonObject>()) {
    const char* localPath = kv.value()["local"] | "";
    if (strlen(localPath) == 0) continue;
    if (pickerCount >= MAX_ROUTE_ENTRIES + 1) { skipped++; continue; }
    pickerLabels[pickerCount] = String(kv.key().c_str()); // full repo path as label
    pickerPaths[pickerCount] = String(localPath);
    pickerCount++;
  }
  if (skipped > 0) {
    Serial.print("Route picker: ");
    Serial.print(skipped);
    Serial.println(" synced route(s) not shown — MAX_ROUTE_ENTRIES cap (no scrolling yet).");
  }
}

// ---- generic row hit-test: given a touch Y, a list top, and a fixed
// row height, returns which row index was hit, or -1 for none.
int hitRow(int y, int top, int rowH, int count) {
  if (y < top) return -1;
  int row = (y - top) / rowH;
  if (row < 0 || row >= count) return -1;
  return row;
}

#define ROW_H 34
#define ROW_TOP 28

void drawRoutePicker() {
  tft.fillScreen(PAPER);
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Pick a Route", 6, 5);

  tft.setFreeFont(&FreeSansBold9pt7b);
  for (int i = 0; i < pickerCount; i++) {
    int y = ROW_TOP + i * ROW_H;
    tft.drawRect(10, y, 300, ROW_H - 6, INK);
    tft.setTextColor(INK, PAPER);
    tft.setTextDatum(ML_DATUM);
    String label = pickerLabels[i];
    if (label.length() > 32) label = label.substring(0, 29) + "...";
    tft.drawString(label, 20, y + (ROW_H - 6) / 2);
  }
  int backY = ROW_TOP + pickerCount * ROW_H;
  tft.drawRect(10, backY, 300, ROW_H - 6, ACCENT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ACCENT, PAPER);
  tft.drawString("Back", 160, backY + (ROW_H - 6) / 2);
  tft.setTextDatum(TL_DATUM);
}

void handlePickerTap(int sy) {
  int row = hitRow(sy, ROW_TOP, ROW_H, pickerCount + 1); // +1 = Back row
  Serial.print("Picker tap: rawY=");
  Serial.print(lockedYRaw);
  Serial.print(" sy=");
  Serial.print(sy);
  Serial.print(" -> row=");
  Serial.println(row);
  if (row >= 0 && row < pickerCount) {
    Serial.print("Loading route: ");
    Serial.println(pickerPaths[row]);
    if (loadRouteFromFS(pickerPaths[row].c_str())) {
      idx = 0;
      mode = MODE_CUE;
      drawCue(idx);
    } else {
      Serial.println("Load failed — falling back to error cue.");
      loadFallbackErrorRoute();
      mode = MODE_CUE;
      drawCue(idx);
    }
  } else {
    // Back row, or a tap that missed every row
    mode = MODE_OPTIONS;
    drawOptionsMenu();
  }
}

// ---- options menu: 3x2 grid of square-ish tiles instead of stacked
// rows. Deliberately switched — every earlier row-based screen only
// ever needed a coarse left/right or a handful of stacked bands, but
// tonight's calibration debugging showed exactly how little margin a
// thin row leaves for a gloved, imprecise tap. Tiles this size (~100
// x 110px) give real height AND width to land on, not just width.
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

// Draws a (possibly two-word) label centered in a tile, splitting to
// two lines only if it doesn't fit on one — checked against the
// REAL measured text width, not a guessed character count.
void drawTileLabel(const char* label, int cx, int cy, int maxW) {
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(MC_DATUM);
  if (tft.textWidth(label) <= maxW) {
    tft.drawString(label, cx, cy);
    return;
  }
  String s(label);
  int splitAt = s.indexOf(' ');
  if (splitAt < 0) { tft.drawString(label, cx, cy); return; } // no space to split on
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

// 2D grid hit-test — same forgiving convention as hitRow(): a tap in
// a tile's trailing gap still counts as that tile, maximizing hit
// area for a gloved tap rather than demanding pixel precision.
int hitGrid(int sx, int sy) {
  if (sx < GRID_MARGIN || sy < GRID_MARGIN) return -1;
  int col = (sx - GRID_MARGIN) / (TILE_W + GRID_GAP);
  int row = (sy - GRID_MARGIN) / (TILE_H + GRID_GAP);
  if (col < 0 || col >= GRID_COLS || row < 0 || row >= GRID_ROWS) return -1;
  return row * GRID_COLS + col;
}

void runSyncWithOverlay() {
  tft.fillRect(20, 80, 280, 80, INK);
  tft.drawRect(20, 80, 280, 80, PAPER);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Syncing routes...", 160, 120);
  tft.setTextDatum(TL_DATUM);

  syncRoutes();

  tft.fillRect(20, 80, 280, 80, INK);
  tft.drawRect(20, 80, 280, 80, PAPER);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Done - see Serial", 160, 120);
  tft.setTextDatum(TL_DATUM);
  delay(1200);
  drawOptionsMenu(); // stay in the menu after syncing, not back to the cue
}

// ---- Zero Trip: sets the display offset to the current cue's
// cumulative mileage, matching the rally convention of re-zeroing
// your bike's trip meter at the same moment. Pure logic reusing the
// offsetMi variable and math that's already been part of drawCue()
// since the very first renderer stage — nothing new here except
// actually wiring a button to it.
void doZeroTrip() {
  offsetMi = route[idx].cumulative;
  tft.fillRect(20, 90, 280, 60, INK);
  tft.drawRect(20, 90, 280, 60, PAPER);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Trip zeroed - reset bike meter too", 160, 120);
  tft.setTextDatum(TL_DATUM);
  delay(1200);
  mode = MODE_CUE;
  drawCue(idx);
}

// ---- QR Code: encodes an Apple Maps link to the CURRENT cue's
// coordinates. Version 6 / ECC_LOW gives generous capacity margin
// above any realistic URL length here; the length check below is a
// second, defensive layer — this library has no bounds protection of
// its own, so an oversized input could otherwise corrupt memory
// rather than fail cleanly.
#define QR_VERSION 6
void showQRCode() {
  Cue &c = route[idx];
  char url[100];
  int n = snprintf(url, sizeof(url), "https://maps.apple.com/?daddr=%.6f,%.6f", c.lat, c.lon);
  if (n < 0 || n >= (int)sizeof(url)) {
    Serial.println("QR: URL too long, aborting (this shouldn't happen for lat/lon).");
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

  Serial.print("QR encoded: ");
  Serial.println(url);
  mode = MODE_QR;
}

// ---- Setup is real work — on-device WiFi entry needs network
// scanning, a virtual keyboard, and NVS persistence, none of which
// exist yet. This is a clearly-labeled placeholder, not a silent gap
// or a rushed half-build.
void showSetupStub() {
  tft.fillRect(20, 90, 280, 60, INK);
  tft.drawRect(20, 90, 280, 60, PAPER);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Setup - Coming Soon", 160, 120);
  tft.setTextDatum(TL_DATUM);
  delay(1000);
  drawOptionsMenu();
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
    case 1: buildRouteList(); mode = MODE_PICKER; drawRoutePicker(); break;
    case 2: showQRCode(); break;
    case 3: doZeroTrip(); break;
    case 4: showSetupStub(); break;
    case 5: mode = MODE_CUE; drawCue(idx); break;
    default: break; // tap missed every tile — stay on the menu
  }
}

// ---- long-press: opens the menu from cue view; from anywhere else
// (menu or picker), acts as a universal "back to cue" escape hatch —
// always a way out, regardless of how deep in the UI you are.
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
  Serial.println("=== WiFi + GitHub Connectivity Test ===");

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS.begin() FAILED — check Partition Scheme.");
    loadFallbackErrorRoute();
  } else {
    seedDemoFileIfMissing("/demo_route.json");
    if (!loadRouteFromFS("/demo_route.json")) {
      loadFallbackErrorRoute();
    }
  }

  tft.init();
  tft.setRotation(1);
  touch.begin();
  drawCue(idx);

  connectWiFi(); // blocking, up to 15s — screen already shows the
                  // first cue underneath while this runs
}

void loop() {
  auto p = touch.getTouch();
  bool touching = p.zRaw > 200; // same threshold confirmed against your unit

  // Position is captured ONCE per press, early — not continuously
  // overwritten through to release. A resistive panel is noisiest
  // right as a finger lifts off (contact degrading unevenly), which
  // is exactly what "continuously overwrite until release" was
  // sampling. This instead waits SETTLE_MS for the panel to
  // stabilize after initial contact, takes ONE reading, and locks
  // it — ignoring everything after, including the noisy lift-off
  // moment. Left-right zone taps (cue view) never surfaced this,
  // since a coarse 33/67 split tolerates far more noise than
  // three-way menu row hit-testing does.
  if (touching && pressActive && !positionLocked &&
      millis() - pressStart >= SETTLE_MS) {
    lockedXRaw = p.xRaw;
    lockedYRaw = p.yRaw;
    positionLocked = true;
  }

  if (touching && !pressActive) {
    // press just started
    pressActive = true;
    longPressFired = false;
    positionLocked = false;
    pressStart = millis();
  } else if (touching && pressActive) {
    // held — check for long-press threshold
    if (!longPressFired && millis() - pressStart >= LONG_PRESS_MS) {
      longPressFired = true;
      handleLongPress();
    }
  } else if (!touching && pressActive) {
    // release
    unsigned long heldFor = millis() - pressStart;
    pressActive = false;
    if (!longPressFired && heldFor >= MIN_PRESS_MS) {
      // genuine short tap — dispatch by current UI mode, using the
      // LOCKED early-press position, not a fresh/late sample
      int sx = constrain(map(lockedXRaw, TS_MINX, TS_MAXX, 0, 320), 0, 319);
      int sy = constrain(map(lockedYRaw, TS_MINY, TS_MAXY, 0, 240), 0, 239);

      if (mode == MODE_CUE) {
        float fx = sx / 320.0;
        if (fx < 0.33) {
          Serial.println("Zone tap: LEFT (prev)");
          prevCue();
        } else {
          Serial.println("Zone tap: RIGHT (next)");
          nextCue();
        }
      } else if (mode == MODE_OPTIONS || mode == MODE_PICKER) {
        // Visible crosshair at the COMPUTED position, held briefly
        // before the action fires — kept as general "tap confirmed"
        // feedback (useful with gloves, no tactile click), shortened
        // now that calibration's confirmed solid rather than needing
        // to linger for diagnostic reading.
        tft.fillCircle(sx, sy, 5, ACCENT);
        tft.drawCircle(sx, sy, 8, ACCENT);
        delay(180);
        if (mode == MODE_OPTIONS) handleOptionsTap(sx, sy);
        else handlePickerTap(sy);
      } else if (mode == MODE_QR) {
        // any tap anywhere closes the QR screen — no precise hit
        // target needed, it's a single dismiss action
        mode = MODE_CUE;
        drawCue(idx);
      }
    }
    // if heldFor < MIN_PRESS_MS, silently ignored (phantom touch)
  }
}
