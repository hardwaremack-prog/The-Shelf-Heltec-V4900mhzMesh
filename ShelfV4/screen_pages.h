// The board's screen: a slow carousel of pages while it's on.
//   status (name, address, storage) - now playing - watch face - today's affirmation -
//   big clock - color of the day - CPU temperature
// A song that starts playing takes over the screen for 30 seconds, then stays in the carousel
// for 10 minutes. The clock pages need the time (from the internet, or from the first browser
// that opens The Shelf). A short press of the button shows the next page.
// Included once, from main.cpp, after drawStatusPage().
#include "daily_gen.h"

enum { P_STATUS, P_WATCH, P_NOW, P_AFFIRM, P_CLOCK, P_COLOR, P_TEMP, P_COUNT };
static int page = P_STATUS;
static uint32_t pageAt = 0, pageLen = 10000;
static volatile bool nextPageNow = false;
static char npName[96] = "";
static volatile uint32_t npAt = 0;                 // when the last song started (0: none)

static bool clockOK() { return time(nullptr) > 1700000000; }
static void localNow(struct tm* t) { time_t n = time(nullptr) + (time_t)tzMin * 60; gmtime_r(&n, t); }
static uint32_t dayNumber() { return (uint32_t)(time(nullptr) / 86400); }   // same day as The Shelf's Home page

static bool nowPlayingFresh() { return npAt && millis() - npAt < 30000; }
static bool nowPlayingRecent() { return npAt && millis() - npAt < 600000; }

// Called by the web server when a song starts (any task): remember its name and show it.
void screenNowPlaying(const String& file) {
  String n = file;
  int dot = n.lastIndexOf('.');
  if (dot > 0) n.remove(dot);
  n.replace('_', ' ');
  snprintf(npName, sizeof npName, "%s", n.c_str());
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
  oled.setFont(u8g2_font_5x7_tf);
  // a little music note
  oled.drawDisc(3, 7, 2); oled.drawVLine(5, 0, 8); oled.drawLine(5, 0, 9, 2);
  oled.drawStr(13, 7, "NOW PLAYING");
  if (staOK || apActive) { String h = hostName; oled.drawStr(128 - oled.getStrWidth(h.c_str()), 7, h.c_str()); }
  oled.drawHLine(0, 10, 128);
  char name[96]; snprintf(name, sizeof name, "%s", npName);
  oled.setFont(u8g2_font_helvB12_tf);
  drawLoop(34, name, el);
  // bouncing bars
  scrolling = true;
  uint32_t f = millis() / 110;
  for (int i = 0; i < 16; i++) {
    uint32_t h = (f * 2654435761u + i * 40503u) ^ (f >> 3) * 977u;
    int bh = 2 + (int)((h >> 7) % 15);
    oled.drawBox(i * 8 + 1, 64 - bh, 6, bh);
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
  struct tm t; localNow(&t);
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
  float sec = t.tm_sec + (millis() % 1000) / 1000.0f;
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
  // a thermometer
  oled.drawRFrame(8, 16, 9, 32, 4); oled.drawCircle(12, 53, 7); oled.drawDisc(12, 53, 5);
  oled.setDrawColor(0); oled.drawBox(9, 44, 7, 6); oled.setDrawColor(1);
  int lvl = constrain((int)((c - 20) * 26 / 70), 2, 26);   // 20..90 C fills the tube
  oled.drawBox(10, 48 - lvl, 5, lvl + 3);
  char b[24];
  snprintf(b, sizeof b, "%.0f\xC2\xB0" "C", c);
  oled.setFont(u8g2_font_helvB18_tf);
  oled.drawUTF8(30, 38, b);
  snprintf(b, sizeof b, "%.0f\xC2\xB0" "F", c * 9 / 5 + 32);
  oled.setFont(u8g2_font_helvB10_tf);
  oled.drawUTF8(128 - oled.getUTF8Width(b), 38, b);
  uint32_t up = millis() / 60000;
  if (up >= 1440) snprintf(b, sizeof b, "Up %ud %uh", (unsigned)(up / 1440), (unsigned)(up % 1440 / 60));
  else snprintf(b, sizeof b, "Up %uh %um", (unsigned)(up / 60), (unsigned)(up % 60));
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(30, 52, b);
  snprintf(b, sizeof b, "%u KB memory free", (unsigned)(ESP.getFreeHeap() / 1024));
  oled.drawStr(30, 62, b);
}

// ---- the carousel
static bool pageReady(int p) {
  switch (p) {
    case P_NOW: return nowPlayingRecent() && npName[0];
    case P_WATCH: case P_CLOCK: case P_AFFIRM: case P_COLOR: return clockOK();
    default: return true;
  }
}
static uint32_t pageDuration(int p) {
  switch (p) {
    case P_STATUS: return 10000;
    case P_WATCH: return 9000;
    case P_CLOCK: return 6000;
    case P_TEMP: return 6000;
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
  if (skip || now - pageAt >= pageLen || !pageReady(page)) {
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
    default: drawStatusPage(); break;
  }
  oled.sendBuffer();
}
