// The Shelf for Heltec V4 - 900MHz Coms: a simple LoRa text mesh between Shelf boards.
//
// 2.4 GHz too: every board (Heltec V3/V4 and plain ESP32) also sends and hears the same
// packets over ESP-NOW, the ESP32's own board-to-board Wi-Fi frames. No router or Wi-Fi
// network is needed between boards, and phones can stay joined to a board's own Wi-Fi while
// it talks. A plain ESP32 (no LoRa) meshes over 2.4 GHz only; a Heltec carries messages
// between the two, so 2.4 GHz and 900 MHz boards end up on one mesh. ESP-NOW only hears
// boards on the same Wi-Fi channel: boards on their own Wi-Fi all use channel 1, and a board
// joined to a home Wi-Fi uses its router's channel.
//
// Every board with Coms switched on is also a relay: anything new it hears is
// re-sent once (after a short random wait) until the message's hop limit runs out.
// Message ids are remembered so nothing loops. Not Meshtastic - its own channel.
//
// Air settings: 915.0 MHz, 125 kHz, SF9, CR 4/5, sync word 0x12, 16-symbol preamble.
#include "shelf.h"
#include <SPI.h>
#ifndef SHELF_NO_RADIO
#include <RadioLib.h>
#else
// No LoRa radio on this board (a plain ESP32): a do-nothing stand-in keeps the rest of Coms
// compiling (the log, the HTTP API) without linking RadioLib. radioStart() never gets this far.
#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_LORA_DETECTED 1
#define RADIOLIB_SX126X_SYNC_WORD_PRIVATE 0x12
struct Module { Module(int, int, int, int, SPIClass&) {} };
struct SX1262 {
  explicit SX1262(Module*) {}
  int begin(float, float, int, int, int, int, int, float) { return -1; }
  void setDio2AsRfSwitch(bool) {} void setCRC(int) {} void setCurrentLimit(float) {}
  void setPacketReceivedAction(void (*)(void)) {} void clearPacketReceivedAction() {}
  int startReceive() { return -1; } int sleep() { return 0; }
  size_t getPacketLength() { return 0; } int readData(uint8_t*, size_t) { return -1; }
  float getRSSI() { return 0; } float getSNR() { return 0; }
  int scanChannel() { return 0; } int setOutputPower(int) { return 0; } int transmit(const uint8_t*, size_t) { return -1; }
};
#endif
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "esp_rom_crc.h"

// Heltec V4 radio wiring (from Heltec's datasheet / Meshtastic's heltec_v4 variant)
#define LORA_NSS   8
#define LORA_SCK   9
#define LORA_MOSI  10
#define LORA_MISO  11
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_DIO1  14
// Front-end module (GC1109 on V4.2, KCT8103L on V4.3): its LDO, enable, and mode pins.
#define FEM_POWER  7    // HIGH = power the FEM
#define FEM_EN     2    // HIGH = chip enabled
#define FEM_CPS    46   // V4.2 (GC1109): HIGH = full PA while sending (free pin on V4.3)
#define FEM_CTX    5    // V4.3 (KCT8103L): HIGH while sending, LOW = receive through the LNA (free on V4.2)
// Both are switched around every send, as Meshtastic does, so either board revision works.

static const float FREQ = 915.0;
static const uint8_t MAGIC0 = 'S', MAGIC1 = '9';
static const uint8_t T_TEXT = 1, T_SYNC_REQ = 2, T_REPLAY = 3, T_RECEIPT = 4;   // packet types (byte 2)
static const uint8_t T_HELLO = 5, T_ADMIN_REQ = 6, T_ADMIN_RESP = 7, T_WHO = 8;
// file transfer: offer / accept / reject go over the mesh as usual; the rest over the transfer link
static const uint8_t T_FOFFER = 20, T_FACCEPT = 21, T_FREJECT = 22, T_FDATA = 23, T_FWEND = 24, T_FACK = 25, T_FDONE = 26, T_FDONEACK = 27, T_FCANCEL = 28;
// Remote admin: commands are sent in the clear (amateur rules don't allow hiding a
// message's meaning) and signed with an 8-byte HMAC-SHA256 made from the target board's
// admin password, with a rising counter so a recorded command can't be played back.
static const uint8_t C_STATUS = 1, C_READONLY = 2, C_REBOOT = 3, C_SCREEN = 4, C_POWER = 5, C_RECEIPTS = 6,
                     C_WIFI_OWN = 7, C_WIFI_HOME = 8, C_NAME = 9, C_PING = 10, C_LIST = 11;
static const uint8_t RS_OK = 0, RS_BADPASS = 1, RS_STALE = 2, RS_UNKNOWN = 3, RS_NOADMIN = 4, RS_LOCKED = 5;
static const uint32_t HELLO_EVERY_MS = 15UL * 60 * 1000;
static const size_t REPLY_MAX = 250 - 27 - 8;         // (250: the most one ESP-NOW frame carries)          // remote-admin reply data, one packet
static const uint32_t SYNC_WINDOW_S = 12 * 3600;                  // catch-up covers the last 12 hours
static const int SYNC_MAX = 60;
// No practical hop limit: every board relays a message once and remembers its id, so a
// message travels as far as there are boards to carry it and can't loop.
static const uint8_t HOPS = 255;
static const size_t MAX_TEXT = 180, MAX_NAME = 16;
static const char* LOG_FILE = DATA_ROOT "/coms-log.jsonl";
static const char* RECEIPT_FILE = DATA_ROOT "/coms-receipts.jsonl";

static SPIClass loraSPI(HSPI);              // the SD card has FSPI
static SX1262* radio = nullptr;
static Preferences cprefs;
static SemaphoreHandle_t comsLock;

static bool enabled = false, radioUp = false;
static volatile bool bandLora = true, bandNow = true;   // each band can be switched off on its own (Heltec)
// "Received" receipts: when on, this board answers each message it hears with a short
// receipt that floods back to the sender, so the sender sees who got it.
static bool receiptsOn = true;
static std::vector<uint32_t> myIds;                       // my recent message ids
struct Rcpt { uint32_t msg; uint32_t node; String name; };
static std::vector<Rcpt> receipts;
static String myName;
static uint32_t myNode = 0;
static int level = 1;                       // 0 low, 1 normal, 2 high
#ifdef SHELF_V3
static const int8_t CHIP_DBM[3] = {11, 17, 22}; // V3: no amplifier, the SX1262 alone tops out at 22 dBm
static const char* LEVELS_DBM = "[11,17,22]";
#else
static const int8_t CHIP_DBM[3] = {0, 9, 16}; // + ~11 dB from the FEM: about 11 / 20 / 27 dBm out
static const char* LEVELS_DBM = "[11,20,27]";
#endif
static uint32_t heardCount = 0, relayedCount = 0, sentCount = 0;
static String lastError;

struct Entry { String json; };
static std::vector<String> logRam;          // newest last, capped
static const size_t LOG_KEEP = COMS_LOG_KEEP;

struct Neighbor { uint32_t node; String name; float rssi, snr; uint64_t at; };
static std::vector<Neighbor> neighbors;

// Every board on the mesh (heard directly or through relays), from their hellos and messages.
struct Member { uint32_t node; String name; uint8_t board, batt, flags, hops; float rssi; uint64_t at; };
static std::vector<Member> members;
static uint32_t helloAt = 0, lastHelloSent = 0, whoAnswerAt = 0;

// Remote admin, this board asking: sessions hold the key made from the password you typed.
struct Session { String id; uint32_t node; uint8_t key[32]; uint32_t at; };
static std::vector<Session> sessions;
struct Pending { uint32_t handle, req, node, counter; uint8_t cmd; String arg; uint8_t key[32]; uint32_t sentAt; int state; uint8_t status; String data; bool retried; };
static std::vector<Pending> pendings;

static uint32_t seen[1024]; static int seenPos = 0;
static bool wasSeen(uint32_t id) { for (uint32_t s : seen) if (s == id) return true; return false; }
static void markSeen(uint32_t id) { seen[seenPos] = id; seenPos = (seenPos + 1) % 1024; }

struct Tx { uint8_t buf[256]; size_t len; uint32_t due; bool nowSent; };
static std::vector<Tx> txq;

// Catch-up: a board that just came on asks its neighbors for the last 12 hours.
struct SyncJob { uint32_t requester = 0; uint32_t due = 0; uint32_t created = 0; std::vector<uint32_t> have; bool active = false; };
static SyncJob syncJob;
static uint32_t lastReplayHeardAt = 0;       // someone else already answering a request
static uint32_t lastServed[8] = {0}, lastServedAt[8] = {0};
static uint32_t syncAskAt = 0, syncAsks = 0, syncedCount = 0;

static volatile bool rxFlag = false;
static void IRAM_ATTR onRx() { rxFlag = true; }

// ---- 2.4 GHz: ESP-NOW broadcast frames, the same packets as on 900 MHz
static bool nowUp = false;
static uint32_t nowSentCount = 0, nowHeardCount = 0;
struct NowPkt { uint8_t len; uint8_t buf[250]; };
static const uint8_t NOW_RING = 24;               // (room for a file transfer's bursts)
static NowPkt nowRing[NOW_RING];
static volatile uint8_t nowW = 0, nowR = 0;
static portMUX_TYPE nowMux = portMUX_INITIALIZER_UNLOCKED;
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static wifi_interface_t nowIf = WIFI_IF_STA;

