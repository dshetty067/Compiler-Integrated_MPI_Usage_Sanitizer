#include "MPISanitizerPass.h"

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include <cassert>

using namespace llvm;
using namespace mpi_san;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isMPIFunction(const std::string &name) {
  return name.rfind("MPI_", 0) == 0 || name.rfind("PMPI_", 0) == 0;
}

MPICallKind MPISanitizerPass::classifyCall(const std::string &name) {
  if (name == "MPI_Send"   || name == "PMPI_Send")   return MPICallKind::Send;
  if (name == "MPI_Recv"   || name == "PMPI_Recv")   return MPICallKind::Recv;
  if (name == "MPI_Isend"  || name == "PMPI_Isend")  return MPICallKind::Isend;
  if (name == "MPI_Irecv"  || name == "PMPI_Irecv")  return MPICallKind::Irecv;
  if (name == "MPI_Bcast"  || name == "PMPI_Bcast")  return MPICallKind::Bcast;
  if (name == "MPI_Reduce" || name == "PMPI_Reduce") return MPICallKind::Reduce;
  if (name == "MPI_Allreduce" || name == "PMPI_Allreduce") return MPICallKind::Allreduce;
  if (name == "MPI_Scatter" || name == "PMPI_Scatter") return MPICallKind::Scatter;
  if (name == "MPI_Gather"  || name == "PMPI_Gather")  return MPICallKind::Gather;
  if (name == "MPI_Allgather"|| name == "PMPI_Allgather") return MPICallKind::Allgather;
  if (name == "MPI_Barrier" || name == "PMPI_Barrier") return MPICallKind::Barrier;
  if (name == "MPI_Wait"    || name == "PMPI_Wait")   return MPICallKind::Wait;
  if (name == "MPI_Waitall" || name == "PMPI_Waitall") return MPICallKind::Waitall;
  if (name == "MPI_Finalize"|| name == "PMPI_Finalize") return MPICallKind::Finalize;
  return MPICallKind::Unknown;
}

// ---------------------------------------------------------------------------
// Runtime function declarations (hooked in before each MPI call)
// ---------------------------------------------------------------------------

void MPISanitizerPass::declareRuntimeFunctions() {
  LLVMContext &C = *Ctx;
  auto *VoidTy  = Type::getVoidTy(C);
  auto *I8PtrTy = PointerType::getUnqual(Type::getInt8Ty(C));
  auto *I32Ty   = Type::getInt32Ty(C);
  auto *I64Ty   = Type::getInt64Ty(C);

  // __mpisan_init()
  RTInit = Mod->getOrInsertFunction("__mpisan_init",
      FunctionType::get(VoidTy, {}, false));

  // __mpisan_finalize()
  RTFinalize = Mod->getOrInsertFunction("__mpisan_finalize",
      FunctionType::get(VoidTy, {}, false));

  // __mpisan_send(buf, count, mpi_type_id, dest, tag, comm, file, line, is_blocking)
  RTSend = Mod->getOrInsertFunction("__mpisan_send",
      FunctionType::get(VoidTy,
        {I8PtrTy, I64Ty, I64Ty, I32Ty, I32Ty, I64Ty, I8PtrTy, I32Ty, I32Ty}, false));

  // __mpisan_recv(buf, count, mpi_type_id, src, tag, comm, file, line, is_blocking)
  RTRecv = Mod->getOrInsertFunction("__mpisan_recv",
      FunctionType::get(VoidTy,
        {I8PtrTy, I64Ty, I64Ty, I32Ty, I32Ty, I64Ty, I8PtrTy, I32Ty, I32Ty}, false));

  // __mpisan_isend(buf, count, mpi_type_id, dest, tag, comm, req_ptr, file, line)
  RTIsend = Mod->getOrInsertFunction("__mpisan_isend",
      FunctionType::get(VoidTy,
        {I8PtrTy, I64Ty, I64Ty, I32Ty, I32Ty, I64Ty, I8PtrTy, I8PtrTy, I32Ty}, false));

  // __mpisan_irecv(buf, count, mpi_type_id, src, tag, comm, req_ptr, file, line)
  RTIrecv = Mod->getOrInsertFunction("__mpisan_irecv",
      FunctionType::get(VoidTy,
        {I8PtrTy, I64Ty, I64Ty, I32Ty, I32Ty, I64Ty, I8PtrTy, I8PtrTy, I32Ty}, false));

  // __mpisan_collective(op_name, buf, count, mpi_type_id, comm, file, line)
  RTCollective = Mod->getOrInsertFunction("__mpisan_collective",
      FunctionType::get(VoidTy,
        {I8PtrTy, I8PtrTy, I64Ty, I64Ty, I64Ty, I8PtrTy, I32Ty}, false));

  // __mpisan_wait(req_ptr, file, line)
  RTWait = Mod->getOrInsertFunction("__mpisan_wait",
      FunctionType::get(VoidTy, {I8PtrTy, I8PtrTy, I32Ty}, false));

  // __mpisan_waitall(count, req_array, file, line)
  RTWaitall = Mod->getOrInsertFunction("__mpisan_waitall",
      FunctionType::get(VoidTy, {I32Ty, I8PtrTy, I8PtrTy, I32Ty}, false));

  // __mpisan_barrier(comm, file, line)
  RTBarrier = Mod->getOrInsertFunction("__mpisan_barrier",
      FunctionType::get(VoidTy, {I64Ty, I8PtrTy, I32Ty}, false));
}

