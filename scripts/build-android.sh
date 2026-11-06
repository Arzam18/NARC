#!/usr/bin/env bash
# Build NARC Genesys 5.1 for Android ARM64 (exact NEON + Dotprod-target variants).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

: "${ANDROID_NDK_HOME:=${ANDROID_NDK_ROOT:-}}"
if [[ -z "$ANDROID_NDK_HOME" || ! -d "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64" ]]; then
    echo "ERROR: ANDROID_NDK_HOME must point to the installed Android NDK (Linux host toolchain)." >&2
    exit 1
fi

TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
CXX="$TOOLCHAIN/bin/aarch64-linux-android24-clang++"
STRIP="$TOOLCHAIN/bin/llvm-strip"

NET="${NET:-$ROOT/networks/narc-genesys.nnue}"
OUT_ROOT="${OUT_ROOT:-$ROOT/build/android-arm64}"
MODE="${1:-all}"

[[ -x "$CXX" ]] || { echo "ERROR: Android NDK compiler not found: $CXX" >&2; exit 1; }
[[ -x "$STRIP" ]] || { echo "ERROR: llvm-strip not found: $STRIP" >&2; exit 1; }
[[ -s "$NET" ]] || { echo "ERROR: NARC 5.1 NNUE network missing: $NET" >&2; exit 1; }

python3 - "$NET" <<'PY'
from pathlib import Path
import struct, sys
p = Path(sys.argv[1])
data = p.read_bytes()
if len(data) < 36 or data[:8] != b"DRNNUE03":
    raise SystemExit(f"ERROR: not a DRNNUE03 network: {p}")
shape = struct.unpack_from("<7I", data, 8)
expected_shape = (12288, 1024, 8, 16, 255, 64, 400)
if shape != expected_shape:
    raise SystemExit(f"ERROR: expected NARC 5.1 1024-wide network {expected_shape}, got {shape}")
inputs, hidden, buckets, _kb, _qa, _qb, _scale = shape
expected_size = 36 + 2 * inputs * hidden + 2 * hidden + 2 * buckets * 2 * hidden + 4 * buckets
if len(data) < expected_size or len(data) > expected_size + 64:
    raise SystemExit(f"ERROR: unexpected network size {len(data):,}; payload requires {expected_size:,} bytes (up to 64 bytes padding accepted)")
print(f"Verified NARC 5.1 network: {p} ({len(data):,} bytes; payload {expected_size:,} bytes; {hidden} hidden; 16 king buckets)")
PY

build_one() {
    local variant="$1" arch="$2"
    local out_dir="$OUT_ROOT/$variant"
    local out="$out_dir/narc-genesys-5.1-android-arm64-$variant"
    mkdir -p "$out_dir"
    echo "=== NARC Genesys 5.1 Android ARM64 $variant ==="
    echo "Compiler: $($CXX --version | head -n 1)"
    echo "Target: aarch64-linux-android24"
    echo "Architecture: $arch"
    echo "NNUE: $NET"
    "$CXX" \
        -std=c++20 -O3 -DNDEBUG -pthread \
        -march="$arch" -mtune=cortex-a76 \
        -fstack-protector-strong -ffunction-sections -fdata-sections \
        -D_FORTIFY_SOURCE=2 -DNARC_NET=\"$NET\" \
        src/*.cpp -o "$out" \
        -static-libstdc++ -Wl,--gc-sections \
        -Wl,-z,relro,-z,now,-z,noexecstack -Wl,-z,max-page-size=16384
    "$STRIP" --strip-debug "$out"
    file "$out" | tee "$out_dir/file.txt"
    sha256sum "$out" | tee "$out_dir/SHA256SUMS"
    cat > "$out_dir/BUILD-INFO.txt" <<INFO
Engine: NARC Genesys 5.1
Platform: Android ARM64 (arm64-v8a), API 24+
Compiler: Android NDK Clang
Optimization: -O3 -DNDEBUG
Target architecture: $arch
CPU tuning: Cortex-A76
NNUE: Official NARC Genesys 5.1 network; embedded at compile time
NNUE width: 1024; input features: 12288; king buckets: 16
NEON: Exact AArch64 NEON kernels for accumulator row updates, cache updates and SCReLU output.
Dotprod: ARMv8.2-A Dotprod build uses an exact UDOT decomposition of SCReLU output with signed 16-bit weights. The low/high-byte split and signed-high-byte correction preserve the original arithmetic; no model quantization is performed.
C++ runtime: statically linked libc++; Android Bionic remains dynamic.
INFO
}

case "$MODE" in
    all)
        build_one neon armv8-a+simd
        build_one dotprod armv8.2-a+dotprod
        ;;
    neon)
        build_one neon armv8-a+simd
        ;;
    dotprod)
        build_one dotprod armv8.2-a+dotprod
        ;;
    *) echo "Usage: $0 [all|neon|dotprod]" >&2; exit 2 ;;
esac
