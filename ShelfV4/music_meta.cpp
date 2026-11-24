// The Shelf for Heltec V4 - artist, title and cover art straight from MP3 (ID3v2) tags.
// The full Shelf reads tags with TagLib and fetches art online; the board reads what's
// already inside each song, in the background, and caches it on the card.
#include "shelf.h"

static volatile bool dirty = true;
volatile int musicReaders = 0;      // Music lists being sent right now: the scan waits for them
static volatile uint32_t dirtyAt = 0;
void markMusicDirty() { dirty = true; dirtyAt = millis(); }

static uint32_t syncsafe(const uint8_t* b) { return ((b[0] & 0x7f) << 21) | ((b[1] & 0x7f) << 14) | ((b[2] & 0x7f) << 7) | (b[3] & 0x7f); }
static uint32_t be32(const uint8_t* b) { return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }

static void putUtf8(String& o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// Decodes an ID3 text frame body (encoding byte + text) to UTF-8.
static String id3Text(const uint8_t* p, size_t n) {
  if (!n) return "";
  uint8_t enc = p[0]; p++; n--;
  String o;
  if (enc == 0 || enc == 3) {                       // Latin-1 / UTF-8
    for (size_t i = 0; i < n && p[i]; i++) { if (enc == 0 && p[i] >= 0x80) putUtf8(o, p[i]); else o += (char)p[i]; }
  } else {                                          // UTF-16 with BOM, or UTF-16BE
    bool le = false; size_t i = 0;
    if (enc == 1 && n >= 2) { if (p[0] == 0xFF && p[1] == 0xFE) { le = true; i = 2; } else if (p[0] == 0xFE && p[1] == 0xFF) i = 2; }
    for (; i + 1 < n; i += 2) {
      uint16_t u = le ? (p[i] | (p[i + 1] << 8)) : ((p[i] << 8) | p[i + 1]);
      if (!u) break;
      if (u >= 0xD800 && u < 0xE000) continue;      // skip surrogates (emoji in tags are rare)
      putUtf8(o, u);
    }
  }
  o.trim();
  return o;
}

// Reads artist/title and saves embedded cover art. Returns false if there's no ID3v2 tag.
static bool readId3(const String& path, String& artist, String& title, const String& artOut) {
  File f = store->open(path, FILE_READ);
  if (!f) return false;
  uint8_t h[10];
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4) { f.close(); return false; }
  uint8_t ver = h[3];
  uint32_t tagSize = syncsafe(h + 6);
  if (h[5] & 0x40 && ver >= 3) {                     // skip extended header
    uint8_t eh[4]; f.read(eh, 4);
    uint32_t es = ver == 4 ? syncsafe(eh) : be32(eh) ;
    f.seek(f.position() + (ver == 4 ? es - 4 : es));
  }
  uint32_t end = 10 + std::min<uint32_t>(tagSize, f.size());
  bool gotArt = false;
  while (f.position() + (ver == 2 ? 6 : 10) < end) {
    uint8_t fh[10];
    char id[5] = {0};
    uint32_t sz;
    if (ver == 2) {
      if (f.read(fh, 6) != 6) break;
      memcpy(id, fh, 3); sz = (fh[3] << 16) | (fh[4] << 8) | fh[5];
    } else {
      if (f.read(fh, 10) != 10) break;
      memcpy(id, fh, 4); sz = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
    }
    if (!id[0] || sz == 0 || f.position() + sz > end) break;
    bool isArtist = !strcmp(id, "TPE1") || !strcmp(id, "TP1");
    bool isTitle = !strcmp(id, "TIT2") || !strcmp(id, "TT2");
    bool isPic = !strcmp(id, "APIC") || !strcmp(id, "PIC");
    if ((isArtist || isTitle) && sz < 1024) {
      uint8_t buf[1024]; f.read(buf, sz);
      String t = id3Text(buf, sz);
      if (isArtist && !artist.length()) artist = t;
      if (isTitle && !title.length()) title = t;
    } else if (isPic && !gotArt && artOut.length() && sz < 3u * 1024 * 1024) {
      uint32_t start = f.position();
      uint8_t hb[256]; size_t hn = f.read(hb, std::min<uint32_t>(sz, sizeof hb));
      size_t i = 1;                                  // after encoding byte
      uint8_t enc = hb[0];
      if (ver == 2) i += 3;                          // 3-char image format
      else { while (i < hn && hb[i]) i++; i++; }     // mime string
      i++;                                           // picture type
      if (enc == 1 || enc == 2) { while (i + 1 < hn && (hb[i] || hb[i + 1])) i += 2; i += 2; }
      else { while (i < hn && hb[i]) i++; i++; }     // description
      if (i < hn) {
        File out = store->open(artOut, FILE_WRITE);
        if (out) {
          f.seek(start + i);
          uint32_t left = sz - i; uint8_t cb[2048]; bool ok = true;
          while (left) { int k = f.read(cb, std::min<uint32_t>(left, sizeof cb)); if (k <= 0 || out.write(cb, k) != (size_t)k) { ok = false; break; } left -= k; }
          out.close();
          if (ok) gotArt = true; else store->remove(artOut);   // (only ever a song with no art yet: no link changes)
        }
      }
      f.seek(start + sz);
    } else {
      f.seek(f.position() + sz);
    }
  }
  f.close();
  return true;
}