// ---------------------------------------------------------------------------
// Utility: string literal as i8*
// ---------------------------------------------------------------------------

Value *MPISanitizerPass::getStringPtr(const std::string &s, IRBuilder<> &B) {
  // Create a global constant string
  auto *strConst = ConstantDataArray::getString(*Ctx, s, true);
  auto *GV = new GlobalVariable(*Mod, strConst->getType(), true,
      GlobalValue::PrivateLinkage, strConst, ".mpisan.str");
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  return B.CreateBitCast(GV, PointerType::getUnqual(Type::getInt8Ty(*Ctx)));
}

Value *MPISanitizerPass::castToI8Ptr(Value *V, IRBuilder<> &B) {
  if (V->getType()->isPointerTy())
    return B.CreateBitCast(V, PointerType::getUnqual(Type::getInt8Ty(*Ctx)));
  // Integer to pointer
  return B.CreateIntToPtr(V, PointerType::getUnqual(Type::getInt8Ty(*Ctx)));
}

Value *MPISanitizerPass::castToI64(Value *V, IRBuilder<> &B) {
  auto *I64Ty = Type::getInt64Ty(*Ctx);
  if (V->getType()->isPointerTy())
    return B.CreatePtrToInt(V, I64Ty);
  if (V->getType()->isIntegerTy())
    return B.CreateZExtOrTrunc(V, I64Ty);
  return ConstantInt::get(I64Ty, 0);
}

// ---------------------------------------------------------------------------
// Get source location info from debug metadata
// ---------------------------------------------------------------------------

static std::pair<std::string, int> getDebugLoc(CallInst *CI) {
  if (auto DL = CI->getDebugLoc()) {
    auto *scopeMeta = DL.get();
    std::string file = "<unknown>";

    // Safely cast to DIScope
    if (auto *scope = dyn_cast<DIScope>(scopeMeta)) {
      // Try direct file from scope
      if (auto *diFileObj = scope->getFile()) {
        file = diFileObj->getFilename().str();
      } 
      // Fallback: try subprogram
      else if (auto *SP = dyn_cast<DISubprogram>(scope)) {
        if (SP->getFile()) {
          file = SP->getFile()->getFilename().str();
        }
      }
    }

    return {file, (int)DL.getLine()};
  }

  return {"<unknown>", 0};
}

// ---------------------------------------------------------------------------
// Per-call instrumentation helpers
// ---------------------------------------------------------------------------

// MPI_Send(buf, count, datatype, dest, tag, comm) -> int
void MPISanitizerPass::instrSend(CallInst *CI, IRBuilder<> &B, bool isBlocking) {
  auto [file, line] = getDebugLoc(CI);
  auto *buf     = castToI8Ptr(CI->getArgOperand(0), B);
  auto *count   = castToI64(CI->getArgOperand(1), B);
  auto *dtype   = castToI64(CI->getArgOperand(2), B);   // MPI_Datatype is int/ptr
  auto *dest    = B.CreateZExtOrTrunc(CI->getArgOperand(3), Type::getInt32Ty(*Ctx));
  auto *tag     = B.CreateZExtOrTrunc(CI->getArgOperand(4), Type::getInt32Ty(*Ctx));
  auto *comm    = castToI64(CI->getArgOperand(5), B);
  auto *fileStr = getStringPtr(file, B);
  auto *lineV   = ConstantInt::get(Type::getInt32Ty(*Ctx), line);
  auto *blockV  = ConstantInt::get(Type::getInt32Ty(*Ctx), isBlocking ? 1 : 0);
  B.CreateCall(RTSend, {buf, count, dtype, dest, tag, comm, fileStr, lineV, blockV});
}

