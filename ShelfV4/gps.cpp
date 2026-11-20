// The Shelf on a Heltec V4 - GPS: Heltec's L76K GNSS kit on the V4's GNSS connector.
//
// Switched on from the GPS tab. Reads the GPS's NMEA sentences (9600 baud): where it is, height,
// speed, heading, the time, and the satellites. While it has a fix, the board's location follows
// it (the screen's earth map and the weather), the board's clock is set from it if nobody else
// has, and the trail is kept (a point every 10 s when it moves) for the map and a GPX download.
//
//   GET  /api/gps                 everything it knows right now (and the trail since the last call's "since")
//   POST /api/gps/power {on}      switch the GPS on or off (remembered)
//   POST /api/gps/clear           start a new trail
//   GET  /api/gps/track.gpx       the trail as a GPX file (for maps and mapping apps)
//
// The GNSS connector shares GPIO 39-42 with the microSD's in-line layout: with the card wired
// that way, the GPS can't be used (wire it to CS 4, MISO 6, CLK 47, MOSI 48 instead).
#include "shelf.h"
#include <Preferences.h>

#ifdef HAS_GNSS
static HardwareSerial gpsSerial(1);
static Preferences gp;
static bool gpsOn = false, started = false;
static char line[100]; static int lineLen = 0;
static uint32_t lastSentence = 0, lastFix = 0, sentences = 0, startedAt = 0;

struct GpsNow {
  bool fix = false; int quality = 0;
  double lat = NAN, lon = NAN;
  float altM = NAN, speedKmh = NAN, course = NAN, hdop = NAN, geoidM = NAN;
  int satsUsed = 0, satsView = 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;          // UTC, from the GPS
} G;
struct TrackPt { float lat, lon, alt, kmh; uint32_t t; };
static std::vector<TrackPt> track;
static const size_t TRACK_MAX = 720;                         // 2 hours at one point every 10 s
static float maxKmh = 0, distKm = 0;
static portMUX_TYPE gpsMux = portMUX_INITIALIZER_UNLOCKED;

static bool havePins() { return !(onSD && !sdOnAlt); }       // the card on 39-42 has the GNSS pins

static void powerPins(bool on) {
  pinMode(GNSS_EN, OUTPUT); digitalWrite(GNSS_EN, on ? LOW : HIGH);
  pinMode(GNSS_STANDBY, OUTPUT); digitalWrite(GNSS_STANDBY, HIGH);
  pinMode(GNSS_RESET, OUTPUT); digitalWrite(GNSS_RESET, HIGH);
}
static void gpsStart() {
  if (started || !havePins()) return;
  powerPins(true);
  gpsSerial.setRxBufferSize(1024);
  gpsSerial.begin(9600, SERIAL_8N1, GNSS_RX, GNSS_TX);
  started = true; startedAt = millis() | 1; lastSentence = 0; G = GpsNow();
  Serial.println("[shelf] GPS on");
}
static void gpsStop() {
  if (!started) return;
  gpsSerial.end();
  powerPins(false);
  started = false; G.fix = false;
  Serial.println("[shelf] GPS off");
}

