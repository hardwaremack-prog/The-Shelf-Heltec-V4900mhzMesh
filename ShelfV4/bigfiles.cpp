// The Shelf for Heltec V4 - Big Transfer (the "bigfiles" tab), Heltec V4 MMC build only.
//
// The same API as The Shelf's server.js, so its page works unchanged: a big file comes up in
// 16 MB chunks, each one appended where the last stopped; a dropped connection only costs the
// chunk in flight, and dropping the same file in again picks up where it left off.
//
// The card is FAT32 (the board's FAT driver has no exFAT), where one file stops at 4 GB, so a
// file is kept in 1 GB pieces and joined again on the fly when it's downloaded:
//   /Big Files/<name>                 a file of 1 GB or less, as an ordinary file
//   /Big Files/<name>.parts/000 001…  anything bigger, in 1 GB pieces (a PC can join them with
//                                     "copy /b 000+001+002 name" or "cat 0* > name")
//   /.shelf/big/<id>/info.json, 000…  uploads still coming in
#include "shelf.h"
#include <esp_heap_caps.h>

#if defined(SHELF_MMC)
#define BIG_ROOT   "/Big Files"
#define BIG_IN     "/.shelf/big"
#define BIG_PART   (1024ULL * 1024ULL * 1024ULL)     // 1 GB pieces: well inside FAT32's 4 GB
#define BIG_MAGIC  0x53484247u

struct BigCtx {               // one chunk being written (internal, DMA-capable RAM; the request frees it with free())
  uint32_t magic;
  int code;                   // 0 fine so far; else the HTTP error to answer with
  uint32_t part;              // which piece is being written
  uint32_t synced;            // bytes of that piece safely on the card
  uint64_t received;          // whole upload, before this chunk
  size_t fill;                // streaming: bytes waiting in buf
  bool inRam;                 // the chunk is collected in PSRAM first (see bigRam)
  char path[64];              // the piece's path
  char dir[40];
  alignas(4) uint8_t buf[UPLOAD_BUF];
};

// A whole chunk is collected in PSRAM while it arrives, so the card never holds up the Wi-Fi
// (the network stack's receive window is small: every pause in reading the socket slows the
// sender down), then goes to the card in big writes with one flush. The card's DMA can't read
// PSRAM, so the writes go through the chunk's own 16 KB of internal RAM. One chunk at a time;
// a second upload alongside streams straight to the card instead.
#define BIG_RAM (1024 * 1024)         // the page sends 1 MB chunks to this board
static uint8_t* bigRam = nullptr;
static void* bigRamOwner = nullptr;
static uint32_t bigRamAt = 0;

