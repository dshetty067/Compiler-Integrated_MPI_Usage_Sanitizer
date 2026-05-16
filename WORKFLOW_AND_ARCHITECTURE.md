# MPI Sanitizer Workflow and Architecture

This document summarizes the intended build, instrumentation, runtime checking,
and evaluation flow for the Compiler-Integrated MPI Usage Sanitizer.

## Project Workflow

```mermaid
flowchart TD
    A[Developer writes MPI C test/program] --> B[build.sh]
    B --> C[Install/check dependencies: LLVM, Clang, Open MPI, CMake]
    B --> D[CMake configure]
    D --> E[Build LLVM pass plugin<br/>build/MPISanitizerPass.so]
    D --> F[Build runtime library<br/>build/libmpisan_rt.a and libmpisan_rt.so]
    E --> G[Compile program to LLVM IR]
    F --> H[Link runtime into instrumented binary]
    G --> I[Run LLVM opt pass<br/>-passes=mpi-sanitizer]
    I --> J[Instrumented LLVM IR]
    J --> H
    H --> K[run.sh]
    K --> L[mpirun executes each testcase]
    L --> M[Runtime hooks validate MPI usage]
    M --> N[Per-test logs in results/*.log]
    N --> O[Evaluation summary<br/>results/evaluation_report.txt]
```

## Runtime Execution Workflow

```mermaid
sequenceDiagram
    participant App as MPI Application
    participant Hook as Injected __mpisan_* Hook
    participant RT as libmpisan_rt
    participant MPI as MPI Library
    participant Log as results/*.log or stderr

    App->>MPI: MPI_Init
    App->>Hook: __mpisan_send / recv / collective / wait
    Hook->>RT: Pass buffer, count, datatype, rank, tag, comm, file:line
    RT->>RT: Update send/request/collective tables
    RT->>RT: Check type, count, aliasing, NULL buffer, leaks, deadlock heuristic
    RT-->>Log: Emit [MPISAN] ERROR/WARN/INFO when needed
    Hook-->>App: Return to original call site
    App->>MPI: Original MPI_* call executes
    App->>Hook: __mpisan_finalize before MPI_Finalize
    Hook->>RT: Audit active nonblocking requests
    RT-->>Log: Emit clean exit or error count
    App->>MPI: MPI_Finalize
```

## Architecture Diagram

```mermaid
flowchart LR
    subgraph Source["Source Programs"]
        T[Testcases<br/>testcases/*.c]
        M[Mini-apps<br/>miniapp_heat1d_*.c]
    end

    subgraph Compiler["Compiler Integration"]
        CL[clang<br/>C to LLVM IR]
        PASS[LLVM Module Pass<br/>src/pass/MPISanitizerPass.cpp]
        IR[Instrumented IR<br/>MPI calls plus __mpisan_* hooks]
    end

    subgraph Runtime["Runtime Checking Library"]
        API[Runtime API<br/>src/runtime/mpisan_rt.h]
        CORE[Runtime Core<br/>src/runtime/mpisan_rt.c]
        SEND[Send table<br/>type/count matching]
        REQ[Request table<br/>aliasing and leaked requests]
        COLL[Collective history<br/>ordering/deadlock heuristics]
        LOG[Diagnostics<br/>stderr or MPISAN_LOG]
    end

    subgraph Build["Build Artifacts"]
        PLUGIN[build/MPISanitizerPass.so]
        STATIC[build/libmpisan_rt.a]
        SHARED[build/libmpisan_rt.so]
        BIN[Instrumented test binaries]
    end

    subgraph Evaluation["Evaluation"]
        RUN[run.sh]
        MPI[mpirun]
        RESULTS[results/*.log]
        REPORT[results/evaluation_report.txt]
    end

    T --> CL
    M --> CL
    CL --> PASS
    PASS --> IR
    PASS -.built as.-> PLUGIN
    API --> CORE
    CORE --> SEND
    CORE --> REQ
    CORE --> COLL
    CORE --> LOG
    CORE -.built as.-> STATIC
    CORE -.built as.-> SHARED
    IR --> BIN
    STATIC --> BIN
    RUN --> MPI
    BIN --> MPI
    MPI --> CORE
    LOG --> RESULTS
    RESULTS --> REPORT
```

## Main Components

| Component | Path | Responsibility |
|---|---|---|
| LLVM pass | `src/pass/MPISanitizerPass.cpp` | Scans LLVM IR for `MPI_*` / `PMPI_*` calls and injects runtime hooks before them. |
| Runtime API | `src/runtime/mpisan_rt.h` | Declares hook functions used by instrumented IR. |
| Runtime implementation | `src/runtime/mpisan_rt.c` | Maintains MPI metadata tables, performs checks, and emits diagnostics. |
| Build script | `build.sh` | Installs dependencies, configures CMake, builds pass/runtime, and builds testcase binaries. |
| Test runner | `run.sh` | Runs correct, buggy, deadlock, and mini-app tests under `mpirun`; writes logs and report. |
| Test suite | `testcases/*.c` | Contains clean examples, seeded MPI bugs, and heat1d mini-app variants. |

## Instrumentation Flow

```mermaid
flowchart TD
    A[Original call<br/>MPI_Send buf, count, type, dest, tag, comm] --> B[LLVM pass finds CallInst]
    B --> C[Extract arguments from IR]
    C --> D[Read DebugLoc<br/>source file and line]
    D --> E[Insert hook before original call]
    E --> F[__mpisan_send buf, count, type, dest, tag, comm, file, line]
    F --> G[Original MPI_Send still executes unchanged]
```

The same pattern is used for `MPI_Recv`, `MPI_Isend`, `MPI_Irecv`,
`MPI_Wait`, `MPI_Waitall`, `MPI_Barrier`, supported collectives, and
`MPI_Finalize`.

## Detection Responsibilities

| Bug class | Runtime hook/check |
|---|---|
| Type mismatch | Record sends in the send table; compare matching receives by datatype. |
| Count mismatch | Compare receive count against recorded send count. |
| Buffer aliasing | Compare new nonblocking buffer ranges with active request ranges. |
| Leaked request | Scan active request table during `__mpisan_finalize`. |
| NULL buffer | Check pointer arguments before MPI call executes. |
| Collective misuse | Record collective operation history per communicator. |
| Deadlock-prone sends | Apply a local rank-ordering heuristic for blocking sends. |

## Important Build Note

The sanitizer only detects MPI misuse when test/program binaries are actually
instrumented by `MPISanitizerPass.so` and linked with `libmpisan_rt.a`. Merely
linking `libmpisan_rt.a` into an otherwise normal MPI binary is not enough,
because the runtime is entered through compiler-injected `__mpisan_*` calls.