static void onNowRecv(const uint8_t*, const uint8_t* data, int len) {   // Wi-Fi task: copy and go
  if (len < 8 || len > 250 || data[0] != MAGIC0 || data[1] != MAGIC1) return;
  portENTER_CRITICAL(&nowMux);
  if ((uint8_t)(nowW - nowR) < NOW_RING) { NowPkt& p = nowRing[nowW % NOW_RING]; p.len = len; memcpy(p.buf, data, len); nowW++; }
  portEXIT_CRITICAL(&nowMux);
}
// whichever side of Wi-Fi is running: its own network (AP) or the home one (STA)
static wifi_interface_t pickIf() {
  wifi_mode_t m = WIFI_MODE_NULL; esp_wifi_get_mode(&m);
  if (m == WIFI_MODE_AP) return WIFI_IF_AP;
  if (m == WIFI_MODE_STA) return WIFI_IF_STA;
  return WiFi.status() == WL_CONNECTED ? WIFI_IF_STA : WIFI_IF_AP;
}
static bool nowStart() {
  wifi_mode_t m = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&m) != ESP_OK || m == WIFI_MODE_NULL) return false;
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onNowRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6); peer.channel = 0; peer.encrypt = false; peer.ifidx = nowIf = pickIf();
  if (esp_now_add_peer(&peer) != ESP_OK) { esp_now_deinit(); return false; }
  return true;
}
static void nowStop() { if (nowUp) { esp_now_unregister_recv_cb(); esp_now_deinit(); } nowUp = false; nowR = nowW; }
static void nowSend(const uint8_t* b, size_t len) {
  if (!nowUp || len > 250) return;
  wifi_interface_t want = pickIf();
  if (want != nowIf) {                                   // Wi-Fi switched sides (joined or left home Wi-Fi)
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BCAST, 6); peer.channel = 0; peer.encrypt = false; peer.ifidx = nowIf = want;
    esp_now_mod_peer(&peer);
  }
  // A broadcast frame gets no acknowledgement and no retry, so one lost to a busy channel is
  // simply gone. Every packet goes out twice, a moment apart; boards drop the copy they've
  // already seen (by its id), so it costs nothing but a little air time.
  if (esp_now_send(BCAST, b, len) == ESP_OK) nowSentCount++;
  vTaskDelay(pdMS_TO_TICKS(60 + esp_random() % 90));
  esp_now_send(BCAST, b, len);
}
static uint8_t wifiChannel() { uint8_t ch = 0; wifi_second_chan_t sc; esp_wifi_get_channel(&ch, &sc); return ch; }

// For the OLED
String comsLastLine; uint32_t comsLastAt = 0;
bool comsOn() { return enabled && (radioUp || nowUp); }
bool comsWifiOn() { return enabled && bandNow; }

static String hex8(uint32_t v) { char b[9]; snprintf(b, sizeof b, "%08x", v); return b; }

static void femOn(bool on) {
#if defined(SHELF_V3) || defined(SHELF_NO_RADIO)
  (void)on; return;                         // the V3 has no front-end module
#endif
  pinMode(FEM_POWER, OUTPUT); pinMode(FEM_EN, OUTPUT);
  pinMode(FEM_CTX, OUTPUT);
#if !CONFIG_SPIRAM_MODE_OCT
  pinMode(FEM_CPS, OUTPUT); digitalWrite(FEM_CPS, LOW);   // (the V4-R8's GPIO 46 is its white LED)
#endif
  digitalWrite(FEM_POWER, on ? HIGH : LOW);
  digitalWrite(FEM_EN, on ? HIGH : LOW);
  digitalWrite(FEM_CTX, LOW);
  if (on) delay(5);
}
static void femTx(bool tx) {
#if defined(SHELF_V3) || defined(SHELF_NO_RADIO)
  (void)tx; return;
#endif
#if !CONFIG_SPIRAM_MODE_OCT
  digitalWrite(FEM_CPS, tx ? HIGH : LOW);
#endif
  digitalWrite(FEM_CTX, tx ? HIGH : LOW);
}

static void addLog(const String& json) {
  {
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    logRam.push_back(json);
    if (logRam.size() > LOG_KEEP) logRam.erase(logRam.begin(), logRam.begin() + (logRam.size() - LOG_KEEP));
    xSemaphoreGiveRecursive(comsLock);
  }
  File f = store->open(LOG_FILE, FILE_APPEND);
  if (f) { f.println(json); f.close(); }
}

static void loadLog() {
  File f = store->open(LOG_FILE, FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  if (sz > 96 * 1024) {                     // keep the file from growing forever
    f.seek(sz - 64 * 1024); f.readStringUntil('\n');
  }
  std::vector<String> lines;
  while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l.startsWith("{")) lines.push_back(l); }
  f.close();
  if (lines.size() > LOG_KEEP) lines.erase(lines.begin(), lines.begin() + (lines.size() - LOG_KEEP));
  logRam = lines;
  if (sz > 96 * 1024) {                     // rewrite the trimmed log
    File o = store->open(LOG_FILE, FILE_WRITE);
    if (o) { for (auto& l : logRam) o.println(l); o.close(); }
  }
}

static bool radioStart() {
#ifdef SHELF_NO_RADIO
  lastError = "This board has no LoRa radio."; return false;
#endif
  femOn(true);
  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  if (!radio) radio = new SX1262(new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, loraSPI));
  int st = radio->begin(FREQ, 125.0, 9, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, CHIP_DBM[level], 16, 1.8);
  if (st != RADIOLIB_ERR_NONE) { lastError = String("Radio didn't start (code ") + st + ")"; femOn(false); return false; }
  radio->setDio2AsRfSwitch(true);
  radio->setCRC(2);
  radio->setCurrentLimit(140);
  radio->setPacketReceivedAction(onRx);
  radio->startReceive();
  lastError = "";
  return true;
}

static void radioStop() {
  if (radio && radioUp) { radio->clearPacketReceivedAction(); radio->sleep(); }
  femOn(false);
  radioUp = false;
}

// ------------------------------------------------------------ packets
// [0]'S' [1]'9' [2]proto [3]hopsLeft [4]hopsTaken [5..8]msgId [9..12]origin [13..16]sender
// [17]nameLen [18..]name [..]text
// Replay packets (type 3) carry the message's age in seconds at [17..20], before the name.
static size_t buildPacket(uint8_t* b, uint32_t id, uint32_t origin, uint8_t hopsLeft, uint8_t hopsTaken, const String& name, const String& text,
                          uint8_t type = T_TEXT, uint32_t ageS = 0) {
  size_t n = 0;
  b[n++] = MAGIC0; b[n++] = MAGIC1; b[n++] = type; b[n++] = hopsLeft; b[n++] = hopsTaken;
  memcpy(b + n, &id, 4); n += 4; memcpy(b + n, &origin, 4); n += 4; memcpy(b + n, &myNode, 4); n += 4;
  if (type == T_REPLAY) { memcpy(b + n, &ageS, 4); n += 4; }
  size_t nl = std::min(name.length(), MAX_NAME); b[n++] = nl; memcpy(b + n, name.c_str(), nl); n += nl;
  size_t tl = std::min(text.length(), MAX_TEXT); memcpy(b + n, text.c_str(), tl); n += tl;
  return n;
}

static void queueTx(const uint8_t* buf, size_t len, uint32_t delayMs) {
  Tx t; memcpy(t.buf, buf, len); t.len = len; t.due = millis() + delayMs; t.nowSent = false;
  xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
  if (txq.size() < (ESP.getPsramSize() ? 80u : 24u)) txq.push_back(t);    // no PSRAM: a short queue
  xSemaphoreGiveRecursive(comsLock);
}

static String entryJson(uint32_t id, uint32_t origin, const String& name, const String& text, int hops, bool mine, float rssi, float snr,
                        uint64_t t = 0, bool synced = false) {
  String j = "{\"id\":\"" + hex8(id) + "\",\"node\":\"" + hex8(origin) + "\",\"from\":\"" + jsonEsc(name) + "\",\"text\":\"" + jsonEsc(text) +
             "\",\"t\":" + String((double)(t ? t : nowMs()), 0) + ",\"hops\":" + hops + ",\"mine\":" + (mine ? "true" : "false");
  if (synced) j += ",\"synced\":true";
  if (!mine) j += ",\"rssi\":" + String(rssi, 0) + ",\"snr\":" + String(snr, 1);
  return j + "}";
}

static void handleSyncRequest(const uint8_t* b, size_t len) {
  // [2]=2 [3..6] requester [7] k [8..] k message ids it already has
  if (len < 8) return;
  uint32_t req; memcpy(&req, b + 3, 4);
  if (req == myNode) return;
  for (int i = 0; i < 8; i++) if (lastServed[i] == req && millis() - lastServedAt[i] < 120000) return;  // served it just now
  size_t k = std::min((size_t)b[7], (len - 8) / 4);
  xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
  syncJob.active = true; syncJob.requester = req; syncJob.created = millis();
  syncJob.due = millis() + 500 + esp_random() % 2500;   // random wait: whoever goes first answers
  syncJob.have.clear();
  for (size_t i = 0; i < k; i++) { uint32_t id; memcpy(&id, b + 8 + i * 4, 4); syncJob.have.push_back(id); }
  xSemaphoreGiveRecursive(comsLock);
}

static bool isMine(uint32_t msg) { return std::find(myIds.begin(), myIds.end(), msg) != myIds.end(); }

static void noteReceipt(uint32_t msg, uint32_t node, const String& name, bool save) {
  xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
  for (auto& r : receipts) if (r.msg == msg && r.node == node) { xSemaphoreGiveRecursive(comsLock); return; }
  receipts.push_back({msg, node, name});
  if (receipts.size() > COMS_RCPT_KEEP) receipts.erase(receipts.begin(), receipts.begin() + COMS_RCPT_KEEP / 4);
  xSemaphoreGiveRecursive(comsLock);
  if (save) {
    File f = store->open(RECEIPT_FILE, FILE_APPEND);
    if (f) { f.printf("{\"m\":\"%s\",\"n\":\"%s\",\"by\":\"%s\"}\n", hex8(msg).c_str(), hex8(node).c_str(), jsonEsc(name).c_str()); f.close(); }
  }
}

// [2]=4 [3]hopsLeft [4]hopsTaken [5..8]receipt id [9..12]who received it [13..16]sender of this copy
// [17..20]message id it's for [21]nameLen [22..]name
static size_t buildReceipt(uint8_t* b, uint32_t rid, uint32_t receiver, uint8_t hopsLeft, uint8_t hopsTaken, uint32_t msg, const String& name) {
  size_t n = 0;
  b[n++] = MAGIC0; b[n++] = MAGIC1; b[n++] = T_RECEIPT; b[n++] = hopsLeft; b[n++] = hopsTaken;
  memcpy(b + n, &rid, 4); n += 4; memcpy(b + n, &receiver, 4); n += 4; memcpy(b + n, &myNode, 4); n += 4;
  memcpy(b + n, &msg, 4); n += 4;
  size_t nl = std::min(name.length(), MAX_NAME); b[n++] = nl; memcpy(b + n, name.c_str(), nl); n += nl;
  return n;
}

