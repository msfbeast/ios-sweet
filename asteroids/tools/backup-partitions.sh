#!/bin/bash
# Pull raw partition images from a Phone (3a) booted into OrangeFox (adb as root).
# Read-only on the phone: it only runs `dd if=` and `sha256sum`.
#
#   asteroids/tools/backup-partitions.sh [OUTDIR]     (default asteroids/backup/<date>)
#
# Each image is checked against a SHA-256 computed on the phone. The device
# images (persist, modemst*, fsg, fsc, ...) hold the IMEI and calibration:
# keep the output off git and off cloud sync (.gitignore covers asteroids/backup/).
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${1:-asteroids/backup/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

# Irreplaceable per-device data first, then both A/B slots of the boot chain.
PARTS="persist modemst1 modemst2 fsg fsc devinfo
boot_a boot_b init_boot_a init_boot_b vendor_boot_a vendor_boot_b
dtbo_a dtbo_b vbmeta_a vbmeta_b vbmeta_system_a vbmeta_system_b
vbmeta_vendor_a vbmeta_vendor_b recovery_a recovery_b"

adb get-state >/dev/null
[ "$(adb shell id -u | tr -d '\r')" = 0 ] || { echo "adb is not root: boot OrangeFox first" >&2; exit 1; }
adb shell ls -l /dev/block/by-name/ > "$OUT/by-name.txt"

for p in $PARTS; do
	dev=/dev/block/by-name/$p
	if ! adb shell "test -e $dev"; then
		echo "skip $p (no such partition)"
		continue
	fi
	want=$(adb shell "sha256sum $dev" | cut -d' ' -f1)
	adb exec-out "dd if=$dev bs=1M 2>/dev/null" > "$OUT/$p.img"
	got=$(shasum -a 256 "$OUT/$p.img" | cut -d' ' -f1)
	if [ "$want" = "$got" ]; then
		echo "ok   $p ($(wc -c < "$OUT/$p.img" | tr -d ' ') bytes)"
	else
		echo "BAD  $p: phone $want, file $got" >&2
		exit 1
	fi
	echo "$got  $p.img" >> "$OUT/SHA256SUMS"
done
echo "saved to $OUT"
