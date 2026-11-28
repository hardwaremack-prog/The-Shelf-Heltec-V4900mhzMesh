# Fast network build (Heltec V4 MMC only)

Arduino-ESP32 2.0.17 ships its network stack (lwIP) prebuilt with small TCP buffers: about
6 KB may be in flight per connection, which held one upload to ~7 Mbps and four at once to
~9 Mbps. These files rebuild that one library with bigger buffers. `build.sh` puts them into
the core only while it builds `shelf-v4-mmc`, then puts the originals back, so every other
board keeps the stock settings.

| | Stock | Here |
|---|---|---|
| TCP receive window per connection | 5,760 B (4 × MSS) | 22,976 B (16 × MSS) |
| TCP send buffer per connection | 5,760 B | 11,488 B (8 × MSS) |
| tcpip task mailbox | 32 | 64 |
| Wi-Fi RX buffers: static (internal RAM) / dynamic (PSRAM) | 8 / 32 | 10 / 64 |
| Wi-Fi RX / TX block-ack window | 16 / 6 | 20 / 12 |
| Wi-Fi TX buffers | 8 static | 64 dynamic (PSRAM) |

- `liblwip.a`: IDF v4.4.7's lwIP (esp-lwip a45be9e), the same 87 objects as the shipped
  `tools/sdk/esp32s3/lib/liblwip.a`, built with `sdkconfig.h` below. Its exported symbols are
  identical to the stock library; only tcp.c, tcp_in.c, tcp_out.c, tcpip.c and api_msg.c differ.
- `sdkconfig.h`: the core's `qio_qspi/include/sdkconfig.h` with the values above, plus
  `SHELF_FASTNET`. The Wi-Fi counts take effect because the firmware calls
  `WiFi.useStaticBuffers(true)` when `SHELF_FASTNET` is set (otherwise Arduino substitutes
  its own fixed counts).
- `build_lwip.sh`: rebuilds `liblwip.a` (fetches IDF v4.4.7's lwIP the first time). Run with
  the core's stock `sdkconfig.h` instead and every object comes out identical to the shipped one.
