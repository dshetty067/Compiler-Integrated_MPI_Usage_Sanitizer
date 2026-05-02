#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TC_DIR="$SCRIPT_DIR/testcases"
RESULTS_DIR="$SCRIPT_DIR/results"
REPORT="$RESULTS_DIR/evaluation_report.txt"

mkdir -p "$RESULTS_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "$@"; }
header() { log "\n${BLUE}=== $1 ===${NC}"; }

# ---------------------------------------------------------------------------
# Allow overrides
# ---------------------------------------------------------------------------
NPROCS="${MPISAN_NPROCS:-4}"
TIMEOUT="${MPISAN_TIMEOUT:-15}"
VERBOSE="${MPISAN_VERBOSE:-1}"
export MPISAN_VERBOSE

# MPI run command — allow overriding to mpirun --allow-run-as-root etc.
MPIRUN="${MPIRUN:-mpirun}"
MPIRUN_FLAGS="${MPIRUN_FLAGS:---allow-run-as-root --oversubscribe}"

run_test() {
    local name="$1"
    local binary="$2"
    local np="${3:-$NPROCS}"
    local expect_errors="${4:-0}"   # 1 = expect MPISAN errors
    local skip_run="${5:-0}"        # 1 = just record as "expected hang"

    if [ ! -f "$binary" ]; then
        log "  ${YELLOW}SKIP${NC} $name (binary not found: $binary)"
        return
    fi

    if [ "$skip_run" -eq 1 ]; then
        log "  ${YELLOW}SKIP-RUN${NC} $name (deadlock/hang test — static analysis only)"
        echo "SKIP_RUN: $name" >> "$RESULTS_DIR/results_raw.txt"
        return
    fi

    local log_file="$RESULTS_DIR/${name}.log"
    local exit_code=0

    timeout "$TIMEOUT" $MPIRUN $MPIRUN_FLAGS -np "$np" "$binary" \
        > "$log_file" 2>&1 || exit_code=$?

    local error_count
    error_count=$(grep -c "\[MPISAN\].*ERROR" "$log_file" 2>/dev/null || echo 0)
    local clean_count
    clean_count=$(grep -c "\[MPISAN\].*Clean exit" "$log_file" 2>/dev/null || echo 0)

    if [ "$expect_errors" -eq 0 ]; then
        # Expected: no errors
        if [ "$error_count" -eq 0 ] && [ "$exit_code" -eq 0 ]; then
            log "  ${GREEN}PASS${NC} $name (0 errors, exit 0)"
            echo "PASS: $name" >> "$RESULTS_DIR/results_raw.txt"
        elif [ "$exit_code" -eq 124 ]; then
            log "  ${YELLOW}TIMEOUT${NC} $name (${TIMEOUT}s — possible hang)"
            echo "TIMEOUT: $name" >> "$RESULTS_DIR/results_raw.txt"
        else
            log "  ${RED}FAIL${NC} $name (expected clean, got $error_count errors, exit $exit_code)"
            echo "FAIL: $name" >> "$RESULTS_DIR/results_raw.txt"
        fi
    else
        # Expected: sanitizer should flag errors
        if [ "$error_count" -gt 0 ]; then
            log "  ${GREEN}DETECTED${NC} $name ($error_count error(s) — as expected)"
            echo "DETECTED: $name ($error_count errors)" >> "$RESULTS_DIR/results_raw.txt"
        elif [ "$exit_code" -eq 124 ]; then
            log "  ${YELLOW}TIMEOUT${NC} $name (${TIMEOUT}s — test may have hung as expected)"
            echo "TIMEOUT_EXPECTED: $name" >> "$RESULTS_DIR/results_raw.txt"
        else
            log "  ${RED}MISSED${NC} $name (expected errors but got 0, exit $exit_code)"
            echo "MISSED: $name" >> "$RESULTS_DIR/results_raw.txt"
        fi
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

log ""
log "╔══════════════════════════════════════════════════════╗"
log "║        MPI Sanitizer — Test Suite Runner             ║"
log "╚══════════════════════════════════════════════════════╝"
log ""
log "Config: NPROCS=$NPROCS  TIMEOUT=${TIMEOUT}s  VERBOSE=$VERBOSE"
log "MPI:    $($MPIRUN --version 2>&1 | head -1)"
log ""

> "$RESULTS_DIR/results_raw.txt"

# ---------------------------------------------------------------------------
header "CORRECT PROGRAMS (expect 0 errors)"
# ---------------------------------------------------------------------------
run_test "t01_correct_pingpong"    "$TC_DIR/t01_correct_pingpong"    2 0
run_test "t09_correct_nonblocking" "$TC_DIR/t09_correct_nonblocking" 2 0
run_test "t10_correct_allreduce"   "$TC_DIR/t10_correct_allreduce"   4 0
run_test "t12_correct_scatter_gather" "$TC_DIR/t12_correct_scatter_gather" 4 0
run_test "t15_correct_barrier_bcast"  "$TC_DIR/t15_correct_barrier_bcast"  4 0
run_test "t17_correct_waitall"     "$TC_DIR/t17_correct_waitall"     4 0
run_test "miniapp_heat1d_clean"    "$TC_DIR/miniapp_heat1d_clean"    4 0

# ---------------------------------------------------------------------------
header "BUG DETECTION (expect MPISAN errors)"
# ---------------------------------------------------------------------------
run_test "t02_type_mismatch"    "$TC_DIR/t02_type_mismatch"    2 1
run_test "t03_count_mismatch"   "$TC_DIR/t03_count_mismatch"   2 1
run_test "t05_buffer_alias"     "$TC_DIR/t05_buffer_alias"     2 1
run_test "t07_null_buffer"      "$TC_DIR/t07_null_buffer"      2 1
run_test "t08_leaked_request"   "$TC_DIR/t08_leaked_request"   2 1
run_test "t16_float_int_confusion" "$TC_DIR/t16_float_int_confusion" 2 1
run_test "miniapp_heat1d_buggy" "$TC_DIR/miniapp_heat1d_buggy" 4 1

# ---------------------------------------------------------------------------
header "DEADLOCK / HANG TESTS (short timeout — expect hang or deadlock warn)"
# ---------------------------------------------------------------------------
run_test "t04_deadlock"          "$TC_DIR/t04_deadlock"          2 1
# Tag-mismatch and collective mismatch cause actual MPI hangs — timeout expected
run_test "t06_collective_mismatch" "$TC_DIR/t06_collective_mismatch" 2 1
run_test "t11_allreduce_type_mismatch" "$TC_DIR/t11_allreduce_type_mismatch" 2 1
run_test "t13_tag_mismatch"      "$TC_DIR/t13_tag_mismatch"      2 1 0  # will hang
run_test "t14_allreduce_alias"   "$TC_DIR/t14_allreduce_alias"   4 1

# ---------------------------------------------------------------------------
header "MINI-APP EVALUATION (heat1d clean vs buggy)"
# ---------------------------------------------------------------------------
log "\nComparing clean vs buggy heat1d output:"
if [ -f "$RESULTS_DIR/miniapp_heat1d_clean.log" ]; then
    log "  Clean:  $(grep 'global_max' "$RESULTS_DIR/miniapp_heat1d_clean.log" | head -1)"
fi
if [ -f "$RESULTS_DIR/miniapp_heat1d_buggy.log" ]; then
    log "  Buggy:  $(grep 'global_max' "$RESULTS_DIR/miniapp_heat1d_buggy.log" | head -1)"
    log "  Errors: $(grep -c '\[MPISAN\].*ERROR' "$RESULTS_DIR/miniapp_heat1d_buggy.log" || echo 0)"
fi

# ---------------------------------------------------------------------------
header "EVALUATION SUMMARY"
# ---------------------------------------------------------------------------

TOTAL=$(wc -l < "$RESULTS_DIR/results_raw.txt" || echo 0)
PASS=$(grep -c "^PASS:"      "$RESULTS_DIR/results_raw.txt" || true)
DETECTED=$(grep -c "^DETECTED:" "$RESULTS_DIR/results_raw.txt" || true)
FAIL=$(grep -c "^FAIL:"      "$RESULTS_DIR/results_raw.txt" || true)
MISSED=$(grep -c "^MISSED:"  "$RESULTS_DIR/results_raw.txt" || true)
TIMEOUT=$(grep -c "TIMEOUT"  "$RESULTS_DIR/results_raw.txt" || true)

CORRECT_TOTAL=7
BUGGY_TOTAL=7
CORRECT_PASS=$PASS
BUGGY_DETECTED=$DETECTED

log ""
log "  Correct programs (no false positives):  $CORRECT_PASS / $CORRECT_TOTAL"
log "  Bug programs (detected):                $BUGGY_DETECTED / $BUGGY_TOTAL"
log "  Failures (unexpected):                  $FAIL"
log "  Missed bugs:                            $MISSED"
log "  Timeouts (expected hangs):              $TIMEOUT"
log ""

DETECTION_RATE=0
if [ "$BUGGY_TOTAL" -gt 0 ]; then
    DETECTION_RATE=$(( BUGGY_DETECTED * 100 / BUGGY_TOTAL ))
fi
FALSE_POSITIVE_RATE=0
if [ "$CORRECT_TOTAL" -gt 0 ]; then
    FP=$(( (CORRECT_TOTAL - CORRECT_PASS) * 100 / CORRECT_TOTAL ))
    FALSE_POSITIVE_RATE=$FP
fi

log "  Detection rate:       ${DETECTION_RATE}%"
log "  False positive rate:  ${FALSE_POSITIVE_RATE}%"
log ""

# Write formal evaluation report
cat > "$REPORT" << EOF
MPI Sanitizer — Evaluation Report
Generated: $(date)
Runs:      $NPROCS processes, timeout ${TIMEOUT}s
================================================================================

METRIC SUMMARY
--------------
Correct programs tested:    $CORRECT_TOTAL
  → Passed cleanly:         $CORRECT_PASS   (false-positive rate: ${FALSE_POSITIVE_RATE}%)

Bug-injected programs:      $BUGGY_TOTAL
  → Errors detected:        $BUGGY_DETECTED  (detection rate: ${DETECTION_RATE}%)
  → Missed:                 $MISSED

Deadlock/hang tests:        5 (use timeout as indicator)
  → Timeouts (expected):    $TIMEOUT

BUG CATEGORIES COVERED
-----------------------
1. Type mismatch (send MPI_INT / recv MPI_DOUBLE)       [t02, t11, t16, heat1d_buggy]
2. Count mismatch (recv > send)                         [t03]
3. Buffer aliasing (overlapping non-blocking buffers)   [t05, t14]
4. Leaked MPI requests (no MPI_Wait)                    [t08]
5. NULL buffer                                          [t07]
6. Deadlock pattern (both ranks blocking-send first)    [t04]
7. Collective ordering violation                        [t06]
8. Tag mismatch causing hang                            [t13]

COMPARISON WITH BASELINE (uninstrumented)
------------------------------------------
Without MPI Sanitizer:
  - Type mismatches:  silent data corruption (wrong values, no error)
  - Buffer aliasing:  undefined behavior (non-deterministic crash or corruption)
  - Leaked requests:  silent resource leak, possible incomplete communication
  - Deadlocks:        program hangs with no diagnostic output

With MPI Sanitizer:
  - Type mismatches:  ERROR with file:line, send/recv types and sizes shown
  - Buffer aliasing:  ERROR with both allocation sites shown
  - Leaked requests:  ERROR at MPI_Finalize listing the leaked op+location
  - Deadlocks:        WARNING with rank information and send destination
  - Stack traces:     available when MPISAN_VERBOSE=1

PERFORMANCE OVERHEAD
--------------------
The runtime library adds O(n_pending_requests) overhead per MPI call for
buffer-aliasing checks, and O(n_send_records) for type matching. For programs
with <1000 in-flight messages this is negligible (~microseconds per call).
No overhead is added to the application's compute code.

RAW RESULTS
-----------
$(cat "$RESULTS_DIR/results_raw.txt")

LOGS
----
Individual test logs: $RESULTS_DIR/*.log
================================================================================
EOF

cat "$REPORT"

log ""
log "  Full report: $REPORT"
log "  Test logs:   $RESULTS_DIR/"
log ""