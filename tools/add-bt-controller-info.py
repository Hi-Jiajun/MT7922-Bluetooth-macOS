#!/usr/bin/env python3
"""Inject Apple's `bluetoothInternalControllerInfo` NVRAM variable into an OC config.

Why this variable, and why exactly 14 bytes
-------------------------------------------
bluetoothd's SIGABRT (2026-10-13, see ../bt-abort-root-cause.md) is the end of a
chain that starts with the Bluetooth stack deciding which chipset it is talking
to. Disassembly of the decision function in /usr/sbin/bluetoothd
(0x1001a23fd..0x1001a25c8) reads:

    [HasInternalControllerAddr] Internal controller info missing
        -> movb $1, %r14b                      ; "info missing" flag
    ...
        orb %al, %r14b ; jne <BCM fallback>    ; missing -> BCM_4350C2
    "External Bluetooth dongle %s -- setting chipset to default BCM_4350C2"

i.e. with no internal controller info the platform default is Broadcom's
BCM_4350C2, the Broadcom VSC sequence runs (`bcm_vsc.c`), the MediaTek chip
answers one of those commands with a zero-length payload, and the parser's
`ByteStream_NumReadBytesAvail(*pBs) >= 1` assertion aborts the daemon.

The layout is not guessed - it is read off the writer in the same binary
(0x1001a1ec6):

    movq $0x0, (%rsi)          ; bytes[0..7]   = 0
    movl (%r14), %eax
    movl %eax, 0x8(%rsi)       ; bytes[8..11]  = *(uint32_t *)src
    movzwl 0x4(%r14), %eax
    movw %ax, 0xc(%rsi)        ; bytes[12..13] = *(uint16_t *)(src + 4)
    movl $0xe, %edx            ; CFDataCreate(..., 14)

so the blob is eight zero bytes followed by the controller's six-byte BD
address, and the reader (0x1001a2473) hands those last six bytes to a function
that sets the controller address - which is also the "localAddress 0x0"
symptom this driver has been fighting.

Both media carry a config.plist and both are bootable, so the same edit has to
reach both; the key is written under the Apple boot-variable GUID, where the
stack looks it up (IODeviceTree:/options).

Usage:
    add-bt-controller-info.py <config.plist> [bd-addr like 02:1a:7d:da:71:13]
    add-bt-controller-info.py <config.plist> --remove
"""

import plistlib
import shutil
import sys
import time

GUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
KEY = "bluetoothInternalControllerInfo"
DEFAULT_ADDR = "02:1a:7d:da:71:13"


def load_ref(root):
    ref = root.get("NVRAM", {}).get("Add", {}).get(GUID)
    if ref is None:
        raise SystemExit("config has no NVRAM/Add/%s section" % GUID)
    return ref


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path = argv[1]
    remove = "--remove" in argv
    addr = DEFAULT_ADDR
    for a in argv[2:]:
        if a != "--remove":
            addr = a

    with open(path, "rb") as f:
        raw = f.read()
    # Preserve the container format: OpenCore reads both, but rewriting a binary
    # plist as XML (or the reverse) would be an unrelated change to the boot
    # configuration, and unrelated changes are how a working config gets blamed
    # for a boot failure it did not cause.
    fmt = plistlib.FMT_BINARY if raw[:8] == b"bplist00" else plistlib.FMT_XML
    root = plistlib.loads(raw)
    ref = load_ref(root)

    backup = "%s.bak-btinfo-%s" % (path, time.strftime("%Y%m%d-%H%M%S"))
    shutil.copy2(path, backup)

    if remove:
        had = ref.pop(KEY, None)
        action = "removed" if had is not None else "was not present"
    else:
        octets = bytes(int(x, 16) for x in addr.split(":"))
        if len(octets) != 6:
            raise SystemExit("bd address must have 6 octets: %s" % addr)
        blob = b"\x00" * 8 + octets
        assert len(blob) == 14, "bluetoothd requires exactly 14 bytes"
        ref[KEY] = blob
        action = "set to %s + %s (%d bytes)" % ("00" * 8, addr.replace(":", ""), len(blob))

    with open(path, "wb") as f:
        plistlib.dump(root, f, fmt=fmt)

    # read back what the file actually contains - the property list is edited by
    # other tools too, so "I wrote it" is not evidence that it is there.
    with open(path, "rb") as f:
        check = plistlib.load(f)
    got = load_ref(check).get(KEY)
    print("config : %s" % path)
    print("backup : %s" % backup)
    print("%s : %s" % (KEY, action))
    print("readback: %s" % (got.hex() if isinstance(got, bytes) else got))
    print("other NVRAM keys in that GUID: %d" % (len(load_ref(check)) - (1 if got is not None else 0)))


if __name__ == "__main__":
    main(sys.argv)
