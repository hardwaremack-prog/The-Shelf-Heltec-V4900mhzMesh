// The Shelf on a board - folder locks: a read-only viewing password for chosen shelves and tools.
//
// The same feature as The Shelf on a computer (public/locks.js draws it), with the board's admin
// as "the host": a browser unlocked with the admin password (or, before one is set, nobody) picks
// which areas are locked and sets one viewing password. Everyone else has to enter that password
// to look at a locked area, and even then it's strictly read-only: changing a locked area is only
// ever allowed for the admin.
//
//   GET    /api/locks/status        what's locked, and whether this browser may look / is the host
//   POST   /api/locks/unlock        {password}  look at locked areas for 2 hours (cookie shelfUnlock)
//   POST   /api/locks/relock        forget this browser's unlock
//   PUT    /api/locks/config        host: {areas:[...], password, enabled}
//   POST   /api/locks/lock-everyone host: sign everyone out of locked areas now
//   DELETE /api/locks               host: remove the password and every lock
#include "shelf.h"
#include <Preferences.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

static Preferences lp;
static const char* const TOOL_IDS[] = {"notes", "documents", "sheets", "presentations", "drawings", "messages"};
static const char* const TOOL_LABELS[] = {"Notes", "Docs", "Sheets", "Slides", "Draw", "Messages"};
static const int NUM_TOOLS = 6;

static uint32_t lockedBits = 0;      // which areas: bit i = CATEGORIES[i], bit NUM_CATEGORIES + j = TOOL_IDS[j]
static bool haveKey = false, enabled = true;
static uint8_t salt[16], key[32];
static uint32_t epoch = 1;

struct Unlock { char tok[33]; uint32_t at; uint32_t epoch; };
static std::vector<Unlock> unlocks;
static const uint32_t UNLOCK_MS = 2UL * 3600 * 1000;
static int fails = 0; static uint32_t waitUntil = 0;

static int areaCount() { return NUM_CATEGORIES + NUM_TOOLS; }
static String areaId(int i) { return i < NUM_CATEGORIES ? String(CATEGORIES[i]) : String(TOOL_IDS[i - NUM_CATEGORIES]); }
static int areaIndex(String id) {
  id.toLowerCase();
  for (int i = 0; i < areaCount(); i++) { String a = areaId(i); a.toLowerCase(); if (a == id) return i; }
  return -1;
}

static void derive(const String& pw, uint8_t out[32]) {
  mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const uint8_t*)pw.c_str(), pw.length(), salt, sizeof salt, 2048, 32, out);
  mbedtls_md_free(&ctx);
}

static void save() {
  lp.putUInt("bits", lockedBits); lp.putUInt("ep", epoch); lp.putBool("on", enabled);
  if (haveKey) { lp.putBytes("salt", salt, 16); lp.putBytes("key", key, 32); } else { lp.remove("salt"); lp.remove("key"); }
}

void locksBegin() {
  lp.begin("locks", false);
  lockedBits = lp.getUInt("bits", 0); epoch = lp.getUInt("ep", 1); enabled = lp.getBool("on", true);
  if (lp.getUChar("nshelf", 7) < NUM_CATEGORIES) {   // a shelf was added (Music (Opus)): the tools' bits move up one
    uint32_t shelves = lockedBits & 0x7F, tools = lockedBits >> 7;
    lockedBits = shelves | (tools << NUM_CATEGORIES);
    lp.putUInt("bits", lockedBits); lp.putUChar("nshelf", NUM_CATEGORIES);
  }
  haveKey = lp.getBytes("salt", salt, 16) == 16 && lp.getBytes("key", key, 32) == 32;
}

// Locks only mean anything once a viewing password exists, the switch is on, and there's an admin
// to manage them (clearing the admin password with the button turns them off too).
static uint32_t activeBits() { return (enabled && haveKey && adminIsSet()) ? lockedBits : 0; }

// The host: a browser unlocked with the admin password. With no admin password, nobody is.
static bool isHost(AsyncWebServerRequest* r) { return adminIsSet() && adminUnlocked(r); }

static String cookie(AsyncWebServerRequest* r, const char* name) {
  const AsyncWebHeader* h = r->getHeader("Cookie");
  if (!h) return "";
  const String& v = h->value();
  String k = String(name) + "=";
  int p = v.indexOf(k);
  if (p < 0) return "";
  int e = v.indexOf(';', p);
  return v.substring(p + k.length(), e < 0 ? v.length() : e);
}
static bool hasUnlocked(AsyncWebServerRequest* r) {
  String t = cookie(r, "shelfUnlock");
  if (t.length() != 32) return false;
  for (auto it = unlocks.begin(); it != unlocks.end();) {
    if (millis() - it->at > UNLOCK_MS || it->epoch != epoch) { it = unlocks.erase(it); continue; }
    if (t == it->tok) return true;
    ++it;
  }
  return false;
}

