// The Shelf for Heltec V4 - card writer in the background (Heltec V4 MMC only).
//
// The speed test showed the card itself saves ~6 MB/s, far more than Wi-Fi brings in (~1 MB/s),
// yet saving still cut uploads from 9 to under 7 Mbps: every write was done on the network
// task, and while it waited on the card no connection was read, so the senders slowed down.
// Here the network task only copies incoming bytes into 16 KB blocks from a pool in PSRAM and
// queues them; a task on the other core writes them to the card (through a small internal
// buffer: the card's DMA can't read PSRAM). A request waits only at its very end, for its own
// blocks to be on the card, before it answers.
//
// Used by Big Transfer chunks (bigfiles.cpp) and the speed test's "saving to card" uploads.
#include "shelf.h"

#if defined(SHELF_MMC)
#include <esp_heap_caps.h>
#include <freertos/queue.h>

#define PIPE_BLOCKS 40                 // most blocks there can be (PSRAM pool: 40 x 16 KB = 640 KB)
#define PIPE_FAST_MAX 12               // internal-RAM pool: up to 12 x 16 KB...
#define PIPE_FAST_MIN 8                // ...and at least 8 (4 connections each filling one, plus a queue)
#define PIPE_KEEP_FREE (48 * 1024)     // internal DMA memory left for Wi-Fi and everything else
#define PIPE_SLOTS  8                  // requests writing at once
#define SPEED_FILE  TMP_ROOT "/speed/pipe.bin"

enum : uint8_t { J_BIG, J_MARK, J_SPEED, J_SPEED_CLOSE };
struct Job { uint8_t kind, slot; uint32_t len; uint64_t off; uint8_t* blk; };
struct Slot {
  bool used;
  uint8_t target;                    // PIPE_BIG or PIPE_SPEED
  char id[17];                       // Big Transfer upload id
  uint64_t pos;                      // where the next block goes
  uint8_t* blk; size_t fill;         // the block being filled (network task only)
  volatile int pending;              // queued jobs not yet done (writer decrements)
  volatile bool err;
  uint32_t last;
};
static Slot slots[PIPE_SLOTS];
static QueueHandle_t jobQ = nullptr, freeQ = nullptr;
static uint8_t* bounce = nullptr;
static File speedF;                    // writer task only
// What the speed test reports (reset at its start): did the writer run, and who waited on whom
static volatile uint32_t stTakeWaitMs = 0, stFinishWaitMs = 0, stWriteMs = 0, stBlocks = 0, stMaxQueued = 0, stT0 = 0;
static bool pipeFailed = false;
static bool poolFast = false;          // blocks in internal RAM: the card's DMA reads them directly
static int poolBlocks = 0;
static volatile int speedActive = 0;

bool bigPipeWrite(const char* id, uint64_t off, const uint8_t* buf, size_t n);   // bigfiles.cpp
bool bigPipeMark(const char* id, uint64_t off);

static void writerTask(void*) {
  Job j;
  for (;;) {
    if (xQueueReceive(jobQ, &j, portMAX_DELAY) != pdTRUE) continue;
    Slot& s = slots[j.slot];
    bool ok = true;
    switch (j.kind) {
      case J_BIG: {
        uint32_t q = uxQueueMessagesWaiting(jobQ) + 1; if (q > stMaxQueued) stMaxQueued = q;
        uint32_t t = millis();
        const uint8_t* src = poolFast ? j.blk : (memcpy(bounce, j.blk, j.len), bounce);
        ok = bigPipeWrite(s.id, j.off, src, j.len);
        stWriteMs += millis() - t; stBlocks++;
        break; }
      case J_MARK:
        ok = bigPipeMark(s.id, j.off);
        break;
      case J_SPEED: {
        uint32_t q = uxQueueMessagesWaiting(jobQ) + 1; if (q > stMaxQueued) stMaxQueued = q;
        uint32_t t = millis();
        const uint8_t* src = poolFast ? j.blk : (memcpy(bounce, j.blk, j.len), bounce);
        if (!speedF) { mkdirs(TMP_ROOT "/speed"); speedF = store->open(SPEED_FILE, FILE_WRITE); }
        ok = speedF && speedF.write(src, j.len) == j.len;
        stWriteMs += millis() - t; stBlocks++;
        break; }
      case J_SPEED_CLOSE:
        if (speedActive == 0 && speedF) { speedF.close(); store->remove(SPEED_FILE); }
        break;
    }
    if (j.blk) xQueueSend(freeQ, &j.blk, portMAX_DELAY);
    if (!ok) s.err = true;
    __atomic_sub_fetch(&s.pending, 1, __ATOMIC_SEQ_CST);
  }
}

