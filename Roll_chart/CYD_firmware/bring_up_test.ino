/*
  CYD Bring-Up Test — Roll Chart Project
  ---------------------------------------
  Purpose: prove the display, touch panel, and onboard flash storage
  work BEFORE any cue/route logic is written. Three isolated stages,
  toggled by TEST_TOUCH / TEST_LFS below. Display and touch are already
  confirmed working on your unit; LittleFS is the new one.

  Storage decision: SD card was dropped after repeated mount failures
  that survived fixing the real underlying SPI-peripheral conflict —
  likely a bad or incompatible card, not worth further debugging time.
  Route storage moves to onboard flash via LittleFS instead. Unlike SD,
  this needs no extra wiring, no card to seat, and no second SPI bus to
  reason about — it's a filesystem living on the same flash chip the
  firmware itself runs from.

  IMPORTANT — Partition Scheme: LittleFS needs actual flash space set
  aside for it. In the Arduino IDE, check Tools > Partition Scheme and
  pick one that includes filesystem space (anything with "SPIFFS" in
  its name reserves this correctly — LittleFS and SPIFFS share the same
  underlying partition slot despite the differing name). If you're
  using a scheme with no FS partition at all, LittleFS.begin() will
  fail immediately no matter what the code does.

  ── WHY TOUCH USES A DIFFERENT LIBRARY THAN THE FIRST VERSION ─────────
  The classic ESP32 only has TWO usable hardware SPI peripherals. This
  board wires display, touch, and (formerly) SD to three separate sets
  of pins, but that doesn't create three independent hardware buses —
  TFT_eSPI claims one hardware peripheral internally, which used to
  leave touch and SD fighting over the single remaining one. The fix:
  touch runs over "bit-banged" software SPI (XPT2046_Bitbang) instead
  of hardware SPI. Confirmed against this board's own community
  troubleshooting doc (github.com/witnessmenow/ESP32-Cheap-Yellow-
  Display/blob/main/TROUBLESHOOTING.md) and discussion #88 there.

  ── REQUIRED SETUP BEFORE THIS COMPILES ──────────────────────────────
  1. Board: install "esp32" in Boards Manager (by Espressif). Select
     board "ESP32 Dev Module" once installed — NOT any C6/S3/other
     variant, the CYD uses the classic ESP32 (WROOM-32).

  2. Libraries (Library Manager):
       - TFT_eSPI              (Bodmer)
       - XPT2046_Bitbang_Slim  (TheNitek / Claus Naveke)
     If you also have a plain "XPT2046_Bitbang" library installed
     (not "_Slim"), remove it or rename its folder — Arduino will
     silently pick whichever one it finds first if both expose a file
     named XPT2046_Bitbang.h, and only "_Slim" has the xRaw/yRaw/zRaw
     API this sketch uses.

  3. TFT_eSPI User_Setup.h — THIS IS THE STEP THAT TRIPS EVERYONE UP.
     TFT_eSPI needs to know which ESP32 pins the CYD wired to the
     display. Using the library's default config gives a black screen
     with no error message.
       - Search "CYD TFT_eSPI User_Setup ESP32-2432S028R" and use the
         community-maintained setup file for this exact board.
       - Replace the contents of:
           <ArduinoLibraries>/TFT_eSPI/User_Setup.h
         with that file (back up the original first).

  4. Touch calibration below (TS_MINX etc.) was measured against real
     corner taps on your actual unit — confirmed working.

  5. LittleFS needs no separate library install — it ships built into
     the ESP32 Arduino core. Just the partition scheme check above.
  ──────────────────────────────────────────────────────────────────────
*/
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <LittleFS.h>

// ---- toggle which stage runs -------------------------------------------
#define TEST_TOUCH 1
#define TEST_LFS   1

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

// Software (bit-banged) SPI for touch — this is the actual fix. No
// SPIClass object needed here at all; the library toggles these pins
// directly in code rather than using a hardware SPI peripheral.
XPT2046_Bitbang touch(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);

// Roll chart palette — same convention as the web tools, so anything
// that looks right here will look right in the cue renderer later.
#define INK   0x18E3   // dark navy, approximates #1a1a2e in RGB565
#define PAPER 0xF79E   // cream, approximates #f5f2e8 in RGB565
#define ACCENT 0xC186  // red, approximates #c0392b in RGB565

