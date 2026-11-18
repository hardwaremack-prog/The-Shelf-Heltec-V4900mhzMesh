// Draws the board's screen pages on a computer with the real U8g2 library and fonts, using the
// firmware's own drawing code (main.cpp's status page and screen_pages.h), for the README pictures.
//   tools/screen-sim/render.sh   ->  docs/screens/*.png
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <algorithm>
#include <sys/time.h>
#include <ctime>
#include "u8g2.h"
#include "shim.h"
#include "status_page.inc"   // extracted from main.cpp by render.sh
#include "../../ShelfV4/screen_pages.h"

static void dump(const char* name) {
  FILE* f = fopen(name, "w");
  const uint8_t* b = u8g2_GetBufferPtr(&U);         // SSD1306 layout: 8 rows of 128 bytes, a byte is 8 pixels down
  for (int y = 0; y < 64; y++) { for (int x = 0; x < 128; x++) fputc((b[(y / 8) * 128 + x] >> (y % 8)) & 1 ? '1' : '0', f); fputc('\n', f); }
  fclose(f);
}
static void shot(int p, uint32_t el, const char* name) {
  oled.clearBuffer(); scrolling = false;
  switch (p) {
    case P_NOW: drawNowPage(el); break;
    case P_WATCH: drawWatchPage(); break;
    case P_CLOCK: drawClockPage(); break;
    case P_AFFIRM: drawAffirmPage(el); break;
    case P_COLOR: drawColorPage(el); break;
    case P_TEMP: drawTempPage(); break;
    default: drawStatusPage(); break;
  }
  dump(name);
}
int main() {
  u8g2_Setup_ssd1306_128x64_noname_f(&U, U8G2_R0, u8x8_byte_empty, u8x8_dummy_cb);   // drawn into memory only
  shot(P_STATUS, 0, "status.txt");
  shot(P_WATCH, 0, "watch.txt");
  fakeMillis = 5000; screenShowing("Music", "Fleetwood Mac - Dreams.mp3"); shot(P_NOW, 0, "now-playing.txt");
  screenShowing("Games", "blockscape.html"); shot(P_NOW, 0, "game.txt");
  screenShowing("Pictures", "Harsens Island sunset.jpg"); shot(P_NOW, 0, "picture.txt");
  shot(P_AFFIRM, 0, "affirmation.txt");
  shot(P_CLOCK, 100, "clock.txt");
  shot(P_COLOR, 0, "color.txt");
  fakeMillis = (2 * 24 * 60 + 3 * 60 + 12) * 60000u;   // up 2 days, 3 hours
  shot(P_TEMP, 0, "temperature.txt");
  return 0;
}
