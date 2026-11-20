// Just enough of Arduino and the board for the screen code to run on a computer.
#pragma once
#define PROGMEM
#define BOARD_SHORT "ESP32"
#define BOARD_NAME "ESP32"
#define FW_VERSION "sim"
#define SHELF_ESP32 1
template <class T, class U2> static auto min(T a, U2 b) -> decltype(a + b) { return a < b ? a : b; }
template <class T, class U2> static auto max(T a, U2 b) -> decltype(a + b) { return a > b ? a : b; }
template <class T> static T constrain(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint32_t fakeMillis = 0;
static uint32_t millis() { return fakeMillis; }
// a fixed moment for the pictures: Wed 18 Nov 2026, 10:08:37 AM in Michigan (UTC-5)
static const time_t FAKE_NOW = 1795014517;
static time_t fake_time(time_t* t) { if (t) *t = FAKE_NOW; return FAKE_NOW; }
static int fake_gettimeofday(struct timeval* tv, void*) { tv->tv_sec = FAKE_NOW; tv->tv_usec = 0; return 0; }
#define time fake_time
#define gettimeofday fake_gettimeofday
static float temperatureRead() { return 47.2f; }
static uint32_t getCpuFrequencyMhz() { return 80; }
struct { uint32_t getFreeHeap() { return 118 * 1024; } } ESP;

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v) : s(std::to_string(v)) {}
  const char* c_str() const { return s.c_str(); }
  unsigned length() const { return s.size(); }
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(char c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  void remove(unsigned i) { s.erase(i); }
  void replace(char a, char b) { std::replace(s.begin(), s.end(), a, b); }
  String substring(unsigned a, unsigned b) const { return String(s.substr(a, b - a)); }
  void toUpperCase() { for (auto& c : s) c = toupper(c); }
  bool operator==(const char* o) const { return s == o; }
  String operator+(const String& o) const { return String(s + o.s); }
  String operator+(const char* o) const { return String(s + o); }
  String operator+(int o) const { return String(s + std::to_string(o)); }
};
static String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }

static u8g2_t U;
struct Oled {
  void clearBuffer() { u8g2_ClearBuffer(&U); }
  void sendBuffer() {}
  void setFont(const uint8_t* f) { u8g2_SetFont(&U, f); }
  int drawStr(int x, int y, const char* t) { return u8g2_DrawStr(&U, x, y, t); }
  int drawUTF8(int x, int y, const char* t) { return u8g2_DrawUTF8(&U, x, y, t); }
  int getStrWidth(const char* t) { return u8g2_GetStrWidth(&U, t); }
  int getUTF8Width(const char* t) { return u8g2_GetUTF8Width(&U, t); }
  void drawHLine(int x, int y, int w) { u8g2_DrawHLine(&U, x, y, w); }
  void drawVLine(int x, int y, int h) { u8g2_DrawVLine(&U, x, y, h); }
  void drawLine(int a, int b, int c, int d) { u8g2_DrawLine(&U, a, b, c, d); }
  void drawPixel(int x, int y) { u8g2_DrawPixel(&U, x, y); }
  void drawDisc(int x, int y, int r) { u8g2_DrawDisc(&U, x, y, r, U8G2_DRAW_ALL); }
  void drawCircle(int x, int y, int r) { u8g2_DrawCircle(&U, x, y, r, U8G2_DRAW_ALL); }
  void drawTriangle(int a, int b, int c, int d, int e, int f) { u8g2_DrawTriangle(&U, a, b, c, d, e, f); }
  void drawBox(int x, int y, int w, int h) { u8g2_DrawBox(&U, x, y, w, h); }
  void drawFrame(int x, int y, int w, int h) { u8g2_DrawFrame(&U, x, y, w, h); }
  void drawRFrame(int x, int y, int w, int h, int r) { u8g2_DrawRFrame(&U, x, y, w, h, r); }
  void setDrawColor(int c) { u8g2_SetDrawColor(&U, c); }
} oled;

// the board, as the pictures show it: an ESP32 with its screen plugged in line, on home Wi-Fi
static bool oledInline = true, onSD = true, staOK = true, apActive = false, firstSetup = false;
static bool comsOn() { return true; }
static int batteryPercent() { return -1; }
static int onlineShelves = 2, tzMin = -300;
static uint32_t comsLastAt = 0; static String comsLastLine;
static String hostName = "shelf-esp32-b21c", ownSSID = "shelf-esp32-b21c", ownPass = "theshelf", wifiSSID = "Home";
static uint64_t storeTotal = 31902400512ULL, storeUsed = 9200000000ULL;
struct IP { String toString() { return String("10.0.0.126"); } };
struct { int RSSI() { return -52; } IP localIP() { return IP(); } IP softAPIP() { return IP(); } } WiFi;
static bool oledOK = true, screenOn = true;
#define WX_HOURS 96
// four days of made-up November weather around Harsens Island, for the pictures
static int wxHistoryCopy(float* out, int max, uint32_t* startUtc) {
  int n = max < WX_HOURS ? max : WX_HOURS;
  for (int i = 0; i < n; i++) { float h = (float)(WX_HOURS - n + i); out[i] = 36 + 9 * sinf((h - 10) / 24 * 2 * (float)M_PI) - h * 0.08f + 3 * sinf(h / 7); }
  if (startUtc) *startUtc = (uint32_t)(FAKE_NOW / 3600 * 3600) - (n - 1) * 3600;
  return n;
}
static bool wxLocation(float& lat, float& lon) { lat = 42.33f; lon = -83.05f; return true; }
static const char* wxStatusText() { return "Getting them..."; }
static void screenWake() {}