static void handleReceipt(const uint8_t* b, size_t len) {
  if (len < 22) return;
  uint8_t hopsLeft = b[3], hopsTaken = b[4];
  uint32_t rid, receiver, msg;
  memcpy(&rid, b + 5, 4); memcpy(&receiver, b + 9, 4); memcpy(&msg, b + 17, 4);
  size_t nl = b[21];
  if (22 + nl > len || nl > MAX_NAME) return;
  String name; for (size_t i = 0; i < nl; i++) name += (char)b[22 + i];
  if (receiver == myNode || wasSeen(rid)) return;
  markSeen(rid);
  if (isMine(msg)) { noteReceipt(msg, receiver, name, true); return; }   // it's for me: done
  if (hopsLeft > 0) {                                                      // someone else's: pass it on
    uint8_t out[64];
    size_t n = buildReceipt(out, rid, receiver, hopsLeft - 1, hopsTaken + 1, msg, name);
    queueTx(out, n, 600 + esp_random() % 2000);
  }
}

// Store and forward for boards on the move: meeting a board we haven't heard for 20 minutes
// (someone walking into range, or coming back) makes this board ask for the last 12 hours
// again, so messages travel with people as they carry boards around. At most once a minute.
static uint32_t lastMeetSync = 0;
static void meetNeighbor(bool fresh) {
  if (!fresh || (lastMeetSync && millis() - lastMeetSync < 60000)) return;
  lastMeetSync = millis();
  syncAsks = 1; syncAskAt = millis() + 2000 + esp_random() % 3000;      // one ask, after a short random wait
}
static void noteNeighbor(uint32_t sender, float rssi, float snr) {
  xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
  Neighbor* nb = nullptr; for (auto& x : neighbors) if (x.node == sender) nb = &x;
  bool fresh = !nb || nowMs() - nb->at > 20ULL * 60 * 1000;
  if (!nb) { neighbors.push_back({sender, "", 0, 0, 0}); nb = &neighbors.back(); }
  nb->rssi = rssi; nb->snr = snr; nb->at = nowMs();
  xSemaphoreGiveRecursive(comsLock);
  meetNeighbor(fresh);
}
static void handleMeshPacket(const uint8_t* b, size_t len, float rssi, float snr);
static void xferPacket(const uint8_t* b, size_t len, float rssi);

static void handlePacket(const uint8_t* b, size_t len, float rssi, float snr) {
  if (len < 8 || b[0] != MAGIC0 || b[1] != MAGIC1) return;
  uint8_t type = b[2];
  if (type >= T_FOFFER && type <= T_FCANCEL) {
    if (type <= T_FREJECT && len >= 17) { uint32_t sender; memcpy(&sender, b + 13, 4); if (sender != myNode) noteNeighbor(sender, rssi, snr); }
    xferPacket(b, len, rssi); return;
  }
  if (type == T_SYNC_REQ) { handleSyncRequest(b, len); return; }
  if (type >= T_HELLO && type <= T_WHO) {
    if (len < 17) return;
    uint32_t sender; memcpy(&sender, b + 13, 4);
    if (sender != myNode) noteNeighbor(sender, rssi, snr);
    handleMeshPacket(b, len, rssi, snr);
    return;
  }
  if (type == T_RECEIPT) {
    if (len >= 17) {                       // still note who we heard
      uint32_t sender; memcpy(&sender, b + 13, 4);
      if (sender != myNode) noteNeighbor(sender, rssi, snr);
    }
    handleReceipt(b, len); return;
  }
  if (type != T_TEXT && type != T_REPLAY) return;
  size_t hdr = type == T_REPLAY ? 22 : 18;
  if (len < hdr) return;
  uint8_t hopsLeft = b[3], hopsTaken = b[4];
  uint32_t id, origin, sender, ageS = 0;
  memcpy(&id, b + 5, 4); memcpy(&origin, b + 9, 4); memcpy(&sender, b + 13, 4);
  if (type == T_REPLAY) { memcpy(&ageS, b + 17, 4); if (sender != myNode) lastReplayHeardAt = millis(); }
  size_t nl = b[hdr - 1];
  if (hdr + nl > len || nl > MAX_NAME) return;
  String name; for (size_t i = 0; i < nl; i++) name += (char)b[hdr + i];
  String text; for (size_t i = hdr + nl; i < len; i++) text += (char)b[i];
  // who we heard directly (the sender of this copy)
  {
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    Neighbor* nb = nullptr; for (auto& x : neighbors) if (x.node == sender) nb = &x;
    bool fresh = !nb || nowMs() - nb->at > 20ULL * 60 * 1000;
    if (!nb) { neighbors.push_back({sender, "", 0, 0, 0}); nb = &neighbors.back(); }
    if (sender == origin) nb->name = name;
    nb->rssi = rssi; nb->snr = snr; nb->at = nowMs();
    xSemaphoreGiveRecursive(comsLock);
    if (sender != myNode && type == T_TEXT) meetNeighbor(fresh);   // (not for catch-up copies: those are a sync already)
  }
  if (origin == myNode || wasSeen(id)) return;
  markSeen(id);
  if (type == T_TEXT) {
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    Member* mb = nullptr; for (auto& x : members) if (x.node == origin) mb = &x;
    if (!mb && members.size() < 64) { members.push_back({origin, name, 0, 255, 0, hopsTaken, rssi, nowMs()}); }
    else if (mb) { mb->name = name; mb->hops = hopsTaken; mb->at = nowMs(); if (!hopsTaken) mb->rssi = rssi; }
    xSemaphoreGiveRecursive(comsLock);
  }
  if (type == T_REPLAY) {                    // catch-up copy: log it with its real age, never relay it
    if (ageS > SYNC_WINDOW_S) return;
    uint64_t t = nowMs() > (uint64_t)ageS * 1000 ? nowMs() - (uint64_t)ageS * 1000 : 0;
    addLog(entryJson(id, origin, name, text, hopsTaken, false, rssi, snr, t, true));
    syncedCount++;
    return;
  }
  heardCount++;
  addLog(entryJson(id, origin, name, text, hopsTaken, false, rssi, snr));
  comsLastLine = name + ": " + text; comsLastAt = millis();
  // relay
  if (hopsLeft > 0) {
    uint8_t out[256];
    size_t n = buildPacket(out, id, origin, hopsLeft - 1, hopsTaken + 1, name, text);
    queueTx(out, n, 400 + esp_random() % 1600);
    relayedCount++;
  }
  // say "received" back to the sender (after the relay has gone out)
  if (receiptsOn) {
    uint8_t out[64];
    size_t n = buildReceipt(out, esp_random(), myNode, HOPS, 0, id, myName);
    queueTx(out, n, 2200 + esp_random() % 2500);
  }
}

// ------------------------------------------------------------ hellos, remote admin
static void relayCopy(const uint8_t* b, size_t len) {     // flood it on: one fewer hop, me as sender
  if (b[3] == 0 || len > 255) return;
  uint8_t out[256]; memcpy(out, b, len);
  out[3]--; out[4]++; memcpy(out + 13, &myNode, 4);
  queueTx(out, len, 400 + esp_random() % 1600);
}

static size_t header(uint8_t* b, uint8_t type, uint32_t id) {
  b[0] = MAGIC0; b[1] = MAGIC1; b[2] = type; b[3] = HOPS; b[4] = 0;
  memcpy(b + 5, &id, 4); memcpy(b + 9, &myNode, 4); memcpy(b + 13, &myNode, 4);
  return 17;
}

static void sendHello(bool answer) {
  uint8_t b[64]; uint32_t id = esp_random(); markSeen(id);
  size_t n = header(b, T_HELLO, id);
#if defined(SHELF_ESP32)
  b[n++] = 32;
#elif defined(SHELF_V3)
  b[n++] = 3;
#else
  b[n++] = 4;
#endif
  int bat = batteryPercentNow(); b[n++] = bat < 0 ? 255 : (uint8_t)bat;
  b[n++] = (adminIsSet() ? 1 : 0) | (readOnlyOn() ? 2 : 0) | (answer ? 4 : 0);
  size_t nl = std::min(myName.length(), MAX_NAME); b[n++] = nl; memcpy(b + n, myName.c_str(), nl); n += nl;
  queueTx(b, n, 0);
  lastHelloSent = millis();
}

// signed part of a request/reply: type + id + origin, then everything after the sender field
static void macOf(const uint8_t key[32], const uint8_t* b, size_t bodyEnd, uint8_t out[8]) {
  uint8_t m[256]; size_t n = 0;
  m[n++] = b[2]; memcpy(m + n, b + 5, 8); n += 8;
  memcpy(m + n, b + 17, bodyEnd - 17); n += bodyEnd - 17;
  hmac8(key, m, n, out);
}

static void sendAdminReq(Pending& p) {
  uint8_t b[256];
  size_t n = header(b, T_ADMIN_REQ, p.req);
  memcpy(b + n, &p.node, 4); n += 4; memcpy(b + n, &p.counter, 4); n += 4;
  b[n++] = p.cmd;
  size_t al = std::min((size_t)p.arg.length(), (size_t)180); b[n++] = al; memcpy(b + n, p.arg.c_str(), al); n += al;
  macOf(p.key, b, n, b + n); n += 8;
  markSeen(p.req);
  queueTx(b, n, 0);
  p.sentAt = millis();
}

static void sendAdminResp(uint32_t dest, uint32_t reqId, uint8_t status, const String& data, const uint8_t* key) {
  uint8_t b[256]; uint32_t id = esp_random();
  size_t n = header(b, T_ADMIN_RESP, id);
  memcpy(b + n, &dest, 4); n += 4; memcpy(b + n, &reqId, 4); n += 4;
  b[n++] = status;
  size_t dl = std::min((size_t)data.length(), REPLY_MAX); b[n++] = dl; memcpy(b + n, data.c_str(), dl); n += dl;
  if (key) macOf(key, b, n, b + n); else memset(b + n, 0, 8);
  n += 8;
  markSeen(id);
  queueTx(b, n, 300 + esp_random() % 600);
}