#if TEST_LFS
// Recursively list what's already on the filesystem — mostly useful
// after a few test runs to confirm old test files got cleaned up, and
// later on, to confirm real route JSONs are actually visible.
void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  fs::File root = fs.open(dirname);
  if (!root || !root.isDirectory()) {
    Serial.println("  (could not open directory)");
    return;
  }
  fs::File file = root.openNextFile();
  while (file) {
    for (uint8_t i = 0; i < (2 - levels); i++) Serial.print("  ");
    if (file.isDirectory()) {
      Serial.print("  [DIR] ");
      Serial.println(file.name());
      if (levels) listDir(fs, file.path(), levels - 1);
    } else {
      Serial.print("  ");
      Serial.print(file.name());
      Serial.print("  (");
      Serial.print(file.size());
      Serial.println(" bytes)");
    }
    file = root.openNextFile();
  }
}

// Proves LittleFS isn't just mounted but actually writable, and that a
// JSON-shaped payload survives a full write + close + reopen + read
// cycle intact — the real operation the route library depends on.
void writeReadTest() {
  const char *path = "/roll_chart_test.txt";
  const char *payload = "{\"test\":\"roll chart LittleFS bring-up\",\"ok\":true}";

  Serial.println("Write/read test:");
  fs::File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.println("  FAILED to open file for writing.");
    return;
  }
  f.print(payload);
  f.close();

  f = LittleFS.open(path, "r");
  if (!f) {
    Serial.println("  FAILED to reopen file for reading.");
    return;
  }
  String readBack = f.readString();
  f.close();
  LittleFS.remove(path); // clean up after ourselves

  if (readBack == payload) {
    Serial.println("  PASSED — wrote and read back an exact match.");
  } else {
    Serial.print("  MISMATCH — wrote: ");
    Serial.println(payload);
    Serial.print("             read:  ");
    Serial.println(readBack);
  }
}
#endif

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
  touch.begin();
  Serial.println("Touch initialized. Tap the screen — raw coordinates will print below.");
#endif

#if TEST_LFS
  // ---- Stage 3: onboard flash storage (LittleFS) ----
  // true = format automatically if no filesystem is found yet. On a
  // brand new board (or after changing partition scheme) there is no
  // LittleFS partition initialized, so the FIRST boot after enabling
  // this will format it — that's expected, not a failure. Subsequent
  // boots mount the already-formatted partition normally.
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS.begin() FAILED.");
    Serial.println("Checklist: Tools > Partition Scheme includes a");
    Serial.println("filesystem partition (look for \"SPIFFS\" in the name).");
  } else {
    Serial.println("LittleFS mounted OK.");
    Serial.print("Total: ");
    Serial.print(LittleFS.totalBytes() / 1024);
    Serial.print(" KB   Used: ");
    Serial.print(LittleFS.usedBytes() / 1024);
    Serial.println(" KB");

    Serial.println("Root directory contents:");
    listDir(LittleFS, "/", 1);

    writeReadTest();
  }
#endif
}

void loop() {
#if TEST_TOUCH
  // getTouch() returns a struct with x/y (library-calibrated) and
  // xRaw/yRaw/zRaw (raw ADC values). Using auto here deliberately —
  // avoids hardcoding the exact struct type name from a library I
  // can't compile against myself.
  auto p = touch.getTouch();

  // No confirmed "not touched" sentinel for this library from what I
  // could verify, so this uses a pressure threshold instead — the same
  // approach that worked for the previous library. TOUCH_THRESHOLD is
  // a starting guess: watch the zRaw values printed below while NOT
  // touching vs firmly touching the screen, and adjust this constant
  // to sit cleanly between those two ranges.
  const int TOUCH_THRESHOLD = 200;
  if (p.zRaw > TOUCH_THRESHOLD) {
    Serial.print("touch raw: x=");
    Serial.print(p.xRaw);
    Serial.print("  y=");
    Serial.print(p.yRaw);
    Serial.print("  zRaw=");
    Serial.println(p.zRaw);

    // Same calibration constants as before — carried over as a starting
    // point, but re-verify against real corner taps since this library
    // may not report the same orientation as the old one did (see note
    // 6 at the top of this file).
    int sx = constrain(map(p.xRaw, TS_MINX, TS_MAXX, 0, 320), 0, 319);
    int sy = constrain(map(p.yRaw, TS_MINY, TS_MAXY, 0, 240), 0, 239);
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
