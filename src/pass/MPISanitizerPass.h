#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace mpi_san {

// Maps MPI type constants to their byte sizes (used for type-checking)
struct MPITypeInfo {
  std::string name;
  int byteSize;   // -1 = unknown/struct
};

// All MPI function categories we instrument
enum class MPICallKind {
  Send,
  Recv,
  Isend,
  Irecv,
  Bcast,
  Reduce,
  Allreduce,
  Scatter,
  Gather,
  Allgather,
  Barrier,
  Wait,
  Waitall,
  Finalize,
  Unknown
};

struct MPICallSite {
  llvm::CallInst *CI;
  MPICallKind Kind;
  std::string FuncName;
};

// New-style LLVM pass (PassManager)
class MPISanitizerPass : public llvm::PassInfoMixin<MPISanitizerPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);

private:
  llvm::Module *Mod = nullptr;
  llvm::LLVMContext *Ctx = nullptr;

  // Runtime hook declarations (inserted into module)
  llvm::FunctionCallee RTInit;
  llvm::FunctionCallee RTFinalize;
  llvm::FunctionCallee RTSend;
  llvm::FunctionCallee RTRecv;
  llvm::FunctionCallee RTIsend;
  llvm::FunctionCallee RTIrecv;
  llvm::FunctionCallee RTCollective;
  llvm::FunctionCallee RTWait;
  llvm::FunctionCallee RTWaitall;
  llvm::FunctionCallee RTBarrier;

  void declareRuntimeFunctions();
  void instrumentFunction(llvm::Function &F);
  void instrumentCall(llvm::CallInst *CI, const std::string &name);

  MPICallKind classifyCall(const std::string &name);

  void instrSend(llvm::CallInst *CI, llvm::IRBuilder<> &B, bool isBlocking);
  void instrRecv(llvm::CallInst *CI, llvm::IRBuilder<> &B, bool isBlocking);
  void instrCollective(llvm::CallInst *CI, llvm::IRBuilder<> &B, const std::string &name);
  void instrWait(llvm::CallInst *CI, llvm::IRBuilder<> &B, bool isWaitall);
  void instrBarrier(llvm::CallInst *CI, llvm::IRBuilder<> &B);

  llvm::Value *getStringPtr(const std::string &s, llvm::IRBuilder<> &B);
  llvm::Value *castToI8Ptr(llvm::Value *V, llvm::IRBuilder<> &B);
  llvm::Value *castToI64(llvm::Value *V, llvm::IRBuilder<> &B);
};

} // namespace mpi_san