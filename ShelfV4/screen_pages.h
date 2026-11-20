// The board's screen: a slow carousel of pages while it's on.
//   status (name, address, storage) - now playing - watch face - today's affirmation -
//   big clock - color of the day - CPU temperature
// A song that starts playing takes over the screen for 30 seconds, then stays in the carousel
// for 10 minutes. The clock pages need the time (from the internet, or from the first browser
// that opens The Shelf). One press of the button stops the pages turning, another starts them
// again; two quick presses show the next page.
// Included once, from main.cpp, after drawStatusPage().
#include "daily_gen.h"
#include "world_map.h"

enum { P_STATUS, P_WATCH, P_NOW, P_EARTH, P_GPS, P_AFFIRM, P_CLOCK, P_WXGRAPH, P_COLOR, P_TEMP, P_COUNT };
static int page = P_STATUS;
static uint32_t pageAt = 0, pageLen = 10000;
static volatile bool nextPageNow = false;
static bool pageHeld = false;                       // one press of the button: stay on this page (another: turn again)
static uint32_t holdNoteAt = 0;                     // when it changed, for the little note on screen
static char npName[96] = "";
static char npLabel[20] = "NOW PLAYING";
static volatile bool npMusic = true;               // a song (the bars bounce) or a game, program, video, picture
static volatile uint32_t npAt = 0;                 // when it started (0: nothing yet)

static bool clockOK() { return time(nullptr) > 1700000000; }
static void localNow(struct tm* t) { time_t n = time(nullptr) + (time_t)tzMin * 60; gmtime_r(&n, t); }
static uint32_t dayNumber() { return (uint32_t)(time(nullptr) / 86400); }   // same day as The Shelf's Home page

static bool nowPlayingFresh() { return npAt && millis() - npAt < 30000; }
static bool nowPlayingRecent() { return npAt && millis() - npAt < 600000; }

// Called by the web server (any task) when something is opened from a shelf: a song, a game,
// a program, a video or a picture. Shows its name for 30 seconds, then keeps it in the rotation.
void screenShowing(const String& cat, const String& file) {
  bool music = cat == "Music" || cat == "MIDI";
  const char* label = music ? "NOW PLAYING" : cat == "Games" ? "PLAYING GAME" : cat == "Programs" ? "PROGRAM"
                    : cat == "Videos" ? "WATCHING" : cat == "Pictures" ? "PICTURE" : cat == "Books" ? "READING" : nullptr;
  if (!label) return;
  String n = file;
  if (music) {                                      // songs: just the title
    int dot = n.lastIndexOf('.');
    if (dot > 0) n.remove(dot);
    n.replace('_', ' ');
  }
  npAt = 0;                                         // (the screen task skips it while it's being changed)
  snprintf(npName, sizeof npName, "%s", n.c_str());
  snprintf(npLabel, sizeof npLabel, "%s", label);
  npMusic = music;
  npAt = millis() | 1;
  screenWake();
}

