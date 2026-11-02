#!/usr/bin/env bash
# Builds every CPU variant into release/NARC-Genesys-<version>/ (Windows, MSYS2 UCRT64) and zips it.
#   ./release.sh            (NET=<network.nnue> picks another network; TAG=<name> is added to every file name)
# Every build this CPU can run must reproduce the same bench signature; builds it cannot run
# (for example AVX-512 here) are packaged and reported as not run.
set -euo pipefail
cd "$(dirname "$0")"
VERSION=$(sed -n 's/.*NAME = "NARC Genesys \([^"]*\)".*/\1/p' src/uci.cpp)
NAME="NARC-Genesys-$VERSION${TAG:+-$TAG}"
DIR="release/$NAME"
rm -rf "$DIR" "release/$NAME-windows.zip"
mkdir -p "$DIR/logo"
ref=""
for target in avx512 bmi2 avx2 sse42 generic; do
  exe="$DIR/$NAME-$target.exe"
  ./build.sh "$target" "$exe" 2>&1 | grep -E "error|built" || true
  [ -f "$exe" ] || { echo "build failed: $target" >&2; exit 1; }
  if out=$("$exe" bench 12 2>/dev/null | tail -1) && [ -n "$out" ]; then
    nodes=${out%% *}
    [ -z "$ref" ] && ref=$nodes
    [ "$nodes" = "$ref" ] || { echo "$target: bench $nodes differs from $ref" >&2; exit 1; }
    echo "$target: bench $nodes nodes, ${out##* nodes } ok"
  else
    echo "$target: not run here (this CPU lacks its instructions)"
  fi
  cp assets/narc-genesys-arena.bmp "$DIR/$NAME-$target.bmp"   # Arena shows <exe name>.bmp as the engine logo
done
cp assets/narc-genesys-banner.png assets/narc-genesys-icon.png assets/narc-genesys.ico "$DIR/logo/"
sed "s/@VERSION@/$VERSION${TAG:+-$TAG}/g; s/@BENCH@/$ref/g" assets/README-release.txt > "$DIR/README.txt"
cp ReadMe.txt "$DIR/About.txt"
(cd release && powershell -NoProfile -Command "Compress-Archive -Path '$NAME/*' -DestinationPath '$NAME-windows.zip' -Force")
(cd release && sha256sum "$NAME-windows.zip" "$NAME"/*.exe > "$NAME.sha256")
ls -la "$DIR" "release/$NAME-windows.zip"
