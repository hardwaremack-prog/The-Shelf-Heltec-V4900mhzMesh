#!/usr/bin/env bash
# Rebuilds Arduino-ESP32 2.0.17's liblwip.a (ESP32-S3) from IDF v4.4.7's lwip with a given sdkconfig.h
# usage: fastnet/build_lwip.sh [sdkconfig-dir] [out.a]      (defaults: fastnet/ and fastnet/liblwip.a)
# CORE=<arduino-esp32 2.0.17 folder> if it isn't ~/Arduino/hardware/espressif/esp32
# Checked against the shipped library: with the core's own sdkconfig.h this rebuilds every
# object with identical machine code, so the only differences are the settings changed on purpose.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CFG=${1:-$HERE}; OUT=${2:-$HERE/liblwip.a}; OUT=$(readlink -f "$(dirname "$OUT")")/$(basename "$OUT")
P=${CORE:-$HOME/Arduino/hardware/espressif/esp32}; SDK=$P/tools/sdk/esp32s3
IDF=$HERE/.idf; L=$IDF/components/lwip
if [ ! -d "$L/lwip/src" ]; then   # IDF v4.4.7's lwip (what Arduino-ESP32 2.0.17 is built from)
  rm -rf "$IDF"
  git clone -q --depth 1 --branch v4.4.7 --filter=blob:none --sparse https://github.com/espressif/esp-idf.git "$IDF"
  git -C "$IDF" sparse-checkout set components/lwip
  rm -rf "$L/lwip"; git clone -q https://github.com/espressif/esp-lwip.git "$L/lwip"
  git -C "$L/lwip" checkout -q a45be9e438f6cf9c54ec150581819c3b95d5af6b
fi
CC=$P/tools/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-gcc
AR=$P/tools/xtensa-esp32s3-elf/bin/xtensa-esp32s3-elf-ar
INC=$(sed -n 's/^compiler.cpreprocessor.flags.esp32s3=//p' $P/platform.txt | sed "s#{compiler.sdk.path}#$SDK#g" | tr -d '"')
# our lwip sources' own headers come first (same files as the shipped ones), then the sdkconfig dir
INC="-I$L/include/apps -I$L/include/apps/sntp -I$L/lwip/src/include -I$L/port/esp32/include -I$L/port/esp32/include/arch $INC -I$CFG"
CFLAGS="-mlongcalls -ffunction-sections -fdata-sections -Wno-error=unused-function -Wno-error=unused-variable -Wno-error=deprecated-declarations -Wno-unused-parameter -Wno-sign-compare -ggdb -Os -freorder-blocks -Wwrite-strings -fstack-protector -fstrict-volatile-bitfields -Wno-error=unused-but-set-variable -fno-jump-tables -fno-tree-switch-conversion -std=gnu99 -Wno-old-style-declaration -Wno-address -w"
SRCS="apps/sntp/sntp.c lwip/src/api/api_lib.c lwip/src/api/api_msg.c lwip/src/api/err.c lwip/src/api/if_api.c lwip/src/api/netbuf.c lwip/src/api/netdb.c lwip/src/api/netifapi.c lwip/src/api/sockets.c lwip/src/api/tcpip.c lwip/src/apps/sntp/sntp.c lwip/src/apps/netbiosns/netbiosns.c lwip/src/core/def.c lwip/src/core/dns.c lwip/src/core/inet_chksum.c lwip/src/core/init.c lwip/src/core/ip.c lwip/src/core/mem.c lwip/src/core/memp.c lwip/src/core/netif.c lwip/src/core/pbuf.c lwip/src/core/raw.c lwip/src/core/stats.c lwip/src/core/sys.c lwip/src/core/tcp.c lwip/src/core/tcp_in.c lwip/src/core/tcp_out.c lwip/src/core/timeouts.c lwip/src/core/udp.c lwip/src/core/ipv4/autoip.c lwip/src/core/ipv4/dhcp.c lwip/src/core/ipv4/etharp.c lwip/src/core/ipv4/icmp.c lwip/src/core/ipv4/igmp.c lwip/src/core/ipv4/ip4.c lwip/src/core/ipv4/ip4_napt.c lwip/src/core/ipv4/ip4_addr.c lwip/src/core/ipv4/ip4_frag.c lwip/src/core/ipv6/dhcp6.c lwip/src/core/ipv6/ethip6.c lwip/src/core/ipv6/icmp6.c lwip/src/core/ipv6/inet6.c lwip/src/core/ipv6/ip6.c lwip/src/core/ipv6/ip6_addr.c lwip/src/core/ipv6/ip6_frag.c lwip/src/core/ipv6/mld6.c lwip/src/core/ipv6/nd6.c lwip/src/netif/ethernet.c lwip/src/netif/lowpan6.c lwip/src/netif/slipif.c lwip/src/netif/ppp/auth.c lwip/src/netif/ppp/ccp.c lwip/src/netif/ppp/chap-md5.c lwip/src/netif/ppp/chap-new.c lwip/src/netif/ppp/chap_ms.c lwip/src/netif/ppp/demand.c lwip/src/netif/ppp/eap.c lwip/src/netif/ppp/ecp.c lwip/src/netif/ppp/eui64.c lwip/src/netif/ppp/fsm.c lwip/src/netif/ppp/ipcp.c lwip/src/netif/ppp/ipv6cp.c lwip/src/netif/ppp/lcp.c lwip/src/netif/ppp/magic.c lwip/src/netif/ppp/mppe.c lwip/src/netif/ppp/multilink.c lwip/src/netif/ppp/ppp.c lwip/src/netif/ppp/pppapi.c lwip/src/netif/ppp/pppcrypt.c lwip/src/netif/ppp/pppoe.c lwip/src/netif/ppp/pppol2tp.c lwip/src/netif/ppp/pppos.c lwip/src/netif/ppp/upap.c lwip/src/netif/ppp/utils.c lwip/src/netif/ppp/vj.c port/esp32/hooks/tcp_isn_default.c port/esp32/hooks/lwip_default_hooks.c port/esp32/debug/lwip_debug.c port/esp32/freertos/sys_arch.c port/esp32/netif/dhcp_state.c port/esp32/netif/wlanif.c port/esp32/netif/ethernetif.c port/esp32/vfs_lwip.c apps/ping/esp_ping.c apps/ping/ping.c apps/ping/ping_sock.c apps/dhcpserver/dhcpserver.c"
W=$(mktemp -d); rm -f "$OUT"
i=0
for s in $SRCS; do
  i=$((i+1)); d=$W/$i; mkdir -p $d
  $CC $CFLAGS -fmacro-prefix-map=$IDF=IDF $INC -DESP_PLATFORM -DIDF_VER=\"v4.4.7-dirty\" -D_GNU_SOURCE -D_POSIX_READER_WRITER_LOCKS -DHAVE_CONFIG_H -c $L/$s -o $d/$(basename $s).obj &
  if (( i % 8 == 0 )); then wait; fi
done
wait
i=0
for s in $SRCS; do i=$((i+1)); $AR q "$OUT" $W/$i/$(basename $s).obj; done
echo "built $(ar t $OUT | wc -l) objects -> $OUT"
