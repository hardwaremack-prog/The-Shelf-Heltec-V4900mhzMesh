// The Shelf for Heltec V4 - the Tools: Home board, Notes, Docs, Sheets, Slides, Draw,
// Visitors, weather, and the settings endpoints The Shelf's web app asks for.
#include "shelf.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <Preferences.h>

static const char* const COLLECTIONS[] = {"notes", "documents", "presentations", "sheets", nullptr};
static String collDir(const String& kind) { return String(DATA_ROOT "/") + kind; }

// The "updated" stamp is always the last thing in files the board writes.
static uint64_t updatedOf(const String& path) {
  File f = store->open(path, FILE_READ);
  if (!f) return 0;
  size_t sz = f.size(), n = std::min(sz, (size_t)96);
  char b[97]; f.seek(sz - n); n = f.read((uint8_t*)b, n); b[n] = 0; f.close();
  const char* p = strstr(b, "\"updated\":");
  const char* last = nullptr;
  while (p) { last = p; p = strstr(p + 1, "\"updated\":"); }
  return last ? strtoull(last + 10, nullptr, 10) : 0;
}

static void listByUpdated(AsyncWebServerRequest* r, const String& dir, const char* suffix) {
  std::vector<std::pair<uint64_t, String>> v;
  File d = store->open(dir);
  if (d && d.isDirectory()) {
    File c = d.openNextFile();
    while (c) {
      String n = c.name();
      if (!c.isDirectory() && n.endsWith(suffix)) { String p = c.path(); c.close(); v.push_back({updatedOf(p), p}); }
      else c.close();
      c = d.openNextFile();
    }
    d.close();
  }
  std::sort(v.begin(), v.end(), [](const std::pair<uint64_t, String>& a, const std::pair<uint64_t, String>& b) { return a.first > b.first; });
  auto* paths = new std::vector<String>();
  for (auto& e : v) paths->push_back(e.second);
  sendJsonArrayOfFiles(r, paths);
}

// ------------------------------------------------------------ Notes / Documents / Presentations / Sheets
static bool collection(AsyncWebServerRequest* r, const String& kind, const String& id, WebRequestMethod m) {
  String dir = collDir(kind);
  mkdirs(dir);
  if (!id.length()) {
    if (m == HTTP_GET) { listByUpdated(r, dir, ".json"); return true; }
    if (m == HTTP_POST) {
      if (!bodyValidObject(r)) { sendErr(r, 400, "Invalid item"); return true; }
      String nid = uuid4(), path = dir + "/" + nid + ".json";
      String tail = ",\"id\":\"" + nid + "\",\"updated\":" + String((double)nowMs(), 0) + "}";
      if (!bodyWriteWithTail(r, path, tail)) { sendErr(r, 500, "Could not save."); return true; }
      statsDirty = true;
      sendJsonFile(r, path); return true;
    }
    return false;
  }
  if (!isValidId(id)) { sendErr(r, 400, "Invalid id"); return true; }
  String path = dir + "/" + id + ".json";
  if (m == HTTP_GET) { if (!store->exists(path)) sendErr(r, 404, "Not found"); else sendJsonFile(r, path); return true; }
  if (m == HTTP_PUT) {
    if (!store->exists(path)) { sendErr(r, 404, "Not found"); return true; }
    if (!bodyValidObject(r)) { sendErr(r, 400, "Invalid item"); return true; }
    String tail = ",\"id\":\"" + id + "\",\"updated\":" + String((double)nowMs(), 0) + "}";
    if (!bodyWriteWithTail(r, path, tail)) { sendErr(r, 500, "Could not save."); return true; }
    statsDirty = true;
    sendJsonFile(r, path); return true;
  }
  if (m == HTTP_DELETE) {
    if (!store->remove(path)) { sendErr(r, 404, "Not found"); return true; }
    sendJson(r, 200, "{\"deleted\":\"" + id + "\"}"); return true;
  }
  return false;
}

// ------------------------------------------------------------ Draw
static String drawingJson(const String& id, const String& title, uint64_t updated) {
  return "{\"id\":\"" + id + "\",\"title\":\"" + jsonEsc(title) + "\",\"updated\":" + String((double)updated, 0) +
         ",\"url\":\"/data-files/drawings/" + id + ".png?v=" + String((uint32_t)(updated / 1000)) + "\"}";
}

