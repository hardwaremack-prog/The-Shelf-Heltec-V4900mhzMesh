// The Shelf for Heltec V4 - Big Transfer (the "bigfiles" tab), Heltec V4 MMC build only.
//
// The same API as The Shelf's server.js. A big file comes up in 1 MB chunks, three at a time
// over separate connections (the network stack only lets ~6 KB be in flight per connection,
// so one connection tops out around 6 Mbps); each chunk is written straight into its place in
// the file, whatever order they land in. A dropped connection only costs the chunk in flight,
// and dropping the same file in again picks up where it left off.
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

#define BIG_CHUNK  (1024 * 1024)                    // every chunk but the last is exactly this

struct BigCtx {               // one chunk being written (internal, DMA-capable RAM; the request frees it with free())
  uint32_t magic;
  int code;                   // 0 fine so far; else the HTTP error to answer with
  uint64_t off;               // where the chunk goes in the whole file
  size_t got;                 // bytes of it written so far
  size_t fill;                // bytes waiting in buf
  char id[17];
  int8_t slot;                // card writer slot (cardpipe.cpp), or -1: written right here
  alignas(4) uint8_t buf[UPLOAD_BUF];
};

// The upload being written. FAT here has no file locking: two handles writing one file would
// each keep their own idea of its size and chain of clusters and wreck it, so every chunk
// goes through these single handles. (All web callbacks run on the one network task, so they
// never write at the same moment.)
static String upId;
static String upDir;
static uint64_t upSize = 0;
static File upF;                       // the piece being written
static int32_t upPart = -1;
static File upDoneF;                   // one byte per chunk: 1 = on the card
static std::vector<uint8_t> upDone;
static uint32_t upFirstGap = 0;        // chunks before this are all done
// The card writer task (cardpipe.cpp) writes through these handles too: everything that
// touches them holds this lock.
static SemaphoreHandle_t upMux = nullptr;
struct UpLock {
  UpLock() { if (!upMux) upMux = xSemaphoreCreateRecursiveMutex(); xSemaphoreTakeRecursive(upMux, portMAX_DELAY); }
  ~UpLock() { xSemaphoreGiveRecursive(upMux); }
};

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

static uint32_t chunkCount(uint64_t size) { return (uint32_t)((size + BIG_CHUNK - 1) / BIG_CHUNK); }
static uint64_t prefixBytes() {
  while (upFirstGap < upDone.size() && upDone[upFirstGap]) upFirstGap++;
  return std::min(upSize, (uint64_t)upFirstGap * BIG_CHUNK);
}
static void upClose() {
  if (upF) { upF.flush(); upF.close(); }
  if (upDoneF) upDoneF.close();
  upPart = -1; upId = ""; upDir = ""; upDone.clear(); upDone.shrink_to_fit(); upFirstGap = 0; upSize = 0;
}
// Makes `id` the upload being written: loads which chunks are already on the card.
static bool upOpen(const String& id) {
  if (upId == id && upDoneF) return true;
  upClose();
  String dir = String(BIG_IN "/") + id;
  JsonDocument info;
  if (!loadJsonFile(dir + "/info.json", info)) return false;
  uint64_t size = info["size"] | (uint64_t)0;
  if (!size) return false;
  uint32_t n = chunkCount(size);
  upDone.assign(n, 0);
  String donePath = dir + "/done";
  if (store->exists(donePath)) {
    File f = store->open(donePath, FILE_READ);
    if (f) { f.read(upDone.data(), n); f.close(); }
  } else {
    // started by 1.20.3 or earlier: one piece after another, so the whole chunks at the front are done
    Parts p = partsOf(dir);
    for (uint32_t k = 0; k < n && (uint64_t)(k + 1) * BIG_CHUNK <= p.bytes; k++) upDone[k] = 1;
    if (p.bytes >= size) for (auto& d : upDone) d = 1;
    File f = store->open(donePath, FILE_WRITE);
    if (f) { f.write(upDone.data(), n); f.close(); }
  }
  upDoneF = store->open(donePath, "r+");
  if (!upDoneF) { upDone.clear(); return false; }
  upId = id; upDir = dir; upSize = size; upFirstGap = 0;
  return true;
}
// Writes n bytes at `off` in the whole file (never across a 1 GB boundary: chunks divide it).
static bool upWrite(uint64_t off, const uint8_t* buf, size_t n) {
  int32_t part = (int32_t)(off / BIG_PART);
  uint32_t at = (uint32_t)(off % BIG_PART);
  for (int t = 0; t < 3; t++) {
    if (!upF || upPart != part) {
      if (upF) { upF.flush(); upF.close(); }
      String path = upDir + "/" + partName(part);
      upF = store->open(path, store->exists(path) ? "r+" : "w+");
      upPart = upF ? part : -1;
    }
    // Only seek when not already there: in order (one chunk at a time) every block follows the
    // last, and a backwards seek makes FAT walk the file's cluster chain from its start.
    // Seeking past the end makes the file longer; the gap is filled by the chunks still coming.
    if (upF && (upF.position() == at || upF.seek(at)) && upF.write(buf, n) == n) return true;
    if (upF) upF.close();
    upPart = -1;
    sdRetries++;
    delay(100 * (t + 1));
  }
  sdFailures++;
  return false;
}
static bool upMarkDone(uint64_t off) {
  if (upF) upF.flush();
  uint32_t k = (uint32_t)(off / BIG_CHUNK);
  if (k >= upDone.size()) return false;
  upDone[k] = 1;
  uint8_t one = 1;
  bool ok = upDoneF.seek(k) && upDoneF.write(&one, 1) == 1;
  upDoneF.flush();
  return ok;
}