static BigCtx* bigCtx(AsyncWebServerRequest* r) {
  BigCtx* c = (BigCtx*)r->_tempObject;
  return (c && c->magic == BIG_MAGIC) ? c : nullptr;
}
static String partName(uint32_t i) { char b[8]; snprintf(b, sizeof b, "%03u", (unsigned)i); return b; }
static bool isHexId(const String& s) {
  if (s.length() != 16) return false;
  for (size_t i = 0; i < s.length(); i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}
// /api/bigfiles/<id>/<verb>: the id, or "" if the url isn't one of those
static String idOf(const String& url) {
  const char* pre = "/api/bigfiles/";
  if (!url.startsWith(pre)) return "";
  String rest = url.substring(strlen(pre));
  int slash = rest.indexOf('/');
  String id = slash < 0 ? rest : rest.substring(0, slash);
  return isHexId(id) ? id : "";
}

struct Parts { uint32_t count = 0; uint64_t bytes = 0, last = 0; uint64_t mtime = 0; };
static Parts partsOf(const String& dir) {
  Parts p;
  std::vector<DirEnt> ents; listDir(dir, ents);
  for (auto& e : ents) {
    if (e.dir || e.name.length() != 3 || !isdigit((unsigned char)e.name[0]) || !isdigit((unsigned char)e.name[1]) ||
        !isdigit((unsigned char)e.name[2])) continue;
    uint32_t i = e.name.toInt();
    p.bytes += e.size;
    if (i + 1 > p.count) { p.count = i + 1; p.last = e.size; }
    if (e.mtimeMs > p.mtime) p.mtime = e.mtimeMs;
  }
  return p;
}

bool isBigChunk(const String& url) { return url.startsWith("/api/bigfiles/") && url.endsWith("/chunk"); }

static bool openPart(AsyncWebServerRequest* r, BigCtx* c, uint64_t already) {
  snprintf(c->path, sizeof c->path, "%s/%s", c->dir, partName(c->part).c_str());
  if (already) {
    r->_tempFile = store->open(c->path, "r+");
    if (r->_tempFile && !r->_tempFile.seek((uint32_t)already)) r->_tempFile.close();
  } else {
    r->_tempFile = store->open(c->path, FILE_WRITE);
  }
  c->synced = (uint32_t)already;
  return (bool)r->_tempFile;
}
// Writes n bytes (never past the end of the current piece) without flushing; a refused write
// is done again from where the card last had it right (writeBlockRetry reopens and seeks).
static bool writeSlice(AsyncWebServerRequest* r, BigCtx* c, const uint8_t* src, size_t n) {
  if (src != c->buf) memcpy(c->buf, src, n);
  if (r->_tempFile && r->_tempFile.write(c->buf, n) == n) { c->synced += n; return true; }
  r->_tempFile.close();
  return writeBlockRetry(r->_tempFile, c->path, c->buf, n, c->synced);
}
static bool nextPartIfFull(AsyncWebServerRequest* r, BigCtx* c) {
  if ((uint64_t)c->synced < BIG_PART) return true;
  r->_tempFile.flush(); r->_tempFile.close();
  c->part++;
  return openPart(r, c, 0);
}
static bool finishChunk(AsyncWebServerRequest* r, BigCtx* c) {
  if (!r->_tempFile) return false;
  r->_tempFile.flush();
  bool ok = r->_tempFile.position() == c->synced;
  r->_tempFile.close();
  statsDirty = true;
  return ok;
}

// The chunk's bytes, as they arrive.
void bigBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (r->_tempObject) return;
    BigCtx* c = (BigCtx*)heap_caps_malloc(sizeof(BigCtx), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!c) c = (BigCtx*)malloc(sizeof(BigCtx));
    if (!c) return;
    memset(c, 0, offsetof(BigCtx, buf));
    c->magic = BIG_MAGIC;
    r->_tempObject = c;
    if (r->client()) r->client()->setRxTimeout(30);
    if (!canWrite(r)) { c->code = 403; return; }
    String id = idOf(r->url());
    String dir = String(BIG_IN "/") + id;
    if (!id.length() || !store->exists(dir + "/info.json")) { c->code = 404; return; }
    JsonDocument info;
    if (!loadJsonFile(dir + "/info.json", info)) { c->code = 404; return; }
    uint64_t size = info["size"] | (uint64_t)0;
    Parts p = partsOf(dir);
    c->received = p.bytes;
    uint64_t off = r->hasParam("offset") ? strtoull(r->getParam("offset")->value().c_str(), nullptr, 10) : UINT64_MAX;
    if (off != p.bytes) { c->code = 409; return; }
    if (p.bytes + total > size) { c->code = 400; return; }
    if (storeTotal && storeTotal - storeUsed < (uint64_t)total + 4 * 1024 * 1024) { c->code = 507; return; }
    strlcpy(c->dir, dir.c_str(), sizeof c->dir);
    uint64_t inLast = p.count ? p.last : 0;
    c->part = p.count ? p.count - 1 : 0;
    if (inLast >= BIG_PART) { c->part++; inLast = 0; }
    if (!openPart(r, c, inLast)) { c->code = 500; return; }
    // the PSRAM buffer: free, or left behind by a chunk whose connection dropped half a minute ago
    if (total <= BIG_RAM && (!bigRamOwner || millis() - bigRamAt > 30000)) {
      if (!bigRam) bigRam = (uint8_t*)heap_caps_malloc(BIG_RAM, MALLOC_CAP_SPIRAM);
      if (bigRam) { bigRamOwner = c; bigRamAt = millis(); c->inRam = true; }
    }
  }
  BigCtx* c = bigCtx(r);
  if (!c || c->code) return;
  if (c->inRam) {
    if (bigRamOwner != c) { c->code = 500; return; }       // (taken over after a 30 s stall)
    if (index + len <= BIG_RAM) memcpy(bigRam + index, data, len);
    bigRamAt = millis();
    if (index + len < total) return;
    const uint8_t* src = bigRam; size_t left = total;      // all here: onto the card
    bool ok = true;
    while (ok && left) {
      ok = nextPartIfFull(r, c);
      if (!ok) break;
      size_t n = std::min(left, (size_t)UPLOAD_BUF);
      if ((uint64_t)n > BIG_PART - c->synced) n = (size_t)(BIG_PART - c->synced);
      ok = writeSlice(r, c, src, n); src += n; left -= n;
    }
    bigRamOwner = nullptr;
    if (!ok || !finishChunk(r, c)) c->code = 500;
    return;
  }
  size_t n0 = len;                                           // streaming: 16 KB at a time
  while (len) {
    uint64_t partRoom = BIG_PART - c->synced - c->fill;
    size_t n = std::min(len, (size_t)UPLOAD_BUF - c->fill);
    if ((uint64_t)n > partRoom) n = (size_t)partRoom;
    memcpy(c->buf + c->fill, data, n); c->fill += n; data += n; len -= n;
    if (c->fill == UPLOAD_BUF || (uint64_t)c->synced + c->fill >= BIG_PART) {
      bool ok = writeSlice(r, c, c->buf, c->fill);
      c->fill = 0;
      if (!ok || !nextPartIfFull(r, c)) { c->code = 500; r->_tempFile.close(); return; }
    }
  }
  if (index + n0 >= total) {
    bool ok = !c->fill || writeSlice(r, c, c->buf, c->fill);
    c->fill = 0;
    if (!ok || !finishChunk(r, c)) c->code = 500;
  }
}