// MPI_Recv(buf, count, datatype, src, tag, comm, status) -> int
void MPISanitizerPass::instrRecv(CallInst *CI, IRBuilder<> &B, bool isBlocking) {
  auto [file, line] = getDebugLoc(CI);
  auto *buf     = castToI8Ptr(CI->getArgOperand(0), B);
  auto *count   = castToI64(CI->getArgOperand(1), B);
  auto *dtype   = castToI64(CI->getArgOperand(2), B);
  auto *src     = B.CreateZExtOrTrunc(CI->getArgOperand(3), Type::getInt32Ty(*Ctx));
  auto *tag     = B.CreateZExtOrTrunc(CI->getArgOperand(4), Type::getInt32Ty(*Ctx));
  auto *comm    = castToI64(CI->getArgOperand(5), B);
  auto *fileStr = getStringPtr(file, B);
  auto *lineV   = ConstantInt::get(Type::getInt32Ty(*Ctx), line);
  auto *blockV  = ConstantInt::get(Type::getInt32Ty(*Ctx), isBlocking ? 1 : 0);
  B.CreateCall(RTRecv, {buf, count, dtype, src, tag, comm, fileStr, lineV, blockV});
}

// MPI_Isend(buf, count, dtype, dest, tag, comm, request)
// MPI_Irecv(buf, count, dtype, src,  tag, comm, request)
void MPISanitizerPass::instrWait(CallInst *CI, IRBuilder<> &B, bool isWaitall) {
  auto [file, line] = getDebugLoc(CI);
  auto *fileStr = getStringPtr(file, B);
  auto *lineV   = ConstantInt::get(Type::getInt32Ty(*Ctx), line);

  if (isWaitall) {
    // MPI_Waitall(count, array_of_requests, array_of_statuses)
    auto *count = B.CreateZExtOrTrunc(CI->getArgOperand(0), Type::getInt32Ty(*Ctx));
    auto *reqs  = castToI8Ptr(CI->getArgOperand(1), B);
    B.CreateCall(RTWaitall, {count, reqs, fileStr, lineV});
  } else {
    // MPI_Wait(request, status)
    auto *req = castToI8Ptr(CI->getArgOperand(0), B);
    B.CreateCall(RTWait, {req, fileStr, lineV});
  }
}

void MPISanitizerPass::instrBarrier(CallInst *CI, IRBuilder<> &B) {
  auto [file, line] = getDebugLoc(CI);
  auto *comm    = castToI64(CI->getArgOperand(0), B);
  auto *fileStr = getStringPtr(file, B);
  auto *lineV   = ConstantInt::get(Type::getInt32Ty(*Ctx), line);
  B.CreateCall(RTBarrier, {comm, fileStr, lineV});
}