// ---- NMEA
static int field(const char* s, int n, char* out, int max) {   // the n-th comma-separated field
  int i = 0;
  while (n > 0 && *s) { if (*s++ == ',') n--; }
  while (*s && *s != ',' && *s != '*' && i < max - 1) out[i++] = *s++;
  out[i] = 0; return i;
}
static double coord(const char* v, const char* hemi) {          // ddmm.mmmm -> degrees
  if (!*v) return NAN;
  double x = atof(v); int deg = (int)(x / 100); double m = x - deg * 100;
  double r = deg + m / 60;
  return (*hemi == 'S' || *hemi == 'W') ? -r : r;
}
static bool checksum(const char* s) {
  const char* star = strchr(s, '*');
  if (!star || s[0] != '$') return false;
  uint8_t c = 0; for (const char* p = s + 1; p < star; p++) c ^= (uint8_t)*p;
  return (uint8_t)strtol(star + 1, nullptr, 16) == c;
}
static double haversineKm(double a1, double o1, double a2, double o2) {
  const double R = 6371.0, r = M_PI / 180;
  double da = (a2 - a1) * r, dl = (o2 - o1) * r;
  double h = sin(da / 2) * sin(da / 2) + cos(a1 * r) * cos(a2 * r) * sin(dl / 2) * sin(dl / 2);
  return 2 * R * asin(sqrt(h));
}
static void onFix() {
  lastFix = millis() | 1;
  wxFromGps(G.lat, G.lon);
  // the clock, if nobody's set it (no internet, no browser yet)
  if (time(nullptr) < 1700000000 && G.y > 2000) {
    struct tm t = {}; t.tm_year = G.y - 1900; t.tm_mon = G.mo - 1; t.tm_mday = G.d; t.tm_hour = G.h; t.tm_min = G.mi; t.tm_sec = G.s;
    setenv("TZ", "UTC0", 1); tzset();
    time_t e = mktime(&t);
    if (e > 1700000000) { struct timeval tv = {e, 0}; settimeofday(&tv, nullptr); }
  }
  if (!isnan(G.speedKmh) && G.speedKmh > maxKmh && G.speedKmh < 1000) maxKmh = G.speedKmh;
  static uint32_t lastPt = 0;
  if (millis() - lastPt < 10000) return;
  portENTER_CRITICAL(&gpsMux);
  bool add = track.empty();
  if (!add) {
    const TrackPt& p = track.back();
    double km = haversineKm(p.lat, p.lon, G.lat, G.lon);
    if (km > 0.01) { add = true; if (km < 5) distKm += km; }       // moved 10 m or more (and not a jump)
  }
  if (add) {
    if (track.size() >= TRACK_MAX) track.erase(track.begin());
    track.push_back({(float)G.lat, (float)G.lon, isnan(G.altM) ? 0 : G.altM, isnan(G.speedKmh) ? 0 : G.speedKmh, (uint32_t)time(nullptr)});
  }
  portEXIT_CRITICAL(&gpsMux);
  lastPt = millis();
}
static void sentence(const char* s) {
  if (!checksum(s)) return;
  sentences++; lastSentence = millis() | 1;
  char f[24]; const char* type = s + 3;                      // $GPRMC, $GNGGA, $BDGSV...
  if (!strncmp(type, "RMC", 3)) {
    char st[4], la[16], ns[4], lo[16], ew[4], sp[12], co[12], dt[12], tm[16];
    field(s, 1, tm, sizeof tm); field(s, 2, st, sizeof st); field(s, 3, la, sizeof la); field(s, 4, ns, sizeof ns);
    field(s, 5, lo, sizeof lo); field(s, 6, ew, sizeof ew); field(s, 7, sp, sizeof sp); field(s, 8, co, sizeof co); field(s, 9, dt, sizeof dt);
    if (strlen(tm) >= 6) { G.h = (tm[0] - '0') * 10 + tm[1] - '0'; G.mi = (tm[2] - '0') * 10 + tm[3] - '0'; G.s = (tm[4] - '0') * 10 + tm[5] - '0'; }
    if (strlen(dt) == 6) { G.d = (dt[0] - '0') * 10 + dt[1] - '0'; G.mo = (dt[2] - '0') * 10 + dt[3] - '0'; G.y = 2000 + (dt[4] - '0') * 10 + dt[5] - '0'; }
    G.fix = st[0] == 'A';
    if (G.fix) {
      G.lat = coord(la, ns); G.lon = coord(lo, ew);
      G.speedKmh = *sp ? atof(sp) * 1.852f : NAN;
      if (*co) G.course = atof(co);
      onFix();
    }
  } else if (!strncmp(type, "GGA", 3)) {
    char q[4], n[6], hd[10], al[12], ge[12];
    field(s, 6, q, sizeof q); field(s, 7, n, sizeof n); field(s, 8, hd, sizeof hd); field(s, 9, al, sizeof al); field(s, 11, ge, sizeof ge);
    G.quality = atoi(q); G.satsUsed = atoi(n);
    G.hdop = *hd ? atof(hd) : NAN;
    if (*al) G.altM = atof(al);
    if (*ge) G.geoidM = atof(ge);
  } else if (!strncmp(type, "GSV", 3)) {
    // satellites in view, per system: sum the GP/GL/GB/GA... totals on each first message
    static int perSys[8]; static uint32_t at = 0;
    char num[4], tot[6]; field(s, 2, num, sizeof num); field(s, 3, tot, sizeof tot);
    int sys = (s[1] == 'G' && s[2] == 'P') ? 0 : (s[2] == 'L') ? 1 : (s[2] == 'B' || s[2] == 'D') ? 2 : (s[2] == 'A') ? 3 : (s[2] == 'Q') ? 4 : 5;
    if (atoi(num) == 1) perSys[sys] = atoi(tot);
    if (millis() - at > 900) { at = millis(); int t = 0; for (int v : perSys) t += v; G.satsView = t; }
    (void)f;
  }
}

