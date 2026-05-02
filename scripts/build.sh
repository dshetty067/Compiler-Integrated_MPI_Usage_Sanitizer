#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
INSTALL_DIR="$SCRIPT_DIR/install"

echo "============================================"
echo "  MPI Sanitizer Build Script"
echo "============================================"
echo ""

# ---------------------------------------------------------------------------
# 1. Install system dependencies
# ---------------------------------------------------------------------------
echo "[1/4] Installing system dependencies..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    llvm-18 llvm-18-dev clang-18 \
    libopenmpi-dev openmpi-bin \
    cmake make git \
    python3 python3-pip \
    binutils-dev \
    2>/dev/null | tail -5

# Symlink llvm-config if needed
if ! command -v llvm-config &>/dev/null; then
    ln -sf /usr/bin/llvm-config-18 /usr/local/bin/llvm-config
fi
if ! command -v clang &>/dev/null; then
    ln -sf /usr/bin/clang-18 /usr/local/bin/clang
    ln -sf /usr/bin/clang++-18 /usr/local/bin/clang++
fi
if ! command -v opt &>/dev/null; then
    ln -sf /usr/bin/opt-18 /usr/local/bin/opt
fi

echo "  LLVM:  $(llvm-config --version)"
echo "  Clang: $(clang --version | head -1)"
echo "  MPI:   $(mpicc --version | head -1)"
echo "  CMake: $(cmake --version | head -1)"

# ---------------------------------------------------------------------------
# 2. Build LLVM pass + runtime library
# ---------------------------------------------------------------------------
echo ""
echo "[2/4] Configuring with CMake..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DLLVM_DIR="$(llvm-config --cmakedir)" \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_VERBOSE_MAKEFILE=OFF \
    2>&1 | tail -10

echo ""
echo "[3/4] Building..."
cmake --build . --parallel "$(nproc)" 2>&1 | tail -20

cmake --install . --prefix "$INSTALL_DIR" 2>&1 | tail -5

echo ""
echo "[4/4] Building test cases..."
cd "$SCRIPT_DIR/testcases"

MPICC="mpicc"
MPIFLAGS="-g -O1"
MPISAN_RT="$BUILD_DIR/libmpisan_rt.a"
MPI_LIBS="$(mpicc --showme:link)"

build_test() {
    local src="$1"
    local out="$2"
    echo "  Building $src -> $out"
    $MPICC $MPIFLAGS -o "$out" "$src" "$MPISAN_RT" $MPI_LIBS -lpthread 2>&1 | \
        sed 's/^/    /' || true
}

for f in t01 t02 t03 t05 t07 t08 t09 t10 t12 t15 t16 t17; do
    src="${SCRIPT_DIR}/testcases/${f}_*.c"
    for s in $src; do
        base=$(basename "$s" .c)
        build_test "$s" "${SCRIPT_DIR}/testcases/${base}"
    done
done

# Deadlock tests — build but don't run automatically (they'll hang)
for f in t04_deadlock t06_collective_mismatch t13_tag_mismatch t11_allreduce_type_mismatch t14_allreduce_alias; do
    s="${SCRIPT_DIR}/testcases/${f}.c"
    if [ -f "$s" ]; then
        echo "  Building $f (deadlock/hang test — run manually)"
        $MPICC $MPIFLAGS -o "${SCRIPT_DIR}/testcases/${f}" "$s" "$MPISAN_RT" $MPI_LIBS -lpthread 2>&1 | \
            sed 's/^/    /' || true
    fi
done

# Mini-apps
for src in miniapp_heat1d_buggy.c miniapp_heat1d_clean.c; do
    base="${src%.c}"
    build_test "${SCRIPT_DIR}/testcases/${src}" "${SCRIPT_DIR}/testcases/${base}"
done

echo ""
echo "============================================"
echo "  Build complete!"
echo "  Pass plugin: $BUILD_DIR/MPISanitizerPass.so"
echo "  Runtime lib: $BUILD_DIR/libmpisan_rt.a"
echo "  Testcases:   $SCRIPT_DIR/testcases/"
echo ""
echo "  Next: ./run.sh"
echo "============================================"