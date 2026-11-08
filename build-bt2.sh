#!/bin/bash
# 无 Xcode 重建 SimpleMtkBtFw.kext（照 WiFi 那条的 mksdk + clang 路线）。
# 用于：换固件、或修补驱动。纯本地编译，不安装、不重启。
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC_DIR:-$HERE/src}"
# Kernel SDK headers: point SDK= at your own extracted kernel SDK
# (see docs/bt-rebuild-chain.md). This project kept it next to the workspace.
SDK="${SDK:-$HOME/Documents/Hackintosh-MacLocal/tools/research/mksdk}"
OUT="$HERE/build"; rm -rf "$OUT"; mkdir -p "$OUT/obj"
VERSION=1.0.0
NAME=SimpleMtkBtFw

COMMON=(-mkernel -arch x86_64 -fno-builtin -fno-common -fno-stack-protector \
        -nostdinc -mmacosx-version-min=10.12)
CXXFLAGS=("${COMMON[@]}" -fno-exceptions -fno-rtti -std=gnu++14 \
        -Wno-deprecated-register -Wno-nullability-completeness \
        -Wno-invalid-offsetof -Wno-inconsistent-missing-override \
        -Wno-unused-command-line-argument)
CFLAGS=("${COMMON[@]}" -std=gnu11 -Wno-unused-command-line-argument)
DEFS=(-DKERNEL -DKERNEL_PRIVATE -D__KERNEL__ -DAPPLE -DDRIVER_PRIVATE \
      -D"MODULE_VERSION=\"$VERSION\"" -D"PRODUCT_NAME=\"$NAME\"")
INC=(-I"$SDK/Headers" -I"$SRC" -I"$SDK/Headers/IOKit" \
     -I"$SDK/Headers/libkern" -I"$SDK/Headers/bsd")

ok=0; n=0
for f in "$SRC"/*.cpp "$SRC"/*.c; do
  [ -f "$f" ] || continue
  n=$((n+1))
  b=$(basename "$f"); o="$OUT/obj/${b}.o"
  if false; then
    if clang -x c "${CFLAGS[@]}" "${DEFS[@]}" "${INC[@]}" -c -o "$o" "$f" 2> "$o.err"; then
      ok=$((ok+1)); printf '  ok   %s\n' "$b"; continue
    fi
  elif clang -x c++ "${CXXFLAGS[@]}" "${DEFS[@]}" "${INC[@]}" -c -o "$o" "$f" 2> "$o.err"; then
    ok=$((ok+1)); printf '  ok   %s\n' "$b"; continue
  fi
  if false; then
    ok=$((ok+1)); printf '  ok   %s\n' "$b"
  else
    printf '  FAIL %s\n' "$b"
  fi
done
echo "=== compiled $ok / $n ==="
if [ "$ok" -ne "$n" ]; then
  echo "--- 首个错误 ---"
  for e in "$OUT"/obj/*.err; do [ -s "$e" ] && { echo "## $(basename "$e")"; head -12 "$e"; break; }; done
  exit 1
fi
echo "=== 链接 ==="
clang -arch x86_64 -mkernel -nostdinc -nostdlib -Wl,-kext -Wl,-no_arch_warnings \
  -o "$OUT/$NAME" "$OUT"/obj/*.o 2>&1 | tail -20
[ -f "$OUT/$NAME" ] && { echo "链接成功"; file "$OUT/$NAME"; } || { echo "链接失败"; exit 2; }