static bool drawings(AsyncWebServerRequest* r, const String& id, WebRequestMethod m) {
  String dir = DATA_ROOT "/drawings";
  mkdirs(dir);
  if (!id.length() && m == HTTP_GET) {
    // meta files are small: build the list in memory
    std::vector<std::pair<uint64_t, String>> v;
    File d = store->open(dir);
    if (d) { File c = d.openNextFile(); while (c) { String n = c.name(); String p = c.path(); c.close();
      if (n.endsWith(".json")) { JsonDocument j(&psAlloc); if (loadJsonFile(p, j)) v.push_back({j["updated"] | 0ULL, drawingJson(j["id"] | "", j["title"] | "Untitled", j["updated"] | 0ULL)}); }
      c = d.openNextFile(); } d.close(); }
    std::sort(v.begin(), v.end(), [](const std::pair<uint64_t, String>& a, const std::pair<uint64_t, String>& b) { return a.first > b.first; });
    String s = "["; for (size_t i = 0; i < v.size(); i++) { if (i) s += ","; s += v[i].second; } s += "]";
    sendJson(r, 200, s); return true;
  }
  JsonDocument filter(&psAlloc); filter["title"] = true;
  if (!id.length() && m == HTTP_POST) {
    JsonDocument b(&psAlloc); parseBody(r, b, &filter);
    String nid = uuid4();
    if (!bodyDataUrlToFile(r, "dataUrl", dir + "/" + nid + ".png", 20u * 1024 * 1024)) { sendErr(r, 400, "Expected a PNG data URL"); return true; }
    String title = b["title"] | "Untitled"; if (!title.length()) title = "Untitled";
    uint64_t t = nowMs();
    JsonDocument meta(&psAlloc); meta["id"] = nid; meta["title"] = title; meta["updated"] = t;
    saveJsonFile(dir + "/" + nid + ".json", meta);
    sendJson(r, 200, drawingJson(nid, title, t)); return true;
  }
  if (!isValidId(id)) { sendErr(r, 400, "Invalid id"); return true; }
  String metaPath = dir + "/" + id + ".json";
  if (m == HTTP_PUT) {
    if (!store->exists(metaPath)) { sendErr(r, 404, "Not found"); return true; }
    JsonDocument b(&psAlloc); parseBody(r, b, &filter);
    String tmp = dir + "/" + id + ".png.part";
    if (bodyDataUrlToFile(r, "dataUrl", tmp, 20u * 1024 * 1024)) moveFile(tmp, dir + "/" + id + ".png");
    String title = b["title"] | "Untitled"; if (!title.length()) title = "Untitled";
    uint64_t t = nowMs();
    JsonDocument meta(&psAlloc); meta["id"] = id; meta["title"] = title; meta["updated"] = t;
    saveJsonFile(metaPath, meta);
    sendJson(r, 200, drawingJson(id, title, t)); return true;
  }
  if (m == HTTP_DELETE) {
    store->remove(metaPath); store->remove(dir + "/" + id + ".png");
    sendJson(r, 200, "{\"deleted\":\"" + id + "\"}"); return true;
  }
  return false;
}

// ------------------------------------------------------------ Home board (portal)
static const char* PORTAL_FILE = DATA_ROOT "/portal.json";
static void loadPortal(JsonDocument& p) {
  if (!loadJsonFile(PORTAL_FILE, p) || !p.is<JsonObject>()) p.to<JsonObject>();
  if (!p["links"].is<JsonArray>()) p["links"].to<JsonArray>();
  if (!p["posts"].is<JsonArray>()) p["posts"].to<JsonArray>();
}

