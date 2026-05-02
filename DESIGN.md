# Design Document — MPI Sanitizer

## Problem Statement

MPI programs are notoriously hard to debug. The most common classes of bugs:

1. **Type mismatches** — `MPI_Send(..., MPI_INT, ...)` matched with `MPI_Recv(..., MPI_DOUBLE, ...)`. The MPI standard says this is undefined behavior; in practice it silently corrupts data.

2. **Buffer aliasing** — reusing a buffer while a non-blocking operation on it is still in flight. The data received may be garbage, and the outcome depends on scheduling.

3. **Collective ordering violations** — all processes in a communicator must call the same collective in the same order. Calling `MPI_Bcast` on rank 0 while rank 1 calls `MPI_Reduce` is undefined.

4. **Deadlocks** — the classic case: rank 0 does `MPI_Send` to rank 1, rank 1 does `MPI_Send` to rank 0 — both block waiting for a recv that never comes.

Existing tools like MUST (RWTH Aachen) detect many of these, but operate as a standalone Java-based external tool that intercepts PMPI. This has limitations:
- Cannot access LLVM IR type information
- Cannot elide checks the compiler could prove unnecessary
- Requires a separate multi-step setup
- Source locations are derived from DWARF but sometimes miss nested inlining

---

## Design Goals

| Goal | Approach |
|---|---|
| Detect the 4 bug classes above | LLVM pass + runtime library |
| Accurate source locations | Extract from LLVM DebugLoc at instrumentation time |
| Low overhead | Compact runtime tables; O(1) or O(n) checks per MPI call |
| Easy to use | Single shared library, one compiler flag |
| Compiler-integrated | Can leverage IR type info for future extensions |

---

## Architecture Overview

### Component 1: LLVM Module Pass (`MPISanitizerPass`)

- Runs as a loadable plugin (`MPISanitizerPass.so`)
- Registered for the `mpi-sanitizer` pipeline stage and also the optimizer-last hook (runs at any optimization level)
- Scans every `CallInst` in the module for functions named `MPI_*` or `PMPI_*`
- For each call site, inserts a call to the corresponding runtime hook immediately **before** the MPI call
- Extracts `buf`, `count`, `datatype`, `src/dest`, `tag`, `comm`, and `request` arguments directly from IR
- Reads `DebugLoc` metadata to embed file:line into every hook call as string literal constants

### Component 2: Runtime Library (`libmpisan_rt`)

- Plain C library (no C++ runtime dependency)
- Uses a flat array-based message record table (no dynamic allocation in hot path)
- Uses `pthread_mutex` for thread safety (MPI threads can call from multiple OS threads)
- All error output to stderr (or `MPISAN_LOG`) with rank prefix
- Calls `backtrace()` / `backtrace_symbols()` for optional stack traces

---

## Data Structures

### Send Record Table (`g_send_table[4096]`)

```c
struct MsgRecord {
  active, src_rank, dst_rank, tag, comm,
  mpi_type, count, buf_addr, buf_bytes,
  file, line
};
```

Populated on every `__mpisan_send()` call. Searched on every `__mpisan_recv()` to find matching send for type comparison. Record is consumed (marked inactive) when matched.

### Non-blocking Request Table (`g_req_table[2048]`)

```c
struct ReqRecord {
  active, req_ptr (address of MPI_Request),
  kind (SEND/RECV), buf, count, mpi_type,
  peer, tag, comm, file, line
};
```

Populated on `Isend`/`Irecv`. Cleared on `Wait`/`Waitall`. Audited at `Finalize` for leaks. Also checked for buffer overlap on each new operation.

### Collective Sequence Tracker

Per-communicator sequence number + circular history buffer. Used to record the order of collectives on each rank for ordering-violation analysis.

---

## Detection Algorithms

### Type Mismatch

On `__mpisan_recv(buf, count, recv_type, src, tag, comm, ...)`:
1. Search send table for a record with matching `(comm, tag, dst_rank=rank)`
2. If found: compare `send_type != recv_type` → ERROR
3. If `recv_count > send_count` → ERROR (overread)
4. Mark record consumed

### Buffer Aliasing

On `__mpisan_isend` / `__mpisan_irecv`:
1. Compute `[buf, buf + count*sizeof(type))` 
2. For each active request in `g_req_table`: check if ranges overlap
3. If overlap → ERROR

### Deadlock Heuristic

On `__mpisan_send(..., is_blocking=1)`:
1. Check `g_last_blocked_send_to[dest]` — if it equals our rank, dest is waiting for us → circular dependency → ERROR
2. Set `g_last_blocked_send_to[rank] = dest`

(This is a local heuristic; a full global deadlock check would require a MPI-level coordinator like MUST's deadlock detection algorithm.)

### Leaked Requests

At `__mpisan_finalize()`:
1. Scan all `g_req_table` entries
2. Any still-active → ERROR with originating file:line

---

## Alternatives Considered

### Alternative 1: PMPI Profiling Interface Only (no LLVM pass)

Pro: Simpler, no compiler required.  
Con: No access to IR type info, source locations are less reliable, cannot elide checks.  
Decision: Use both — LLVM pass provides instrumentation; runtime provides checking logic.

### Alternative 2: Clang AST Plugin (instead of IR pass)

Pro: Can see C types directly.  
Con: AST plugins are harder to write portably; PMPI wrappers and aliased function pointers are hard to track at AST level. IR is the canonical representation after all macro expansion.  
Decision: IR pass is more robust.

### Alternative 3: Dynamic Binary Instrumentation (PIN / Valgrind)

Pro: Works on already-compiled binaries.  
Con: ~5-10x overhead; cannot access source-level type info; requires separate install.  
Decision: Compiler-integrated is faster and more precise.

### Alternative 4: Full Formal Deadlock Detection (à la MUST/iGrind)

Pro: Provably complete deadlock detection.  
Con: Requires global coordination across all ranks at every blocking call; significant overhead; complex to implement correctly.  
Decision: Use a local heuristic for now; a future extension could add a coordinator rank.

---

## Limitations and Future Work

- **Deadlock detection is heuristic**: the current algorithm detects the classic symmetric-send pattern but not all deadlock topologies.
- **Custom MPI datatypes**: `MPI_Type_create_struct` etc. are not yet tracked (recorded as "UNKNOWN_TYPE").
- **MPI_IN_PLACE**: aliasing check doesn't special-case `MPI_IN_PLACE` yet.
- **Multi-threaded MPI**: `MPI_THREAD_MULTIPLE` programs may have races between the instrumentation hooks; locking scope could be improved.
- **Compile-time elision**: the pass currently instruments all MPI calls unconditionally; a future version could use alias analysis to skip provably-safe pairs.