// ---- Opus / Ogg Vorbis: the comment packet (TITLE=, ARTIST=, and the cover as METADATA_BLOCK_PICTURE, base64).
// Read straight from the Ogg pages a byte at a time, so even a big cover needs no more RAM than a small buffer.
// (Songs shrunk by The Shelf's upload page are .opus files made this way.)
namespace {
struct OggIn {
  File& f; uint8_t lace[255]; int nseg = 0, si = 0, left = 0; bool endAfter = false;
  uint8_t buf[512]; int bp = 0, bn = 0;          // read in 512-byte pieces (a byte at a time took seconds per cover)
  explicit OggIn(File& file) : f(file) {}
  int rd() {
    if (bp >= bn) { int n = f.read(buf, sizeof buf); bp = 0; bn = n > 0 ? n : 0; if (!bn) return -1; }
    return buf[bp++];
  }
  bool rdn(uint8_t* d, int n) { for (int i = 0; i < n; i++) { int c = rd(); if (c < 0) return false; d[i] = (uint8_t)c; } return true; }
  bool page() {
    uint8_t h[27];
    if (!rdn(h, 27) || memcmp(h, "OggS", 4) != 0) return false;
    nseg = h[26]; si = 0;
    return rdn(lace, nseg);
  }
  int get() {                      // next byte of the current packet; -1 at its end, -2 at the end of the file
    while (left == 0) {
      if (endAfter) { endAfter = false; return -1; }
      if (si >= nseg) { if (!page()) return -2; continue; }
      left = lace[si++]; endAfter = left < 255;
    }
    left--; return rd();
  }
  bool skipPacket() { int c; while ((c = get()) >= 0) {} return c == -1; }
  bool u32le(uint32_t& v) { v = 0; for (int i = 0; i < 4; i++) { int c = get(); if (c < 0) return false; v |= (uint32_t)c << (8 * i); } return true; }
  bool skip(uint32_t n) { while (n--) if (get() < 0) return false; return true; }
};
int b64val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1;
}
}