void portalMediaDone(AsyncWebServerRequest* r) {
  UploadCtx* u = (UploadCtx*)r->_tempObject;
  if (!u || u->magic != UPLOAD_MAGIC || !u->count || u->err) return sendErr(r, 400, "Only images and videos can be pasted onto the board.");
  FsGuard g;
  JsonDocument p(&psAlloc); loadPortal(p);
  JsonObject post = p["posts"].as<JsonArray>().add<JsonObject>();
  post["id"] = uuid4();
  post["type"] = u->mediaType;
  post["mediaUrl"] = String("/data-files/portal-media/") + u->saved;
  post["color"] = r->hasParam("color", true) ? r->getParam("color", true)->value() : String("amber");
  post["created"] = nowMs();
  saveJsonFile(PORTAL_FILE, p);
  String s; serializeJson(post, s); sendJson(r, 200, s);
}

static bool portal(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  FsGuard g;
  if (url == "/api/portal" && m == HTTP_GET) { JsonDocument p(&psAlloc); loadPortal(p); sendDoc(r, p); return true; }
  if (url == "/api/portal/posts/media" && m == HTTP_POST) { portalMediaDone(r); return true; }
  JsonDocument p(&psAlloc); loadPortal(p);
  JsonDocument b(&psAlloc); parseBody(r, b);
  if (url == "/api/portal/links" && m == HTTP_POST) {
    JsonObject l = p["links"].as<JsonArray>().add<JsonObject>();
    l["id"] = uuid4();
    String u = b["url"] | "";
    String label = b["label"] | ""; if (!label.length()) label = u;
    l["label"] = label; l["url"] = u;
    saveJsonFile(PORTAL_FILE, p);
    String s; serializeJson(l, s); sendJson(r, 200, s); return true;
  }
  if (url == "/api/portal/posts" && m == HTTP_POST) {
    JsonObject post = p["posts"].as<JsonArray>().add<JsonObject>();
    post["id"] = uuid4(); post["type"] = "text";
    post["text"] = b["text"] | ""; post["color"] = b["color"] | "amber"; post["created"] = nowMs();
    saveJsonFile(PORTAL_FILE, p);
    String s; serializeJson(post, s); sendJson(r, 200, s); return true;
  }
  if (m == HTTP_DELETE && (url.startsWith("/api/portal/links/") || url.startsWith("/api/portal/posts/"))) {
    bool links = url.startsWith("/api/portal/links/");
    String id = url.substring(18);
    JsonArray a = p[links ? "links" : "posts"].as<JsonArray>();
    for (size_t i = 0; i < a.size(); i++) {
      if (String(a[i]["id"] | "") == id) {
        String media = a[i]["mediaUrl"] | "";
        if (media.startsWith("/data-files/portal-media/")) {
          String f = media.substring(25);
          if (f.indexOf('/') < 0 && f.indexOf("..") < 0) store->remove(String(DATA_ROOT "/portal-media/") + f);
        }
        a.remove(i); break;
      }
    }
    saveJsonFile(PORTAL_FILE, p);
    sendJson(r, 200, "{\"deleted\":\"" + jsonEsc(id) + "\"}"); return true;
  }
  return false;
}

// ------------------------------------------------------------ Visitors
static const char* VISITORS_FILE = DATA_ROOT "/visitors.json";
void recordVisitor(const String& ip) {
  if (!ip.length() || ip == "127.0.0.1") return;
  FsGuard g;
  JsonDocument v(&psAlloc);
  if (!loadJsonFile(VISITORS_FILE, v) || !v.is<JsonObject>()) v.to<JsonObject>();
  uint64_t now = nowMs();
  JsonObject o = v[ip].is<JsonObject>() ? v[ip].as<JsonObject>() : v[ip].to<JsonObject>();
  o["ip"] = ip;
  if (o["hostname"].isNull()) o["hostname"] = nullptr;
  if (o["firstSeenAt"].isNull()) o["firstSeenAt"] = now;
  o["lastSeenAt"] = now;
  saveJsonFile(VISITORS_FILE, v);
}

