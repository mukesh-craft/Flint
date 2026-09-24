#!/usr/bin/env bash
# v0.17: portable build — Termux, generic Linux, macOS (Xcode CLT).
# Requires on PATH: clang/clang++, llvm-config, python3 (+headers), make.
set -e

OS="$(uname -s 2>/dev/null || echo unknown)"
ARCH="$(uname -m 2>/dev/null || echo unknown)"
echo "=== Flint build on $OS/$ARCH ==="

# Termux's C++ stdlib needs explicit -lc++_shared; elsewhere the driver
# handles it. Probe once instead of hardcoding the platform.
STDLIB_FLAG=""
if [ -n "${PREFIX:-}" ] && [ -f "$PREFIX/lib/libc++_shared.so" ]; then
    STDLIB_FLAG="-lc++_shared"
fi

CXX=${CXX:-clang++}
CC=${CC:-clang}
# -O2 for speed (llvm-config does not add an -O flag here, so the compiler was
# previously built unoptimized). Hardening flags add stack-smashing protection,
# fortified libc calls, format-string warnings and PIE at negligible cost.
# (-D_FORTIFY_SOURCE needs -O; -z relro/now/noexecstack are ELF-only, so the
# linker extras apply on Linux and are skipped elsewhere.)
OPT="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fPIE"
CXXFLAGS="$(llvm-config --cxxflags | sed 's/-Werror//g') $OPT"
LDFLAGS="$(llvm-config --ldflags) $(llvm-config --libs) $STDLIB_FLAG -pie"
case "$OS" in
    Linux*) LDFLAGS="$LDFLAGS -Wl,-z,relro,-z,now,-z,noexecstack" ;;
esac

# Runtime C files get the same hardening plus size-trimming sections.
# -ffunction-sections/-fdata-sections put each symbol in its own section so
# the linker's --gc-sections (added in spawnLinker) can discard everything
# the program doesn't reference (AI engine, crypto, net, ...). Object files
# grow slightly; final binaries shrink a lot.
CFLAGS_RT="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -fPIE -ffunction-sections -fdata-sections"

# Incremental helper: skip rebuild when $1 (output) is newer than all $2... inputs.
needs_rebuild() {
    out="$1"; shift
    [ ! -f "$out" ] && return 0
    for src in "$@"; do
        [ ! -f "$src" ] && return 0
        [ "$src" -nt "$out" ] && return 0
    done
    return 1
}

echo "=== Building Flint compiler (flintc) ==="
if needs_rebuild flintc src/main.cpp src/flint_os.cpp src/flint_os.h; then
    $CXX $CXXFLAGS -std=c++17 src/main.cpp src/flint_os.cpp $LDFLAGS -o flintc
else
    echo "  flintc up to date (skipped)"
fi

echo "=== Building profiled build (FLINTC_PROFILE) ==="
if needs_rebuild flintc_prof src/main.cpp src/flint_os.cpp src/flint_os.h; then
$CXX $CXXFLAGS -std=c++17 -DFLINTC_PROFILE src/main.cpp src/flint_os.cpp $LDFLAGS -o flintc_prof 2>/dev/null && \
  echo "  profiled compiler: ./flintc_prof" || \
  echo "  (skipped)"
else
    echo "  flintc_prof up to date (skipped)"
fi

echo "=== Building Flint runtime library ==="
if needs_rebuild runtime.o runtime/runtime.c; then
$CC -c $CFLAGS_RT runtime/runtime.c -o runtime.o
else echo "  runtime.o up to date (skipped)"; fi
if needs_rebuild runtime.bc runtime/runtime.c; then
$CC -c $CFLAGS_RT -emit-bc runtime/runtime.c -o runtime.bc 2>/dev/null || \
  $CC -c $CFLAGS_RT -emit-llvm runtime/runtime.c -o runtime.bc
else echo "  runtime.bc up to date (skipped)"; fi

echo "=== Building Flint Python runtime ==="
PYINC=$(python3-config --includes 2>/dev/null)
if needs_rebuild pyruntime.o runtime/pyruntime.c; then
if $CC -c $CFLAGS_RT $PYINC runtime/pyruntime.c -o pyruntime.o 2>/dev/null; then
    echo "  python runtime: ./pyruntime.o"
else
    echo "  (skipped - Python headers not available)"
    touch pyruntime.o 2>/dev/null || true
fi
else echo "  pyruntime.o up to date (skipped)"; fi

echo "=== Building Flint FFI helper ==="
if needs_rebuild ffi_helper.o examples/ffi_helper.c; then
$CC -c $CFLAGS_RT examples/ffi_helper.c -o ffi_helper.o
fi
echo "  ffi helper:   ./ffi_helper.o"

echo "=== Building Flint tensor runtime (parallel) ==="
( needs_rebuild flint_tensor.o runtime/flint_tensor.c && $CC -c $CFLAGS_RT runtime/flint_tensor.c -o flint_tensor.o ) &
( needs_rebuild flint_ai.o runtime/flint_ai.c && $CC -c $CFLAGS_RT runtime/flint_ai.c -o flint_ai.o ) &
( needs_rebuild flint_ai_opt.o runtime/flint_ai_opt.c && $CC -c $CFLAGS_RT runtime/flint_ai_opt.c -o flint_ai_opt.o ) &
wait
echo "  tensor:       ./flint_tensor.o"
echo "  flint_ai:     ./flint_ai.o"
echo "  flint_ai_opt: ./flint_ai_opt.o"

echo "=== Building Flint Standard Library (parallel) ==="
( needs_rebuild flint_serial.o runtime/flint_serial.c && $CC -c $CFLAGS_RT runtime/flint_serial.c -o flint_serial.o ) &
( needs_rebuild flint_crypto.o runtime/flint_crypto.c && $CC -c $CFLAGS_RT runtime/flint_crypto.c -o flint_crypto.o ) &
( needs_rebuild flint_net.o runtime/flint_net.c && $CC -c $CFLAGS_RT runtime/flint_net.c -o flint_net.o ) &
( needs_rebuild flint_aegis.o runtime/flint_aegis.c && $CC -c $CFLAGS_RT runtime/flint_aegis.c -o flint_aegis.o ) &
( needs_rebuild flint_chan.o runtime/flint_chan.c && $CC -c $CFLAGS_RT runtime/flint_chan.c -o flint_chan.o ) &
wait
echo "  flint_serial: ./flint_serial.o"
echo "  flint_crypto: ./flint_crypto.o"
echo "  flint_net:    ./flint_net.o"
echo "  flint_aegis:  ./flint_aegis.o"
echo "  flint_chan:   ./flint_chan.o"

echo "=== Build complete ==="
echo "  compiler:     ./flintc"
echo "  profiled:     ./flintc_prof"
echo "  runtime:      ./runtime.o"
echo "  tensor:       ./flint_tensor.o"
echo "  flint_ai:     ./flint_ai.o"
echo "  flint_ai_opt: ./flint_ai_opt.o"
echo "  flint_serial: ./flint_serial.o"
echo "  flint_crypto: ./flint_crypto.o"
echo "  flint_net:    ./flint_net.o"
echo "  flint_aegis:  ./flint_aegis.o"
echo "  flint_chan:   ./flint_chan.o"
