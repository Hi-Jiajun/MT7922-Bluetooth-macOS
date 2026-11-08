#!/usr/bin/env python3
"""Put the three NVRAM keys bluetoothd needs into an OpenCore config.plist.

Why these three, measured on 2026-10-14 (boot 08:07:01, 24H32)
-------------------------------------------------------------
With `mtkbtcsr` armed the USB device presents as a CSR external dongle, and the
first bluetoothd of that boot did take the third-party path:

    08:07:28.805 Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE
    08:07:29.068 OI_HCICMD_ReadLocalVersionInformation failed: STATUS 408, but we move forward
    08:07:29.080 ERROR -- Third Party Dongle has the same address as the internal module
    08:07:29.080 initCmdReadBdAddrCB failed: STATUS 158
    08:07:29.080 ERROR -- Third Party USB Bluetooth dongle failed initialization.
                 Switching to internal Bluetooth module

That failure makes bluetoothd write its own flag back into the firmware tree
(the string is in /usr/sbin/bluetoothd:
`IORegistryEntrySetCFProperty() failed for bluetoothExternalDongleFailed`), and
every bluetoothd after it then skips the dongle entirely:

    External Bluetooth dongle was not found -- setting chipset to default BCM_4350C2
    HCI Transport is set to H4BC
    UART open(/dev/cu.BLTH) port failed to appear after 15 seconds

- which is the 16-second crash loop this machine was stuck in, not a MediaTek
problem at all.

So the three keys are:

  bluetoothExternalDongleFailed          = 0x00        clear the "dongle failed,
                                                       use internal" flag
  bluetoothHostControllerSwitchBehavior  = "always"    keep preferring the
                                                       external dongle (this is
                                                       the documented recipe for
                                                       USB BT dongles on macOS)
  bluetoothInternalControllerInfo        = 00*8 + addr  the *internal* module's
                                                       address must differ from
                                                       the dongle's, otherwise
                                                       the "same address" check
                                                       above rejects it. The
                                                       dongle owns 02:1a:7d:da:71:13,
                                                       so the internal one is
                                                       offset by one.

Measured result of exactly this combination, no reboot needed (bluetoothd reads
these at startup): after `launchctl bootout/bootstrap`, the daemon stopped
crashing and `system_profiler SPBluetoothDataType` reported
`State: On / Chipset: THIRD_PARTY_DONGLE / Address: 02:1A:7D:DA:71:13`.

Both media carry a bootable config.plist, so the same edit has to reach both.

Usage:
    bt-nvram-shape.py <config.plist> [more.plist ...]
    bt-nvram-shape.py <config.plist> --show
    bt-nvram-shape.py <config.plist> --remove
    bt-nvram-shape.py <config.plist> --internal-addr 02:1a:7d:da:71:14
"""

import plistlib
import shutil
import subprocess
import sys
import time

GUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
KEY_INFO = "bluetoothInternalControllerInfo"
KEY_FAILED = "bluetoothExternalDongleFailed"
KEY_SWITCH = "bluetoothHostControllerSwitchBehavior"

# The dongle's own address (read off the chip by SimpleMtkBtFw via HCI 0x1009).
DONGLE_ADDR = "02:1a:7d:da:71:13"
DEFAULT_INTERNAL_ADDR = "02:1a:7d:da:71:14"


def section(root):
    ref = root.get("NVRAM", {}).get("Add", {}).get(GUID)
    if ref is None:
        raise SystemExit("config has no NVRAM/Add/%s section" % GUID)
    return ref


def addr_to_blob(addr):
    octets = bytes(int(x, 16) for x in addr.split(":"))
    if len(octets) != 6:
        raise SystemExit("bd address must have 6 octets: %s" % addr)
    blob = b"\x00" * 8 + octets
    assert len(blob) == 14, "bluetoothd requires exactly 14 bytes"
    return blob


def show(path):
    with open(path, "rb") as f:
        ref = section(plistlib.load(f))
    print("config : %s" % path)
    for key in (KEY_INFO, KEY_FAILED, KEY_SWITCH):
        got = ref.get(key, None)
        if isinstance(got, bytes):
            print("  %-38s = data(%d) %s" % (key, len(got), got.hex()))
        else:
            print("  %-38s = %r" % (key, got))


def edit(path, remove, internal_addr):
    with open(path, "rb") as f:
        raw = f.read()
    # Preserve the container format: OpenCore reads both, but rewriting a binary
    # plist as XML (or the reverse) would be an unrelated change to the boot
    # configuration, and unrelated changes are how a working config gets blamed
    # for a boot failure it did not cause.
    fmt = plistlib.FMT_BINARY if raw[:8] == b"bplist00" else plistlib.FMT_XML
    root = plistlib.loads(raw)
    ref = section(root)
    before = dict(ref)

    backup = "%s.bak-btnvram-%s" % (path, time.strftime("%Y%m%d-%H%M%S"))
    shutil.copy2(path, backup)

    if remove:
        ref.pop(KEY_FAILED, None)
        ref.pop(KEY_SWITCH, None)
        ref.pop(KEY_INFO, None)
    else:
        ref[KEY_FAILED] = b"\x00"
        ref[KEY_SWITCH] = "always"
        ref[KEY_INFO] = addr_to_blob(internal_addr)

    with open(path, "wb") as f:
        plistlib.dump(root, f, fmt=fmt)

    # Read back from disk. The property list is edited by other tools too, so
    # "I wrote it" is not evidence that it is there.
    with open(path, "rb") as f:
        check = plistlib.load(f)
    after = section(check)

    lint = subprocess.run(["plutil", "-lint", path], capture_output=True, text=True)
    changed = sorted(set(before) ^ set(after)) or [
        k for k in sorted(set(before) & set(after)) if before[k] != after[k]
    ]
    print("config : %s" % path)
    print("backup : %s" % backup)
    print("lint   : %s" % (lint.stdout.strip() or lint.stderr.strip()))
    print("keys touched in this GUID : %d (%s)" % (len(changed), ", ".join(changed)))
    print("other keys in this GUID   : %d" % (len(after) - len(changed)))
    for key in (KEY_INFO, KEY_FAILED, KEY_SWITCH):
        got = after.get(key, None)
        if isinstance(got, bytes):
            print("  %-38s = data(%d) %s" % (key, len(got), got.hex()))
        else:
            print("  %-38s = %r" % (key, got))
    if not remove and isinstance(after.get(KEY_INFO), bytes):
        tail = after[KEY_INFO][8:]
        if tail == addr_to_blob(DONGLE_ADDR)[8:]:
            print("WARNING: internal address equals the dongle's own address;")
            print("         bluetoothd rejects the dongle with")
            print("         'Third Party Dongle has the same address as the internal module'.")


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)
    internal_addr = DEFAULT_INTERNAL_ADDR
    if "--internal-addr" in argv:
        internal_addr = argv[argv.index("--internal-addr") + 1]
    for path in args:
        if "--show" in argv:
            show(path)
        else:
            edit(path, "--remove" in argv, internal_addr)


if __name__ == "__main__":
    main(sys.argv)
