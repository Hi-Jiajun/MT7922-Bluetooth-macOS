#!/bin/bash
#
# 把 build-bt2.sh 的输出组装成可注入的 kext bundle。
#
#   bash assemble-kext.sh [binary]
#
# 默认读 ./build/SimpleMtkBtFw（build-bt2.sh 的链接产物），写 ./kext/SimpleMtkBtFw.kext，
# 然后打印二进制 sha。组装本身没有魔法：kext 就是 Contents/Info.plist + Contents/MacOS/<name>。
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$HERE/build/SimpleMtkBtFw}"
OUT="$HERE/kext/SimpleMtkBtFw.kext"

[ -f "$BIN" ] || { echo "没有可执行文件：$BIN（先跑 SRC_DIR=./src bash build-bt2.sh）" >&2; exit 1; }
[ -f "$HERE/src/Info.plist" ] || { echo "缺少 src/Info.plist" >&2; exit 1; }

mkdir -p "$OUT/Contents/MacOS"
cp "$HERE/src/Info.plist" "$OUT/Contents/Info.plist"
cp "$BIN" "$OUT/Contents/MacOS/SimpleMtkBtFw"

printf 'assembled %s\n' "$OUT"
printf 'sha256   %s\n' "$(shasum -a 256 "$OUT/Contents/MacOS/SimpleMtkBtFw" | cut -c1-16)"
printf '符号检查 %s (_kmod_info 计数，必须 >= 1)\n' \
    "$(nm -a "$OUT/Contents/MacOS/SimpleMtkBtFw" 2>/dev/null | grep -c kmod_info)"