// ------------------------------------------------------------ outside temperature, last 4 days
static String wxKey;                                // the weather cache's place (see weather())
static String wxErr;                                // why there's no graph yet (settings page)
// now: for The Shelf's Home page (°F, Open-Meteo's weather code)
static float curF = NAN; static int curCode = -1; static bool curDay = true; static uint32_t curAt = 0;
// The board learns where it is from The Shelf's Home page (its weather asks with the browser's
// location), keeps that, and once an hour (on a home Wi-Fi with internet) fetches the hourly
// temperature for the past 4 days from Open-Meteo, for the screen's graph.
static Preferences wxp; static bool wxpOpen = false;
static float wxLat = NAN, wxLon = NAN;
static bool wxManual = false;                       // set by hand on the settings page: browsers don't change it
static String wxPlace;
static float hist[WX_HOURS]; static int histN = 0; static uint32_t histStart = 0;
static uint32_t histAt = 0; static volatile bool histBusy = false;
static portMUX_TYPE histMux = portMUX_INITIALIZER_UNLOCKED;
static void wxOpen() {
  if (wxpOpen) return;
  wxp.begin("wx", false); wxpOpen = true;
  wxLat = wxp.getFloat("lat", NAN); wxLon = wxp.getFloat("lon", NAN);
  wxManual = wxp.getBool("manual", false); wxPlace = wxp.getString("place", "");
}
void wxRemember(double lat, double lon) {
  wxOpen();
  if (wxManual) return;
  if (!isnan(wxLat) && fabs(wxLat - lat) < 0.05 && fabs(wxLon - lon) < 0.05) return;
  wxLat = lat; wxLon = lon; wxp.putFloat("lat", wxLat); wxp.putFloat("lon", wxLon);
  wxPlace = "From a browser"; wxp.putString("place", wxPlace);
  histAt = 0;                                       // somewhere new: fetch again soon
}
static String urlEnc(const String& v) {
  String o; char h[4];
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_') o += c; else { snprintf(h, sizeof h, "%%%02X", (uint8_t)c); o += h; }
  }
  return o;
}
// The board's location, from its settings page:
//   GET  /api/board/location               {lat, lon, place, manual}
//   GET  /api/board/location/search?q=...  places with that name (Open-Meteo's geocoding)
//   POST /api/board/location               {lat, lon, place}  set it by hand;  {auto:true}  back to the browser's
static void wxPush(AsyncWebServerRequest* r);
bool handleLocationApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (url == "/api/board/wxdata" && m == HTTP_POST) { wxPush(r); return true; }
  if (url != "/api/board/location" && url != "/api/board/location/search") return false;
  wxOpen();
  if (url == "/api/board/location/search") {
    String q = r->hasParam("q") ? r->getParam("q")->value() : "";
    q.trim();
    if (q.length() < 2) { sendErr(r, 400, "Type a town, city or ZIP code."); return true; }
    if (!staOK) { sendErr(r, 502, "The board needs your home Wi-Fi (with internet) to look places up."); return true; }
    HTTPClient http; http.setTimeout(8000);
    String u = "http://geocoding-api.open-meteo.com/v1/search?count=6&language=en&format=json&name=" + urlEnc(q);
    if (!http.begin(u)) { sendErr(r, 502, "Couldn't reach the place finder."); return true; }
    int code = http.GET();
    if (code != 200) { http.end(); sendErr(r, 502, String("Couldn't reach the place finder (") + code + ")."); return true; }
    JsonDocument f; JsonObject fe = f["results"][0].to<JsonObject>();
    fe["name"] = true; fe["latitude"] = true; fe["longitude"] = true; fe["admin1"] = true; fe["country"] = true; fe["postcodes"] = true;
    JsonDocument j(&psAlloc);
    DeserializationError e = deserializeJson(j, http.getStream(), DeserializationOption::Filter(f));
    http.end();
    if (e) { sendErr(r, 502, "Unexpected answer from the place finder."); return true; }
    JsonDocument out(&psAlloc); JsonArray a = out.to<JsonArray>();
    for (JsonObject p : j["results"].as<JsonArray>()) {
      JsonObject o = a.add<JsonObject>();
      String nm = p["name"] | "";
      if (!p["admin1"].isNull()) nm += String(", ") + (const char*)p["admin1"];
      if (!p["country"].isNull()) nm += String(", ") + (const char*)p["country"];
      o["place"] = nm; o["lat"] = p["latitude"]; o["lon"] = p["longitude"];
    }
    sendDoc(r, out);
    return true;
  }
  if (m == HTTP_POST) {
    JsonDocument b; parseBody(r, b);
    if (b["auto"] | false) {
      wxManual = false; wxPlace = ""; wxp.putBool("manual", false); wxp.remove("place");
      wxLat = wxLon = NAN; wxp.remove("lat"); wxp.remove("lon"); histAt = 0;   // found again: browser or internet address
    } else {
      float la = b["lat"] | NAN, lo = b["lon"] | NAN;
      if (isnan(la) || isnan(lo) || la < -90 || la > 90 || lo < -180 || lo > 180) { sendErr(r, 400, "That isn't a place on the map."); return true; }
      wxLat = la; wxLon = lo; wxManual = true; wxPlace = String(b["place"] | "").substring(0, 80);
      wxp.putFloat("lat", wxLat); wxp.putFloat("lon", wxLon); wxp.putBool("manual", true); wxp.putString("place", wxPlace);
      histAt = 0; wxKey = ""; curAt = 0; portENTER_CRITICAL(&histMux); histN = 0; portEXIT_CRITICAL(&histMux);   // the new place's
    }
  }
  JsonDocument o;
  if (isnan(wxLat)) { o["lat"] = nullptr; o["lon"] = nullptr; } else { o["lat"] = wxLat; o["lon"] = wxLon; }
  o["place"] = wxPlace; o["manual"] = wxManual;
  o["hours"] = histN; o["error"] = wxErr; o["online"] = staOK;
  if (histAt && histN) o["minutesAgo"] = (millis() - histAt) / 60000;
  sendDoc(r, o);
  return true;
}
const char* wxStatusText() {                        // the screen's graph page, while it has nothing to draw
  if (!staOK) return "Needs home Wi-Fi";
  if (wxErr.length()) return "Couldn't get them yet";
  return "Getting them...";
}
// The GPS has a fix: the board is exactly here (beats a typed-in ZIP or an internet-address guess).
// Saved when it's moved a kilometre or so, and the weather is fetched for the new place.
void wxFromGps(double lat, double lon) {
  wxOpen();
  bool moved = isnan(wxLat) || fabs(wxLat - lat) > 0.01 || fabs(wxLon - lon) > 0.01;
  wxLat = lat; wxLon = lon;
  if (moved) {
    wxp.putFloat("lat", wxLat); wxp.putFloat("lon", wxLon);
    wxPlace = "From the GPS"; wxp.putString("place", wxPlace);
    if (isnan(wxLat) || histAt) histAt = 0;
  }
}
bool wxLocation(float& lat, float& lon) { wxOpen(); lat = wxLat; lon = wxLon; return !isnan(wxLat); }
int wxHistoryCopy(float* out, int max, uint32_t* startUtc) {
  portENTER_CRITICAL(&histMux);
  int n = std::min(histN, max);
  memcpy(out, hist + (histN - n), n * sizeof(float));
  if (startUtc) *startUtc = histStart + (histN - n) * 3600;
  portEXIT_CRITICAL(&histMux);
  return n;
}
// No location yet (browsers only share theirs over https, and the board is plain http): guess it
// from the home internet connection's address. Good to a city or so; set it by hand to be exact.
static bool getJson(const String& url, JsonDocument& doc, JsonDocument& filter);
static bool ipLocate() {
  JsonDocument f; f["success"] = true; f["latitude"] = true; f["longitude"] = true; f["city"] = true; f["region"] = true;
  f["status"] = true; f["lat"] = true; f["lon"] = true; f["regionName"] = true;
  JsonDocument j;
  float la = NAN, lo = NAN; String city, region;
  if (getJson("https://ipwho.is/", j, f) && (j["success"] | true) && !j["latitude"].isNull()) {
    la = j["latitude"]; lo = j["longitude"]; city = j["city"] | ""; region = j["region"] | "";
  } else if (getJson("http://ip-api.com/json/?fields=status,lat,lon,city,regionName", j, f) && String(j["status"] | "") == "success") {
    la = j["lat"]; lo = j["lon"]; city = j["city"] | ""; region = j["regionName"] | "";
  }
  if (isnan(la)) return false;
  wxLat = la; wxLon = lo;
  wxPlace = city + ", " + region + " (from your internet address)";
  wxp.putFloat("lat", wxLat); wxp.putFloat("lon", wxLon); wxp.putString("place", wxPlace);
  return true;
}
// Keep the hours up to now, newest last (from Open-Meteo's hourly time / temperature_2m arrays).
static bool storeHours(JsonArray t, JsonArray v) {
  uint32_t now = (uint32_t)time(nullptr) + 60;
  static float tmp[WX_HOURS]; int n = 0; uint32_t first = 0;
  for (size_t i = 0; i < t.size() && i < v.size(); i++) {
    uint32_t ts = t[i] | 0u;
    if (!ts || ts > now || v[i].isNull()) continue;                 // past hours only
    if (n == WX_HOURS) { memmove(tmp, tmp + 1, (n - 1) * sizeof(float)); n--; first += 3600; }
    if (!n) first = ts;
    tmp[n++] = v[i].as<float>();
  }
  if (n < 12) return false;
  portENTER_CRITICAL(&histMux);
  memcpy(hist, tmp, n * sizeof(float)); histN = n; histStart = first;
  portEXIT_CRITICAL(&histMux);
  return true;
}

