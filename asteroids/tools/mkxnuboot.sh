#!/bin/bash
# Package an asteroids xnu kernel into a `fastboot boot` image for the Phone (3a):
#   ADT (asteroids/tools/mkadt.py) -> payload (tools/mkpayload.py) -> shim + payload
#
#   [CPUS=n] [OFF="..."] asteroids/tools/mkxnuboot.sh KERNEL [EXTRA BOOT-ARGS...]
#
# KERNEL is kernel.development.sweet from the xnu-kernel-asteroids CI
# artifact (never upstream's xnu-kernel: that one is built for sweet).
# Output: asteroids/shim/out/asteroids-xnu-boot.img
set -euo pipefail
cd "$(dirname "$0")/../.."
KERNEL=$1
shift
OUT=asteroids/shim/out
mkdir -p "$OUT"

# M2 boot-args: early console only. No rd=ufsroot0 / ufsrw / rootrw: sweet's
# persistent log would write the end of userdata. sweet's keys, touch and
# USB drivers start by default and poke Redmi MMIO, so switch them off.
# CPUS: cores to start (8 = all, A520 x4 + A720 x4, tested 2026-10-20; 1 = fallback).
ARGS="debug=0xc serial=3 cpus=${CPUS:-8} keepsyms=1 -enable_kprintf_spam -noprogress"
# OFF lists the sweet drivers to keep off (override: OFF="sweetusb=0" ...).
OFF=${OFF-"sweetkeys=0 sweettouch=0 sweetusb=0"}
ARGS="$ARGS wdt=-1 $OFF $*"

python3 asteroids/tools/mkadt.py "$OUT/asteroids.adt"
python3 tools/mkpayload.py "$KERNEL" "$OUT/asteroids.adt" "$OUT/xnu-payload.bin" --cmdline "$ARGS"
echo "cmdline: $ARGS"
asteroids/shim/build.sh "$(pwd)/$OUT/xnu-payload.bin"