// ---- text that's too wide
static const int SCROLL_PXS = 30;                   // pixels a second
// one pass: wait 1.5 s, slide until the end shows, wait 1.5 s
static uint32_t passMs(int tw) { return tw <= 128 ? 0 : 1500 + (uint32_t)(tw - 128) * 1000 / SCROLL_PXS + 1500; }
static void drawPass(int y, const char* t, uint32_t el) {
  int tw = oled.getUTF8Width(t);
  if (tw <= 128) { oled.drawUTF8((128 - tw) / 2, y, t); return; }
  scrolling = true;
  int off = el < 1500 ? 0 : (int)min<uint32_t>((el - 1500) * SCROLL_PXS / 1000, (uint32_t)(tw - 128));
  oled.drawUTF8(-off, y, t);
}
// round and round (the song name)
static void drawLoop(int y, const char* t, uint32_t el) {
  int tw = oled.getUTF8Width(t);
  if (tw <= 128) { oled.drawUTF8((128 - tw) / 2, y, t); return; }
  scrolling = true;
  int span = tw + 40, off = el < 1500 ? 0 : (int)(((el - 1500) * SCROLL_PXS / 1000) % span);
  oled.drawUTF8(-off, y, t);
  oled.drawUTF8(span - off, y, t);
}
static void header(const char* t) {
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr((128 - oled.getStrWidth(t)) / 2, 7, t);
  oled.drawHLine(0, 10, 128);
}
static const char* const WDAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// ---- pages
static void drawNowPage(uint32_t el) {
  char name[96], label[20];
  snprintf(name, sizeof name, "%s", npName); snprintf(label, sizeof label, "%s", npLabel);
  bool music = npMusic;
  oled.setFont(u8g2_font_5x7_tf);
  int lw = oled.getStrWidth(label), lx = (128 - lw) / 2;
  if (music) { lx = 13; oled.drawDisc(3, 7, 2); oled.drawVLine(5, 0, 8); oled.drawLine(5, 0, 9, 2); }   // a music note
  oled.drawStr(lx, 7, label);
  oled.drawHLine(0, 10, 128);
  oled.setFont(u8g2_font_helvB12_tf);
  drawLoop(34, name, el);
  if (music) {                                      // bouncing bars along the bottom
    scrolling = true;
    uint32_t f = millis() / 110;
    for (int i = 0; i < 16; i++) {
      uint32_t h = (f * 2654435761u + i * 40503u) ^ (f >> 3) * 977u;
      int bh = 2 + (int)((h >> 7) % 15);
      oled.drawBox(i * 8 + 1, 64 - bh, 6, bh);
    }
  } else {                                          // which shelf, along the bottom
    const char* shelf = !strcmp(label, "PLAYING GAME") ? "from the Games shelf" : !strcmp(label, "PROGRAM") ? "from the Programs shelf"
                      : !strcmp(label, "WATCHING") ? "from the Videos shelf" : !strcmp(label, "PICTURE") ? "from the Pictures shelf" : "from the Books shelf";
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr((128 - oled.getStrWidth(shelf)) / 2, 61, shelf);
  }
}

