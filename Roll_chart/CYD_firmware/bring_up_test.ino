/*
  CYD Bring-Up Test — Roll Chart Project
  ---------------------------------------
  Purpose: prove the display and touch panel work BEFORE any cue/route
  logic is written. Two isolated stages, toggled by TEST_TOUCH below,
  so a problem in one system doesn't get confused with a problem in
  the other.

  ── REQUIRED SETUP BEFORE THIS COMPILES ──────────────────────────────
  1. Board: install "esp32" in Boards Manager (by Espressif). Select
     board "ESP32 Dev Module" once installed.

  2. Libraries (Library Manager):
       - TFT_eSPI          (Bodmer)
       - XPT2046_Touchscreen (Paul Stoffregen)

  3. TFT_eSPI User_Setup.h — THIS IS THE STEP THAT TRIPS EVERYONE UP.
     TFT_eSPI needs to know which ESP32 pins the CYD wired to the
     display. Using the library's default config gives a black screen
     with no error message.
       - Search "CYD TFT_eSPI User_Setup ESP32-2432S028R" and use the
         community-maintained setup file for this exact board.
       - Replace the contents of:
           <ArduinoLibraries>/TFT_eSPI/User_Setup.h
         with that file (back up the original first).
     Do this before opening this sketch, or you'll be debugging two
     unknowns (your code + wrong pin config) at once.

  4. The touch pin numbers below (XPT2046_*) are the commonly-published
     pinout for this board family. If your specific listing/revision
     differs, cross-check against whatever community pinout thread
     you used for step 3 — I have not been able to verify these
     against your exact hardware since I don't have the board.
  ──────────────────────────────────────────────────────────────────────
*/

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ---- toggle which stage runs -------------------------------------------
// Start with 0: prove the screen alone works first.
// Once that's confirmed, flip to 1 to bring up touch.
#define TEST_TOUCH 1

// ---- commonly published XPT2046 touch pins for ESP32-2432S028R --------
// Verify these against your board's community pinout before trusting them.
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ---- touch calibration, measured from real corner taps ----------------
// No axis swap or inversion needed: raw X increases left->right and raw Y
// increases top->bottom, matching screen orientation at rotation(1).
// Small margin added inside the measured extremes since corner taps with
// a stylus tend to land just shy of the true physical edge.
#define TS_MINX 250
#define TS_MAXX 3700
#define TS_MINY 380
#define TS_MAXY 3750

TFT_eSPI tft = TFT_eSPI();

// No bus name (VSPI/HSPI) passed here on purpose — those constants were
// removed in newer ESP32 Arduino cores (3.x) and this form works across
// both old and new core versions. Pins are assigned explicitly below in
// touchSPI.begin() instead of via the constructor.
SPIClass touchSPI;
XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);

// Roll chart palette — same convention as the web tools, so anything
// that looks right here will look right in the cue renderer later.
#define INK   0x18E3   // dark navy, approximates #1a1a2e in RGB565
#define PAPER 0xF79E   // cream, approximates #f5f2e8 in RGB565
#define ACCENT 0xC186  // red, approximates #c0392b in RGB565

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== CYD Bring-Up Test ===");

  // ---- Stage 1: display ----
  tft.init();

  // Landscape orientation. The CYD's USB-C port can end up on the left
  // or right depending on rotation value — if the test pattern below
  // renders sideways or upside-down, try 1 or 3 here.
  tft.setRotation(1);

  drawTestScreen();
  Serial.println("Display initialized. If you see a test pattern, screen wiring is good.");

#if TEST_TOUCH
  // ---- Stage 2: touch ----
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);
  Serial.println("Touch initialized. Tap the screen — raw coordinates will print below.");
#endif
}

void loop() {
#if TEST_TOUCH
  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    Serial.print("touch raw: x=");
    Serial.print(p.x);
    Serial.print("  y=");
    Serial.print(p.y);
    Serial.print("  z(pressure)=");
    Serial.println(p.z);

    // Draw a small crosshair at the RAW coordinate scaled roughly onto
    // the 320x240 screen just so you get a visual too. This mapping is
    // NOT calibrated — raw touch coordinates rarely match screen pixels
    // 1:1 straight out of the box. That calibration is the next step
    // after this test passes, not something to solve here.
    // Calibrated mapping (see TS_MINX/MAXX/MINY/MAXY above), clamped so a
    // touch just outside the measured corners doesn't fly off-screen.
    int sx = constrain(map(p.x, TS_MINX, TS_MAXX, 0, 320), 0, 319);
    int sy = constrain(map(p.y, TS_MINY, TS_MAXY, 0, 240), 0, 239);
    tft.fillCircle(sx, sy, 4, ACCENT);

    delay(80); // crude debounce so one touch doesn't flood Serial
  }
#else
  // Stage 1 only: nothing to do, the test pattern is already drawn.
  delay(1000);
#endif
}

// Draws a static test pattern that exercises fill, stroke, text, and
// the same ink/paper/accent palette the roll chart screens will use —
// so a working test here means the actual cue renderer's drawing calls
// will work too, not just that *some* pixels lit up.
void drawTestScreen() {
  tft.fillScreen(PAPER);

  // top bar, like the real cue screen will have
  tft.fillRect(0, 0, 320, 24, INK);
  tft.setTextColor(PAPER, INK);
  tft.setTextSize(1);
  tft.setCursor(6, 8);
  tft.print("CYD BRING-UP TEST");
  tft.setCursor(280, 8);
  tft.print("1/1");

  // glyph tile border, same box the roll chart glyph will sit in
  tft.drawRect(8, 34, 96, 96, INK);

  // a simple arrow inside it, hand-drawn with primitives — this is the
  // same drawing style (lines + arrowhead) the real glyph renderer uses
  int cx = 56, cy = 82, r = 34;
  tft.drawLine(cx, cy + r, cx, cy - r, INK);
  tft.drawLine(cx, cy - r, cx - 14, cy - r + 14, INK);
  tft.drawLine(cx, cy - r, cx + 14, cy - r + 14, INK);

  // road text placeholder
  tft.setTextColor(INK, PAPER);
  tft.setTextSize(3);
  tft.setCursor(112, 50);
  tft.print("NC-268 W");

  // big mileage placeholder, bottom right — the number that matters most
  tft.setTextSize(4);
  tft.setCursor(190, 190);
  tft.print("12.3");
  tft.setTextSize(2);
  tft.setCursor(295, 195);
  tft.print("mi");

  // color bars along the very bottom — quick visual check that RGB565
  // colors aren't swapped (a common symptom of a wrong display driver
  // setting in User_Setup.h is red/blue channels being reversed)
  int barW = 320 / 6;
  uint16_t bars[] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_CYAN, TFT_MAGENTA };
  for (int i = 0; i < 6; i++) {
    tft.fillRect(i * barW, 232, barW, 8, bars[i]);
  }
}
