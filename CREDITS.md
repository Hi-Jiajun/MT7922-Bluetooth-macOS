# Credits

## Upstream driver source

The kernel-extension source in `src/` is derived from **SimpleMtkBtFw** by
[laobamac (王孝慈)](https://github.com/laobamac/SimpleMtkBtFw), licensed under the
**GNU General Public License v3.0**. The following files come from that upstream project,
some with local modifications:

| File | Origin |
| --- | --- |
| `src/SimpleMtkBtFw.cpp` | upstream |
| `src/BtMtk.{cpp,h}` | upstream |
| `src/USBDeviceController.{cpp,hpp}` | upstream |
| `src/Hci.h` | upstream |
| `src/linux.h` | upstream |
| `src/Log.h` | upstream |
| `src/zutil.{c,h}` | upstream (zlib-derived) |
| `src/FwBinary.cpp` | upstream packaging of the MediaTek firmware blob |
| `src/Info.plist` | upstream personality definition |

## Firmware

The Bluetooth firmware embedded in `src/FwBinary.cpp` is
`BT_RAM_CODE_MT7922_1_1_hdr.bin` from the [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
tree, redistributed under the terms MediaTek applies to its firmware blobs. The version
currently embedded is `20260724143815` (517398 bytes before compression). See
`tools/bt-fw-version.py` — the version is the first 14 bytes of the decompressed image.

## What this repository adds on top of upstream

- `build-bt.sh`, `build-bt2.sh`, `assemble-kext.sh` — a rebuild chain that works with
  Command Line Tools only (no `xcodebuild`), using extracted kernel SDK headers.
- `src/kmod-info.cpp` — supplies the `_kmod_info` symbol the kernel requires. Without it
  the link still succeeds and the kernel refuses to load the kext; `strings | grep kmod_info`
  does not reveal this, `nm` does.
- Firmware refresh to the latest linux-firmware image, with byte-level comparison.
- `tools/add-bt-controller-info.py`, `tools/bt-nvram-shape.py` — the NVRAM intervention
  macOS needs (`bluetoothInternalControllerInfo` must carry this chip's own BD address,
  otherwise macOS treats it as a `BCM_4350C2` and aborts while rewriting keychain ACLs).
- `tools/bt-fw-version.py`, `tools/btle-scan.swift` — small read-only helpers.
- `docs/` — the round-by-round record from device identification through the abort
  root cause to a working connection with a real peer (AirPods Pro 3).

## License

This repository as a whole is distributed under **GPL-3.0**, the same license as
SimpleMtkBtFw, because it is a derivative work of it. See `LICENSE`.

If upstream changes its license or structure, this repository's changes need to be
re-aligned with the new upstream.

## Acknowledgements

- laobamac (王孝慈) for SimpleMtkBtFw and the MediaTek HAL groundwork.
- The linux-firmware maintainers for the firmware images.
- The Acidanthera projects (Lilu, VirtualSMC) and the OpenCore team, which the
  deployment path depends on.