// deep sleep / power off: the GPS off, and held off while the board sleeps
void gpsSleep() {
  if (!havePins()) return;
  gpsStop(); powerPins(false);
  gpio_hold_en((gpio_num_t)GNSS_EN); gpio_deep_sleep_hold_en();
}
bool gpsAvailable() { return havePins(); }
bool gpsFix() { return started && G.fix && lastFix && millis() - lastFix < 10000; }

void gpsBegin() {
  gpio_hold_dis((gpio_num_t)GNSS_EN);                       // (held off through a power-off)
  gp.begin("gps", false);
  gpsOn = gp.getBool("on", false);
  if (!havePins()) return;                                   // (don't touch 39-42: they're the card's)
  powerPins(false);
  if (gpsOn) gpsStart();
}
void gpsTick() {
  if (gpsOn && !started) gpsStart();
  if (!started) return;
  while (gpsSerial.available()) {
    char c = gpsSerial.read();
    if (c == '\n' || c == '\r') { if (lineLen > 6) { line[lineLen] = 0; sentence(line); } lineLen = 0; }
    else if (lineLen < (int)sizeof line - 1) line[lineLen++] = c;
    else lineLen = 0;
  }
  if (G.fix && lastFix && millis() - lastFix > 10000) G.fix = false;
}

// a few figures for the screen's GPS page
bool gpsScreen(double& lat, double& lon, float& altM, float& kmh, int& sats, int& view) {
  lat = G.lat; lon = G.lon; altM = G.altM; kmh = G.speedKmh; sats = G.satsUsed; view = G.satsView;
  return gpsFix();
}
bool gpsRunning() { return started; }
bool gpsTalking() { return started && lastSentence && millis() - lastSentence < 5000; }

static String num(double v, int d) { return isnan(v) ? String("null") : String(v, d); }

