// The Shelf for Heltec V4 - speed test (settings page, "Speed test" under This board).
// Separates the Wi-Fi from the card, to see which one sets the speed of a big upload:
//   POST /api/board/speedtest/up?save=0   bytes in over Wi-Fi, thrown away (Wi-Fi only)
//   POST /api/board/speedtest/up?save=1   bytes in over Wi-Fi, written to the card as they come
//                                         (the same way Big Transfer writes), then deleted
//   GET  /api/board/speedtest/down?bytes=  bytes out over Wi-Fi, made up on the spot
//   GET  /api/board/speedtest/card         the card on its own: write then read a test file
// The page runs the uploads over 1 connection and over 3 at once.
#include "shelf.h"
#include <esp_heap_caps.h>

#define SPEED_MAGIC 0x53485354u
#define SPEED_MAX   (8u * 1024u * 1024u)         // one upload request at most
#define SPEED_DIR   TMP_ROOT "/speed"

struct SpeedCtx {             // the request frees it with free()
  uint32_t magic;
  bool save, err;
  uint32_t t0, t1;            // first and last byte, ms
  size_t got, fill;
  uint32_t synced;
  char path[48];
  alignas(4) uint8_t buf[UPLOAD_BUF];
};
static SpeedCtx* speedCtx(AsyncWebServerRequest* r) {
  SpeedCtx* c = (SpeedCtx*)r->_tempObject;
  return (c && c->magic == SPEED_MAGIC) ? c : nullptr;
}

bool isSpeedUpload(const String& url) { return url == "/api/board/speedtest/up"; }

void speedBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (r->_tempObject) return;
    bool save = r->hasParam("save") && r->getParam("save")->value() == "1" && onSD;
    SpeedCtx* c = (SpeedCtx*)(save ? cardBufAlloc(sizeof(SpeedCtx)) : malloc(offsetof(SpeedCtx, buf)));
    if (!c) return;
    memset(c, 0, offsetof(SpeedCtx, buf));
    c->magic = SPEED_MAGIC; c->save = save; c->t0 = millis();
    r->_tempObject = c;
    if (r->client()) r->client()->setRxTimeout(30);
    if (total > SPEED_MAX) { c->err = true; return; }
    if (save) {
      mkdirs(SPEED_DIR);
      snprintf(c->path, sizeof c->path, SPEED_DIR "/s%08x", (unsigned)esp_random());
      r->_tempFile = store->open(c->path, FILE_WRITE);
      if (!r->_tempFile) c->err = true;
    }
  }
  SpeedCtx* c = speedCtx(r);
  if (!c || c->err) return;
  c->got += len;
  if (c->save) {
    size_t n0 = len;
    while (len) {
      size_t n = std::min(len, (size_t)UPLOAD_BUF - c->fill);
      memcpy(c->buf + c->fill, data, n); c->fill += n; data += n; len -= n;
      if (c->fill == UPLOAD_BUF) {
        if (r->_tempFile.write(c->buf, c->fill) != c->fill) { c->err = true; return; }
        c->fill = 0;
      }
    }
    if (index + n0 >= total) {
      if (c->fill && r->_tempFile.write(c->buf, c->fill) != c->fill) c->err = true;
      r->_tempFile.flush(); r->_tempFile.close();
      store->remove(c->path);
    }
  }
  c->t1 = millis();
}

bool handleSpeedTestApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (!url.startsWith("/api/board/speedtest/")) return false;

  if (url == "/api/board/speedtest/up" && m == HTTP_POST) {
    SpeedCtx* c = speedCtx(r);
    if (!c) { sendJson(r, 200, "{\"bytes\":0,\"ms\":0}"); return true; }
    if (c->err) {
      if (c->path[0]) store->remove(c->path);
      sendErr(r, 500, c->got > SPEED_MAX ? "Too big for one speed test piece." : "Couldn't write the test file to the card.");
      return true;
    }
    sendJson(r, 200, String("{\"bytes\":") + c->got + ",\"ms\":" + (c->t1 - c->t0) + ",\"saved\":" + (c->save ? "true" : "false") + "}");
    return true;
  }

  if (url == "/api/board/speedtest/down" && m == HTTP_GET) {
    size_t bytes = r->hasParam("bytes") ? strtoul(r->getParam("bytes")->value().c_str(), nullptr, 10) : 0;
    if (!bytes || bytes > SPEED_MAX) bytes = 4u * 1024u * 1024u;
    AsyncWebServerResponse* res = r->beginResponse("application/octet-stream", bytes,
      [](uint8_t* out, size_t maxLen, size_t index) -> size_t {
        for (size_t i = 0; i < maxLen; i++) out[i] = (uint8_t)(index + i);   // anything but zeros
        return maxLen;
      });
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
    return true;
  }

  if (url == "/api/board/speedtest/card" && m == HTTP_GET) {
    if (!onSD) { sendErr(r, 400, "No microSD card to test."); return true; }
#if defined(SHELF_MMC)
    const size_t total = 16u * 1024u * 1024u;
#else
    const size_t total = 2u * 1024u * 1024u;    // SPI cards are slow: keep the wait short
#endif
    uint8_t* buf = (uint8_t*)cardBufAlloc(UPLOAD_BUF);
    if (!buf) { sendErr(r, 503, "Not enough memory for the test right now."); return true; }
    for (size_t i = 0; i < UPLOAD_BUF; i++) buf[i] = (uint8_t)(i * 7);
    mkdirs(SPEED_DIR);
    String path = String(SPEED_DIR "/card.bin");
    bool ok = true;
    uint32_t t0 = millis();
    File f = store->open(path, FILE_WRITE);
    if (!f) ok = false;
    for (size_t done = 0; ok && done < total; done += UPLOAD_BUF) ok = f.write(buf, UPLOAD_BUF) == UPLOAD_BUF;
    if (f) { f.flush(); f.close(); }
    uint32_t wms = millis() - t0;
    uint32_t rms = 0;
    if (ok) {
      t0 = millis();
      File g = store->open(path, FILE_READ);
      size_t back = 0;
      while (g) { int k = g.read(buf, UPLOAD_BUF); if (k <= 0) break; back += k; }
      if (g) g.close();
      rms = millis() - t0;
      ok = back == total;
    }
    store->remove(path);
    free(buf);
    if (!ok) { sendErr(r, 500, "The card test couldn't write or read its test file."); return true; }
    sendJson(r, 200, String("{\"bytes\":") + total + ",\"writeMs\":" + wms + ",\"readMs\":" + rms + "}");
    return true;
  }
  sendErr(r, 404, "Not found");
  return true;
}