static void hand(int cx, int cy, float a, int len, float w) {
  float s = sinf(a), c = cosf(a);
  int tx = cx + (int)lroundf(s * len), ty = cy - (int)lroundf(c * len);
  int px = (int)lroundf(c * w), py = (int)lroundf(s * w);
  oled.drawTriangle(cx + px, cy + py, cx - px, cy - py, tx, ty);
  oled.drawLine(cx, cy, tx, ty);
}
static void drawWatchPage() {
  struct timeval tv; gettimeofday(&tv, nullptr);      // the second and its fraction from the same clock reading
  time_t lt = tv.tv_sec + (time_t)tzMin * 60;
  struct tm t; gmtime_r(&lt, &t);
  const int cx = 31, cy = 32;
  oled.drawCircle(cx, cy, 31); oled.drawCircle(cx, cy, 30);
  for (int i = 0; i < 60; i++) {
    float a = i * (float)M_PI / 30, s = sinf(a), c = cosf(a);
    if (i % 5 == 0) {
      int r0 = i % 15 == 0 ? 22 : 24;
      for (int k = -1; k <= (i % 15 == 0 ? 1 : 0); k++) {   // quarter marks a bit bolder
        float ka = a + k * 0.025f;
        oled.drawLine(cx + (int)lroundf(sinf(ka) * r0), cy - (int)lroundf(cosf(ka) * r0),
                      cx + (int)lroundf(sinf(ka) * 28), cy - (int)lroundf(cosf(ka) * 28));
      }
    } else {
      oled.drawPixel(cx + (int)lroundf(s * 27), cy - (int)lroundf(c * 27));
    }
  }
  float sec = t.tm_sec + tv.tv_usec / 1e6f;          // (millis() isn't in step with the clock: the hand jumped each second)
  float mn = t.tm_min + t.tm_sec / 60.0f, hr = (t.tm_hour % 12) + mn / 60.0f;
  hand(cx, cy, hr * (float)M_PI / 6, 15, 2.5f);
  hand(cx, cy, mn * (float)M_PI / 30, 23, 2.0f);
  float sa = sec * (float)M_PI / 30;                 // a sweeping second hand with a short tail
  oled.drawLine(cx - (int)lroundf(sinf(sa) * 6), cy + (int)lroundf(cosf(sa) * 6),
                cx + (int)lroundf(sinf(sa) * 26), cy - (int)lroundf(cosf(sa) * 26));
  oled.drawDisc(cx, cy, 2); oled.setDrawColor(0); oled.drawPixel(cx, cy); oled.setDrawColor(1);
  scrolling = true;

  // beside it: the date
  char b[24];
  oled.setFont(u8g2_font_helvB08_tf);
  String wd = String(WDAY[t.tm_wday]).substring(0, 3); wd.toUpperCase();
  oled.drawStr(96 - oled.getStrWidth(wd.c_str()) / 2, 12, wd.c_str());
  oled.setFont(u8g2_font_helvB18_tf);
  snprintf(b, sizeof b, "%d", t.tm_mday);
  oled.drawStr(96 - oled.getStrWidth(b) / 2, 35, b);
  oled.setFont(u8g2_font_5x7_tf);
  snprintf(b, sizeof b, "%s %d", MON[t.tm_mon], t.tm_year + 1900);
  oled.drawStr(96 - oled.getStrWidth(b) / 2, 45, b);
  oled.setFont(u8g2_font_6x10_tf);
  snprintf(b, sizeof b, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  oled.drawStr(96 - oled.getStrWidth(b) / 2, 60, b);
}

static void drawClockPage() {
  struct tm t; localNow(&t);
  char b[24];
  int h = t.tm_hour % 12 ? t.tm_hour % 12 : 12;
  bool colon = millis() % 1000 < 600;
  snprintf(b, sizeof b, colon ? "%d:%02d" : "%d %02d", h, t.tm_min);
  oled.setFont(u8g2_font_logisoso32_tn);
  int tw = oled.getStrWidth(b);
  oled.setFont(u8g2_font_helvB08_tf);
  const char* ap = t.tm_hour < 12 ? "AM" : "PM";
  int aw = oled.getStrWidth(ap), x = (128 - tw - aw - 3) / 2;
  oled.setFont(u8g2_font_logisoso32_tn);
  oled.drawStr(x, 38, b);
  oled.setFont(u8g2_font_helvB08_tf);
  oled.drawStr(x + tw + 3, 38, ap);
  snprintf(b, sizeof b, ":%02d", t.tm_sec);
  oled.drawStr(x + tw + 3, 22, b);
  snprintf(b, sizeof b, "%s, %s %d", WDAY[t.tm_wday], MON[t.tm_mon], t.tm_mday);
  oled.drawStr((128 - oled.getStrWidth(b)) / 2, 58, b);
  scrolling = true;                                  // keep the seconds ticking
}

static const char* todaysAffirmation() {
  return DAILY_AFFIRMATIONS[dayNumber() % (sizeof DAILY_AFFIRMATIONS / sizeof DAILY_AFFIRMATIONS[0])];
}
static int todaysColor() { return (int)(((uint64_t)dayNumber() * 7) % (sizeof DAILY_COLOR_NAMES / sizeof DAILY_COLOR_NAMES[0])); }

static void drawAffirmPage(uint32_t el) {
  header("TODAY'S AFFIRMATION");
  // little sparkles either side of the heading
  for (int sx : {6, 121}) { oled.drawHLine(sx - 2, 4, 5); oled.drawVLine(sx, 2, 5); }
  oled.setFont(u8g2_font_helvB12_tf);
  drawPass(38, todaysAffirmation(), el);
  struct tm t; localNow(&t);
  char b[24]; snprintf(b, sizeof b, "%s, %s %d", WDAY[t.tm_wday], MON[t.tm_mon], t.tm_mday);
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr((128 - oled.getStrWidth(b)) / 2, 62, b);
}

static void drawColorPage(uint32_t el) {
  header("COLOR OF THE DAY");
  int c = todaysColor();
  oled.setFont(u8g2_font_helvB14_tf);
  if (oled.getStrWidth(DAILY_COLOR_NAMES[c]) > 112) oled.setFont(u8g2_font_helvB10_tf);
  int tw = oled.getStrWidth(DAILY_COLOR_NAMES[c]), x = (128 - tw - 12) / 2;
  oled.drawDisc(x + 4, 29, 4);                       // a paint dot beside the name
  oled.drawStr(x + 12, 35, DAILY_COLOR_NAMES[c]);
  oled.setFont(u8g2_font_6x10_tf);
  drawPass(61, DAILY_COLOR_MEANINGS[c], el);         // in the screen's bottom strip (yellow on two-color screens)
}

static void drawTempPage() {
  header("CPU TEMPERATURE");
  float c = temperatureRead();
  // a thermometer, kept in the top (blue) part
  oled.drawRFrame(8, 13, 9, 26, 4); oled.drawCircle(12, 41, 6); oled.drawDisc(12, 41, 4);
  oled.setDrawColor(0); oled.drawBox(9, 35, 7, 4); oled.setDrawColor(1);
  int lvl = constrain((int)((c - 20) * 20 / 70), 2, 20);   // 20..90 C fills the tube
  oled.drawBox(10, 37 - lvl, 5, lvl + 3);
  char b[24];
  snprintf(b, sizeof b, "%.0f\xC2\xB0" "C", c);
  oled.setFont(u8g2_font_helvB18_tf);
  oled.drawUTF8(30, 36, b);
  snprintf(b, sizeof b, "%.0f\xC2\xB0" "F", c * 9 / 5 + 32);
  oled.setFont(u8g2_font_helvB10_tf);
  oled.drawUTF8(128 - oled.getUTF8Width(b), 36, b);
  uint32_t up = millis() / 60000;
  if (up >= 1440) snprintf(b, sizeof b, "Up %ud %uh", (unsigned)(up / 1440), (unsigned)(up % 1440 / 60));
  else snprintf(b, sizeof b, "Up %uh %um", (unsigned)(up / 60), (unsigned)(up % 60));
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr((128 - oled.getStrWidth(b)) / 2, 55, b);   // both lines in the bottom (yellow) strip
  snprintf(b, sizeof b, "%u KB free \xC2\xB7 %u MHz", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)getCpuFrequencyMhz());   // 80 MHz: saving power
  oled.drawUTF8((128 - oled.getUTF8Width(b)) / 2, 63, b);
}