static bool readOggTags(const String& path, String& artist, String& title, const String& artOut) {
  File f = store->open(path, FILE_READ);
  if (!f) return false;
  OggIn o(f);
  bool ok = false;
  do {
    if (!o.skipPacket()) break;                                  // OpusHead / Vorbis identification header
    int c = o.get();
    if (c == 'O') { char m[7]; for (auto& x : m) x = o.get(); if (memcmp(m, "pusTags", 7)) break; }
    else if (c == 3) { char m[6]; for (auto& x : m) x = o.get(); if (memcmp(m, "vorbis", 6)) break; }
    else break;
    uint32_t vl, n;
    if (!o.u32le(vl) || vl > 4096 || !o.skip(vl) || !o.u32le(n)) break;
    ok = true;
    bool gotArt = false;
    for (uint32_t i = 0; i < n && i < 200; i++) {
      uint32_t len;
      if (!o.u32le(len)) break;
      char key[24]; uint32_t k = 0; int ch = 0;
      while (k < len && k < sizeof key - 1) { ch = o.get(); if (ch < 0) break; k++; if (ch == '=') break; key[k - 1] = toupper(ch); }
      if (ch < 0) break;
      key[ch == '=' ? k - 1 : k] = 0;
      uint32_t rest = len - k;
      if (ch == '=' && (!strcmp(key, "ARTIST") || !strcmp(key, "TITLE")) && rest < 512) {
        String v; for (uint32_t j = 0; j < rest; j++) { int b = o.get(); if (b < 0) break; v += (char)b; }
        v.trim();
        if (key[0] == 'A' && !artist.length()) artist = v;
        if (key[0] == 'T' && !title.length()) title = v;
      } else if (ch == '=' && !strcmp(key, "METADATA_BLOCK_PICTURE") && !gotArt && artOut.length()) {
        // base64 of a FLAC picture block: type, mime, description, w/h/depth/colours, then the picture itself
        File out; uint32_t pos = 0, need = 32, mimeL = 0, descL = 0, dataL = 0, wrote = 0; uint8_t hdr[8]; int stage = 0;
        uint32_t acc = 0; int bits = 0; uint8_t wb[512]; size_t wn = 0; bool bad = false;
        for (uint32_t j = 0; j < rest; j++) {
          int b = o.get(); if (b < 0) { bad = true; break; }
          int v = b64val(b); if (v < 0) continue;
          acc = (acc << 6) | v; bits += 6;
          if (bits < 8) continue;
          bits -= 8; uint8_t byte = (acc >> bits) & 0xFF;
          // stage 0: type(4) mimeLen(4); 1: mime; 2: descLen(4); 3: desc; 4: 16 bytes + dataLen(4); 5: data
          if (stage == 5) {
            if (wrote < dataL) { wb[wn++] = byte; wrote++; if (wn == sizeof wb) { if (out.write(wb, wn) != wn) { bad = true; break; } wn = 0; } }
            continue;
          }
          if (stage == 0) { hdr[pos++] = byte; if (pos == 8) { mimeL = (hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7]; pos = 0; stage = mimeL ? 1 : 2; if (mimeL > 256) { bad = true; break; } } }
          else if (stage == 1) { if (++pos == mimeL) { pos = 0; stage = 2; } }
          else if (stage == 2) { hdr[pos++] = byte; if (pos == 4) { descL = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3]; pos = 0; stage = descL ? 3 : 4; if (descL > 4096) { bad = true; break; } } }
          else if (stage == 3) { if (++pos == descL) { pos = 0; stage = 4; } }
          else if (stage == 4) {
            if (pos >= 16) hdr[pos - 16] = byte;
            if (++pos == 20) {
              dataL = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
              if (!dataL || dataL > 3u * 1024 * 1024) { bad = true; break; }
              out = store->open(artOut, FILE_WRITE); if (!out) { bad = true; break; }
              stage = 5;
            }
          }
        }
        (void)need;
        if (out) {
          if (!bad && wn && out.write(wb, wn) != wn) bad = true;
          out.close();
          if (!bad && wrote == dataL) gotArt = true; else store->remove(artOut);
        }
        if (bad) break;
      } else if (!o.skip(rest)) break;
    }
  } while (false);
  f.close();
  return ok;
}

// MP3: ID3v2; Opus / Ogg: the comment packet
static bool hasTagReader(const String& name) { String e = extOf(name); return e == "mp3" || e == "opus" || e == "ogg"; }
static bool readTags(const String& path, String& artist, String& title, const String& artOut) {
  String e = extOf(path);
  if (e == "opus" || e == "ogg") return readOggTags(path, artist, title, artOut);
  return readId3(path, artist, title, artOut);
}

