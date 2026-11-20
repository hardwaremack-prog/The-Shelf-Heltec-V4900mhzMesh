// Dumps the U8g2 fonts the board's screen uses, as JSON, for The Shelf's simulated screen.
#include <stdio.h>
#include <string.h>
#include "u8g2.h"
static u8g2_t U;
static int px(int x, int y) { const uint8_t* b = u8g2_GetBufferPtr(&U); return (b[(y / 8) * 128 + x] >> (y % 8)) & 1; }
static void dump(const char* name, const uint8_t* font) {
  u8g2_SetFont(&U, font);
  int asc = u8g2_GetAscent(&U), desc = u8g2_GetDescent(&U);
  printf("\"%s\":{\"a\":%d,\"d\":%d,\"g\":{", name, asc, desc);
  int first = 1;
  for (int c = 32; c < 256; c++) {
    if (c > 126 && c != 176 && c != 183) continue;
    if (!u8g2_IsGlyph(&U, c)) continue;
    u8g2_ClearBuffer(&U);
    int bx = 16, by = 2 + asc;
    u8g2_DrawGlyph(&U, bx, by, c);
    int adv = u8g2_GetGlyphWidth(&U, c);
    int minx = 999, maxx = -1;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) if (px(x, y)) { if (x < minx) minx = x; if (x > maxx) maxx = x; }
    printf("%s\"%d\":[%d,", first ? "" : ",", c, adv); first = 0;
    if (maxx < 0) { printf("0,0,[]]"); continue; }
    printf("%d,%d,[", minx - bx, maxx - minx + 1);
    for (int y = by - asc; y <= by - desc; y++) {
      unsigned long long row = 0;
      for (int x = minx; x <= maxx; x++) row = (row << 1) | px(x, y);
      printf("%s%llu", y == by - asc ? "" : ",", row);
    }
    printf("]]");
  }
  printf("}}");
}
int main() {
  u8g2_Setup_ssd1306_128x64_noname_f(&U, U8G2_R0, u8x8_byte_empty, u8x8_dummy_cb);
  printf("{");
  dump("helvB08", u8g2_font_helvB08_tf); printf(",");
  dump("f5x7", u8g2_font_5x7_tf); printf(",");
  dump("f4x6", u8g2_font_4x6_tf); printf(",");
  dump("f6x10", u8g2_font_6x10_tf); printf(",");
  dump("helvB10", u8g2_font_helvB10_tf); printf(",");
  dump("helvB12", u8g2_font_helvB12_tf); printf(",");
  dump("helvB14", u8g2_font_helvB14_tf); printf(",");
  dump("helvB18", u8g2_font_helvB18_tf); printf(",");
  dump("logi32", u8g2_font_logisoso32_tn);
  printf("}\n");
  return 0;
}
