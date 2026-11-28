// Heltec V4 MMC: lets the status light flicker while the SD/MMC card is read or written, as the
// SPI builds do through libraries/SD. build.sh links this build with
//   -Wl,--wrap=sdmmc_read_sectors -Wl,--wrap=sdmmc_write_sectors
// so every sector the card driver moves passes through here first.
#if defined(SHELF_MMC)
#include <Arduino.h>
#include <sdmmc_cmd.h>

extern "C" {
extern volatile uint32_t sdActivityMs;    // libraries/SD/src/sd_diskio.cpp
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start, size_t n);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start, size_t n);

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start, size_t n) {
  sdActivityMs = millis();
  esp_err_t r = __real_sdmmc_read_sectors(card, dst, start, n);
  sdActivityMs = millis();
  return r;
}
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start, size_t n) {
  sdActivityMs = millis();
  esp_err_t r = __real_sdmmc_write_sectors(card, src, start, n);
  sdActivityMs = millis();
  return r;
}
}
#endif
