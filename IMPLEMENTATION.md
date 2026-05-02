# Implementation Details — LLVM Pass

## Pass Type

`MPISanitizerPass` is a **Module-level pass** using the new LLVM PassManager API (`PassInfoMixin<MPISanitizerPass>`). A module-level pass is necessary because:
- We need to declare runtime function prototypes into the module
- We may need to inject `__mpisan_init` into `main`, which could be in a different translation unit from MPI call sites

## Pass Registration

```cpp
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "MPISanitizer", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {
      // Register as named pass for -passes=mpi-sanitizer
      PB.registerPipelineParsingCallback(...);
      // Also inject at end of optimizer pipeline automatically
      PB.registerOptimizerLastEPCallback(...);
    }
  };
}
```

The `OptimizerLastEP` hook means the pass runs after all optimization passes, so it instruments the final IR that will be code-generated. This is important: we don't want optimization to hoist or eliminate our instrumentation hooks.

## Function Declaration Strategy

Runtime hooks are declared with `Module::getOrInsertFunction()`. This inserts a `declare` statement into the IR if the function is not already defined. At link time, `libmpisan_rt.a` provides the definitions.

```cpp
RTSend = Mod->getOrInsertFunction("__mpisan_send",
    FunctionType::get(VoidTy,
      {I8PtrTy, I64Ty, I64Ty, I32Ty, I32Ty, I64Ty, I8PtrTy, I32Ty, I32Ty},
      false));
```

All pointer types are `i8*` to avoid type system complications with opaque `MPI_Comm` / `MPI_Datatype` handles.

## Argument Extraction

For each MPI call site (a `CallInst`), we extract operands by position:

```
MPI_Send(buf, count, datatype, dest, tag, comm) → CI->getArgOperand(0..5)
MPI_Recv(buf, count, datatype, src, tag, comm, status) → CI->getArgOperand(0..5)
```

The `datatype` and `comm` arguments are opaque handles (integers or pointers depending on MPI implementation). We cast them to `i64` with `PtrToInt` / `ZExt` / `Trunc` as appropriate — the runtime re-casts them back to `MPI_Datatype` / `MPI_Comm` for comparison.

```cpp
Value *castToI64(Value *V, IRBuilder<> &B) {
  if (V->getType()->isPointerTy())
    return B.CreatePtrToInt(V, I64Ty);
  if (V->getType()->isIntegerTy())
    return B.CreateZExtOrTrunc(V, I64Ty);
  return ConstantInt::get(I64Ty, 0);
}
```

## Source Location Injection

LLVM `DebugLoc` metadata on a `CallInst` gives us file and line:

```cpp
static std::pair<std::string, int> getDebugLoc(CallInst *CI) {
  if (auto &DL = CI->getDebugLoc()) {
    // Extract DIFile from the scope
    ...
    return {file, (int)DL.getLine()};
  }
  return {"<unknown>", 0};
}
```

The file string is embedded as a global string constant in the IR:

```cpp
Value *getStringPtr(const std::string &s, IRBuilder<> &B) {
  auto *strConst = ConstantDataArray::getString(*Ctx, s, true);
  auto *GV = new GlobalVariable(*Mod, strConst->getType(), true,
      GlobalValue::PrivateLinkage, strConst, ".mpisan.str");
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  return B.CreateBitCast(GV, i8PtrTy);
}
```

`PrivateLinkage` + `UnnamedAddr::Global` allows the linker to deduplicate identical strings.

## Instrumentation Insertion Point

We insert hooks **before** the MPI call (not after), using:

```cpp
IRBuilder<> B(CI);  // positions before CI
B.CreateCall(RTSend, {...args...});
// CI (the actual MPI_Send) runs next
```

This means our hook runs before MPI's internal code, capturing the intended arguments. For `MPI_Finalize`, this is also before MPI tears down — important because our `__mpisan_finalize` calls `MPI_Comm_rank` etc.

## Two-Pass Collection

We **collect** call sites first, then **instrument**:

```cpp
// Phase 1: collect
std::vector<std::pair<CallInst*, std::string>> calls;
for (auto &BB : F)
  for (auto &I : BB)
    if (auto *CI = dyn_cast<CallInst>(&I))
      if (isMPIFunction(callee->getName()))
        calls.push_back({CI, name});

// Phase 2: instrument
for (auto &[CI, name] : calls)
  instrumentCall(CI, name);
```

This avoids iterator invalidation: inserting new instructions before a `CallInst` while iterating over the basic block would corrupt the iterator.

## Runtime Library Design

### Type Size Resolution

MPI datatype handles are implementation-defined integers. We build a lookup table at `__mpisan_init` time (after `MPI_Init`) by calling `MPI_Type_size()` on every standard type:

```c
void init_dt_table(void) {
    ADD(MPI_INT, "MPI_INT");     // → 4 bytes
    ADD(MPI_DOUBLE, "MPI_DOUBLE"); // → 8 bytes
    ...
}
```

This handles the fact that `MPI_INT` on OpenMPI is `0x4c000405` while on MPICH it may differ.

### Thread Safety

All global tables are protected by a single `pthread_mutex_t g_lock`. For most MPI programs (single-threaded per rank) this is zero-cost. For `MPI_THREAD_MULTIPLE` programs, lock granularity could be improved per future work.

### Stack Traces

When `MPISAN_VERBOSE=1`, errors print a stack trace using POSIX `backtrace()` + `backtrace_symbols()`. The binary must be compiled with `-g` (debug info) for meaningful output.

## Example Instrumented IR

Source:
```c
MPI_Send(buf, 100, MPI_DOUBLE, dest, tag, MPI_COMM_WORLD);
```

After instrumentation (pseudo-IR):
```llvm
; injected by pass
%buf_i8    = bitcast double* %buf to i8*
%count_i64 = zext i32 100 to i64
%dtype_i64 = ptrtoint %MPI_Datatype* @MPI_DOUBLE to i64
%dest_i32  = load i32, i32* %dest_var
%tag_i32   = i32 42
%comm_i64  = ptrtoint %MPI_Comm* @MPI_COMM_WORLD to i64
%file_ptr  = getelementptr [12 x i8], [12 x i8]* @.mpisan.str, i64 0, i64 0
call void @__mpisan_send(i8* %buf_i8, i64 %count_i64, i64 %dtype_i64,
                          i32 %dest_i32, i32 %tag_i32, i64 %comm_i64,
                          i8* %file_ptr, i32 37, i32 1)

; original call (unchanged)
%ret = call i32 @MPI_Send(double* %buf, i32 100,
                           %MPI_Datatype* @MPI_DOUBLE, i32 %dest_i32,
                           i32 42, %MPI_Comm* @MPI_COMM_WORLD)
```