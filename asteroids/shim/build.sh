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
for f in start.S probe.c console.c fdt.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${T}readelf -r out/probe.o out/console.o out/fdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/probe.o out/console.o out/fdt.o -lgcc -o out/probe.elf
${T}objcopy -O binary out/probe.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
end=$(${T}nm out/probe.elf | awk '$3=="_end"{print $1}')
python3 -I -c 'import sys; f=open(sys.argv[1],"r+b"); f.truncate(int(sys.argv[2],16))' out/Image "$end"
# Ramdisk: one page starting with the "AST3PROB" marker. With a v4 boot image
# ABL puts vendor_boot's ramdisk first, so the probe reports where it lands.
python3 -I -c 'import sys; open(sys.argv[1],"wb").write(b"AST3PROB".ljust(4096, b"\0"))' out/ramdisk.bin
# Same header shape as the stock V3.1 boot.img (v4, no cmdline, no os_version).
python3 ../tools/aosp/mkbootimg.py --header_version 4 --kernel out/Image \
	--ramdisk out/ramdisk.bin -o out/asteroids-probe-boot.img
ls -l out/Image out/asteroids-probe-boot.img
shasum -a 256 out/asteroids-probe-boot.img