// A GET that copes with https (Open-Meteo sends plain http over to https now, and the board's old
// plain-http requests got a redirect instead of the weather). Runs in its own task: the secure
// connection needs more stack than the web server's.
static bool getJson(const String& url, JsonDocument& doc, JsonDocument& filter) {
  HTTPClient http; http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  WiFiClientSecure tls; WiFiClient plain;
  bool ok;
  if (url.startsWith("https://")) { tls.setInsecure(); ok = http.begin(tls, url); } else ok = http.begin(plain, url);
  if (!ok) return false;
  int code = http.GET();
  if (code != 200) { Serial.printf("[shelf] weather: %s answered %d\n", url.substring(0, 40).c_str(), code); http.end(); return false; }
  DeserializationError e = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  return !e;
}

static void wxTask(void*) {
  if (isnan(wxLat) && !ipLocate()) {
    wxErr = "Couldn't work out where the board is. Set its location below.";
    histAt = millis() - 3000000; histBusy = false; vTaskDelete(nullptr); return;
  }
  char url[300];
  snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&current=temperature_2m,weather_code,is_day&hourly=temperature_2m&past_days=4&forecast_days=1&temperature_unit=fahrenheit&timeformat=unixtime", wxLat, wxLon);
  JsonDocument f; f["hourly"]["time"] = true; f["hourly"]["temperature_2m"] = true;
  f["current"]["temperature_2m"] = true; f["current"]["weather_code"] = true; f["current"]["is_day"] = true;
  JsonDocument j(&psAlloc);
  bool ok = getJson(url, j, f) && storeHours(j["hourly"]["time"], j["hourly"]["temperature_2m"]);
  if (ok && !j["current"]["temperature_2m"].isNull()) {
    curF = j["current"]["temperature_2m"]; curCode = j["current"]["weather_code"] | -1; curDay = (j["current"]["is_day"] | 1) != 0; curAt = millis() | 1;
  }
  wxErr = ok ? "" : "Couldn't get the temperatures. Trying again in 5 minutes (or open The Shelf).";
  histAt = ok ? millis() | 1 : millis() - 3300000;                  // didn't work: try again in 5 minutes
  histBusy = false;
  vTaskDelete(nullptr);
}
void wxHistoryTick() {
  if (!staOK || histBusy || time(nullptr) < 1700000000) return;
  wxOpen();
  if (histAt && millis() - histAt < 3600000) return;
  histBusy = true;
  if (xTaskCreatePinnedToCore(wxTask, "wxhist", 12288, nullptr, 1, nullptr, 1) != pdPASS) { histBusy = false; histAt = millis() - 3300000; }
}