// For collective ops: extract first buffer argument and route to __mpisan_collective
void MPISanitizerPass::instrCollective(CallInst *CI, IRBuilder<> &B,
                                       const std::string &name) {
  auto [file, line] = getDebugLoc(CI);
  auto *opStr   = getStringPtr(name, B);
  auto *fileStr = getStringPtr(file, B);
  auto *lineV   = ConstantInt::get(Type::getInt32Ty(*Ctx), line);

  Value *buf   = nullptr;
  Value *count = nullptr;
  Value *dtype = nullptr;
  Value *comm  = nullptr;

  unsigned nArgs = CI->arg_size();

  // MPI_Bcast(buf, count, dtype, root, comm)          - 5 args
  // MPI_Reduce(sendbuf, recvbuf, count, dtype, op, root, comm) - 7 args
  // MPI_Allreduce(sendbuf, recvbuf, count, dtype, op, comm)    - 6 args
  // MPI_Scatter(sendbuf, sc, stype, recvbuf, rc, rtype, root, comm) - 8 args
  // MPI_Gather (sendbuf, sc, stype, recvbuf, rc, rtype, root, comm) - 8 args
  // MPI_Allgather(sendbuf, sc, stype, recvbuf, rc, rtype, comm) - 7 args

  if (name == "MPI_Bcast" || name == "PMPI_Bcast") {
    buf   = nArgs > 0 ? CI->getArgOperand(0) : nullptr;
    count = nArgs > 1 ? CI->getArgOperand(1) : nullptr;
    dtype = nArgs > 2 ? CI->getArgOperand(2) : nullptr;
    comm  = nArgs > 4 ? CI->getArgOperand(4) : nullptr;
  } else if (name == "MPI_Reduce" || name == "PMPI_Reduce") {
    buf   = nArgs > 0 ? CI->getArgOperand(0) : nullptr;
    count = nArgs > 2 ? CI->getArgOperand(2) : nullptr;
    dtype = nArgs > 3 ? CI->getArgOperand(3) : nullptr;
    comm  = nArgs > 6 ? CI->getArgOperand(6) : nullptr;
  } else if (name == "MPI_Allreduce" || name == "PMPI_Allreduce") {
    buf   = nArgs > 0 ? CI->getArgOperand(0) : nullptr;
    count = nArgs > 2 ? CI->getArgOperand(2) : nullptr;
    dtype = nArgs > 3 ? CI->getArgOperand(3) : nullptr;
    comm  = nArgs > 5 ? CI->getArgOperand(5) : nullptr;
  } else if (name == "MPI_Scatter" || name == "PMPI_Scatter" ||
             name == "MPI_Gather"  || name == "PMPI_Gather") {
    buf   = nArgs > 0 ? CI->getArgOperand(0) : nullptr;
    count = nArgs > 1 ? CI->getArgOperand(1) : nullptr;
    dtype = nArgs > 2 ? CI->getArgOperand(2) : nullptr;
    comm  = nArgs > 7 ? CI->getArgOperand(7) : nullptr;
  } else if (name == "MPI_Allgather" || name == "PMPI_Allgather") {
    buf   = nArgs > 0 ? CI->getArgOperand(0) : nullptr;
    count = nArgs > 1 ? CI->getArgOperand(1) : nullptr;
    dtype = nArgs > 2 ? CI->getArgOperand(2) : nullptr;
    comm  = nArgs > 6 ? CI->getArgOperand(6) : nullptr;
  }

  auto *I64Ty = Type::getInt64Ty(*Ctx);
  auto *I8PtrTy = PointerType::getUnqual(Type::getInt8Ty(*Ctx));

  Value *bufV   = buf   ? castToI8Ptr(buf, B)   : ConstantPointerNull::get(cast<PointerType>(I8PtrTy));
  Value *countV = count ? castToI64(count, B)   : ConstantInt::get(I64Ty, 0);
  Value *dtypeV = dtype ? castToI64(dtype, B)   : ConstantInt::get(I64Ty, 0);
  Value *commV  = comm  ? castToI64(comm, B)    : ConstantInt::get(I64Ty, 0);

  B.CreateCall(RTCollective, {opStr, bufV, countV, dtypeV, commV, fileStr, lineV});
}

// ---------------------------------------------------------------------------
// Main instrument-a-call dispatcher
// ---------------------------------------------------------------------------