static bool pipeStart() {
  if (jobQ) return true;
  bool& failed = pipeFailed;
  if (failed) return false;
  QueueHandle_t fq = xQueueCreate(PIPE_BLOCKS, sizeof(uint8_t*));
  QueueHandle_t jq = xQueueCreate(PIPE_BLOCKS + PIPE_SLOTS * 2 + 4, sizeof(Job));
  if (!fq || !jq) { failed = true; return false; }
  // First choice: a few blocks in internal RAM. The writer peaks at ~4 queued blocks, and the
  // card's DMA reads internal RAM directly, so nothing is copied through PSRAM, which the
  // Wi-Fi's own buffers also live in.
  uint8_t* fast[PIPE_FAST_MAX]; int nf = 0;
  while (nf < PIPE_FAST_MAX && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) > PIPE_KEEP_FREE + UPLOAD_BUF) {
    uint8_t* b = (uint8_t*)heap_caps_malloc(UPLOAD_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!b) break;
    fast[nf++] = b;
  }
  if (nf >= PIPE_FAST_MIN) {
    for (int i = 0; i < nf; i++) xQueueSend(fq, &fast[i], 0);
    poolFast = true; poolBlocks = nf;
  } else {
    for (int i = 0; i < nf; i++) free(fast[i]);
    uint8_t* pool = (uint8_t*)heap_caps_malloc(PIPE_BLOCKS * UPLOAD_BUF, MALLOC_CAP_SPIRAM);
    bounce = (uint8_t*)heap_caps_malloc(UPLOAD_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!pool || !bounce) { failed = true; Serial.println("[shelf] card writer: not enough memory, writing directly"); return false; }
    for (int i = 0; i < PIPE_BLOCKS; i++) { uint8_t* b = pool + i * UPLOAD_BUF; xQueueSend(fq, &b, 0); }
    poolFast = false; poolBlocks = PIPE_BLOCKS;
  }
  Serial.printf("[shelf] card writer: %d x 16 KB blocks in %s\n", poolBlocks, poolFast ? "internal RAM" : "PSRAM");
  freeQ = fq; jobQ = jq;
  xTaskCreatePinnedToCore(writerTask, "cardwriter", 10240, nullptr, 3, nullptr, 1);   // network is on core 0
  return true;
}

static bool enqueue(int slot, uint8_t kind, uint64_t off, uint8_t* blk, size_t len) {
  Job j{kind, (uint8_t)slot, (uint32_t)len, off, blk};
  __atomic_add_fetch(&slots[slot].pending, 1, __ATOMIC_SEQ_CST);
  if (xQueueSend(jobQ, &j, pdMS_TO_TICKS(10000)) != pdTRUE) {
    __atomic_sub_fetch(&slots[slot].pending, 1, __ATOMIC_SEQ_CST);
    if (blk) xQueueSend(freeQ, &blk, 0);
    return false;
  }
  return true;
}
static bool takeBlock(Slot& s) {
  if (s.blk) return true;
  // all blocks waiting for the card: wait for one (the card is ~6x faster than Wi-Fi, so it's brief)
  uint32_t t = millis();
  bool got = xQueueReceive(freeQ, &s.blk, pdMS_TO_TICKS(10000)) == pdTRUE;
  stTakeWaitMs += millis() - t;
  if (!got) { s.blk = nullptr; return false; }
  s.fill = 0;
  return true;
}

int pipeOpen(uint8_t target, const char* id, uint64_t off) {
  if (!pipeStart()) return -1;
  uint32_t now = millis();
  for (int i = 0; i < PIPE_SLOTS; i++) {           // a slot left by a request whose connection died
    Slot& s = slots[i];
    if (s.used && s.pending == 0 && now - s.last > 45000) {
      if (s.blk) { xQueueSend(freeQ, &s.blk, 0); s.blk = nullptr; }
      if (s.target == PIPE_SPEED) __atomic_sub_fetch(&speedActive, 1, __ATOMIC_SEQ_CST);
      s.used = false;
    }
  }
  for (int i = 0; i < PIPE_SLOTS; i++) {
    Slot& s = slots[i];
    if (s.used) continue;
    s.used = true; s.target = target; s.pos = off; s.blk = nullptr; s.fill = 0; s.pending = 0; s.err = false; s.last = now;
    strlcpy(s.id, id ? id : "", sizeof s.id);
    if (target == PIPE_SPEED) __atomic_add_fetch(&speedActive, 1, __ATOMIC_SEQ_CST);
    return i;
  }
  return -1;
}