bool handleGpsApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (!url.startsWith("/api/gps")) return false;
  if (url == "/api/gps" && m == HTTP_GET) {
    uint32_t since = r->hasParam("since") ? r->getParam("since")->value().toInt() : 0;
    String s = String("{\"supported\":true,\"pins\":") + (havePins() ? "true" : "false") + ",\"on\":" + (gpsOn ? "true" : "false") +
      ",\"running\":" + (started ? "true" : "false") + ",\"talking\":" + (lastSentence && millis() - lastSentence < 5000 ? "true" : "false") +
      ",\"fix\":" + (gpsFix() ? "true" : "false") + ",\"quality\":" + G.quality +
      ",\"lat\":" + num(G.lat, 7) + ",\"lon\":" + num(G.lon, 7) + ",\"altM\":" + num(G.altM, 1) + ",\"geoidM\":" + num(G.geoidM, 1) +
      ",\"kmh\":" + num(G.speedKmh, 1) + ",\"course\":" + num(G.course, 1) + ",\"hdop\":" + num(G.hdop, 2) +
      ",\"sats\":" + G.satsUsed + ",\"view\":" + G.satsView + ",\"sentences\":" + sentences +
      ",\"utc\":\"" + (G.y ? String(G.y) + "-" + (G.mo < 10 ? "0" : "") + G.mo + "-" + (G.d < 10 ? "0" : "") + G.d + "T" +
                         (G.h < 10 ? "0" : "") + G.h + ":" + (G.mi < 10 ? "0" : "") + G.mi + ":" + (G.s < 10 ? "0" : "") + G.s + "Z" : String("")) + "\"" +
      ",\"since\":" + (startedAt ? (millis() - startedAt) / 1000 : 0) + ",\"fixAgo\":" + (lastFix ? (millis() - lastFix) / 1000 : -1) +
      ",\"maxKmh\":" + String(maxKmh, 1) + ",\"distKm\":" + String(distKm, 3) + ",\"track\":[";
    portENTER_CRITICAL(&gpsMux);
    std::vector<TrackPt> t; for (auto& p : track) if (p.t > since) t.push_back(p);
    portEXIT_CRITICAL(&gpsMux);
    bool first = true; uint32_t last = since;
    for (auto& p : t) { if (!first) s += ","; first = false; s += "[" + String(p.lat, 6) + "," + String(p.lon, 6) + "," + String(p.alt, 0) + "," + String(p.kmh, 1) + "," + p.t + "]"; last = p.t; }
    s += "],\"last\":" + String(last) + "}";
    sendJson(r, 200, s);
    return true;
  }
  if (url == "/api/gps/power" && m == HTTP_POST) {
    JsonDocument b; parseBody(r, b);
    bool on = b["on"] | false;
    if (on && !havePins()) { sendErr(r, 409, "The microSD card is wired to the GNSS connector's pins (CS 42, MOSI 41, CLK 40, MISO 39). Wire it to CS 4, MISO 6, CLK 47, MOSI 48 to use the GPS."); return true; }
    gpsOn = on; gp.putBool("on", on);
    if (!on) gpsStop();
    sendJson(r, 200, String("{\"ok\":true,\"on\":") + (on ? "true" : "false") + "}");
    return true;
  }
  if (url == "/api/gps/clear" && m == HTTP_POST) {
    portENTER_CRITICAL(&gpsMux); track.clear(); portEXIT_CRITICAL(&gpsMux);
    maxKmh = 0; distKm = 0;
    sendJson(r, 200, "{\"ok\":true}");
    return true;
  }
  if (url == "/api/gps/track.gpx" && m == HTTP_GET) {
    portENTER_CRITICAL(&gpsMux); std::vector<TrackPt> t = track; portEXIT_CRITICAL(&gpsMux);
    String g = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"The Shelf\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n<trk><name>The Shelf trail</name><trkseg>\n";
    for (auto& p : t) {
      time_t tt = p.t; struct tm u; gmtime_r(&tt, &u); char ts[24]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &u);
      g += "<trkpt lat=\"" + String(p.lat, 6) + "\" lon=\"" + String(p.lon, 6) + "\"><ele>" + String(p.alt, 1) + "</ele><time>" + ts + "</time></trkpt>\n";
    }
    g += "</trkseg></trk>\n</gpx>\n";
    AsyncWebServerResponse* res = r->beginResponse(200, "application/gpx+xml", g);
    res->addHeader("Content-Disposition", "attachment; filename=\"shelf-trail.gpx\"");
    r->send(res);
    return true;
  }
  sendErr(r, 404, "Not found");
  return true;
}
#else
// no GNSS connector on this board (V3, V4-R8, plain ESP32)
bool gpsAvailable() { return false; }
void gpsSleep() {}
bool gpsFix() { return false; }
bool gpsRunning() { return false; }
bool gpsTalking() { return false; }
void gpsBegin() {}
void gpsTick() {}
bool gpsScreen(double&, double&, float&, float&, int&, int&) { return false; }
bool handleGpsApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (!url.startsWith("/api/gps")) return false;
  if (url == "/api/gps") { sendJson(r, 200, "{\"supported\":false}"); return true; }
  sendErr(r, 404, "This board has no GNSS connector.");
  return true;
}
#endif