// For the card writer task
bool bigPipeWrite(const char* id, uint64_t off, const uint8_t* buf, size_t n) {
  UpLock l;
  return (upId == id || upOpen(id)) && upWrite(off, buf, n);
}
bool bigPipeMark(const char* id, uint64_t off) {
  UpLock l;
  bool ok = (upId == id || upOpen(id)) && upMarkDone(off);
  statsDirty = true;
  return ok;
}

// A chunk's bytes, as they arrive: handed to the card writer (or, without one, 16 KB at a
// time straight into its place in the file).
void bigBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (r->_tempObject) return;
    BigCtx* c = (BigCtx*)heap_caps_malloc(sizeof(BigCtx), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!c) c = (BigCtx*)malloc(sizeof(BigCtx));
    if (!c) return;
    memset(c, 0, offsetof(BigCtx, buf));
    c->magic = BIG_MAGIC; c->slot = -1;
    r->_tempObject = c;
    if (r->client()) r->client()->setRxTimeout(30);
    if (!canWrite(r)) { c->code = 403; return; }
    String id = idOf(r->url());
    UpLock l;
    if (!id.length() || !upOpen(id)) { c->code = 404; return; }
    strlcpy(c->id, id.c_str(), sizeof c->id);
    uint64_t off = r->hasParam("offset") ? strtoull(r->getParam("offset")->value().c_str(), nullptr, 10) : UINT64_MAX;
    // a whole chunk, in its own slot: 1 MB at a 1 MB boundary (the last one may be shorter)
    if (off >= upSize || off % BIG_CHUNK || total != (size_t)std::min((uint64_t)BIG_CHUNK, upSize - off)) { c->code = 400; return; }
    if (storeTotal && storeTotal - storeUsed < (uint64_t)total + 4 * 1024 * 1024) { c->code = 507; return; }
    c->off = off;
    c->slot = (int8_t)pipeOpen(PIPE_BIG, c->id, off);
  }
  BigCtx* c = bigCtx(r);
  if (!c || c->code) return;
  if (c->slot >= 0) {                                        // the card writer saves it
    if (!pipeFeed(c->slot, data, len)) { c->code = 500; return; }
    if (index + len >= total && !pipeEnd(c->slot, true, c->off)) c->code = 500;
    return;
  }
  UpLock l;
  if (upId != c->id && !upOpen(c->id)) { c->code = 404; return; }   // (another upload took the handles meanwhile)
  size_t n0 = len;
  while (len) {
    size_t n = std::min(len, (size_t)UPLOAD_BUF - c->fill);
    memcpy(c->buf + c->fill, data, n); c->fill += n; data += n; len -= n;
    if (c->fill == UPLOAD_BUF) {
      if (!upWrite(c->off + c->got, c->buf, c->fill)) { c->code = 500; return; }
      c->got += c->fill; c->fill = 0;
    }
  }
  if (index + n0 >= total) {
    if (c->fill && !upWrite(c->off + c->got, c->buf, c->fill)) { c->code = 500; return; }
    c->got += c->fill; c->fill = 0;
    if (!upMarkDone(c->off)) c->code = 500;
    statsDirty = true;
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
  if (url.endsWith("/chunk") && m == HTTP_POST) {            // wait for the chunk to be on the card (no lock: the writer needs it)
    BigCtx* c = bigCtx(r);
    if (c && c->slot >= 0) {
      bool ok = pipeFinish(c->slot, 20000);
      c->slot = -1;
      if (!ok && !c->code) c->code = 500;
    }
  }
  UpLock lock;
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
      uint64_t have = upOpen(id) ? prefixBytes() : 0;
      sendJson(r, 200, "{\"uploadId\":\"" + id + "\",\"name\":\"" + jsonEsc(String((const char*)(info["name"] | ""))) + "\",\"receivedBytes\":" + num(have) + ",\"chunkSize\":" + BIG_CHUNK + "}");
      return true;
    }
    if (storeTotal && storeTotal - storeUsed < size + 16ULL * 1024 * 1024) {
      sendErr(r, 507, "There isn't room on the card for that: it needs " + humanSize(size) + " and " + humanSize(storeTotal - storeUsed) + " is free.");
      return true;
    }
    if (upId == id) upClose();
    removeAll(dir);
    mkdirs(dir);
    info.clear(); info["name"] = name; info["size"] = size; info["createdAt"] = nowMs();
    if (!saveJsonFile(dir + "/info.json", info)) { sendErr(r, 500, "Couldn't start the upload on the card."); return true; }
    sendJson(r, 200, "{\"uploadId\":\"" + id + "\",\"name\":\"" + jsonEsc(name) + "\",\"receivedBytes\":0,\"chunkSize\":" + BIG_CHUNK + "}");
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
  bool cancel = m == HTTP_DELETE && url == "/api/bigfiles/" + id;
  if (!cancel && !upOpen(id)) { sendErr(r, 500, "Couldn't read the upload's progress on the card."); return true; }

  if (url.endsWith("/status") && m == HTTP_GET) {
    sendJson(r, 200, "{\"receivedBytes\":" + num(prefixBytes()) + ",\"size\":" + num(size) + ",\"name\":\"" + jsonEsc(name) + "\"}");
    return true;
  }

  if (url.endsWith("/chunk") && m == HTTP_POST) {
    BigCtx* c = bigCtx(r);
    uint64_t have = prefixBytes();
    if (!c) { sendJson(r, 200, "{\"receivedBytes\":" + num(have) + "}"); return true; }   // an empty chunk
    switch (c->code) {
      case 0:   sendJson(r, 200, "{\"receivedBytes\":" + num(have) + "}"); break;
      case 409: sendJson(r, 409, "{\"error\":\"Offset mismatch.\",\"receivedBytes\":" + num(have) + "}"); break;
      case 400: sendErr(r, 400, "That chunk isn't a whole 1 MB piece of the file. Reload the page and drop the file in again."); break;
      case 403: sendErr(r, 403, "The board is read-only right now."); break;
      case 507: sendErr(r, 507, "The card is full."); break;
      default:  sendJson(r, 500, "{\"error\":\"Could not write to the card.\",\"receivedBytes\":" + num(have) + "}"); break;
    }
    return true;
  }

  if (url.endsWith("/complete") && m == HTTP_POST) {
    uint64_t have = prefixBytes();
    if (have != size) {
      sendErr(r, 400, "Only " + humanSize(have) + " of " + humanSize(size) + " has arrived so far -- keep uploading before completing.");
      return true;
    }
    upClose();                                            // let go of the pieces before moving them
    store->remove(dir + "/done");
    Parts p = partsOf(dir);
    if (p.bytes != size) { sendErr(r, 500, "The pieces on the card don't add up to the file's size. Cancel and send it again."); return true; }
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

  if (cancel) {
    if (upId == id) upClose();
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
