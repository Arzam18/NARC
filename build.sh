#!/usr/bin/env bash
# Builds NARC Genesys with g++ (MSYS2 UCRT64 on Windows, any g++ >= 12 elsewhere).
#   ./build.sh [target] [output]
# Targets: native, avx512 (x86-64-v4 + PEXT), bmi2 (x86-64-v3 + PEXT sliders; Intel Haswell+, AMD Zen 3+),
#          avx2 (x86-64-v3, magic sliders; best for AMD Zen 1/2), sse42 (x86-64-v2), generic (any x86-64), debug.
set -euo pipefail
cd "$(dirname "$0")"
[ -d /c/msys64/ucrt64/bin ] && export PATH="/c/msys64/ucrt64/bin:$PATH"
CXX=${CXX:-g++}
TARGET=${1:-native}
OUT=${2:-narc-genesys}
NET=${NET:-networks/narc-genesys.nnue}   # embedded network
case "$TARGET" in
  native) ARCH="-march=native" ;;
  avx512) ARCH="-march=x86-64-v4 -DUSE_PEXT" ;;
  bmi2) ARCH="-march=x86-64-v3 -DUSE_PEXT" ;;
  avx2) ARCH="-march=x86-64-v3" ;;
  sse42) ARCH="-march=x86-64-v2" ;;
  generic) ARCH="-march=x86-64" ;;
  debug) ARCH="-march=native -O0 -g -fsanitize=undefined -D_GLIBCXX_ASSERTIONS" ;;
  *) echo "unknown target '$TARGET'" >&2; exit 1 ;;
esac
OPT="-O3 -flto=auto -DNDEBUG"
[ "$TARGET" = debug ] && OPT=""
STATIC="-static"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) STATIC="-static -Wl,--stack,67108864" ;; esac   # 64 MB thread stacks
[ "$(uname -s)" = Darwin ] && STATIC=""
[ "$TARGET" = debug ] && STATIC=""
mkdir -p "$(dirname "$OUT")"
RES=""
if [ "$TARGET" != debug ] && command -v windres >/dev/null 2>&1 && [ -n "$(echo "$STATIC" | grep stack)" ]; then
  mkdir -p build
  windres -O coff -I assets assets/narc-genesys.rc -o build/narc_res.o   # icon and version information
  RES=build/narc_res.o
fi
# shellcheck disable=SC2086
"$CXX" -std=c++20 -Wall -Wextra -pthread $OPT $ARCH -DNARC_NET=\"$NET\" -DNARC_TAG=\"${TAG:-}\" src/*.cpp $RES -o "$OUT" $STATIC
echo "built $OUT ($TARGET)"