static String fullRemoteStatus() {
  String s = remoteStatusJson(); s.remove(s.length() - 1);
  String core = s + ",\"cm\":\"" + jsonEsc(myName) + "\",\"lv\":" + level + ",\"rc\":" + (receiptsOn ? 1 : 0) + ",\"mb\":" + members.size();
  String full = core + ",\"hd\":" + heardCount + ",\"rl\":" + relayedCount + "}";
  if (full.length() <= REPLY_MAX) return full;
  if (core.length() + 1 <= REPLY_MAX) return core + "}";       // drop the counters to fit one packet
  return String("{\"b\":\"" BOARD_SHORT "\",\"fw\":\"" FW_VERSION "\",\"bat\":") + batteryPercentNow() + ",\"ro\":" + (readOnlyOn() ? 1 : 0) + ",\"lv\":" + level + "}";
}

static void runAdminCommand(uint32_t from, uint32_t reqId, uint8_t cmd, const String& arg) {
  String out; bool ok = true;
  switch (cmd) {
    case C_STATUS: out = fullRemoteStatus(); break;
    case C_PING: out = "pong"; break;
    case C_POWER: level = constrain(arg.toInt(), 0, 2); cprefs.putInt("level", level); out = fullRemoteStatus(); break;
    case C_RECEIPTS: receiptsOn = arg == "1"; cprefs.putBool("receipts", receiptsOn); out = fullRemoteStatus(); break;
    case C_NAME: { String nm = arg; nm.trim(); if (nm.length()) { myName = nm.substring(0, MAX_NAME); cprefs.putString("name", myName); } out = fullRemoteStatus(); break; }
    default:
      ok = remoteBoardCommand(cmd, arg, out);
      if (ok && out.startsWith("{")) out = fullRemoteStatus();
  }
  sendAdminResp(from, reqId, ok ? RS_OK : RS_UNKNOWN, ok ? out : String("Unknown command"), adminKey());
}

static void handleMeshPacket(const uint8_t* b, size_t len, float rssi, float snr) {
  uint8_t type = b[2];
  uint32_t id, origin; memcpy(&id, b + 5, 4); memcpy(&origin, b + 9, 4);
  if (origin == myNode || wasSeen(id)) return;
  markSeen(id);
  uint8_t hopsTaken = b[4];

  if (type == T_HELLO) {
    if (len < 21) return;
    size_t nl = b[20]; if (21 + nl > len || nl > MAX_NAME) return;
    String name; for (size_t i = 0; i < nl; i++) name += (char)b[21 + i];
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    Member* mb = nullptr; for (auto& x : members) if (x.node == origin) mb = &x;
    if (!mb) {
      if (members.size() >= 64) { size_t old = 0; for (size_t i = 1; i < members.size(); i++) if (members[i].at < members[old].at) old = i; members.erase(members.begin() + old); }
      members.push_back({origin, "", 0, 255, 0, 0, 0, 0}); mb = &members.back();
    }
    mb->name = name; mb->board = b[17]; mb->batt = b[18]; mb->flags = b[19]; mb->hops = hopsTaken; mb->at = nowMs();
    if (!hopsTaken) mb->rssi = rssi;
    xSemaphoreGiveRecursive(comsLock);
    relayCopy(b, len);
    return;
  }
  if (type == T_WHO) {                        // someone opened their device list: say hello (once a minute at most)
    relayCopy(b, len);
    if (millis() - lastHelloSent > 60000 && !whoAnswerAt) whoAnswerAt = millis() + 1000 + esp_random() % 7000;
    return;
  }
  if (type == T_ADMIN_REQ) {
    if (len < 27 + 8) return;
    uint32_t target, counter; memcpy(&target, b + 17, 4); memcpy(&counter, b + 21, 4);
    uint8_t cmd = b[25], al = b[26];
    if (27 + (size_t)al + 8 != len) return;
    if (target != myNode) { relayCopy(b, len); return; }
    String arg; for (size_t i = 0; i < al; i++) arg += (char)b[27 + i];
    if (!adminIsSet()) { sendAdminResp(origin, id, RS_NOADMIN, "", nullptr); return; }
    if (adminLockedOut()) { sendAdminResp(origin, id, RS_LOCKED, "", nullptr); return; }
    uint8_t mac[8]; macOf(adminKey(), b, 27 + al, mac);
    if (memcmp(mac, b + 27 + al, 8) != 0) { adminNoteFail(); sendAdminResp(origin, id, RS_BADPASS, "", nullptr); return; }
    uint32_t last = adminLastCounter(origin);
    if (counter <= last) { String d; d.concat((const char*)&last, 4); sendAdminResp(origin, id, RS_STALE, d, nullptr); return; }
    adminNoteOk(); adminSetCounter(origin, counter);
    runAdminCommand(origin, id, cmd, arg);
    return;
  }
  if (type == T_ADMIN_RESP) {
    if (len < 27 + 8) return;
    uint32_t dest, reqId; memcpy(&dest, b + 17, 4); memcpy(&reqId, b + 21, 4);
    uint8_t status = b[25], dl = b[26];
    if (27 + (size_t)dl + 8 != len) return;
    if (dest != myNode) { relayCopy(b, len); return; }
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    for (auto& p : pendings) {
      if (p.req != reqId || p.state != 0 || p.node != origin) continue;
      if (status == RS_OK) {
        uint8_t mac[8]; macOf(p.key, b, 27 + dl, mac);
        if (memcmp(mac, b + 27 + dl, 8) != 0) break;          // not signed with this board's key: ignore
        p.data = ""; for (size_t i = 0; i < dl; i++) p.data += (char)b[27 + i];
        p.status = RS_OK; p.state = 1;
      } else if (status == RS_STALE && !p.retried && dl == 4) {  // its counter for me is ahead (e.g. after a reflash): catch up, try again
        uint32_t last; memcpy(&last, b + 27, 4);
        p.retried = true; p.req = esp_random(); p.counter = adminNextOwnCounter(last + 1);
        sendAdminReq(p);
      } else { p.status = status; p.state = 2; }
      break;
    }
    xSemaphoreGiveRecursive(comsLock);
    return;
  }
}

// ------------------------------------------------------------ Meshed file transfer
// Sends a file (up to 5 MB) from one board to another it hears directly. The offer and the answer
// go over the mesh as usual; then both boards move to a fast link just for the two of them:
//   900 MHz (both have LoRa): 923.3 MHz, 500 kHz wide, SF7 - about 10x the mesh's speed (and less
//     range than the mesh's own settings: think a few hundred yards to a mile, not miles).
//     Meshed Comms chat on 915 MHz pauses on those two boards while it runs.
//   2.4 GHz (a plain ESP32 on either end): ESP-NOW frames, faster still, about Wi-Fi's range.
// The file goes 224 bytes a packet, 32 packets a window; after each window the receiver says
// which it got, and only the missing ones are sent again. A CRC-32 over the whole file checks it
// at the end, and it lands on the same shelf on the other board (named like the original).
static const size_t XF_CHUNK = 224, XF_WIN = 32;
static const uint32_t XF_MAX = 5UL * 1024 * 1024;
static const float XF_FREQ = 923.3;
enum { XF_IDLE, XF_S_PREP, XF_S_OFFER, XF_S_SEND, XF_S_ACK, XF_S_DONE, XF_R_RECV };
enum { XT_LORA = 1, XT_NOW = 2 };
struct XferHist { bool sent, ok; String name, cat, peer, note; uint32_t size, secs; uint64_t at; };
static struct {
  volatile int phase = XF_IDLE;
  uint32_t xid = 0, peer = 0, size = 0, crc = 0, chunks = 0, w = 0, bytes = 0;
  uint8_t transport = 0;
  String peerName, cat, name, path, tmp, status;
  uint32_t t0 = 0, last = 0, sentAt = 0; int tries = 0;
  uint32_t have = 0, need = 0, nIn = 0;          // this window: chunks received (bits), wanted (bits), how many
  uint8_t* buf = nullptr;                        // one window
  uint32_t crcRun = 0;                           // receiver: running CRC of what's written
  bool cancel = false;
  uint32_t lingerUntil = 0;                      // receiver: stay on the link a moment to answer a repeated "done"
  bool lastOk = false;
} X;
static std::vector<XferHist> xhist;
static bool xferAccept = true, loraFast = false;
static String xferPending;                       // a send asked for by the web page: "node\tcat\tname"
static portMUX_TYPE xMux = portMUX_INITIALIZER_UNLOCKED;

static void loraMode(bool fast) {
#ifndef SHELF_NO_RADIO
  if (!radioUp || fast == loraFast) return;
  radio->standby();
  if (fast) { radio->setFrequency(XF_FREQ); radio->setBandwidth(500.0); radio->setSpreadingFactor(7); radio->setPreambleLength(8); }
  else { radio->setFrequency(FREQ); radio->setBandwidth(125.0); radio->setSpreadingFactor(9); radio->setPreambleLength(16); }
  loraFast = fast;
  rxFlag = false;
  radio->startReceive();
#else
  (void)fast;
#endif
}
static void xSend(const uint8_t* b, size_t len) {               // on the transfer link
  if (X.transport == XT_LORA) {
#ifndef SHELF_NO_RADIO
    if (!radioUp) return;
    radio->setOutputPower(CHIP_DBM[level]);
    femTx(true); radio->transmit(b, len); femTx(false);
    rxFlag = false; radio->startReceive();
#endif
  } else if (nowUp) {
    for (int i = 0; i < 5; i++) { if (esp_now_send(BCAST, b, len) == ESP_OK) break; vTaskDelay(pdMS_TO_TICKS(4)); }
    vTaskDelay(pdMS_TO_TICKS(5));                              // (keeps the receiver's ring from overflowing)
  }
}
static size_t xHead(uint8_t* b, uint8_t type) { b[0] = MAGIC0; b[1] = MAGIC1; b[2] = type; memcpy(b + 3, &X.xid, 4); return 7; }
static void xFinish(bool ok, const String& note) {
  XferHist h; h.sent = X.phase != XF_R_RECV; h.ok = ok; h.name = X.name; h.cat = X.cat; h.peer = X.peerName.length() ? X.peerName : hex8(X.peer);
  h.note = note; h.size = X.size; h.secs = (millis() - X.t0) / 1000; h.at = nowMs();
  xhist.insert(xhist.begin(), h); if (xhist.size() > 12) xhist.pop_back();
  if (!ok && X.tmp.length()) { FsGuard g; store->remove(X.tmp); }
  if (X.buf) { free(X.buf); X.buf = nullptr; }
  X.status = note; X.lastOk = ok;
  loraMode(false);
  X.phase = XF_IDLE;
  comsLastLine = String(ok ? (h.sent ? "Sent " : "Got ") : "File: ") + (ok ? X.name : note); comsLastAt = millis();
}
static String peerNameOf(uint32_t node) {
  for (auto& m : members) if (m.node == node && m.name.length()) return m.name;
  for (auto& n : neighbors) if (n.node == node && n.name.length()) return n.name;
  return hex8(node);
}
static uint32_t winCount(uint32_t w) { uint32_t first = w * XF_WIN; return std::min<uint32_t>(XF_WIN, X.chunks - first); }
static uint32_t chunkLen(uint32_t seq) { uint32_t off = seq * XF_CHUNK; return std::min<uint32_t>(XF_CHUNK, X.size - off); }

