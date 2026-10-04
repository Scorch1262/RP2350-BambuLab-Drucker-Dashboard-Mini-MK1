#!/bin/sh
# Baut die Firmware mit arduino-cli (arduino-pico >= 6.2 muss installiert sein).
# Ergebnis: build/DruckerDashboardRP2350-MK1.ino.uf2 (+ .bin fuer das Web-Update)
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_web.py
FQBN="rp2040:rp2040:rpipico2:os=freertos,flash=4194304_1048576,ipbtstack=ipv4onlybig,freq=150"
[ -n "$DD_FQBN" ] && FQBN="$DD_FQBN"
arduino-cli compile -b "$FQBN" \
  --build-property "build.libpicowdefs=-DLWIP_IPV6=0 -DLWIP_IPV4=1 -D__LWIP_MEMMULT=3" \
  --libraries firmware/libraries \
  --output-dir build $DD_EXTRA \
  firmware/DruckerDashboardRP2350-MK1