bool pipeFeed(int slot, const uint8_t* data, size_t len) {
  Slot& s = slots[slot];
  s.last = millis();
  if (s.err) return false;
  while (len) {
    if (!takeBlock(s)) { s.err = true; return false; }
    size_t n = std::min(len, (size_t)UPLOAD_BUF - s.fill);
    memcpy(s.blk + s.fill, data, n); s.fill += n; data += n; len -= n;
    if (s.fill == UPLOAD_BUF) {
      uint8_t* b = s.blk; s.blk = nullptr;
      if (!enqueue(slot, s.target == PIPE_BIG ? J_BIG : J_SPEED, s.pos, b, UPLOAD_BUF)) { s.err = true; return false; }
      s.pos += UPLOAD_BUF;
    }
  }
  return true;
}

// Queues what's left of the request's bytes (and, for Big Transfer, marking its chunk done).
bool pipeEnd(int slot, bool markDone, uint64_t chunkOff) {
  Slot& s = slots[slot];
  if (s.blk) {
    uint8_t* b = s.blk; size_t n = s.fill; s.blk = nullptr;
    if (n) { if (!enqueue(slot, s.target == PIPE_BIG ? J_BIG : J_SPEED, s.pos, b, n)) s.err = true; else s.pos += n; }
    else xQueueSend(freeQ, &b, 0);
  }
  if (markDone && !s.err && !enqueue(slot, J_MARK, chunkOff, nullptr, 0)) s.err = true;
  return !s.err;
}

// Waits until the request's blocks are on the card, then frees its slot. True if all went well.
bool pipeFinish(int slot, uint32_t timeoutMs) {
  Slot& s = slots[slot];
  uint32_t t0 = millis();
  while (s.pending > 0 && millis() - t0 < timeoutMs) vTaskDelay(1);
  stFinishWaitMs += millis() - t0;
  bool ok = s.pending == 0 && !s.err;
  if (s.pending > 0) { s.last = millis(); return false; }   // still busy: the stale sweep frees it later
  if (s.blk) { xQueueSend(freeQ, &s.blk, 0); s.blk = nullptr; }
  if (s.target == PIPE_SPEED && __atomic_sub_fetch(&speedActive, 1, __ATOMIC_SEQ_CST) == 0) {
    Job j{J_SPEED_CLOSE, (uint8_t)slot, 0, 0, nullptr};
    __atomic_add_fetch(&s.pending, 1, __ATOMIC_SEQ_CST);
    if (xQueueSend(jobQ, &j, pdMS_TO_TICKS(1000)) != pdTRUE) __atomic_sub_fetch(&s.pending, 1, __ATOMIC_SEQ_CST);
    // (the close job only touches pending, so the slot can be reused once it's back to 0)
    t0 = millis();
    while (s.pending > 0 && millis() - t0 < 2000) vTaskDelay(1);
  }
  s.used = false;
  return ok;
}

void pipeStatsReset() { stTakeWaitMs = stFinishWaitMs = stWriteMs = stBlocks = stMaxQueued = 0; stT0 = millis(); pipeStart(); }
String pipeStatsJson() {
  return String("{\"on\":") + (jobQ ? "true" : "false") + ",\"failed\":" + (pipeFailed ? "true" : "false") +
         ",\"blocks\":" + stBlocks + ",\"writeMs\":" + stWriteMs + ",\"takeWaitMs\":" + stTakeWaitMs +
         ",\"finishWaitMs\":" + stFinishWaitMs + ",\"maxQueued\":" + stMaxQueued + ",\"ms\":" + (millis() - stT0) +
         ",\"pool\":\"" + (poolFast ? "internal" : "psram") + "\",\"poolBlocks\":" + poolBlocks +
         ",\"freePsram\":" + ESP.getFreePsram() + ",\"freeDma\":" + heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) + "}";
}
#else
void pipeStatsReset() {}
String pipeStatsJson() { return ""; }
#endif
