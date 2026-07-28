/*
  Roll Chart — Touch-Driven Advance Test
  ----------------------------------------------------------------
  Purpose: wire REAL taps to next/previous/long-press against the
  same hardcoded demo route the renderer test used — proves the
  interaction model on actual resistive touch timing, not a 3s
  auto-advance timer. Still no storage/network in this stage.

  Needs glyphs.h in the SAME sketch folder (copy it over from the
  renderer test folder — Arduino sketches don't share files across
  folders automatically).

  ── INTERACTION MODEL (locked in from the web simulators) ─────────────
  - Right 2/3 of screen tap = next cue
  - Left 1/3 of screen tap  = previous cue
  - Long-press (~600ms)     = options (stub for now — just confirms
    detection works; the real menu comes in a later stage)
  - Tapping past the first/last cue = edge flash (brief full-screen
    color invert via tft.invertDisplay(), a genuine TFT_eSPI built-in
    rather than a manual redraw flash)
  - 45ms minimum press duration filter — rejects phantom touches from
    vibration, same threshold used throughout the web prototypes

  Touch pins and calibration constants below are carried over directly
  from the bring-up test — already confirmed working and accurate on
  your actual unit, not re-guessed here.
  ──────────────────────────────────────────────────────────────────
*/

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "glyphs.h"

// ---- touch pins (confirmed working, from the bring-up test) ------------
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ---- touch calibration (measured from real corner taps, confirmed) -----
#define TS_MINX 250
#define TS_MAXX 3700
#define TS_MINY 380
#define TS_MAXY 3750

TFT_eSPI tft = TFT_eSPI();
XPT2046_Bitbang touch(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);

// ---- palette (same convention as the web tools) ------------------------
#define INK    0x18E3   // dark navy, approximates #1a1a2e in RGB565
#define PAPER  0xF79E   // cream, approximates #f5f2e8 in RGB565
#define ACCENT 0xC186   // red, approximates #c0392b in RGB565
#define AMBER  0x8AC0   // approximates #8a5a00 in RGB565

// ---- demo route: same data as the web simulators ------------------------
struct Cue {
  const char* type;
  const char* text;
  const char* compound;
  const char* note;
  bool verify;
  float distFromPrev;
  float cumulative;
};

Cue route[] = {
  { "depart",      "NC-8 N / NC-89 W", "",                              "",                 false, 0.00,  0.00  },
  { "left",        "NC-268 W",         "",                              "",                 false, 6.69,  6.69  },
  { "right",       "NC-66 S",          "then quick left onto NC-66 S",  "staggered crossroads", false, 3.83, 10.52 },
  { "straight",    "NC-268 W",         "",                              "",                 true,  0.06,  10.58 },
  { "caution-R",   "120 deg right curve", "",                           "",                 false, 4.10,  14.68 },
  { "ramp",        "US-52 N",          "",                              "gas at ramp",      false, 6.10,  20.78 },
  { "exit",        "Exit 18: I-74 W",  "",                              "",                 false, 4.35,  25.13 },
  { "sharp-left",  "NC-18 S",          "",                              "",                 false, 6.90,  32.03 },
  { "arrive",      "Arrive",           "",                              "",                 false, 2.10,  34.13 }
};
const int ROUTE_LEN = sizeof(route) / sizeof(route[0]);
const char* ROUTE_NAME = "Demo - Danbury Loop";

int idx = 0;
float offsetMi = 0; // trip zero offset, always 0 in this stage

// ============================================================
// ---- glyph drawing: table lookup + drawBitmap, that's the whole thing ----
// All the shape logic now lives in glyphs.h, generated and visually
// verified offline (see chat log / gen_glyphs.py) rather than computed
// live on-device.
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

// ---- long-press stub: confirms detection works without building the
// real options menu yet (that's a later stage — QR, zero-trip, etc.)
void showLongPressStub() {
  tft.fillRect(40, 90, 240, 60, INK);
  tft.drawRect(40, 90, 240, 60, PAPER);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(PAPER, INK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("OPTIONS (stub)", 160, 120);
  tft.setTextDatum(TL_DATUM);
  Serial.println("Long press detected (options menu not built yet)");
  delay(700);
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

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Touch-Driven Advance Test ===");
  Serial.println("Right 2/3 tap = next, left 1/3 = prev, long-press = stub menu.");

  tft.init();
  tft.setRotation(1);
  touch.begin();
  drawCue(idx);
}

// ---- touch state machine --------------------------------------------
unsigned long pressStart = 0;
bool pressActive = false;
bool longPressFired = false;
int lastTouchXRaw = 0;
const unsigned long LONG_PRESS_MS = 600;
const unsigned long MIN_PRESS_MS = 45; // phantom-touch filter

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
