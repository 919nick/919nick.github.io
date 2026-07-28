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

#define WIFI_SSID     "SSID"
#define WIFI_PASSWORD "PW"
#define GITHUB_OWNER  "Name"
#define GITHUB_REPO   "Repo"
#define GITHUB_TOKEN  "Token Goes Here"
#define GITHUB_BRANCH "main" 

// ---- touch pins + calibration (confirmed working) -----------------------
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define TS_MINX 250
#define TS_MAXX 3700
#define TS_MINY 380
#define TS_MAXY 3750

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

// ---- long-press now runs the real connectivity test instead of the
// old stub message — same detection logic, pointed at something real.
void showLongPressStub() {
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
  drawCue(idx); // redraw the real cue underneath
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


// ---- touch state (was accidentally dropped during reassembly of this
// sketch from the touch test + JSON loader — loop() needs these) --------
unsigned long pressStart = 0;
bool pressActive = false;
bool longPressFired = false;
int lastTouchXRaw = 0;
const unsigned long LONG_PRESS_MS = 600;
const unsigned long MIN_PRESS_MS = 45; // phantom-touch filter

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

  // Capture position continuously WHILE pressed — at the instant of
  // release, touching is already false and the library's reported
  // coordinates at that exact moment aren't something I can vouch
  // for. Using the last known-good position from during the press
  // avoids depending on that.
  if (touching) lastTouchXRaw = p.xRaw;

  if (touching && !pressActive) {
    // press just started
    pressActive = true;
    longPressFired = false;
    pressStart = millis();
  } else if (touching && pressActive) {
    // held — check for long-press threshold
    if (!longPressFired && millis() - pressStart >= LONG_PRESS_MS) {
      longPressFired = true;
      showLongPressStub();
    }
  } else if (!touching && pressActive) {
    // release
    unsigned long heldFor = millis() - pressStart;
    pressActive = false;
    if (!longPressFired && heldFor >= MIN_PRESS_MS) {
      // genuine short tap — dispatch by screen zone, using the position
      // captured during the press, not read fresh after release
      int sx = constrain(map(lastTouchXRaw, TS_MINX, TS_MAXX, 0, 320), 0, 319);
      float fx = sx / 320.0;
      if (fx < 0.33) {
        Serial.println("Zone tap: LEFT (prev)");
        prevCue();
      } else {
        Serial.println("Zone tap: RIGHT (next)");
        nextCue();
      }
    }
    // if heldFor < MIN_PRESS_MS, silently ignored (phantom touch)
  }
}