// ------------------------------------------------------------ Weather (Open-Meteo, cached 10 minutes)
static String wxData; static uint32_t wxAt = 0;
static void weather(AsyncWebServerRequest* r) {
  double lat = r->hasParam("lat") ? r->getParam("lat")->value().toDouble() : NAN;
  double lon = r->hasParam("lon") ? r->getParam("lon")->value().toDouble() : NAN;
  if (r->hasParam("lat") && r->hasParam("lon") && !isnan(lat) && !isnan(lon)) wxRemember(lat, lon);
  // The weather comes from the hourly fetch (wxTask) or from a browser that fetched it for the
  // board (POST /api/board/wxdata); answering doesn't go out to the internet itself.
  if (!curAt || millis() - curAt > 2UL * 3600 * 1000 || isnan(curF)) {
    if (!histBusy && (!histAt || millis() - histAt > 300000)) histAt = 0;   // fetch soon
    return sendErr(r, 503, "The board hasn't got the weather yet.");
  }
  struct W { int c; const char* l; const char* i; };
  static const W wmo[] = {{0,"Clear sky","☀️"},{1,"Mainly clear","\U0001F324️"},{2,"Partly cloudy","⛅"},{3,"Overcast","☁️"},
    {45,"Fog","\U0001F32B️"},{48,"Freezing fog","\U0001F32B️"},{51,"Light drizzle","\U0001F326️"},{53,"Drizzle","\U0001F326️"},
    {55,"Heavy drizzle","\U0001F327️"},{56,"Light freezing drizzle","\U0001F327️"},{57,"Freezing drizzle","\U0001F327️"},
    {61,"Light rain","\U0001F327️"},{63,"Rain","\U0001F327️"},{65,"Heavy rain","\U0001F327️"},{66,"Light freezing rain","\U0001F327️"},
    {67,"Freezing rain","\U0001F327️"},{71,"Light snow","\U0001F328️"},{73,"Snow","\U0001F328️"},{75,"Heavy snow","❄️"},
    {77,"Snow grains","\U0001F328️"},{80,"Light rain showers","\U0001F326️"},{81,"Rain showers","\U0001F327️"},
    {82,"Heavy rain showers","⛈️"},{85,"Light snow showers","\U0001F328️"},{86,"Heavy snow showers","❄️"},
    {95,"Thunderstorm","⛈️"},{96,"Thunderstorm with hail","⛈️"},{99,"Severe thunderstorm","⛈️"}};
  const char* label = "Unknown"; const char* icon = "\U0001F321️";
  for (auto& w : wmo) if (w.c == curCode) { label = w.l; icon = w.i; }
  if (curCode <= 1 && !curDay) icon = "\U0001F319";                          // clear at night: the moon
  JsonDocument out;
  out["tempF"] = (int)lroundf(curF); out["tempC"] = (int)lroundf((curF - 32) * 5 / 9);
  out["condition"] = label; out["icon"] = icon; out["isDay"] = curDay;
  sendDoc(r, out);
}
// A browser that fetched the weather (over https, which it can and the old board code couldn't)
// hands it to the board: {lat, lon, place, time:[unix...], temp:[°F...], now:{f, code, day}}
static void wxPush(AsyncWebServerRequest* r) {
  JsonDocument b(&psAlloc);
  if (!parseBody(r, b)) return sendErr(r, 400, "Invalid request");
  wxOpen();
  float la = b["lat"] | NAN, lo = b["lon"] | NAN;
  if (!wxManual && !isnan(la) && !isnan(lo) && (isnan(wxLat) || fabsf(wxLat - la) > 0.05f || fabsf(wxLon - lo) > 0.05f)) {
    wxLat = la; wxLon = lo; wxp.putFloat("lat", wxLat); wxp.putFloat("lon", wxLon);
    String pl = b["place"] | ""; if (pl.length()) { wxPlace = pl + " (from your internet address)"; wxp.putString("place", wxPlace); }
  }
  if (b["time"].is<JsonArray>() && storeHours(b["time"], b["temp"])) { histAt = millis() | 1; wxErr = ""; }
  if (!b["now"]["f"].isNull()) { curF = b["now"]["f"]; curCode = b["now"]["code"] | -1; curDay = (b["now"]["day"] | 1) != 0; curAt = millis() | 1; }
  sendJson(r, 200, "{\"ok\":true}");
}

