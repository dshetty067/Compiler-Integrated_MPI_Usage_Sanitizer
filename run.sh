#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TC_DIR="$SCRIPT_DIR/testcases"
RESULTS_DIR="${MPISAN_RESULTS_DIR:-$SCRIPT_DIR/results}"
if [ -z "${MPISAN_RESULTS_DIR:-}" ] && [ -e "$RESULTS_DIR" ] && [ ! -w "$RESULTS_DIR" ]; then
    RESULTS_DIR="$SCRIPT_DIR/results.local"
fi
REPORT="$RESULTS_DIR/evaluation_report.txt"

mkdir -p "$RESULTS_DIR"

# Modern, clean terminal colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "$@"; }

NPROCS="${MPISAN_NPROCS:-4}"
TIMEOUT="${MPISAN_TIMEOUT:-15}"
VERBOSE="${MPISAN_VERBOSE:-1}"
export MPISAN_VERBOSE

MPIRUN="${MPIRUN:-mpirun}"
# Added Open MPI timeout flag directly to the flags to prevent internal runner deadlocks
MPIRUN_FLAGS="${MPIRUN_FLAGS:---allow-run-as-root --oversubscribe --timeout $TIMEOUT}"

BUILD_DIR="${MPISAN_BUILD_DIR:-$SCRIPT_DIR/build}"
if [ -z "${MPISAN_BUILD_DIR:-}" ] && [ -f "$SCRIPT_DIR/.mpisan_build_dir" ]; then
    BUILD_DIR="$(cat "$SCRIPT_DIR/.mpisan_build_dir")"
elif [ -z "${MPISAN_BUILD_DIR:-}" ] && [ -e "$BUILD_DIR" ] && [ ! -w "$BUILD_DIR" ]; then
    BUILD_DIR="$SCRIPT_DIR/build.local"
fi
PASS_PLUGIN="$BUILD_DIR/MPISanitizerPass.so"
RUNTIME_LIB="$BUILD_DIR/libmpisan_rt.a"
MPISAN_USE_PASS="${MPISAN_USE_PASS:-0}"

compile_one() {
    local name="$1" src="$2" binary="$3"
    local temp_dir ll_file inst_file
    local compile_log="$RESULTS_DIR/${name}.compile.log"
    : > "$compile_log"

    if [ ! -f "$src" ]; then return 1; fi
    if [ ! -f "$PASS_PLUGIN" ] || [ ! -f "$RUNTIME_LIB" ]; then return 1; fi

    if [ -f "$binary" ] && [ "$binary" -nt "$src" ] \
       && [ "$binary" -nt "$PASS_PLUGIN" ] && [ "$binary" -nt "$RUNTIME_LIB" ]; then
        return 0
    fi

    if [ "$MPISAN_USE_PASS" != "1" ]; then
        clang -g -O1 $(mpicc --showme:compile) "$src" "$RUNTIME_LIB" \
              $(mpicc --showme:link) -lpthread -o "$binary" > "$compile_log" 2>&1
        return $?
    fi

    temp_dir="$(mktemp -d)"
    ll_file="$temp_dir/${name}.ll"
    inst_file="$temp_dir/${name}.bc"

    if ! clang -S -emit-llvm -g -O1 $(mpicc --showme:compile) "$src" -o "$ll_file" >> "$compile_log" 2>&1; then
        rm -rf "$temp_dir"; return 1
    fi
    if ! opt -load-pass-plugin "$PASS_PLUGIN" -passes="mpi-sanitizer" "$ll_file" -o "$inst_file" >> "$compile_log" 2>&1; then
        rm -rf "$temp_dir"; return 1
    fi
    if ! clang "$inst_file" "$RUNTIME_LIB" $(mpicc --showme:link) -lpthread -o "$binary" >> "$compile_log" 2>&1; then
        rm -rf "$temp_dir"; return 1
    fi

    rm -rf "$temp_dir"
    return 0
}

# ---------------------------------------------------------------------------
# Execution and Scanning Loop
# ---------------------------------------------------------------------------
log ""
log "${BLUE}======================================================${NC}"
log "${BLUE}       MPI Sanitizer — Test Suite Engine             ${NC}"
log "${BLUE}======================================================${NC}"
log "Running with NPROCS=$NPROCS | TIMEOUT=${TIMEOUT}s"
log "------------------------------------------------------"

