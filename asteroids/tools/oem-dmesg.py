#!/usr/bin/env python3
"""Turn `fastboot oem dmesg` output back into the previous boot's kernel log.

usage: fastboot oem dmesg 2>&1 | oem-dmesg.py > log.txt
       oem-dmesg.py saved-output.txt > log.txt

ABL sends the log as INFO packets of up to 64 bytes, which fastboot prints
as "(bootloader) <chunk>" lines; chunks split log lines anywhere. This drops
the prefixes and the fastboot status lines and joins the chunks again.
"""
import sys

PREFIX = "(bootloader) "


def clean(lines):
    out = []
    for line in lines:
        line = line.rstrip("\n")
        if line.startswith(PREFIX):
            out.append(line[len(PREFIX):])
        elif line.startswith(("OKAY", "Finished", "FAILED", "fastboot:")):
            continue
        else:
            # A newline inside a chunk: fastboot already broke the line here.
            out.append("\n" + line)
    text = "".join(out)
    return text.replace("oem dmesg start\n", "", 1)


def main():
    src = open(sys.argv[1], errors="replace") if len(sys.argv) > 1 else sys.stdin
    sys.stdout.write(clean(src))


if __name__ == "__main__":
    main()