static bool bigExists(const String& name) {
  return store->exists(String(BIG_ROOT "/") + name) || store->exists(String(BIG_ROOT "/") + name + ".parts");
}
static String bigUnique(const String& name) {
  if (!bigExists(name)) return name;
  int dot = name.lastIndexOf('.');
  String base = dot > 0 ? name.substring(0, dot) : name, ext = dot > 0 ? name.substring(dot) : "";
  for (int k = 1; k < 10000; k++) {
    String c = base + " (" + k + ")" + ext;
    if (!bigExists(c)) return c;
  }
  return base + " (" + String(esp_random() & 0xffff, HEX) + ")" + ext;
}
static String num(uint64_t v) { return String((double)v, 0); }

// /bigfiles-dl/<name>: a whole file, joined from its pieces when it has them
static void sendBig(AsyncWebServerRequest* r, const String& name) {
  String plain = String(BIG_ROOT "/") + name;
  String q = name; q.replace("\"", "'");
  String dispo = "attachment; filename=\"" + q + "\"";
  if (store->exists(plain)) { sendFileRanged(r, plain, true); return; }
  String dir = plain + ".parts";
  Parts p = partsOf(dir);
  if (!p.count) { r->send(404, "text/plain", "Not found"); return; }
  struct Join { String dir; uint32_t part = 0, count = 0; File f; };
  auto j = std::make_shared<Join>();
  j->dir = dir; j->count = p.count;
  auto fill = [j](uint8_t* out, size_t maxLen, size_t) -> size_t {
    size_t n = 0;
    while (n < maxLen && j->part < j->count) {
      if (!j->f) { j->f = store->open(j->dir + "/" + partName(j->part), FILE_READ); if (!j->f) { j->part = j->count; break; } }
      int k = j->f.read(out + n, maxLen - n);
      if (k > 0) { n += k; continue; }
      j->f.close(); j->part++;
    }
    return n;
  };
  // over 4 GB the size doesn't fit the web server's length field: chunked instead
  AsyncWebServerResponse* res = p.bytes < 0xFFFFFFF0ULL ? r->beginResponse("application/octet-stream", (size_t)p.bytes, fill)
                                                        : r->beginChunkedResponse("application/octet-stream", fill);
  res->addHeader("Content-Disposition", dispo);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

bool handleBigfilesApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (url.startsWith("/bigfiles-dl/")) {
    String name = sanitizeFilename(url.substring(strlen("/bigfiles-dl/")));
    if (!onSD || !name.length()) { r->send(404, "text/plain", "Not found"); return true; }
    sendBig(r, name);
    return true;
  }
  if (url != "/api/bigfiles" && !url.startsWith("/api/bigfiles/")) return false;
  if (!onSD) {
    if (url == "/api/bigfiles" && m == HTTP_GET) { sendJson(r, 200, "[]"); return true; }
    sendErr(r, 400, "Big Transfer needs the microSD card, and none was found."); return true;
  }

  if (url == "/api/bigfiles" && m == HTTP_GET) {
    std::vector<DirEnt> ents; listDir(BIG_ROOT, ents);
    std::sort(ents.begin(), ents.end(), [](const DirEnt& a, const DirEnt& b) { return a.mtimeMs > b.mtimeMs; });
    String out = "["; bool first = true;
    for (auto& e : ents) {
      String name = e.name; uint64_t size = e.size, mod = e.mtimeMs;
      if (name.startsWith(".")) continue;
      if (e.dir) {
        if (!name.endsWith(".parts")) continue;
        name = name.substring(0, name.length() - 6);
        Parts p = partsOf(String(BIG_ROOT "/") + e.name);
        size = p.bytes; if (p.mtime) mod = p.mtime;
      }
      String enc = name; enc.replace("%", "%25"); enc.replace(" ", "%20"); enc.replace("#", "%23"); enc.replace("?", "%3F"); enc.replace("&", "%26");
      out += String(first ? "" : ",") + "{\"name\":\"" + jsonEsc(name) + "\",\"size\":" + num(size) + ",\"sizeLabel\":\"" + humanSize(size) +
             "\",\"modified\":" + num(mod) + ",\"url\":\"/bigfiles-dl/" + jsonEsc(enc) + "\"}";
      first = false;
    }
    out += "]";
    sendBody(r, 200, "application/json", std::move(out));
    return true;
  }

  if (url == "/api/bigfiles/start" && m == HTTP_POST) {
    JsonDocument b;
    if (!parseBody(r, b)) { sendErr(r, 400, "Bad request"); return true; }
    String name = sanitizeFilename(String((const char*)(b["name"] | "")));
    double sz = b["size"] | 0.0;
    if (!name.length() || !(sz > 0)) { sendErr(r, 400, "A file name and size are required."); return true; }
    uint64_t size = (uint64_t)sz;
    String key = String((const char*)(b["clientKey"] | ""));
    String id = fnvHex(key.length() ? key : name + "::" + num(size) + "::" + String(esp_random()));
    String dir = String(BIG_IN "/") + id;
    JsonDocument info;
    if (store->exists(dir + "/info.json") && loadJsonFile(dir + "/info.json", info) && (info["size"] | (uint64_t)0) == size) {
      Parts p = partsOf(dir);
      sendJson(r, 200, "{\"uploadId\":\"" + id + "\",\"name\":\"" + jsonEsc(String((const char*)(info["name"] | ""))) + "\",\"receivedBytes\":" + num(p.bytes) + "}");
      return true;
    }
    if (storeTotal && storeTotal - storeUsed < size + 16ULL * 1024 * 1024) {
      sendErr(r, 507, "There isn't room on the card for that: it needs " + humanSize(size) + " and " + humanSize(storeTotal - storeUsed) + " is free.");
      return true;
    }
    removeAll(dir);
    mkdirs(dir);
    info.clear(); info["name"] = name; info["size"] = size; info["createdAt"] = nowMs();
    if (!saveJsonFile(dir + "/info.json", info)) { sendErr(r, 500, "Couldn't start the upload on the card."); return true; }
    sendJson(r, 200, "{\"uploadId\":\"" + id + "\",\"name\":\"" + jsonEsc(name) + "\",\"receivedBytes\":0}");
    return true;
  }

  if (url.startsWith("/api/bigfiles/file/") && m == HTTP_DELETE) {
    String name = sanitizeFilename(url.substring(strlen("/api/bigfiles/file/")));
    String plain = String(BIG_ROOT "/") + name;
    bool ok = name.length() && (store->exists(plain) ? store->remove(plain) : store->exists(plain + ".parts") && removeAll(plain + ".parts"));
    if (!ok) { sendErr(r, 404, "File not found"); return true; }
    statsDirty = true;
    sendJson(r, 200, "{\"deleted\":\"" + jsonEsc(name) + "\"}");
    return true;
  }

  String id = idOf(url);
  String dir = String(BIG_IN "/") + id;
  JsonDocument info;
  if (!id.length() || !store->exists(dir + "/info.json") || !loadJsonFile(dir + "/info.json", info)) {
    sendErr(r, 404, "No upload in progress with that id."); return true;
  }
  uint64_t size = info["size"] | (uint64_t)0;
  String name = String((const char*)(info["name"] | "file"));

  if (url.endsWith("/status") && m == HTTP_GET) {
    sendJson(r, 200, "{\"receivedBytes\":" + num(partsOf(dir).bytes) + ",\"size\":" + num(size) + ",\"name\":\"" + jsonEsc(name) + "\"}");
    return true;
  }

  if (url.endsWith("/chunk") && m == HTTP_POST) {
    BigCtx* c = bigCtx(r);
    uint64_t have = partsOf(dir).bytes;
    if (!c) { sendJson(r, 200, "{\"receivedBytes\":" + num(have) + "}"); return true; }   // an empty chunk
    switch (c->code) {
      case 0:   sendJson(r, 200, "{\"receivedBytes\":" + num(have) + "}"); break;
      case 409: sendJson(r, 409, "{\"error\":\"Offset mismatch.\",\"receivedBytes\":" + num(have) + "}"); break;
      case 400: sendErr(r, 400, "That chunk goes past the end of the file."); break;
      case 403: sendErr(r, 403, "The board is read-only right now."); break;
      case 507: sendErr(r, 507, "The card is full."); break;
      default:  sendJson(r, 500, "{\"error\":\"Could not write to the card.\",\"receivedBytes\":" + num(have) + "}"); break;
    }
    return true;
  }

  if (url.endsWith("/complete") && m == HTTP_POST) {
    Parts p = partsOf(dir);
    if (p.bytes != size) {
      sendErr(r, 400, "Only " + humanSize(p.bytes) + " of " + humanSize(size) + " has arrived so far -- keep uploading before completing.");
      return true;
    }
    mkdirs(BIG_ROOT);
    String fin = bigUnique(name);
    bool ok;
    if (p.count == 1) {                                   // 1 GB or less: an ordinary file
      ok = store->rename(dir + "/000", String(BIG_ROOT "/") + fin);
      if (ok) removeAll(dir);
    } else {                                              // bigger: the folder of pieces moves over whole
      store->remove(dir + "/info.json");
      ok = store->rename(dir, String(BIG_ROOT "/") + fin + ".parts");
    }
    if (!ok) { sendErr(r, 500, "Couldn't move the finished file into Big Files."); return true; }
    statsDirty = true;
    sendJson(r, 200, "{\"name\":\"" + jsonEsc(fin) + "\",\"size\":" + num(size) + "}");
    return true;
  }

  if (m == HTTP_DELETE && url == "/api/bigfiles/" + id) {
    removeAll(dir);
    statsDirty = true;
    sendJson(r, 200, "{\"cancelled\":true}");
    return true;
  }
  sendErr(r, 404, "Not found");
  return true;
}

#else
bool isBigChunk(const String&) { return false; }
void bigBody(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t) {}
bool handleBigfilesApi(AsyncWebServerRequest*, const String&, WebRequestMethod) { return false; }
#endif
