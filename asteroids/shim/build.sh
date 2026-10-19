#!/bin/bash
# Build the asteroids M1 probe shim and wrap it in a boot image v4 for
# `fastboot boot` (nothing is flashed). Needs aarch64-elf-gcc
# (`brew install aarch64-elf-gcc aarch64-elf-binutils`) or aarch64-linux-gnu-gcc.
set -euo pipefail
cd "$(dirname "$0")"
if command -v aarch64-elf-gcc >/dev/null; then T=aarch64-elf-; else T=aarch64-linux-gnu-; fi
CC=${T}gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -I../../shim"
mkdir -p out
for f in start.S probe.c console.c fdt.c loader.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${T}readelf -r out/probe.o out/console.o out/fdt.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/probe.o out/console.o out/fdt.o out/loader.o -lgcc -o out/probe.elf
${T}objcopy -O binary out/probe.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
end=$(${T}nm out/probe.elf | awk '$3=="_end"{print $1}')
python3 -I -c 'import sys; f=open(sys.argv[1],"r+b"); f.truncate(int(sys.argv[2],16))' out/Image "$end"
# M2: with an xnu payload (tools/mkpayload.py) as $1, append it at _end
# (16-aligned, where loader_find() looks) and grow the header's image_size
# (offset 16) so ABL reserves room for it. ABL ignores boot.img's ramdisk.
OUT=out/asteroids-probe-boot.img
if [ -n "${1:-}" ]; then
	python3 -I - out/Image "$1" <<'PY'
import struct, sys
img, payload = sys.argv[1], open(sys.argv[2], 'rb').read()
assert payload[:8] == b'SWEETXNU', 'not a SWEETXNU payload'
d = bytearray(open(img, 'rb').read())
d += b'\0' * (-len(d) % 16) + payload
d += b'\0' * (-len(d) % 4096)
struct.pack_into('<Q', d, 16, len(d))
open(img, 'wb').write(d)
print(f'appended payload: {len(payload)} bytes, image {len(d)} bytes')
PY
	OUT=out/asteroids-xnu-boot.img
fi
# Ramdisk: one page starting with the "AST3PROB" marker. With a v4 boot image
# ABL puts vendor_boot's ramdisk first, so the probe reports where it lands.
python3 -I -c 'import sys; open(sys.argv[1],"wb").write(b"AST3PROB".ljust(4096, b"\0"))' out/ramdisk.bin
# Same header shape as the stock V3.1 boot.img (v4, no cmdline, no os_version).
python3 ../tools/aosp/mkbootimg.py --header_version 4 --kernel out/Image \
	--ramdisk out/ramdisk.bin -o "$OUT"
ls -l out/Image "$OUT"
shasum -a 256 "$OUT"