// ---- sender
static bool xLoadWindow() {
  uint32_t n = winCount(X.w), off = X.w * XF_WIN * XF_CHUNK, want = std::min<uint32_t>(n * XF_CHUNK, X.size - off);
  FsGuard g;
  File f = store->open(X.path, FILE_READ);
  if (!f) return false;
  f.seek(off);
  size_t got = f.read(X.buf, want); f.close();
  X.nIn = n; X.need = n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1);
  return got == want;
}
static void xSendWindow() {
  uint8_t p[256];
  for (uint32_t i = 0; i < X.nIn && !X.cancel; i++) {
    if (!(X.need >> i & 1)) continue;
    uint32_t seq = X.w * XF_WIN + i;
    size_t n = xHead(p, T_FDATA); memcpy(p + n, &seq, 4); n += 4;
    uint32_t len = chunkLen(seq); memcpy(p + n, X.buf + i * XF_CHUNK, len); n += len;
    xSend(p, n);
  }
  size_t n = xHead(p, T_FWEND); memcpy(p + n, &X.w, 4); n += 4; p[n++] = X.nIn;
  xSend(p, n);
  X.phase = XF_S_ACK; X.sentAt = millis();
}
static void xferStartSend(uint32_t node, const String& cat, const String& name) {
  X = decltype(X)();
  X.phase = XF_S_PREP; X.peer = node; X.peerName = peerNameOf(node); X.cat = cat; X.name = name;
  X.path = "/" + cat + "/" + name; X.t0 = millis(); X.xid = esp_random() | 1;
  // size and CRC-32
  {
    FsGuard g;
    File f = store->open(X.path, FILE_READ);
    if (!f || f.isDirectory()) { xFinish(false, "Couldn't open the file."); return; }
    X.size = f.size();
    if (!X.size || X.size > XF_MAX) { f.close(); xFinish(false, X.size ? "Files up to 5 MB only." : "That file is empty."); return; }
    uint8_t* tmp = (uint8_t*)malloc(4096);
    if (!tmp) { f.close(); xFinish(false, "Not enough memory."); return; }
    uint32_t crc = 0; size_t r;
    while ((r = f.read(tmp, 4096)) > 0) crc = esp_rom_crc32_le(crc, tmp, r);
    free(tmp); f.close();
    X.crc = crc;
  }
  X.chunks = (X.size + XF_CHUNK - 1) / XF_CHUNK;
  X.buf = (uint8_t*)malloc(XF_WIN * XF_CHUNK);
  if (!X.buf) { xFinish(false, "Not enough memory."); return; }
  X.phase = XF_S_OFFER; X.tries = 0; X.sentAt = 0; X.status = "Asking " + X.peerName + "...";
}
static void xSendOffer() {
  uint8_t b[256]; uint32_t id = esp_random(); markSeen(id);
  size_t n = header(b, T_FOFFER, id); b[3] = 0;                 // direct only: not relayed
  memcpy(b + n, &X.peer, 4); n += 4; memcpy(b + n, &X.xid, 4); n += 4; memcpy(b + n, &X.size, 4); n += 4; memcpy(b + n, &X.crc, 4); n += 4;
  b[n++] = (radioUp && bandLora) ? 1 : 0;
  size_t cl = std::min<size_t>(X.cat.length(), 15); b[n++] = cl; memcpy(b + n, X.cat.c_str(), cl); n += cl;
  size_t nl = std::min<size_t>(X.name.length(), 200 - n); b[n++] = nl; memcpy(b + n, X.name.c_str(), nl); n += nl;
  queueTx(b, n, 0);
}

// ---- receiver
static void xAnswer(uint8_t type, uint32_t to, uint32_t xid, uint8_t v) {   // accept / reject, over the mesh, now
  uint8_t b[64]; uint32_t id = esp_random(); markSeen(id);
  size_t n = header(b, type, id); b[3] = 0;
  memcpy(b + n, &to, 4); n += 4; memcpy(b + n, &xid, 4); n += 4; b[n++] = v;
  for (int k = 0; k < 2; k++) {                                // twice, to be sure it's heard before we move
    if (nowUp) esp_now_send(BCAST, b, n);
#ifndef SHELF_NO_RADIO
    if (radioUp && !loraFast) { radio->setOutputPower(CHIP_DBM[level]); femTx(true); radio->transmit(b, n); femTx(false); rxFlag = false; radio->startReceive(); }
#endif
    vTaskDelay(pdMS_TO_TICKS(120));
  }
}
static void xWriteWindow() {
  uint32_t n = winCount(X.w), off = X.w * XF_WIN * XF_CHUNK, len = std::min<uint32_t>(n * XF_CHUNK, X.size - off);
  {
    FsGuard g;
    File f = store->open(X.tmp, FILE_APPEND);
    if (f) { f.write(X.buf, len); f.close(); }
  }
  X.crcRun = esp_rom_crc32_le(X.crcRun, X.buf, len);
  X.bytes = off + len;
  X.w++; X.have = 0;
}