// Which locked areas does this request touch? (bits, as lockedBits)
static uint32_t areasFor(AsyncWebServerRequest* r, const String& rawUrl, uint32_t locked) {
  String u = rawUrl; u.replace('\\', '/'); while (u.indexOf("//") >= 0) u.replace("//", "/"); u.toLowerCase();
  uint32_t found = 0;
  auto add = [&](const String& id) { int i = areaIndex(id); if (i >= 0 && (locked & (1u << i))) found |= 1u << i; };
  auto seg = [&](int n) {                      // the n-th piece of the path
    int s = 0;
    for (int i = 0; i < n; i++) { s = u.indexOf('/', s + 1); if (s < 0) return String(""); }
    int e = u.indexOf('/', s + 1);
    return u.substring(s + 1, e < 0 ? u.length() : e);
  };
  uint32_t shelves = locked & ((1u << NUM_CATEGORIES) - 1);
  String a = seg(0), b = seg(1), c = seg(2);
  if (a == "shared") add(b);
  else if (a == "api") {
    if (b == "files" && c.length() && c != "move") add(c);
    else if (b == "notes" || b == "documents" || b == "sheets" || b == "presentations" || b == "drawings") add(b);
    else if (b == "messaging" && (c == "conversations" || c == "send")) add("messages");
  } else if (a == "data-files") {
    if (b == "drawings") add("drawings");
    else if (b == "album-art") add("music");
    else if (b == "video-thumbs") add("videos");
    else if (b == "file-thumbs") found |= shelves;
  } else if (a == "play") found |= shelves;      // games and programs unpacked from a shelf: can't tell which, so any
  // a shelf named in the query string, the upload form, or a move's body
  for (const char* p : {"category", "fromCategory", "toCategory"}) {
    if (r->hasParam(p)) add(r->getParam(p)->value());
    if (r->hasParam(p, true)) add(r->getParam(p, true)->value());
  }
  if (rawUrl == "/api/files/move") {
    JsonDocument f; f["fromCategory"] = true; f["toCategory"] = true;
    JsonDocument d; if (parseBody(r, d, &f)) { add(d["fromCategory"] | ""); add(d["toCategory"] | ""); }
  }
  return found;
}