> "$RESULTS_DIR/results_raw.txt"

shopt -s nullglob
c_files=("$TC_DIR"/*.c)
shopt -u nullglob

if [ "${#c_files[@]}" -eq 0 ]; then
    log "${RED}[!] No target source files found in $TC_DIR${NC}"
    exit 1
fi

TOTAL_TESTS=0
ERRORS_FOUND=0
CLEAN_RUNS=0

for src in "${c_files[@]}"; do
    name="$(basename "$src" .c)"
    binary="$TC_DIR/$name"
    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    log "${CYAN}[*] Scanning:${NC} ${name}.c"

    # Compile 
    if ! compile_one "$name" "$src" "$binary"; then
        log "    ${RED}✗ Compilation Failed${NC} (Check $RESULTS_DIR/${name}.compile.log)"
        echo "$name: COMPILE_FAILED" >> "$RESULTS_DIR/results_raw.txt"
        continue
    fi

    # Run Binary
    log_file="$RESULTS_DIR/${name}.log"
    exit_code=0
    
    # FIX: Added --kill-after=2 to send SIGKILL if SIGTERM fails to stop the hanging MPI ranks
    timeout --kill-after=2 "$TIMEOUT" $MPIRUN $MPIRUN_FLAGS -np "$NPROCS" "$binary" > "$log_file" 2>&1 || exit_code=$?

    # Extract MPISAN logs dynamically
    error_count=$(grep -c "\[MPISAN\].*ERROR" "$log_file" 2>/dev/null || echo 0)
    
    # 124 is standard timeout exit code, 137 indicates it was forced killed via SIGKILL (still a timeout)
    if [ "$exit_code" -eq 124 ] || [ "$exit_code" -eq 137 ]; then
        log "    ${YELLOW}⏱ HANG / TIMEOUT${NC} (Exceeded ${TIMEOUT}s limit — Deadlock Stopped)"
        echo "$name: TIMEOUT" >> "$RESULTS_DIR/results_raw.txt"
        ERRORS_FOUND=$((ERRORS_FOUND + 1))
        
    elif [ "$error_count" -gt 0 ]; then
        log "    ${RED}✗ SANITIZER ERROR(S) DETECTED:${NC}"
        grep "\[MPISAN\].*ERROR" "$log_file" | sed 's/^/      ➔ /'
        
        echo "$name: SANITIZER_ERROR ($error_count)" >> "$RESULTS_DIR/results_raw.txt"
        ERRORS_FOUND=$((ERRORS_FOUND + 1))
        
    elif [ "$exit_code" -ne 0 ]; then
        log "    ${RED}✗ RUNTIME CRASH${NC} (Exit Code: $exit_code)"
        log "    ${YELLOW}Last line of output:${NC}"
        tail -n 1 "$log_file" | sed 's/^/      ➔ /'
        
        echo "$name: CRASHED" >> "$RESULTS_DIR/results_raw.txt"
        ERRORS_FOUND=$((ERRORS_FOUND + 1))
        
    else
        log "    ${GREEN}✓ CLEAN RUN${NC}"
        echo "$name: CLEAN" >> "$RESULTS_DIR/results_raw.txt"
        CLEAN_RUNS=$((CLEAN_RUNS + 1))
    fi
done

# ---------------------------------------------------------------------------
# Simple Terminal Summary Output
# ---------------------------------------------------------------------------
log "\n${BLUE}------------------------------------------------------${NC}"
log "  Execution Summary:"
log "  Total Files Scanned : $TOTAL_TESTS"
log "  Clean Operations    : $CLEAN_RUNS"
log "  Issues Flagged      : $ERRORS_FOUND"
log "${BLUE}------------------------------------------------------${NC}"

{
  echo "MPI Sanitizer Scan Log"
  echo "Generated: $(date)"
  echo "----------------------------------------"
  cat "$RESULTS_DIR/results_raw.txt"
} > "$REPORT"

exit 0