static void xferPacket(const uint8_t* b, size_t len, float rssi) {
  uint8_t type = b[2];
  if (type <= T_FREJECT) {                                     // over the mesh: offers and answers
    if (len < 26) return;
    uint32_t sender, to, xid; memcpy(&sender, b + 13, 4); memcpy(&to, b + 17, 4); memcpy(&xid, b + 21, 4);
    if (to != myNode) return;
    if (type == T_FOFFER) {
      if (len < 35) return;
      uint32_t size, crc; memcpy(&size, b + 25, 4); memcpy(&crc, b + 29, 4);
      bool theyLora = b[33] & 1; size_t p = 34;
      size_t cl = b[p++]; if (p + cl >= len) return;
      String cat; for (size_t i = 0; i < cl; i++) cat += (char)b[p + i]; p += cl;
      size_t nl = b[p++]; if (p + nl > len) return;
      String name; for (size_t i = 0; i < nl; i++) name += (char)b[p + i];
      if (X.phase == XF_R_RECV && X.xid == xid) return;        // (a repeat: we're already on it)
      uint8_t why = 0;
      if (X.phase != XF_IDLE) why = 1;                         // busy
      else if (!xferAccept) why = 2;                           // not taking files
      else if (!size || size > XF_MAX || !isCategory(cat)) why = 3;
      else if (storeTotal && storeTotal - storeUsed < (uint64_t)size + 65536) why = 4;   // no room
      uint8_t tr = (theyLora && radioUp && bandLora) ? XT_LORA : nowUp ? XT_NOW : 0;
      if (!why && !tr) why = 5;
      if (why) { xAnswer(T_FREJECT, sender, xid, why); return; }
      X = decltype(X)();
      X.buf = (uint8_t*)malloc(XF_WIN * XF_CHUNK);
      if (!X.buf) { xAnswer(T_FREJECT, sender, xid, 6); return; }
      X.xid = xid; X.peer = sender; X.peerName = peerNameOf(sender); X.size = size; X.crc = crc; X.cat = cat;
      X.name = sanitizeFilename(name); X.chunks = (size + XF_CHUNK - 1) / XF_CHUNK; X.transport = tr;
      X.tmp = String(TMP_ROOT "/xfer-") + hex8(xid) + ".part";
      { FsGuard g; store->remove(X.tmp); File f = store->open(X.tmp, FILE_WRITE); if (f) f.close(); }
      X.t0 = X.last = millis(); X.status = "Receiving from " + X.peerName;
      X.phase = XF_R_RECV;
      xAnswer(T_FACCEPT, sender, xid, tr);
      if (tr == XT_LORA) loraMode(true);
      screenShowing("Files", X.name);
      return;
    }
    if (xid != X.xid || sender != X.peer) return;
    if (type == T_FACCEPT && X.phase == XF_S_OFFER) {
      X.transport = b[25] == XT_LORA ? XT_LORA : XT_NOW;
      if (X.transport == XT_LORA) loraMode(true);
      vTaskDelay(pdMS_TO_TICKS(400));                          // the receiver is moving over too
      X.w = 0; X.t0 = millis(); X.status = "Sending to " + X.peerName;
      if (!xLoadWindow()) { xFinish(false, "Couldn't read the file."); return; }
      X.phase = XF_S_SEND;
    } else if (type == T_FREJECT && X.phase == XF_S_OFFER) {
      static const char* const WHY[] = {"", "is busy with another file", "isn't taking files (switched off there)", "can't take that file",
                                        "hasn't room for it", "has no radio in common", "is short of memory"};
      uint8_t w = b[25] < 7 ? b[25] : 3;
      xFinish(false, X.peerName + " " + WHY[w] + ".");
    }
    return;
  }
  // on the transfer link
  if (len < 7) return;
  uint32_t xid; memcpy(&xid, b + 3, 4);
  if (xid != X.xid) return;
  X.last = millis();
  if (X.phase == XF_R_RECV || X.lingerUntil) {
    if (type == T_FDATA && len > 11 && X.phase == XF_R_RECV) {
      uint32_t seq; memcpy(&seq, b + 7, 4);
      if (seq / XF_WIN != X.w) return;
      uint32_t i = seq % XF_WIN, l = std::min<uint32_t>(len - 11, XF_CHUNK);
      memcpy(X.buf + i * XF_CHUNK, b + 11, l); X.have |= 1u << i;
    } else if (type == T_FWEND && len >= 12) {
      uint32_t w; memcpy(&w, b + 7, 4);
      uint8_t p[32]; size_t n = xHead(p, T_FACK); memcpy(p + n, &w, 4); n += 4;
      uint32_t bits;
      if (w + 1 == X.w || X.phase != XF_R_RECV) bits = 0xFFFFFFFFu;   // already written (our answer got lost)
      else if (w == X.w) {
        uint32_t cnt = winCount(w), full = cnt == 32 ? 0xFFFFFFFFu : ((1u << cnt) - 1);
        bits = X.have;
        if ((X.have & full) == full) { xWriteWindow(); bits = 0xFFFFFFFFu; }
      } else return;
      memcpy(p + n, &bits, 4); n += 4;
      xSend(p, n);
    } else if (type == T_FDONE && len >= 15) {
      uint32_t crc; memcpy(&crc, b + 7, 4);
      bool ok = X.phase == XF_R_RECV ? (X.w * XF_WIN >= X.chunks && crc == X.crc && X.crcRun == X.crc) : X.lastOk;
      if (X.phase == XF_R_RECV) {
        if (ok) {                                              // into the shelf, named like the original
          FsGuard g;
          String dir = "/" + X.cat; mkdirs(dir);
          String fin = uniqueName(dir, X.name);
          ok = moveFile(X.tmp, dir + "/" + fin);
          if (ok) { X.name = fin; X.tmp = ""; invalidateCounts(); }
        }
        X.lingerUntil = millis() + 3000;
        uint8_t p[16]; size_t n = xHead(p, T_FDONEACK); p[n++] = ok;
        xSend(p, n);
        String note = ok ? "Saved to " + X.cat : "The file didn't check out (sent again, it may work).";
        if (ok && isSongShelf(X.cat)) markMusicDirty();
        int keepT = X.transport; uint32_t keepX = X.xid; uint32_t linger = X.lingerUntil;
        xFinish(ok, note);                                    // (back on the mesh once lingering is over)
        if (keepT == XT_LORA) loraMode(true);
        X.xid = keepX; X.transport = keepT; X.lingerUntil = linger; X.lastOk = ok;
      } else { uint8_t p[16]; size_t n = xHead(p, T_FDONEACK); p[n++] = ok; xSend(p, n); }
    } else if (type == T_FCANCEL && X.phase == XF_R_RECV) xFinish(false, X.peerName + " stopped sending.");
    return;
  }
  if (X.phase == XF_S_ACK && type == T_FACK && len >= 15) {
    uint32_t w, bits; memcpy(&w, b + 7, 4); memcpy(&bits, b + 11, 4);
    if (w != X.w) return;
    uint32_t full = X.nIn == 32 ? 0xFFFFFFFFu : ((1u << X.nIn) - 1);
    if ((bits & full) == full) {
      X.bytes = std::min<uint32_t>((X.w + 1) * XF_WIN * XF_CHUNK, X.size);
      X.w++; X.tries = 0;
      if (X.w * XF_WIN >= X.chunks) {
        uint8_t p[16]; size_t n = xHead(p, T_FDONE); memcpy(p + n, &X.crc, 4); n += 4; memcpy(p + n, &X.size, 4); n += 4;
        xSend(p, n); X.phase = XF_S_DONE; X.sentAt = millis();
      } else if (!xLoadWindow()) xFinish(false, "Couldn't read the file.");
      else X.phase = XF_S_SEND;
    } else { X.need = full & ~bits; X.phase = XF_S_SEND; }
  } else if (X.phase == XF_S_DONE && type == T_FDONEACK && len >= 8) {
    xFinish(b[7] != 0, b[7] ? "Sent to " + X.peerName : X.peerName + " got it, but it didn't check out. Try again.");
  }
}
// called from the Coms task, every turn
static void xferStep() {
  String want;
  portENTER_CRITICAL(&xMux); want = xferPending; xferPending = ""; portEXIT_CRITICAL(&xMux);
  if (want.length() && X.phase == XF_IDLE) {
    int a = want.indexOf('\t'), c = want.indexOf('\t', a + 1);
    xferStartSend(strtoul(want.substring(0, a).c_str(), nullptr, 16), want.substring(a + 1, c), want.substring(c + 1));
  }
  uint32_t now = millis();
  if (X.lingerUntil && X.phase == XF_IDLE && (int32_t)(now - X.lingerUntil) >= 0) { X.lingerUntil = 0; loraMode(false); }
  if (X.cancel && X.phase != XF_IDLE) {
    if (X.phase != XF_S_OFFER && X.phase != XF_S_PREP) { uint8_t p[16]; size_t n = xHead(p, T_FCANCEL); p[n++] = 0; xSend(p, n); }
    xFinish(false, "Stopped."); return;
  }
  switch (X.phase) {
    case XF_S_OFFER:
      if (!X.sentAt || now - X.sentAt > 4000) {
        if (X.tries++ >= 4) { xFinish(false, "No answer from " + X.peerName + "."); return; }
        xSendOffer(); X.sentAt = now;
      }
      break;
    case XF_S_SEND: xSendWindow(); break;
    case XF_S_ACK:
      if (now - X.sentAt > (X.transport == XT_LORA ? 700u : 250u)) {
        if (++X.tries > 12) { xFinish(false, "Lost touch with " + X.peerName + "."); return; }
        uint8_t p[16]; size_t n = xHead(p, T_FWEND); memcpy(p + n, &X.w, 4); n += 4; p[n++] = X.nIn;
        xSend(p, n); X.sentAt = now;
      }
      break;
    case XF_S_DONE:
      if (now - X.sentAt > 900) {
        if (++X.tries > 8) { xFinish(false, "No word back at the end from " + X.peerName + "."); return; }
        uint8_t p[16]; size_t n = xHead(p, T_FDONE); memcpy(p + n, &X.crc, 4); n += 4; memcpy(p + n, &X.size, 4); n += 4;
        xSend(p, n); X.sentAt = now;
      }
      break;
    case XF_R_RECV:
      if (now - X.last > 12000) xFinish(false, "Lost touch with " + X.peerName + ".");
      break;
  }
}
bool xferBusy() { return X.phase != XF_IDLE || X.lingerUntil; }

static String xferJson() {
  String s = String("{\"accept\":") + (xferAccept ? "true" : "false") + ",\"phase\":" + (int)X.phase +
    ",\"sending\":" + (X.phase >= XF_S_PREP && X.phase <= XF_S_DONE ? "true" : "false") + ",\"receiving\":" + (X.phase == XF_R_RECV ? "true" : "false") +
    ",\"name\":\"" + jsonEsc(X.name) + "\",\"cat\":\"" + jsonEsc(X.cat) + "\",\"peer\":\"" + jsonEsc(X.peerName) + "\",\"size\":" + X.size +
    ",\"bytes\":" + X.bytes + ",\"secs\":" + (X.t0 ? (millis() - X.t0) / 1000.0 : 0) + ",\"band\":\"" + (X.transport == XT_LORA ? "900 MHz" : X.transport == XT_NOW ? "2.4 GHz" : "") +
    "\",\"status\":\"" + jsonEsc(X.status) + "\",\"lastOk\":" + (X.lastOk ? "true" : "false") + ",\"max\":" + XF_MAX +
    ",\"lora\":" + (radioUp && bandLora ? "true" : "false") + ",\"wifi\":" + (nowUp ? "true" : "false") + ",\"enabled\":" + (enabled ? "true" : "false") + ",\"peers\":[";
  xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
  bool first = true;
  for (auto& n : neighbors) {
    if (n.node == myNode || nowMs() - n.at > 30ULL * 60 * 1000) continue;
    String nm = n.name.length() ? n.name : peerNameOf(n.node);
    if (!first) s += ","; first = false;
    s += "{\"node\":\"" + hex8(n.node) + "\",\"name\":\"" + jsonEsc(nm) + "\",\"rssi\":" + String(n.rssi, 0) + ",\"ago\":" + (uint32_t)((nowMs() - n.at) / 1000) + "}";
  }
  xSemaphoreGiveRecursive(comsLock);
  s += "],\"history\":[";
  for (size_t i = 0; i < xhist.size(); i++) {
    auto& h = xhist[i];
    if (i) s += ",";
    s += String("{\"sent\":") + (h.sent ? "true" : "false") + ",\"ok\":" + (h.ok ? "true" : "false") + ",\"name\":\"" + jsonEsc(h.name) + "\",\"cat\":\"" + jsonEsc(h.cat) +
         "\",\"peer\":\"" + jsonEsc(h.peer) + "\",\"note\":\"" + jsonEsc(h.note) + "\",\"size\":" + h.size + ",\"secs\":" + h.secs + ",\"t\":" + String((double)h.at, 0) + "}";
  }
  return s + "]}";
}

