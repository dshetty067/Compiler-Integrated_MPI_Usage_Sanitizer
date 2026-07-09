#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SOURCE_FILE="${1:-$SCRIPT_DIR/test.c}"
NPROCS="${2:-${MPISAN_NPROCS:-4}}"
TIMEOUT="${MPISAN_TIMEOUT:-15}"
VERBOSE="${MPISAN_VERBOSE:-1}"
export MPISAN_VERBOSE="$VERBOSE"

MPIRUN="${MPIRUN:-mpirun}"
MPIRUN_FLAGS="${MPIRUN_FLAGS:---allow-run-as-root --oversubscribe}"

BUILD_DIR="${MPISAN_BUILD_DIR:-$SCRIPT_DIR/build}"
if [ -z "${MPISAN_BUILD_DIR:-}" ] && [ -f "$SCRIPT_DIR/.mpisan_build_dir" ]; then
    BUILD_DIR="$(cat "$SCRIPT_DIR/.mpisan_build_dir")"
fi
if [ -z "${MPISAN_BUILD_DIR:-}" ] &&
   [ -f "$SCRIPT_DIR/build.local/MPISanitizerPass.so" ] &&
   [ -f "$SCRIPT_DIR/build.local/libmpisan_rt.a" ]; then
    if [ ! -w "$BUILD_DIR" ] ||
       [ "$SCRIPT_DIR/build.local/libmpisan_rt.a" -nt "$BUILD_DIR/libmpisan_rt.a" ]; then
        BUILD_DIR="$SCRIPT_DIR/build.local"
    fi
fi

PASS_PLUGIN="$BUILD_DIR/MPISanitizerPass.so"
RUNTIME_LIB="$BUILD_DIR/libmpisan_rt.a"
MPISAN_USE_PASS="${MPISAN_USE_PASS:-1}"

RESULTS_DIR="${MPISAN_CUSTOM_RESULTS_DIR:-$SCRIPT_DIR/custom_results}"
if [ -z "${MPISAN_CUSTOM_RESULTS_DIR:-}" ] && [ -e "$RESULTS_DIR" ] && [ ! -w "$RESULTS_DIR" ]; then
    RESULTS_DIR="$SCRIPT_DIR/custom_results.local"
fi
mkdir -p "$RESULTS_DIR"

BINARY="$RESULTS_DIR/custom_mpi_program"
LOG_FILE="$RESULTS_DIR/custom_run.log"

red() { printf '\033[0;31m%s\033[0m\n' "$*"; }
green() { printf '\033[0;32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[1;33m%s\033[0m\n' "$*"; }
blue() { printf '\033[0;34m%s\033[0m\n' "$*"; }

require_file() {
    local path="$1"
    local hint="$2"
    if [ ! -f "$path" ]; then
        red "Missing: $path"
        printf '%s\n' "$hint"
        exit 2
    fi
}

require_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        red "Missing command: $cmd"
        printf 'Install/build dependencies first, then try again.\n'
        exit 2
    fi
}

blue "=== MPI Sanitizer Custom Runner ==="
printf 'Source:  %s\n' "$SOURCE_FILE"
printf 'Ranks:   %s\n' "$NPROCS"
printf 'Timeout: %ss\n' "$TIMEOUT"
printf 'Mode:    %s\n' "$([ "$MPISAN_USE_PASS" = "1" ] && printf 'LLVM pass + runtime' || printf 'runtime wrappers only')"
printf '\n'

require_file "$SOURCE_FILE" "Create test.c or pass a source file path, for example: ./run_custom.sh myprog.c 4"
require_file "$RUNTIME_LIB" "Run ./build.sh first so libmpisan_rt.a exists."
if [ "$MPISAN_USE_PASS" = "1" ]; then
    require_file "$PASS_PLUGIN" "Run ./build.sh first so MPISanitizerPass.so exists."
    require_cmd opt
fi
require_cmd clang
require_cmd mpicc
require_cmd "$MPIRUN"
require_cmd timeout

TMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

LL_FILE="$TMP_DIR/custom.ll"
INST_FILE="$TMP_DIR/custom_inst.bc"

blue "Compiling..."
if [ "$MPISAN_USE_PASS" = "1" ]; then
    clang -S -emit-llvm -g -O1 $(mpicc --showme:compile) "$SOURCE_FILE" -o "$LL_FILE"
    opt -load-pass-plugin "$PASS_PLUGIN" -passes="mpi-sanitizer" "$LL_FILE" -o "$INST_FILE"
    clang "$INST_FILE" "$RUNTIME_LIB" $(mpicc --showme:link) -lpthread -o "$BINARY"
else
    clang -g -O1 $(mpicc --showme:compile) "$SOURCE_FILE" "$RUNTIME_LIB" $(mpicc --showme:link) -lpthread -o "$BINARY"
fi
green "Compiled: $BINARY"

blue "Running with mpirun..."
EXIT_CODE=0
timeout "$TIMEOUT" $MPIRUN $MPIRUN_FLAGS -np "$NPROCS" "$BINARY" > "$LOG_FILE" 2>&1 || EXIT_CODE=$?

ERROR_COUNT="$(grep -c '\[MPISAN\].*ERROR' "$LOG_FILE" 2>/dev/null || true)"
ERROR_COUNT="${ERROR_COUNT// /}"
ERROR_COUNT="${ERROR_COUNT//$'\n'/}"
[ -z "$ERROR_COUNT" ] && ERROR_COUNT=0

printf '\n'
if [ "$EXIT_CODE" -eq 124 ]; then
    yellow "RESULT: TIMEOUT after ${TIMEOUT}s. The MPI program may have hung or deadlocked."
elif [ "$ERROR_COUNT" -gt 0 ]; then
    red "RESULT: MPISAN detected $ERROR_COUNT error(s)."
elif [ "$EXIT_CODE" -ne 0 ]; then
    yellow "RESULT: No MPISAN errors were found, but the program exited with code $EXIT_CODE."
else
    green "RESULT: Clean run. No MPISAN errors detected."
fi

printf '\nLog file: %s\n' "$LOG_FILE"

if [ "$ERROR_COUNT" -gt 0 ]; then
    printf '\nDetected errors:\n'
    grep '\[MPISAN\].*ERROR' "$LOG_FILE" || true
fi

if [ "$EXIT_CODE" -eq 124 ]; then
    exit 124
elif [ "$ERROR_COUNT" -gt 0 ]; then
    exit 1
else
    exit "$EXIT_CODE"
fi