void MPISanitizerPass::instrumentCall(CallInst *CI, const std::string &name) {
  // Insert instrumentation BEFORE the actual MPI call
  IRBuilder<> B(CI);

  MPICallKind kind = classifyCall(name);

  switch (kind) {
    case MPICallKind::Send:
      instrSend(CI, B, true);
      break;
    case MPICallKind::Recv:
      instrRecv(CI, B, true);
      break;
    case MPICallKind::Isend: {
      auto [file, line] = getDebugLoc(CI);
      auto *buf     = castToI8Ptr(CI->getArgOperand(0), B);
      auto *count   = castToI64(CI->getArgOperand(1), B);
      auto *dtype   = castToI64(CI->getArgOperand(2), B);
      auto *dest    = B.CreateZExtOrTrunc(CI->getArgOperand(3), Type::getInt32Ty(*Ctx));
      auto *tag     = B.CreateZExtOrTrunc(CI->getArgOperand(4), Type::getInt32Ty(*Ctx));
      auto *comm    = castToI64(CI->getArgOperand(5), B);
      auto *req     = castToI8Ptr(CI->getArgOperand(6), B);
      auto *fileStr = getStringPtr(file, B);
      auto *lineV   = ConstantInt::get(Type::getInt32Ty(*Ctx), line);
      B.CreateCall(RTIsend, {buf, count, dtype, dest, tag, comm, req, fileStr, lineV});
      break;
    }
    case MPICallKind::Irecv: {
      auto [file, line] = getDebugLoc(CI);
      auto *buf     = castToI8Ptr(CI->getArgOperand(0), B);
      auto *count   = castToI64(CI->getArgOperand(1), B);
      auto *dtype   = castToI64(CI->getArgOperand(2), B);
      auto *src     = B.CreateZExtOrTrunc(CI->getArgOperand(3), Type::getInt32Ty(*Ctx));
      auto *tag     = B.CreateZExtOrTrunc(CI->getArgOperand(4), Type::getInt32Ty(*Ctx));
      auto *comm    = castToI64(CI->getArgOperand(5), B);
      auto *req     = castToI8Ptr(CI->getArgOperand(6), B);
      auto *fileStr = getStringPtr(file, B);
      auto *lineV   = ConstantInt::get(Type::getInt32Ty(*Ctx), line);
      B.CreateCall(RTIrecv, {buf, count, dtype, src, tag, comm, req, fileStr, lineV});
      break;
    }
    case MPICallKind::Bcast:
    case MPICallKind::Reduce:
    case MPICallKind::Allreduce:
    case MPICallKind::Scatter:
    case MPICallKind::Gather:
    case MPICallKind::Allgather:
      instrCollective(CI, B, name);
      break;
    case MPICallKind::Barrier:
      instrBarrier(CI, B);
      break;
    case MPICallKind::Wait:
      instrWait(CI, B, false);
      break;
    case MPICallKind::Waitall:
      instrWait(CI, B, true);
      break;
    case MPICallKind::Finalize:
      B.CreateCall(RTFinalize, {});
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Function walker
// ---------------------------------------------------------------------------

void MPISanitizerPass::instrumentFunction(Function &F) {
  // Collect calls first (don't modify while iterating)
  std::vector<std::pair<CallInst*, std::string>> calls;

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        Function *callee = CI->getCalledFunction();
        if (!callee) continue;
        std::string name = callee->getName().str();
        if (isMPIFunction(name)) {
          calls.push_back({CI, name});
        }
      }
    }
  }

  for (auto &[CI, name] : calls) {
    instrumentCall(CI, name);
  }
}

// ---------------------------------------------------------------------------
// Module-level pass entry
// ---------------------------------------------------------------------------

PreservedAnalyses MPISanitizerPass::run(Module &M, ModuleAnalysisManager &MAM) {
  Mod = &M;
  Ctx = &M.getContext();

  // Check if this module uses any MPI
  bool hasMPI = false;
  for (auto &F : M) {
    if (isMPIFunction(F.getName().str())) { hasMPI = true; break; }
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto *CI = dyn_cast<CallInst>(&I))
          if (auto *callee = CI->getCalledFunction())
            if (isMPIFunction(callee->getName().str())) { hasMPI = true; break; }
  }

  if (!hasMPI) return PreservedAnalyses::all();

  declareRuntimeFunctions();

  // Inject __mpisan_init call at the start of main (if present)
  if (Function *mainFn = M.getFunction("main")) {
    if (!mainFn->isDeclaration()) {
      IRBuilder<> B(&mainFn->getEntryBlock().front());
      B.CreateCall(RTInit, {});
    }
  }

  for (auto &F : M) {
    if (!F.isDeclaration())
      instrumentFunction(F);
  }

  return PreservedAnalyses::none();
}

// ---------------------------------------------------------------------------
// Plugin registration (new pass manager)
// ---------------------------------------------------------------------------

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "MPISanitizer", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {
      PB.registerPipelineParsingCallback(
        [](StringRef Name, ModulePassManager &MPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "mpi-sanitizer") {
            MPM.addPass(mpi_san::MPISanitizerPass());
            return true;
          }
          return false;
        });
      // Also inject automatically at -O0 / full pipeline
      PB.registerOptimizerLastEPCallback(
        [](ModulePassManager &MPM, OptimizationLevel) {
          MPM.addPass(mpi_san::MPISanitizerPass());
        });
    }
  };
}