static void musicMetaRun();
void musicMetaLoop() {
  if (!dirty || millis() - dirtyAt < 4000) return;
  // Short of memory (a plain ESP32 with Bluetooth on, say): try again later rather than crash.
  if (ESP.getMaxAllocHeap() < 24 * 1024 || musicReaders > 0 || (streamsActive > 0 && ESP.getPsramSize() == 0)) { dirtyAt = millis() + 26000; return; }
  dirty = false;
  try { musicMetaRun(); }
  catch (...) { dirty = true; dirtyAt = millis() + 56000; }   // ran out part-way: back off a minute
}

// Without PSRAM (a plain ESP32) there's no room for the whole song list and tag file at once, so
// the scan only saves cover art, walking the folder a song at a time. (Artists aren't shown in the
// list on those boards anyway; the song names carry them.)
static void artOnlyScan() {
  mkdirs(DATA_ROOT "/album-art");
  std::vector<uint64_t> have = dirHashes(DATA_ROOT "/album-art");
  forEachEntry("/Music", [&](const char* name, uint64_t, bool dir) {
    if (dirty) return false;                                     // new uploads: start over later
    if (musicReaders > 0 || streamsActive > 0 || ESP.getMaxAllocHeap() < 16 * 1024) { dirty = true; dirtyAt = millis() + 26000; return false; }
    String n = name;
    if (dir || !hasTagReader(n)) return true;
    String hex = fnvHex(String("Music/") + n);
    if (hasHash(have, hex)) return true;
    String artist, title;
    readTags("/Music/" + n, artist, title, String(DATA_ROOT "/album-art/") + hex + ".jpg");
    delay(2);
    return true;
  });
}

static void musicMetaRun() {
  if (ESP.getPsramSize() == 0) { artOnlyScan(); return; }
  const char* META = DATA_ROOT "/music-meta.json";
  JsonDocument meta(&psAlloc);
  if (!loadJsonFile(META, meta) || !meta.is<JsonObject>()) meta.to<JsonObject>();
  mkdirs(DATA_ROOT "/album-art");

  std::vector<std::pair<String, uint32_t>> files;
  {
    std::vector<DirEnt> all; listDir("/Music", all);
    for (auto& e : all) if (!e.dir && !e.name.startsWith(".")) files.push_back({e.name, (uint32_t)e.size});
  }
  std::vector<String> haveArt = dirNames(DATA_ROOT "/album-art");
  bool changed = false;
  // forget songs that are gone
  std::vector<String> gone;
  for (JsonPair kv : meta.as<JsonObject>()) {
    bool found = false; for (auto& f : files) if (f.first == kv.key().c_str()) { found = true; break; }
    if (!found) gone.push_back(kv.key().c_str());
  }
  for (auto& g : gone) { meta.remove(g); changed = true; }

  int done = 0;
  for (auto& f : files) {
    if (dirty) break;                               // new uploads arrived; start over shortly
    // someone opened Music, or memory is getting short (a board without PSRAM): save what's
    // done, let go of everything, and carry on later
    if (musicReaders > 0 || ESP.getMaxAllocHeap() < 16 * 1024) { dirty = true; dirtyAt = millis() + 26000; break; }
    JsonVariant e = meta[f.first];
    if (!e.isNull() && (e["s"] | 0u) == f.second) continue;
    String artist, title;
    String art = String(DATA_ROOT "/album-art/") + fnvHex(String("Music/") + f.first) + ".jpg";
    if (hasTagReader(f.first)) readTags("/Music/" + f.first, artist, title, hasName(haveArt, art.substring(art.lastIndexOf('/') + 1)) ? String("") : art);
    JsonObject o = meta[f.first].to<JsonObject>();
    o["s"] = f.second;
    if (artist.length()) o["a"] = artist;
    if (title.length()) o["t"] = title;
    changed = true;
    if (++done % 10 == 0) { saveJsonFile(META, meta); changed = false; }
    delay(2);
  }
  if (changed) saveJsonFile(META, meta);
}
