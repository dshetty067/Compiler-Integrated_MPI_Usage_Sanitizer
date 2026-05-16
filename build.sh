#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${MPISAN_BUILD_DIR:-$SCRIPT_DIR/build}"
if [ -z "${MPISAN_BUILD_DIR:-}" ] && [ -e "$BUILD_DIR" ] && [ ! -w "$BUILD_DIR" ]; then
    BUILD_DIR="$SCRIPT_DIR/build.local"
fi
INSTALL_DIR="${MPISAN_INSTALL_DIR:-$SCRIPT_DIR/install}"
if [ -z "${MPISAN_INSTALL_DIR:-}" ] && [ -e "$INSTALL_DIR" ] && [ ! -w "$INSTALL_DIR" ]; then
    INSTALL_DIR="$SCRIPT_DIR/install.local"
fi
echo "$BUILD_DIR" > "$SCRIPT_DIR/.mpisan_build_dir"

echo "============================================"
echo "  MPI Sanitizer Build Script"
echo "============================================"
echo ""

# ---------------------------------------------------------------------------
# 1. Install system dependencies
# ---------------------------------------------------------------------------
echo "[1/4] Installing system dependencies..."
if command -v clang >/dev/null &&
   command -v opt >/dev/null &&
   command -v llvm-config >/dev/null &&
   command -v mpicc >/dev/null &&
   command -v cmake >/dev/null; then
    echo "  Required tools already available; skipping apt install."
elif [ "$(id -u)" -eq 0 ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq \
        llvm-18 llvm-18-dev clang-18 \
        libopenmpi-dev openmpi-bin \
        cmake make git \
        python3 python3-pip \
        binutils-dev \
        2>/dev/null | tail -5
else
    echo "  Missing build dependencies and not running as root."
    echo "  Install clang/LLVM, OpenMPI, CMake, and make, then rerun ./build.sh."
    exit 1
fi

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
CLANG="${CLANG:-clang}"
OPT="${OPT:-opt}"
MPISAN_RT="$BUILD_DIR/libmpisan_rt.a"
PASS_PLUGIN="$BUILD_DIR/MPISanitizerPass.so"
MPI_LIBS="$(mpicc --showme:link)"
MPI_CFLAGS="$(mpicc --showme:compile)"
MPISAN_USE_PASS="${MPISAN_USE_PASS:-0}"

build_test() {
    local src="$1"
    local out="$2"
    local base
    local temp_dir
    local ll_file
    local inst_file
    base="$(basename "$out")"
    temp_dir="$(mktemp -d)"
    ll_file="$temp_dir/${base}.ll"
    inst_file="$temp_dir/${base}.bc"
    echo "  Building $src -> $out"
    if [ "$MPISAN_USE_PASS" = "1" ]; then
        if $CLANG -S -emit-llvm $MPIFLAGS $MPI_CFLAGS "$src" -o "$ll_file" 2>&1 | sed 's/^/    /' &&
           $OPT -load-pass-plugin "$PASS_PLUGIN" -passes="mpi-sanitizer" "$ll_file" -o "$inst_file" 2>&1 | sed 's/^/    /' &&
           $CLANG "$inst_file" "$MPISAN_RT" $MPI_LIBS -lpthread -o "$out" 2>&1 | sed 's/^/    /'; then
            :
        else
            echo "    Instrumented build failed for $src"
        fi
    else
        if $CLANG $MPIFLAGS $MPI_CFLAGS "$src" "$MPISAN_RT" $MPI_LIBS -lpthread -o "$out" 2>&1 | sed 's/^/    /'; then
            :
        else
            echo "    Runtime-wrapper build failed for $src"
        fi
    fi
    rm -rf "$temp_dir"
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
        build_test "$s" "${SCRIPT_DIR}/testcases/${f}"
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