static void send403(AsyncWebServerRequest* r, const char* msg, bool readOnly, bool hostOnly = false) {
  String body = String("{\"error\":\"") + msg + "\"" + (hostOnly ? ",\"hostOnly\":true" : ",\"locked\":true") + (readOnly ? ",\"readOnly\":true" : "") + "}";
  AsyncWebServerResponse* res = r->beginResponse(403, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// Called for every request before it's answered: true means it was turned away here.
bool lockGate(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  uint32_t locked = activeBits();
  if (!locked || url.startsWith("/api/locks")) return false;
  if (isHost(r)) return false;
  if (url.startsWith("/api/board/card/")) { send403(r, "Backups are admin-only while folder locks are on.", false, true); return true; }
  uint32_t touched = areasFor(r, url, locked);
  if (!touched) return false;
  bool write = !(m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS);
  if (write) { send403(r, "This folder is locked and read-only. Only the admin can change it.", true); return true; }
  if (hasUnlocked(r)) return false;
  send403(r, "This folder is locked. Enter the viewing password to open it.", false);
  return true;
}
// Uploads write as they arrive, before handleRequest: the shelf comes from the form.
bool lockBlocksUpload(AsyncWebServerRequest* r) {
  uint32_t locked = activeBits();
  if (!locked || isHost(r) || !r->hasParam("category", true)) return false;
  int i = areaIndex(r->getParam("category", true)->value());
  return i >= 0 && (locked & (1u << i));
}

static String statusJson(AsyncWebServerRequest* r) {
  bool host = isHost(r);
  uint32_t act = activeBits();
  String s = String("{\"isLocal\":") + (host || !adminIsSet() ? "true" : "false") + ",\"adminSet\":" + (adminIsSet() ? "true" : "false") +
             ",\"hasPassword\":" + (haveKey ? "true" : "false") + ",\"enabled\":" + (enabled ? "true" : "false") + ",\"areas\":[";
  for (int i = 0; i < areaCount(); i++) {
    if (i) s += ",";
    s += String("{\"id\":\"") + areaId(i) + "\",\"label\":\"" + (i < NUM_CATEGORIES ? CATEGORIES[i] : TOOL_LABELS[i - NUM_CATEGORIES]) +
         "\",\"group\":\"" + (i < NUM_CATEGORIES ? "Shelves" : "Tools") + "\"}";
  }
  s += "],\"locked\":[";
  bool first = true;
  for (int i = 0; i < areaCount(); i++) if (act & (1u << i)) { if (!first) s += ","; s += "\"" + areaId(i) + "\""; first = false; }
  s += "]";
  if (host || !adminIsSet()) {
    s += ",\"configured\":["; first = true;
    for (int i = 0; i < areaCount(); i++) if (lockedBits & (1u << i)) { if (!first) s += ","; s += "\"" + areaId(i) + "\""; first = false; }
    s += "]";
  }
  s += String(",\"unlocked\":") + (host || hasUnlocked(r) ? "true" : "false") + "}";
  return s;
}

static bool needHost(AsyncWebServerRequest* r) {
  if (!adminIsSet()) { sendErr(r, 400, "Set an admin password first, on the board's settings page (the gear on the board card). The admin is the one who can lock folders."); return false; }
  if (!isHost(r)) { send403(r, "Only the admin can change folder locks. Unlock write mode with the admin password first.", false, true); return false; }
  return true;
}

bool handleLocksApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (url != "/api/locks" && !url.startsWith("/api/locks/")) return false;
  if (url == "/api/locks/status") { sendJson(r, 200, statusJson(r)); return true; }
  if (url == "/api/locks/unlock" && m == HTTP_POST) {
    if (waitUntil && (int32_t)(waitUntil - millis()) > 0) {
      sendErr(r, 429, String("Too many wrong tries. Wait ") + ((waitUntil - millis()) / 1000 + 1) + " seconds."); return true;
    }
    JsonDocument b; parseBody(r, b);
    String pw = b["password"] | "";
    uint8_t k[32]; bool ok = false;
    if (haveKey) { derive(pw, k); uint8_t d = 0; for (int i = 0; i < 32; i++) d |= k[i] ^ key[i]; ok = d == 0; }
    if (!ok) { if (++fails >= 5) { fails = 0; waitUntil = millis() + 60000; } sendErr(r, 401, "Wrong password."); return true; }
    fails = 0; waitUntil = 0;
    Unlock u; String t = uuid4(); t.replace("-", ""); strlcpy(u.tok, t.c_str(), sizeof u.tok); u.at = millis(); u.epoch = epoch;
    if (unlocks.size() >= 16) unlocks.erase(unlocks.begin());
    unlocks.push_back(u);
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", String("shelfUnlock=") + u.tok + "; HttpOnly; Path=/; Max-Age=7200; SameSite=Lax");
    r->send(res);
    return true;
  }
  if (url == "/api/locks/relock" && m == HTTP_POST) {
    String t = cookie(r, "shelfUnlock");
    for (auto it = unlocks.begin(); it != unlocks.end(); ++it) if (t == it->tok) { unlocks.erase(it); break; }
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", "shelfUnlock=; HttpOnly; Path=/; Max-Age=0; SameSite=Lax");
    r->send(res);
    return true;
  }
  if (url == "/api/locks/config" && m == HTTP_PUT) {
    if (!needHost(r)) return true;
    JsonDocument b; if (!parseBody(r, b)) { sendErr(r, 400, "Invalid request"); return true; }
    uint32_t bits = lockedBits;
    if (b["areas"].is<JsonArray>()) {
      bits = 0;
      for (JsonVariant a : b["areas"].as<JsonArray>()) {
        int i = areaIndex(a | "");
        if (i < 0) { sendErr(r, 400, String("Unknown area: ") + (const char*)(a | "")); return true; }
        bits |= 1u << i;
      }
    }
    if (b["password"].is<const char*>()) {
      String pw = b["password"].as<const char*>();
      if (pw.length() < 4 || pw.length() > 64) { sendErr(r, 400, "Password must be at least 4 characters."); return true; }
      for (int i = 0; i < 16; i += 4) { uint32_t x = esp_random(); memcpy(salt + i, &x, 4); }
      derive(pw, key); haveKey = true;
      epoch++;                                     // a new password signs everyone out of locked areas
    }
    if (bits && !haveKey) { sendErr(r, 400, "Set a viewing password before locking folders."); return true; }
    if (!b["enabled"].isNull()) { bool on = b["enabled"] | false; if (on != enabled) epoch++; enabled = on; }
    lockedBits = bits;
    save();
    sendJson(r, 200, statusJson(r));
    return true;
  }
  if (url == "/api/locks/lock-everyone" && m == HTTP_POST) {
    if (!needHost(r)) return true;
    epoch++; unlocks.clear(); save();
    sendJson(r, 200, "{\"ok\":true}");
    return true;
  }
  if (url == "/api/locks" && m == HTTP_DELETE) {
    if (!needHost(r)) return true;
    lockedBits = 0; haveKey = false; enabled = true; epoch++; unlocks.clear(); save();
    sendJson(r, 200, "{\"ok\":true}");
    return true;
  }
  sendErr(r, 404, "Not found");
  return true;
}