static bool linked = false;
static uint32_t nowRetryAt = 0, radioRetryAt = 0;
static void comsTask(void*) {
  for (;;) {
    if (!enabled) {
      if (radioUp) radioStop();
      if (nowUp) nowStop();
      linked = false;
      vTaskDelay(pdMS_TO_TICKS(200)); continue;
    }
    if (!bandNow && nowUp) nowStop();
    if (bandNow && !nowUp && (int32_t)(millis() - nowRetryAt) >= 0) { nowUp = nowStart(); if (!nowUp) nowRetryAt = millis() + 5000; }
#ifndef SHELF_NO_RADIO
    if (!bandLora && radioUp) radioStop();
    if (bandLora && !radioUp && (int32_t)(millis() - radioRetryAt) >= 0) { radioUp = radioStart(); if (!radioUp) radioRetryAt = millis() + 5000; }
#endif
    if (!radioUp && !nowUp) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (!linked) {
      linked = true;
      syncAsks = 0; syncAskAt = millis() + 3000 + esp_random() % 2000;   // ask for the last 12 hours shortly after starting
      helloAt = millis() + 6000 + esp_random() % 6000;                    // then say hello to the mesh
    }
    if (radioUp && rxFlag) {
      rxFlag = false;
      uint8_t buf[256];
      size_t len = radio->getPacketLength();
      if (len > 0 && len <= sizeof buf && radio->readData(buf, len) == RADIOLIB_ERR_NONE)
        handlePacket(buf, len, radio->getRSSI(), radio->getSNR());
      radio->startReceive();
    }
    while (nowR != nowW) {                                 // 2.4 GHz: everything heard since last time
      NowPkt p;
      portENTER_CRITICAL(&nowMux); p = nowRing[nowR % NOW_RING]; nowR++; portEXIT_CRITICAL(&nowMux);
      nowHeardCount++;
      handlePacket(p.buf, p.len, 0, 0);                    // (0 dBm: heard over 2.4 GHz)
    }
    xferStep();
    if (X.phase == XF_S_SEND) continue;                    // (a window goes straight out, window after window)
    // just came on: ask the neighbors for the last 12 hours (again after 2 minutes, in case nobody heard)
    if (syncAsks < 2 && (int32_t)(millis() - syncAskAt) >= 0) {
      uint8_t q[256]; size_t n = 0;
      q[n++] = MAGIC0; q[n++] = MAGIC1; q[n++] = T_SYNC_REQ; memcpy(q + n, &myNode, 4); n += 4;
      std::vector<uint32_t> have;
      xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
      for (int i = (int)logRam.size() - 1; i >= 0 && have.size() < 40; i--) {
        int p = logRam[i].indexOf("\"id\":\""); if (p >= 0) have.push_back(strtoul(logRam[i].substring(p + 6, p + 14).c_str(), nullptr, 16));
      }
      xSemaphoreGiveRecursive(comsLock);
      q[n++] = have.size();
      for (uint32_t id : have) { memcpy(q + n, &id, 4); n += 4; }
      queueTx(q, n, 0);
      syncAsks++; syncAskAt = millis() + 120000;
    }
    // hello every 15 minutes (a minute of jitter), and when someone asks who's there
    if (helloAt && (int32_t)(millis() - helloAt) >= 0) { sendHello(false); helloAt = millis() + HELLO_EVERY_MS - 60000 + esp_random() % 120000; }
    if (whoAnswerAt && (int32_t)(millis() - whoAnswerAt) >= 0) { whoAnswerAt = 0; if (millis() - lastHelloSent > 60000) sendHello(true); }
    // answer someone's catch-up request, unless another board already started answering
    bool serve = false; SyncJob job;
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    if (syncJob.active && (int32_t)(millis() - syncJob.due) >= 0) { job = syncJob; syncJob.active = false; serve = true; }
    xSemaphoreGiveRecursive(comsLock);
    if (serve && !(lastReplayHeardAt && (int32_t)(lastReplayHeardAt - job.created) > 0)) {
      for (int i = 7; i > 0; i--) { lastServed[i] = lastServed[i - 1]; lastServedAt[i] = lastServedAt[i - 1]; }
      lastServed[0] = job.requester; lastServedAt[0] = millis();
      std::vector<String> pick;
      uint64_t now = nowMs(), cutoff = now > SYNC_WINDOW_S * 1000ULL ? now - SYNC_WINDOW_S * 1000ULL : 0;
      xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
      for (int i = (int)logRam.size() - 1; i >= 0 && (int)pick.size() < SYNC_MAX; i--) pick.push_back(logRam[i]);
      xSemaphoreGiveRecursive(comsLock);
      uint32_t spacing = 0;
      for (int i = (int)pick.size() - 1; i >= 0; i--) {            // oldest first
        JsonDocument e(&psAlloc);
        if (deserializeJson(e, pick[i])) continue;
        uint64_t t = e["t"] | 0ULL;
        if (t < cutoff || t > now + 60000) continue;
        uint32_t id = strtoul(e["id"] | "0", nullptr, 16), origin = strtoul(e["node"] | "0", nullptr, 16);
        if (origin == job.requester) continue;
        if (std::find(job.have.begin(), job.have.end(), id) != job.have.end()) continue;
        uint8_t out[256];
        size_t n = buildPacket(out, id, origin, 0, e["hops"] | 0, String(e["from"] | ""), String(e["text"] | ""), T_REPLAY, (uint32_t)((now - t) / 1000));
        queueTx(out, n, spacing); spacing += 1200;
      }
    }
    // send anything that's due, listening first so boards don't talk over each other
    Tx t; bool have = false;
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    for (size_t i = 0; i < txq.size(); i++) if ((int32_t)(millis() - txq[i].due) >= 0) { t = txq[i]; txq.erase(txq.begin() + i); have = true; break; }
    xSemaphoreGiveRecursive(comsLock);
    if (have && !t.nowSent) { nowSend(t.buf, t.len); t.nowSent = true; }   // 2.4 GHz goes straight out
    if (have && radioUp && loraFast) {                     // the radio's on the transfer link: 900 MHz waits
      xSemaphoreTakeRecursive(comsLock, portMAX_DELAY); t.due = millis() + 1000; txq.push_back(t); xSemaphoreGiveRecursive(comsLock);
      have = false;
    }
    if (have && radioUp) {
      int cad = radio->scanChannel();
      if (cad == RADIOLIB_LORA_DETECTED) {
        t.due = millis() + 300 + esp_random() % 900;
        xSemaphoreTakeRecursive(comsLock, portMAX_DELAY); txq.push_back(t); xSemaphoreGiveRecursive(comsLock);
      } else {
        radio->setOutputPower(CHIP_DBM[level]);
        femTx(true);
        int st = radio->transmit(t.buf, t.len);
        femTx(false);
        if (st != RADIOLIB_ERR_NONE) lastError = String("Send failed (code ") + st + ")";
      }
      rxFlag = false;
      radio->startReceive();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Deep sleep (main.cpp): radios off first
void comsSleep() { if (radioUp) radioStop(); if (nowUp) nowStop(); }

static bool comsAllowed = true;      // false: switched off in the memory switches (nothing loaded, no task)
void comsBegin(bool on) {
  comsLock = xSemaphoreCreateRecursiveMutex();
  cprefs.begin("coms", false);
  comsAllowed = on;
  uint64_t mac = ESP.getEfuseMac();
  (void)mac;
  myNode = meshNodeId();
  myName = cprefs.getString("name", hostName);
  level = constrain(cprefs.getInt("level", 1), 0, 2);
  enabled = cprefs.getBool("on", false);
  receiptsOn = cprefs.getBool("receipts", true);
  bandLora = cprefs.getBool("lora", true); bandNow = cprefs.getBool("now", true);
  xferAccept = cprefs.getBool("xaccept", true);
#ifdef SHELF_NO_RADIO
  bandNow = true;                                    // 2.4 GHz is all a plain ESP32 has
#endif
  if (!on) { enabled = false; Serial.println("[shelf] Meshed Comms is switched off (memory switch)"); return; }
  loadLog();
  for (auto& l : logRam) if (l.indexOf("\"mine\":true") >= 0) {     // my messages, so receipts match
    int p = l.indexOf("\"id\":\""); if (p >= 0) myIds.push_back(strtoul(l.substring(p + 6, p + 14).c_str(), nullptr, 16));
  }
  File rf = store->open(RECEIPT_FILE, FILE_READ);
  if (rf) {
    while (rf.available()) {
      String l = rf.readStringUntil('\n');
      JsonDocument d(&psAlloc);
      if (!deserializeJson(d, l)) noteReceipt(strtoul(d["m"] | "0", nullptr, 16), strtoul(d["n"] | "0", nullptr, 16), String(d["by"] | ""), false);
    }
    rf.close();
  }
  xTaskCreatePinnedToCore(comsTask, "coms", 8192, nullptr, 2, nullptr, 1);
}

// ------------------------------------------------------------ HTTP
bool handleComsApi(AsyncWebServerRequest* r, const String& url, WebRequestMethod m) {
  if (!url.startsWith("/api/coms")) return false;
  if (url == "/api/coms/files" && m == HTTP_GET) { sendJson(r, 200, xferJson()); return true; }
  if (url.startsWith("/api/coms/files/") && m == HTTP_POST) {
    JsonDocument b; parseBody(r, b);
    if (url == "/api/coms/files/send") {
      if (!enabled) { sendErr(r, 400, "Switch Meshed Comms on first."); return true; }
      if (xferBusy()) { sendErr(r, 409, "A file is already on its way. Wait for it, or stop it."); return true; }
      String node = b["node"] | "", cat = b["cat"] | "", name = b["name"] | "";
      if (!isCategory(cat) || !name.length() || name.indexOf('/') >= 0 || node.length() != 8) { sendErr(r, 400, "Pick a board and a file."); return true; }
      portENTER_CRITICAL(&xMux); xferPending = node + "\t" + cat + "\t" + name; portEXIT_CRITICAL(&xMux);
      X.status = "Getting ready..."; X.cancel = false;
      sendJson(r, 200, "{\"ok\":true}"); return true;
    }
    if (url == "/api/coms/files/cancel") { X.cancel = true; sendJson(r, 200, "{\"ok\":true}"); return true; }
    if (url == "/api/coms/files/settings") {
      if (b["accept"].is<bool>()) { xferAccept = b["accept"].as<bool>(); cprefs.putBool("xaccept", xferAccept); }
      sendJson(r, 200, xferJson()); return true;
    }
  }
  if (url == "/api/coms" && m == HTTP_GET) {
    String s = String("{\"enabled\":") + (enabled ? "true" : "false") + ",\"radio\":" + ((radioUp || nowUp) ? "true" : "false") +
      ",\"lora\":" + (radioUp ? "true" : "false") + ",\"wifi\":" + (nowUp ? "true" : "false") + ",\"bandLora\":" + (bandLora ? "true" : "false") + ",\"bandWifi\":" + (bandNow ? "true" : "false") + ",\"channel\":" + wifiChannel() +
#ifdef SHELF_NO_RADIO
      ",\"hasLora\":false" +
#else
      ",\"hasLora\":true" +
#endif
      ",\"name\":\"" + jsonEsc(myName) + "\",\"node\":\"" + hex8(myNode) + "\",\"level\":" + level +
      ",\"receipts\":" + (receiptsOn ? "true" : "false") + ",\"levels\":" + LEVELS_DBM + ",\"freq\":915.0,\"heard\":" + heardCount + ",\"relayed\":" + relayedCount + ",\"sent\":" + sentCount + ",\"synced\":" + syncedCount +
      ",\"error\":\"" + jsonEsc(lastError) + "\",\"neighbors\":[";
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    bool first = true;
    for (auto& n : neighbors) {
      if (nowMs() - n.at > 30ULL * 60 * 1000) continue;
      if (!first) s += ","; first = false;
      s += "{\"node\":\"" + hex8(n.node) + "\",\"name\":\"" + jsonEsc(n.name) + "\",\"rssi\":" + String(n.rssi, 0) + ",\"snr\":" + String(n.snr, 1) +
           ",\"at\":" + String((double)n.at, 0) + "}";
    }
    s += "],\"devices\":[";
    first = true;
    for (auto& d : members) {
      if (nowMs() - d.at > 24ULL * 3600 * 1000) continue;
      if (!first) s += ","; first = false;
      s += "{\"node\":\"" + hex8(d.node) + "\",\"name\":\"" + jsonEsc(d.name) + "\",\"board\":" + d.board + ",\"batt\":" + (d.batt == 255 ? -1 : (int)d.batt) +
           ",\"admin\":" + ((d.flags & 1) ? "true" : "false") + ",\"ro\":" + ((d.flags & 2) ? "true" : "false") + ",\"hops\":" + d.hops +
           ",\"rssi\":" + String(d.rssi, 0) + ",\"at\":" + String((double)d.at, 0) + "}";
    }
    s += "],\"log\":[";
    for (size_t i = 0; i < logRam.size(); i++) { if (i) s += ","; s += logRam[i]; }
    s += "],\"receivedBy\":{";           // message id -> who said "received"
    std::vector<uint32_t> done;
    for (auto& rc : receipts) {
      if (std::find(done.begin(), done.end(), rc.msg) != done.end()) continue;
      done.push_back(rc.msg);
      if (done.size() > 1) s += ",";
      s += "\"" + hex8(rc.msg) + "\":[";
      bool f2 = true;
      for (auto& x : receipts) if (x.msg == rc.msg) { if (!f2) s += ","; s += "\"" + jsonEsc(x.name.length() ? x.name : hex8(x.node)) + "\""; f2 = false; }
      s += "]";
    }
    xSemaphoreGiveRecursive(comsLock);
    sendJson(r, 200, s + "}}");
    return true;
  }
  if (url == "/api/coms/remote/result") {
    uint32_t req = strtoul(r->hasParam("req") ? r->getParam("req")->value().c_str() : "0", nullptr, 16);
    String out;
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
    for (auto& p : pendings) {
      if (p.handle != req) continue;
      if (p.state == 0 && millis() - p.sentAt > 90000) { p.state = 2; p.status = 255; }
      static const char* const WHY[] = {"", "Wrong admin password for that board.", "It didn't accept the command counter. Try again.",
                                        "That board doesn't know this command. Update its firmware.",
                                        "That board has no admin password yet. Set one on its settings page first.",
                                        "That board had too many wrong passwords. Try again in 5 minutes."};
      String err = p.state != 2 ? String("") : p.status == 255 ? String("No answer after 90 seconds. It may be out of range, or its Coms may be off.")
                                                               : String(p.status < 6 ? WHY[p.status] : "It said no.");
      bool js = p.data.startsWith("{");
      out = String("{\"state\":\"") + (p.state == 0 ? "waiting" : p.state == 1 ? "done" : "failed") + "\",\"cmd\":" + p.cmd +
            ",\"secs\":" + ((millis() - p.sentAt) / 1000) + ",\"error\":\"" + jsonEsc(err) + "\"" +
            (p.state == 1 ? (js ? ",\"status\":" + p.data : ",\"text\":\"" + jsonEsc(p.data) + "\"") : String("")) + "}";
      break;
    }
    xSemaphoreGiveRecursive(comsLock);
    if (!out.length()) { sendErr(r, 404, "That request isn't known any more."); return true; }
    sendJson(r, 200, out);
    return true;
  }
  if (m != HTTP_POST) { sendErr(r, 405, "Use POST"); return true; }
  JsonDocument b(&psAlloc); parseBody(r, b);
  if (url.startsWith("/api/coms/remote/")) {
    if (!enabled || (!radioUp && !nowUp)) { sendErr(r, 409, "Turn Coms on first."); return true; }
    if (url == "/api/coms/remote/who") {
      uint8_t q[32]; uint32_t id = esp_random(); markSeen(id);
      size_t n = header(q, T_WHO, id);
      queueTx(q, n, 0);
      if (millis() - lastHelloSent > 60000) whoAnswerAt = millis() + 2000;
      sendJson(r, 200, "{\"ok\":true}");
      return true;
    }
    auto startCmd = [&](Session& se, uint8_t cmd, const String& arg) {
      Pending p; p.req = esp_random(); p.handle = p.req; p.node = se.node; p.cmd = cmd; p.arg = arg; memcpy(p.key, se.key, 32);
      p.counter = adminNextOwnCounter(0); p.state = 0; p.status = 0; p.retried = false; p.sentAt = millis();
      xSemaphoreTakeRecursive(comsLock, portMAX_DELAY);
      if (pendings.size() >= 16) pendings.erase(pendings.begin());
      pendings.push_back(p);
      sendAdminReq(pendings.back());
      xSemaphoreGiveRecursive(comsLock);
      se.at = millis();
      return p.req;
    };
    if (url == "/api/coms/remote/connect") {
      uint32_t node = strtoul(b["node"] | "0", nullptr, 16);
      String pw = b["password"] | "";
      if (!node || node == myNode) { sendErr(r, 400, "Pick another board from the list."); return true; }
      if (!pw.length()) { sendErr(r, 400, "Type that board's admin password."); return true; }
      Session se; se.node = node; se.at = millis(); se.id = uuid4(); se.id.replace("-", "");
      adminDeriveKey(pw, node, se.key);
      for (auto it = sessions.begin(); it != sessions.end();) it = (millis() - it->at > 3600000UL) ? sessions.erase(it) : it + 1;
      if (sessions.size() >= 8) sessions.erase(sessions.begin());
      sessions.push_back(se);
      uint32_t req = startCmd(sessions.back(), C_STATUS, "");
      sendJson(r, 200, "{\"session\":\"" + se.id + "\",\"req\":\"" + hex8(req) + "\"}");
      return true;
    }
    String sid = b["session"] | "";
    Session* se = nullptr; for (auto& x : sessions) if (x.id == sid) se = &x;
    if (url == "/api/coms/remote/disconnect") {
      for (auto it = sessions.begin(); it != sessions.end(); ++it) if (it->id == sid) { sessions.erase(it); break; }
      sendJson(r, 200, "{\"ok\":true}");
      return true;
    }
    if (!se) { sendErr(r, 401, "Connect again: that session ended."); return true; }
    if (url == "/api/coms/remote/cmd") {
      int cmd = b["cmd"] | 0;
      if (cmd < 1 || cmd > 11) { sendErr(r, 400, "Unknown command"); return true; }
      uint32_t req = startCmd(*se, (uint8_t)cmd, String(b["arg"] | ""));
      sendJson(r, 200, "{\"req\":\"" + hex8(req) + "\"}");
      return true;
    }
    sendErr(r, 404, "Not found");
    return true;
  }
  if (url == "/api/coms/toggle") {
    if (!comsAllowed) { sendErr(r, 409, "Meshed Comms is switched off to save memory. Turn it on in the board's settings (Memory), then restart."); return true; }
    enabled = b["on"] | false;
    cprefs.putBool("on", enabled);
    sendJson(r, 200, String("{\"enabled\":") + (enabled ? "true" : "false") + "}");
    return true;
  }
  if (url == "/api/coms/send") {
    if (!enabled) { sendErr(r, 409, "Turn Coms on first."); return true; }
    String text = b["text"] | ""; text.trim();
    if (!text.length()) { sendErr(r, 400, "Type a message first."); return true; }
    if (text.length() > MAX_TEXT) { sendErr(r, 400, "That's too long for one radio message (180 characters max)."); return true; }
    uint32_t id = esp_random();
    markSeen(id);
    myIds.push_back(id); if (myIds.size() > 400) myIds.erase(myIds.begin());
    uint8_t out[256];
    size_t n = buildPacket(out, id, myNode, HOPS, 0, myName, text);
    queueTx(out, n, 0);
    sentCount++;
    String e = entryJson(id, myNode, myName, text, 0, true, 0, 0);
    addLog(e);
    sendJson(r, 200, e);
    return true;
  }
  if (url == "/api/coms/settings") {
    String nm = b["name"] | ""; nm.trim();
    if (nm.length()) { myName = nm.substring(0, MAX_NAME); cprefs.putString("name", myName); }
    if (b["level"].is<int>()) { level = constrain(b["level"].as<int>(), 0, 2); cprefs.putInt("level", level); }
    if (b["receipts"].is<bool>()) { receiptsOn = b["receipts"].as<bool>(); cprefs.putBool("receipts", receiptsOn); }
#ifndef SHELF_NO_RADIO
    if (b["lora"].is<bool>() || b["wifi"].is<bool>()) {             // the two bands, each on or off
      bool lo = b["lora"].is<bool>() ? b["lora"].as<bool>() : (bool)bandLora, wi = b["wifi"].is<bool>() ? b["wifi"].as<bool>() : (bool)bandNow;
      if (!lo && !wi) { sendErr(r, 400, "Keep one band on, or switch Meshed Comms off."); return true; }
      bandLora = lo; bandNow = wi; cprefs.putBool("lora", lo); cprefs.putBool("now", wi);
    }
#endif
    sendJson(r, 200, "{\"ok\":true,\"name\":\"" + jsonEsc(myName) + "\",\"level\":" + level + "}");
    return true;
  }
  if (url == "/api/coms/clear") {
    xSemaphoreTakeRecursive(comsLock, portMAX_DELAY); logRam.clear(); receipts.clear(); xSemaphoreGiveRecursive(comsLock);
    store->remove(LOG_FILE); store->remove(RECEIPT_FILE);
    sendJson(r, 200, "{\"ok\":true}");
    return true;
  }
  return false;
}
