#!/data/data/com.termux/files/usr/bin/bash
set -e

CXX=clang++
# -O2 for speed (llvm-config does not add an -O flag here, so the compiler was
# previously built unoptimized). Hardening flags add stack-smashing protection
# and fortified libc calls at negligible cost.
OPT="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2"
CXXFLAGS="$(llvm-config --cxxflags | sed 's/-Werror//g') $OPT"
LDFLAGS="$(llvm-config --ldflags) $(llvm-config --libs) -lc++_shared"

# Runtime C files get the same hardening.
CFLAGS_RT="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2"

echo "=== Building Flint compiler (flintc) ==="
$CXX $CXXFLAGS -std=c++17 src/main.cpp $LDFLAGS -o flintc

echo "=== Building profiled build (FLINTC_PROFILE) ==="
$CXX $CXXFLAGS -std=c++17 -DFLINTC_PROFILE src/main.cpp $LDFLAGS -o flintc_prof 2>/dev/null && \
  echo "  profiled compiler: ./flintc_prof" || \
  echo "  (skipped)"

echo "=== Building Flint runtime library ==="
clang -c $CFLAGS_RT runtime/runtime.c -o runtime.o
clang -c $CFLAGS_RT -emit-bc runtime/runtime.c -o runtime.bc 2>/dev/null || \
  clang -c $CFLAGS_RT -emit-llvm runtime/runtime.c -o runtime.bc

echo "=== Building Flint Python runtime ==="
PYINC=$(python3-config --includes 2>/dev/null)
if clang -c $CFLAGS_RT $PYINC runtime/pyruntime.c -o pyruntime.o 2>/dev/null; then
    echo "  python runtime: ./pyruntime.o"
else
    echo "  (skipped - Python headers not available)"
    touch pyruntime.o 2>/dev/null || true
fi

echo "=== Building Flint FFI helper ==="
clang -c $CFLAGS_RT examples/ffi_helper.c -o ffi_helper.o
echo "  ffi helper:   ./ffi_helper.o"

echo "=== Building Flint tensor runtime ==="
clang -c $CFLAGS_RT runtime/flint_tensor.c -o flint_tensor.o
echo "  tensor:       ./flint_tensor.o"

echo "=== Building Flint AI Engine (v2 architecture) ==="
clang -c $CFLAGS_RT runtime/flint_ai.c -o flint_ai.o
echo "  flint_ai:     ./flint_ai.o"

echo "=== Building Flint AI Optimizer (SIMD + MT + Sparse + f32) ==="
clang -c $CFLAGS_RT runtime/flint_ai_opt.c -o flint_ai_opt.o
echo "  flint_ai_opt: ./flint_ai_opt.o"

echo "=== Building Flint Standard Library ==="
clang -c $CFLAGS_RT runtime/flint_serial.c -o flint_serial.o
echo "  flint_serial: ./flint_serial.o"
clang -c $CFLAGS_RT runtime/flint_crypto.c -o flint_crypto.o
echo "  flint_crypto: ./flint_crypto.o"
clang -c $CFLAGS_RT runtime/flint_net.c -o flint_net.o
echo "  flint_net:    ./flint_net.o"

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
