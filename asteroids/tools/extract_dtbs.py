#!/usr/bin/env python3
"""Split the device trees out of an Android vendor_boot (v3/v4) and dtbo image.

usage: extract_dtbs.py vendor_boot.img dtbo.img OUTDIR

Writes OUTDIR/base-N.dtb (SoC trees from vendor_boot) and OUTDIR/dtbo-N.dtbo
(board overlays), plus OUTDIR/index.txt with each tree's model/compatible
and the msm-id / board-id the bootloader matches on.
"""
import struct
import sys
from pathlib import Path

FDT_MAGIC = 0xD00DFEED


def align(n, page):
    return (n + page - 1) // page * page


def split_fdts(blob):
    """Concatenated FDT blobs -> list of bytes."""
    out, off = [], 0
    while off + 8 <= len(blob):
        magic, size = struct.unpack_from(">II", blob, off)
        if magic != FDT_MAGIC:
            break
        out.append(blob[off:off + size])
        off += size
    return out


def vendor_boot_dtbs(data):
    if data[:8] != b"VNDRBOOT":
        sys.exit("not a vendor_boot image")
    version, page = struct.unpack_from("<II", data, 8)
    ramdisk_size = struct.unpack_from("<I", data, 24)[0]
    # magic8 version page kernel_addr ramdisk_addr ramdisk_size cmdline[2048]
    # tags_addr name[16] header_size dtb_size dtb_addr(u64)
    off = 28 + 2048 + 4 + 16
    header_size, dtb_size = struct.unpack_from("<II", data, off)
    if version < 3:
        sys.exit(f"vendor_boot v{version} not supported")
    dtb_off = align(header_size, page) + align(ramdisk_size, page)
    return version, split_fdts(data[dtb_off:dtb_off + dtb_size])


def dtbo_entries(data):
    magic, total, hdr, esize, count, eoff, page, ver = struct.unpack_from(">8I", data, 0)
    if magic != 0xD7B7AB1E:
        sys.exit("not a dtbo table")
    out = []
    for i in range(count):
        size, off = struct.unpack_from(">II", data, eoff + i * esize)
        out.append(data[off:off + size])
    return out


def props(fdt):
    """Root-node properties we care about, read straight from the struct block."""
    off_struct, off_strings = struct.unpack_from(">II", fdt, 8)[0], struct.unpack_from(">I", fdt, 12)[0]
    p, depth, found = off_struct, 0, {}
    want = {"model", "compatible", "qcom,msm-id", "qcom,board-id", "qcom,pmic-id"}
    while p < len(fdt):
        tok = struct.unpack_from(">I", fdt, p)[0]
        p += 4
        if tok == 1:  # BEGIN_NODE
            end = fdt.index(b"\0", p)
            p = align(end + 1, 4)
            depth += 1
            if depth > 1:
                break
        elif tok == 3:  # PROP
            ln, noff = struct.unpack_from(">II", fdt, p)
            p += 8
            name = fdt[off_strings + noff:fdt.index(b"\0", off_strings + noff)].decode()
            val = fdt[p:p + ln]
            p = align(p + ln, 4)
            if name in want:
                if name in ("model", "compatible"):
                    found[name] = " ".join(v.decode() for v in val.split(b"\0") if v)
                else:
                    found[name] = " ".join(f"{v:#x}" for v in struct.unpack(f">{ln // 4}I", val))
        elif tok in (2, 4):  # END_NODE, NOP
            continue
        elif tok == 9:
            break
    return found


def main():
    vb, dtbo, outdir = map(Path, sys.argv[1:4])
    outdir.mkdir(parents=True, exist_ok=True)
    index = []
    version, bases = vendor_boot_dtbs(vb.read_bytes())
    index.append(f"vendor_boot v{version}: {len(bases)} base trees")
    for i, fdt in enumerate(bases):
        (outdir / f"base-{i}.dtb").write_bytes(fdt)
        index.append(f"base-{i}.dtb  {props(fdt)}")
    overlays = dtbo_entries(dtbo.read_bytes())
    index.append(f"dtbo: {len(overlays)} overlays")
    for i, fdt in enumerate(overlays):
        (outdir / f"dtbo-{i}.dtbo").write_bytes(fdt)
        index.append(f"dtbo-{i}.dtbo  {props(fdt)}")
    (outdir / "index.txt").write_text("\n".join(index) + "\n")
    print("\n".join(index))


if __name__ == "__main__":
    main()
