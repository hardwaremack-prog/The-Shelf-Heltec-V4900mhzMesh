// The board's own LED as a status light: the blue LED on a plain ESP32 (GPIO 2 on NodeMCU-32S /
// DevKitC boards; the red one next to it is the power light), the white LED on a Heltec
// (GPIO 35 on the V4 and V3, GPIO 46 on the V4-R8, whose GPIO 35 belongs to its PSRAM).
// The Heltec's orange light is its battery charger's and can't be switched.
//   starting up           slow blink (on half a second, off half a second)
//   web page ready        breathing: a slow, smooth fade in and out
//   microSD card busy     a quick flicker for as long as the card is being read or written
//   updating              fast flash (firmware or web app being written)
#include "shelf.h"

#if defined(SHELF_ESP32)
#define LED_PIN 2
#define LED_MAX 255
#elif CONFIG_SPIRAM_MODE_OCT
#define LED_PIN 46
#define LED_MAX 160               // the Heltec's white LED is bright: a little gentler
#else
#define LED_PIN 35
#define LED_MAX 160
#endif
#define LED_CH  6                 // an LEDC channel nothing else uses
extern "C" volatile uint32_t sdActivityMs;   // libraries/SD/src/sd_diskio.cpp (SPI card), sd_mmc_activity.cpp (SD/MMC)
static volatile bool webUp = false;

static void ledTask(void*) {
  for (;;) {
    uint32_t now = millis();
    uint32_t duty;
    if (otaPercent >= 0) duty = (now / 70) % 2 ? LED_MAX : 0;                         // updating: fast flash
    else if (!webUp) duty = (now / 500) % 2 ? LED_MAX : 0;                            // starting: slow blink
    else if (sdActivityMs && now - sdActivityMs < 90) duty = (now / 35) % 2 ? LED_MAX : 0;   // card busy: flicker
    else {                                                                         // ready: breathing, ~4 s a breath
      float t = (now % 4000) / 4000.0f;
      float v = 0.5f - 0.5f * cosf(t * 2 * PI);
      duty = (uint32_t)(4 + v * v * (LED_MAX - 4));                                          // squared: looks even to the eye
    }
    ledcWrite(LED_CH, duty);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

void ledBegin() {
  gpio_hold_dis((gpio_num_t)LED_PIN);       // (held off through a deep sleep)
  ledcSetup(LED_CH, 5000, 8);
  ledcAttachPin(LED_PIN, LED_CH);
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 1);
}
void ledWebUp() { webUp = true; }
void ledOff() {                             // deep sleep: dark, and held dark while asleep
  ledcDetachPin(LED_PIN);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
  gpio_hold_en((gpio_num_t)LED_PIN);
  gpio_deep_sleep_hold_en();
}