// The sunlit earth: the world map (80°N to 60°S: the far north and Antarctica are left off) with day and night as they are right now. Land in daylight is
// solid, in twilight half-lit, at night dim; the line between day and night is dotted across the
// oceans; the sun sits over the spot where it's straight overhead; where the board is blinks.
static void sunPosition(time_t t, float& decl, float& subLon) {
  struct tm g; gmtime_r(&t, &g);
  float hours = g.tm_hour + g.tm_min / 60.0f + g.tm_sec / 3600.0f;
  float y = 2 * (float)M_PI / 365 * (g.tm_yday + (hours - 12) / 24);       // NOAA's approximation
  decl = 0.006918f - 0.399912f * cosf(y) + 0.070257f * sinf(y) - 0.006758f * cosf(2 * y) + 0.000907f * sinf(2 * y)
       - 0.002697f * cosf(3 * y) + 0.00148f * sinf(3 * y);
  float eqt = 229.18f * (0.000075f + 0.001868f * cosf(y) - 0.032077f * sinf(y) - 0.014615f * cosf(2 * y) - 0.040849f * sinf(2 * y));
  subLon = -15.0f * (hours - 12 + eqt / 60);                                // degrees, east positive
  while (subLon < -180) subLon += 360; while (subLon > 180) subLon -= 360;
}
static void drawEarthPage() {
  float decl, subLon; sunPosition(time(nullptr), decl, subLon);
  float sd = sinf(decl), cd = cosf(decl);
  static float cosCol[128];
  for (int x = 0; x < 128; x++) cosCol[x] = cosf(((-180 + (x + 0.5f) * 360 / 128) - subLon) * (float)M_PI / 180);
  for (int y = 0; y < 64; y++) {
    float lat = (WORLD_TOP - (y + 0.5f) * (WORLD_TOP - WORLD_BOTTOM) / 64) * (float)M_PI / 180, a = sinf(lat) * sd, b = cosf(lat) * cd;
    for (int x = 0; x < 128; x++) {
      float z = a + b * cosCol[x];                                         // the sun's height there (sine)
      bool land = (WORLD_MAP[y][x >> 3] >> (7 - (x & 7))) & 1;
      bool on;
      if (land) on = z > 0.05f ? true : z > -0.10f ? ((x + y) & 1) : (!(x & 1) && !(y & 1));   // day, twilight, night
      else on = fabsf(z) < 0.025f && ((x + y) % 3 == 0);                  // the day/night line, over the sea
      if (on) oled.drawPixel(x, y);
    }
  }
  int sx = (int)lroundf((subLon + 180) * 128 / 360), sy = (int)lroundf((WORLD_TOP - decl * 180 / (float)M_PI) * 64 / (WORLD_TOP - WORLD_BOTTOM));
  // the sun: a dot with rays (where it's straight overhead right now)
  oled.setDrawColor(0); oled.drawDisc(sx, sy, 6); oled.setDrawColor(1);
  oled.drawDisc(sx, sy, 2);
  for (int k = 0; k < 8; k++) {
    float a = k * (float)M_PI / 4, c = cosf(a), d = sinf(a);
    oled.drawLine(sx + (int)lroundf(c * 4), sy + (int)lroundf(d * 4), sx + (int)lroundf(c * 6), sy + (int)lroundf(d * 6));
  }
  // here: a target that flashes between a ring and a filled spot, so it's easy to find
  float hl, ho;
  if (wxLocation(hl, ho)) {
    int hx = (int)lroundf((ho + 180) * 128 / 360), hy = (int)lroundf((WORLD_TOP - hl) * 64 / (WORLD_TOP - WORLD_BOTTOM));
    oled.setDrawColor(0); oled.drawDisc(hx, hy, 4); oled.setDrawColor(1);
    if ((millis() / 400) % 2) oled.drawCircle(hx, hy, 3); else oled.drawDisc(hx, hy, 2);
    oled.drawPixel(hx, hy);
  }
  struct tm t; localNow(&t);                                               // the time, bottom left
  char b[16]; snprintf(b, sizeof b, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  oled.setFont(u8g2_font_5x7_tf);
  int w = oled.getStrWidth(b);
  oled.setDrawColor(0); oled.drawBox(0, 55, w + 3, 9); oled.setDrawColor(1);
  oled.drawStr(1, 63, b);
  scrolling = true;                                                        // keep "here" blinking
}

// Outside temperature, hour by hour for the last 4 days, with the freezing line (32 °F).
static void drawWxGraphPage() {
  static float v[WX_HOURS]; uint32_t start = 0;
  int n = wxHistoryCopy(v, WX_HOURS, &start);
  if (n < 2) {                                      // nothing yet: say why, so the page isn't a mystery
    header("OUTSIDE \xB7 LAST 4 DAYS");
    oled.setFont(u8g2_font_6x10_tf);
    drawPass(34, "Outside temperature", 0);
    oled.setFont(u8g2_font_5x7_tf);
    const char* m = wxStatusText();
    oled.drawStr(max(0, (128 - (int)oled.getStrWidth(m)) / 2), 58, m);
    return;
  }
  float lo = v[0], hi = v[0];
  for (int i = 1; i < n; i++) { lo = min(lo, v[i]); hi = max(hi, v[i]); }
  float dlo = lo, dhi = hi;
  if (lo < 45 && hi > 20) { dlo = min(dlo, 32.0f); dhi = max(dhi, 32.0f); }   // keep freezing on the graph when it's near
  if (dhi - dlo < 10) { float m = (dhi + dlo) / 2; dlo = m - 5; dhi = m + 5; }
  const int X0 = 16, X1 = 127, Y0 = 12, Y1 = 46;                          // the graph sits in the top (blue) part
  auto Y = [&](float f) { return Y1 - (int)lroundf((f - dlo) * (Y1 - Y0) / (dhi - dlo)); };
  auto X = [&](int i) { return X0 + (int)lroundf((float)i * (X1 - X0) / (WX_HOURS - 1)) + (WX_HOURS - n) * (X1 - X0) / (WX_HOURS - 1); };
  // heading: now, top right
  char b[40];
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(0, 7, "OUTSIDE \xB7 4 DAYS");
  snprintf(b, sizeof b, "%.0f\xC2\xB0" "F now", v[n - 1]);
  oled.drawUTF8(128 - oled.getUTF8Width(b), 7, b);
  // scale: high and low at the left
  oled.setFont(u8g2_font_4x6_tf);
  snprintf(b, sizeof b, "%.0f", dhi); oled.drawStr(0, Y0 + 5, b);
  snprintf(b, sizeof b, "%.0f", dlo); oled.drawStr(0, Y1, b);
  // the freezing line, dashed, with 32 at the left
  if (32 >= dlo && 32 <= dhi) {
    int fy = Y(32);
    for (int x = X0; x <= X1; x += 4) oled.drawHLine(x, fy, 2);
    if (fy - 3 > Y0 + 6 && fy + 3 < Y1 - 5) oled.drawStr(0, fy + 3, "32");
  }
  // midnights: a dotted line each day, and the day's name underneath (local time)
  long off = (long)tzMin * 60;
  uint32_t firstLocal = start + off;
  uint32_t mid = (firstLocal / 86400 + 1) * 86400;                         // first local midnight after the start
  int prevX = X(0);
  struct tm d;
  oled.setFont(u8g2_font_5x7_tf);
  for (;; mid += 86400) {
    int hi_ = (int)((mid - firstLocal) / 3600);
    int mx = hi_ < n ? X(hi_) : X(n - 1);
    time_t dayT = (time_t)(mid - 86400); gmtime_r(&dayT, &d);              // the day that ends at this midnight
    char nm[4]; snprintf(nm, sizeof nm, "%.3s", WDAY[d.tm_wday]);
    int cx = (prevX + mx) / 2 - oled.getStrWidth(nm) / 2;
    if (mx - prevX > 16) oled.drawStr(cx, 56, nm);
    if (hi_ >= n) break;
    for (int y = Y0; y <= Y1; y += 3) oled.drawPixel(mx, y);
    prevX = mx;
  }
  // the temperature
  for (int i = 1; i < n; i++) oled.drawLine(X(i - 1), Y(v[i - 1]), X(i), Y(v[i]));
  oled.drawDisc(X(n - 1), Y(v[n - 1]), 1);
  // low and high, along the bottom
  oled.setFont(u8g2_font_4x6_tf);
  snprintf(b, sizeof b, "lo %.0f\xB0  hi %.0f\xB0  - - freezing", lo, hi);
  oled.drawStr((128 - oled.getStrWidth(b)) / 2, 63, b);
}

// GPS (Heltec V4 with the GPS kit, switched on in the GPS tab): where, how high, how fast,
// the ham radio grid square, and the satellites.
static void gridSquare(double lat, double lon, char* out) {           // Maidenhead locator, 6 characters
  double x = lon + 180, y = lat + 90;
  out[0] = 'A' + (int)(x / 20); out[1] = 'A' + (int)(y / 10);
  out[2] = '0' + (int)fmod(x / 2, 10); out[3] = '0' + (int)fmod(y, 10);
  out[4] = 'a' + (int)fmod(x * 12, 24); out[5] = 'a' + (int)fmod(y * 24, 24); out[6] = 0;
}
static void drawGpsPage() {
  double lat, lon; float alt, kmh; int sats, view;
  bool fix = gpsScreen(lat, lon, alt, kmh, sats, view);
  char b[40];
  if (!fix) {
    header("GPS");
    oled.setFont(u8g2_font_6x10_tf);
    if (!gpsTalking()) { drawPass(30, "No word from the GPS", 0); oled.setFont(u8g2_font_5x7_tf); drawPass(44, "Is the GPS kit plugged in?", 0); }
    else {
      drawPass(30, "Looking for satellites", 0);
      oled.setFont(u8g2_font_5x7_tf);
      snprintf(b, sizeof b, "%d in view", view); oled.drawStr((128 - oled.getStrWidth(b)) / 2, 44, b);
      oled.drawStr((128 - oled.getStrWidth("Best outdoors, sky in view")) / 2, 62, "Best outdoors, sky in view");
      // a little radar sweep, so it's clearly working
      float a = (millis() % 2000) / 2000.0f * 2 * (float)M_PI;
      oled.drawCircle(118, 22, 6); oled.drawLine(118, 22, 118 + (int)lroundf(sinf(a) * 6), 22 - (int)lroundf(cosf(a) * 6));
      scrolling = true;
    }
    return;
  }
  snprintf(b, sizeof b, "GPS \xB7 %d SATS", sats); header(b);
  oled.setFont(u8g2_font_6x10_tf);
  snprintf(b, sizeof b, "%c %.5f", lat >= 0 ? 'N' : 'S', fabs(lat)); oled.drawStr(0, 22, b);
  snprintf(b, sizeof b, "%c %.5f", lon >= 0 ? 'E' : 'W', fabs(lon)); oled.drawStr(0, 33, b);
  oled.setFont(u8g2_font_5x7_tf);
  if (isnan(alt)) snprintf(b, sizeof b, "alt --"); else snprintf(b, sizeof b, "%.0f ft", alt * 3.28084f);
  oled.drawStr(128 - oled.getStrWidth(b), 22, b);
  if (isnan(kmh)) snprintf(b, sizeof b, "-- mph"); else snprintf(b, sizeof b, "%.0f mph", kmh * 0.621371f);
  oled.drawStr(128 - oled.getStrWidth(b), 33, b);
  char g[8]; gridSquare(lat, lon, g);
  oled.setFont(u8g2_font_helvB10_tf);
  snprintf(b, sizeof b, "Grid %s", g); oled.drawStr((128 - oled.getStrWidth(b)) / 2, 58, b);
  // the satellites in use as dots along the bottom of the blue part
  for (int i = 0; i < sats && i < 24; i++) oled.drawDisc(4 + i * 5, 42, 1);
}

// ---- the carousel
static bool pageReady(int p) {
  switch (p) {
    case P_NOW: return nowPlayingRecent() && npName[0];   // a song, game, program, video or picture
    case P_WATCH: case P_CLOCK: case P_AFFIRM: case P_COLOR: case P_EARTH: return clockOK();
    case P_WXGRAPH: return clockOK();
    case P_GPS: return gpsRunning();
    default: return true;
  }
}
static uint32_t pageDuration(int p) {
  switch (p) {
    case P_STATUS: return 10000;
    case P_WATCH: return 9000;
    case P_CLOCK: return 6000;
    case P_TEMP: return 6000;
    case P_EARTH: return 9000;
    case P_GPS: return 9000;
    case P_WXGRAPH: return 9000;
    case P_NOW: return 10000;
    case P_AFFIRM: oled.setFont(u8g2_font_helvB12_tf); return max<uint32_t>(7000, passMs(oled.getUTF8Width(todaysAffirmation())));
    case P_COLOR: oled.setFont(u8g2_font_6x10_tf); return max<uint32_t>(7000, passMs(oled.getUTF8Width(DAILY_COLOR_MEANINGS[todaysColor()])));
  }
  return 8000;
}

static void drawScreen() {
  scrolling = false;
  if (!oledOK || !screenOn) return;
  uint32_t now = millis();
  bool skip = nextPageNow; nextPageNow = false;
  if (skip && nowPlayingFresh()) { npAt = (now - 30000) | 1; skip = false; }   // button: past the song
  oled.clearBuffer();
  // things to show right away: joining the Wi-Fi, setup, a Coms message, a song that just started
  bool urgent = firstSetup || (!staOK && !apActive) || (comsLastAt && now - comsLastAt < 15000);
  if (urgent) { drawStatusPage(); oled.sendBuffer(); return; }
  if (nowPlayingFresh()) { drawNowPage(now - npAt); oled.sendBuffer(); return; }
  if (skip || (!pageHeld && now - pageAt >= pageLen) || !pageReady(page)) {   // held: two presses still turn it
    for (int i = 0; i < P_COUNT; i++) { page = (page + 1) % P_COUNT; if (pageReady(page)) break; }
    pageAt = now; pageLen = pageDuration(page);
  }
  uint32_t el = now - pageAt;
  switch (page) {
    case P_NOW: drawNowPage(el); break;
    case P_WATCH: drawWatchPage(); break;
    case P_CLOCK: drawClockPage(); break;
    case P_AFFIRM: drawAffirmPage(el); break;
    case P_COLOR: drawColorPage(el); break;
    case P_TEMP: drawTempPage(); break;
    case P_EARTH: drawEarthPage(); break;
    case P_GPS: drawGpsPage(); break;
    case P_WXGRAPH: drawWxGraphPage(); break;
    default: drawStatusPage(); break;
  }
  if (holdNoteAt && now - holdNoteAt < 1500) {      // "Holding" / "Turning" for a moment after a press
    const char* t = pageHeld ? "Holding this page" : "Pages turning";
    oled.setFont(u8g2_font_6x10_tf);
    int w = oled.getStrWidth(t) + 8, x = (128 - w) / 2;
    oled.setDrawColor(0); oled.drawBox(x - 1, 21, w + 2, 16); oled.setDrawColor(1);
    oled.drawRFrame(x, 22, w, 14, 3); oled.drawStr(x + 4, 32, t);
    scrolling = true;                               // so it goes away on time
  } else holdNoteAt = 0;
  oled.sendBuffer();
}
