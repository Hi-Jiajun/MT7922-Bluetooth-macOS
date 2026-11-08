#!/usr/bin/env python3
"""Report the real MediaTek BT firmware version embedded in a SimpleMtkBtFw source.

Why: the kext publishes `fw_name` ("BT_RAM_CODE_MT7922_1_1_hdr.bin") but not the
version, and `strings` on the built kext finds nothing useful because the blob is
zlib-compressed inside `FwBinary.cpp` (the array starts 0x78 0x9C). The version is
the FIRST 14 BYTES OF THE DECOMPRESSED IMAGE - a fixed-width build timestamp that
linux-firmware's filenames and the local README both quote - so the only way to
answer "is the firmware we ship the newest one?" without running the chip is to
inflate it here. `strings` on the compressed data sometimes shows a *different*
date (an inner build-time string): that is the trap this tool avoids.

Usage:
    bt-fw-version.py <FwBinary.cpp> [more.cpp ...]

Reference points (from ../bt-rebuild/README.md, 2026-10-13):
    20251118163139  515,478   upstream release package (what was injected then)
    20251118163447  515,478   upstream master
    20260724143815  517,398   linux-firmware latest at that time
"""

import re
import sys
import zlib

ARRAY = re.compile(r"const unsigned char (\w+)\[\]\s*=\s*\{(.*?)\};", re.S)
FWENT = re.compile(r'MTK_BT_FW\("([^"]+)",\s*(\w+),\s*(\w+)\)')
BYTE = re.compile(r"0x([0-9A-Fa-f]{2})")


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    for path in argv[1:]:
        text = open(path, "r", errors="replace").read()
        blobs = {}
        for m in ARRAY.finditer(text):
            blobs[m.group(1)] = bytes(int(x, 16) for x in BYTE.findall(m.group(2)))
        names = {sym: name for name, sym, _ in FWENT.findall(text)}
        print("== %s" % path)
        for sym, blob in blobs.items():
            label = names.get(sym, sym)
            try:
                raw = zlib.decompress(blob)
            except zlib.error as exc:
                print("   %-34s %8d B compressed, inflate failed: %s" % (label, len(blob), exc))
                continue
            print("   %-34s %8d B compressed -> %8d B raw, header version %s"
                  % (label, len(blob), len(raw), raw[:14].decode("ascii", "replace")))


if __name__ == "__main__":
    main(sys.argv)