// ------------------------------------------------------------ router
bool handleToolsApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (handleLocationApi(r, url, m)) return true;
  for (int i = 0; COLLECTIONS[i]; i++) {
    String base = String("/api/") + COLLECTIONS[i];
    if (url == base) return collection(r, COLLECTIONS[i], "", m);
    if (url.startsWith(base + "/")) {
      String id = url.substring(base.length() + 1);
      if (String(COLLECTIONS[i]) == "sheets" && id == "import") { sendErr(r, 501, "Importing spreadsheet files needs The Shelf on a computer. You can still make sheets here."); return true; }
      return collection(r, COLLECTIONS[i], id, m);
    }
  }
  if (url == "/api/drawings") return drawings(r, "", m);
  if (url.startsWith("/api/drawings/")) return drawings(r, url.substring(14), m);
  if (url.startsWith("/api/portal")) return portal(r, url, m);

  if (url == "/api/visitors") {
    if (m == HTTP_DELETE) { store->remove(VISITORS_FILE); sendJson(r, 200, "{\"cleared\":true}"); return true; }
    JsonDocument v(&psAlloc); loadJsonFile(VISITORS_FILE, v);
    std::vector<JsonObject> list; for (JsonPair kv : v.as<JsonObject>()) list.push_back(kv.value().as<JsonObject>());
    std::sort(list.begin(), list.end(), [](JsonObject a, JsonObject b) { return (a["lastSeenAt"] | 0ULL) > (b["lastSeenAt"] | 0ULL); });
    JsonDocument out(&psAlloc); JsonArray a = out.to<JsonArray>(); for (auto& o : list) a.add(o);
    sendDoc(r, out); return true;
  }
  if (url == "/api/weather") { weather(r); return true; }

  // ---- settings The Shelf's page reads. On the board everyone on your Wi-Fi can use it,
  //      like Heltec Shelf Lite; there's no "host computer" to sit at, so every visitor
  //      is treated as the host (the host-only switches are hidden by board.css).
  if (url == "/api/auth/status") {
    sendJson(r, 200, "{\"isLocal\":true,\"protectionEnabled\":false,\"secureMode\":false,\"loggedIn\":false,\"username\":null,\"role\":\"write\"}");
    return true;
  }
  if (url == "/api/auth/mode" && m == HTTP_GET) { sendJson(r, 200, "{\"secureMode\":false}"); return true; }
  if (url.startsWith("/api/auth/")) { sendErr(r, 403, "Accounts aren't available on the Heltec board -- anyone on your home Wi-Fi can use it."); return true; }
  if (url == "/api/power") { if (m == HTTP_GET) sendJson(r, 200, "{\"on\":true}"); else sendErr(r, 403, "The board is always on. Unplug it to turn it off."); return true; }
  if (url == "/api/localhost-only") { if (m == HTTP_GET) sendJson(r, 200, "{\"localhostOnly\":false}"); else sendErr(r, 403, "Not available on the board."); return true; }
  if (url == "/api/open-write-access") { if (m == HTTP_GET) sendJson(r, 200, "{\"openWriteAccess\":true}"); else sendErr(r, 403, "On the board, anyone on your Wi-Fi can already make changes."); return true; }
  if (url == "/api/network-info") {
    sendJson(r, 200, "{\"ips\":[\"" + hostName + ".local\"],\"port\":80,\"ip\":\"" + currentIP() + "\"}");
    return true;
  }
  if (url.startsWith("/api/backup")) { sendErr(r, 501, "Backups aren't available on the board -- copy the microSD card to your computer instead. Everything is in normal folders, with The Shelf's own data in the hidden .shelf folder."); return true; }
  if (url.startsWith("/api/ai/")) { sendErr(r, 501, "The AI assistant needs The Shelf on a computer."); return true; }
  if (url.startsWith("/api/imagine")) {
    if (url == "/api/imagine/status") { sendJson(r, 200, "{\"hasKey\":false,\"canManageKey\":false}"); return true; }
    sendErr(r, 501, "Create with AI needs The Shelf on a computer."); return true;
  }
  if (url.startsWith("/api/bigfiles")) {
    if (url == "/api/bigfiles" && m == HTTP_GET) { sendJson(r, 200, "[]"); return true; }
    sendErr(r, 501, "Big Transfer isn't available on the board. Use the normal shelves (up to 4 GB per file)."); return true;
  }
  if (url.startsWith("/api/vnc/")) { sendErr(r, 501, "Remote desktop (VNC) needs The Shelf on a computer."); return true; }
  return false;
}
