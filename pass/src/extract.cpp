#include "extract.hpp"

#include "llvm/Pass.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"

#include "llvm/Bitcode/BitcodeWriter.h"

#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/Support/raw_ostream.h"

#include <boost/iterator/transform_iterator.hpp>

#include "declare.h"
#include "identify.h"

using namespace pass;
using namespace llvm;

namespace pass {

  // globals
  constexpr bool Debug = true;
  static const char* EmbeddedModuleName = "easy_jit_module";

  // pass configuration
  static const char* Command = "easy_jit";
  static const char* Description = "Pass to extract functions to ir.";
  constexpr bool IsAnalysis = false;
  constexpr bool IsCfgOnly = false;
  static RegisterPass<ExtractAndEmbed> X(Command, Description, IsCfgOnly, IsAnalysis);

  char ExtractAndEmbed::ID = 0;

  ExtractAndEmbed::ExtractAndEmbed()
    : llvm::ModulePass(ID) {}

  bool addGlobalIfUsedByExtracted(GlobalValue& GV,
                                  const FunToInlineMap &Fun2Extract,
                                  SmallPtrSetImpl<GlobalValue*> &Globals) {

    auto IsExtractedInst = [&Fun2Extract](User* U) {
      Instruction* UI = dyn_cast<Instruction>(U);
      if(!UI)
        return false;
      return Fun2Extract.count(UI->getParent()->getParent()) != 0;
    };

    bool Used = std::any_of(GV.user_begin(), GV.user_end(), IsExtractedInst);
    if(Used)
      Globals.insert(&GV);
    return Used;
  }

  template<class Range>
  void getReferencedGlobalsIn(const FunToInlineMap& Fun2Extract,
                                  Range &&R,
                                  SmallPtrSetImpl<GlobalValue*> &Globals) {
    for(auto &FOrGV : R) {
      Function* F = dyn_cast<Function>(&FOrGV);
      if(Fun2Extract.count(F))
        continue;

      if(addGlobalIfUsedByExtracted(FOrGV, Fun2Extract, Globals) && Debug)
        errs() << "Global " << FOrGV.getName() << " referenced by extracted function.\n";
    }
  }

  SmallPtrSet<GlobalValue*, 8> getReferencedGlobals(const FunToInlineMap& Fun2Extract) {
      Module &M = *Fun2Extract.front().first->getParent();

      SmallPtrSet<GlobalValue*, 8> Globals;
      getReferencedGlobalsIn(Fun2Extract, M.globals(), Globals);
      getReferencedGlobalsIn(Fun2Extract, M.functions(), Globals);
      return Globals;
  }

  bool ValidForExtraction(const SmallPtrSetImpl<GlobalValue*>& Globals) {
    bool Valid = std::all_of(Globals.begin(), Globals.end(),
                             std::mem_fun(&GlobalValue::hasExternalLinkage));
    if(!Valid && Debug) {
      errs() << "Cannot extract module: global has private/internal linkage.\n";
    }

    return Valid;
  }

  void CreateJITHook(Function *F, SmallVectorImpl<Value *> const &Params, GlobalVariable* BitcodeGV) {
    std::string FName = F->getName();

    Module *M = F->getParent();
    LLVMContext &C = F->getContext();

    Function *Hook =
        Function::Create(F->getFunctionType(), F->getLinkage(), "hook", M);
    F->replaceAllUsesWith(Hook);
    Hook->takeName(F);

    // get the hook function to the runtime
    Function *JITHook = Declare<declare::JitHook>(*Hook->getParent());
    Function *JITHookEnd = Declare<declare::JitHookEnd>(*Hook->getParent());
    assert(JITHook && JITHookEnd);

    Type *I8Ptr = Type::getInt8PtrTy(C);

    // create a single block
    BasicBlock *Entry = BasicBlock::Create(C, "entry", Hook);
    IRBuilder<> B(Entry);

    // create a string
    Constant *Init = ConstantDataArray::getString(C, FName, true);
    Constant *FNameGV = new GlobalVariable(*M, Init->getType(), true,
                                           GlobalValue::InternalLinkage, Init,
                                           ".easy_jit_fun_name");
    FNameGV = ConstantExpr::getPointerCast(FNameGV, I8Ptr);

    // get the ir variable
    ArrayType *IRVarTy =
        cast<ArrayType>(BitcodeGV->getType()->getContainedType(0));
    Constant *IRSize =
        ConstantInt::get(Type::getInt64Ty(C), IRVarTy->getNumElements());
    Constant* IRVar = ConstantExpr::getPointerCast(BitcodeGV, I8Ptr);

    int optlevel = cast<ConstantInt>(*Params.begin())->getSExtValue();
    Constant* OptLevel = ConstantInt::get(Type::getInt32Ty(C), optlevel);

    SmallVector<Value *, 8> FunArgsVal;
    SmallVector<Value *, 8> HookArgs = {FNameGV, IRVar, IRSize, OptLevel};

    size_t arg_idx = 0;
    auto FArgsIter = F->arg_begin();
    for (auto &V : Hook->args()) {
      auto Where = std::find(Params.begin()+1, Params.end(), &*FArgsIter);
      if (Where != Params.end()) {
        HookArgs.push_back(B.getInt32(arg_idx)); // pass the index of the parameter

        Value* VAsI64 = &V;
        size_t VSizeInBit = VAsI64->getType()->getPrimitiveSizeInBits();

        if(VAsI64->getType()->isFloatingPointTy()) {
          VAsI64 = B.CreateFPToUI(VAsI64, IntegerType::get(C, VSizeInBit));
        } else if (VAsI64->getType()->isPointerTy()) {
          VAsI64 = B.CreatePtrToInt(VAsI64, IntegerType::get(C, VSizeInBit));
        }

        if(VAsI64->getType()->getPrimitiveSizeInBits() < 64) { // pass the parameter value
          VAsI64 = B.CreateZExt(VAsI64, Type::getInt64Ty(C));
        }

        HookArgs.push_back(&V);
      }
      FunArgsVal.push_back(&V);
      ++FArgsIter;
      ++arg_idx;
    }
    HookArgs.push_back(B.getInt32(-1));

    // create the call
    Value *JitResult = B.CreateCall(JITHook, HookArgs);

    Value *Cast = B.CreatePointerCast(JitResult, Hook->getType());

    Value *Call = B.CreateCall(Cast, FunArgsVal);

    B.CreateCall(JITHookEnd, {JitResult});

    if (Call->getType()->isVoidTy())
      ReturnInst::Create(C, Entry);
    else
      ReturnInst::Create(C, Call, Entry);
    F->eraseFromParent();

    M->dump();
  }

  void GVMakeExternalDeclaration(GlobalValue* GV) {
    GV->setLinkage(GlobalValue::ExternalLinkage);
    if(Function* F = dyn_cast<Function>(GV)) {
      F->getBasicBlockList().clear();
    } else if (GlobalVariable *G = dyn_cast<GlobalVariable>(GV)){
      G->setInitializer(nullptr);
    } else assert(false);
  }

  std::unique_ptr<Module> GetModuleForJITCompilation(const FunToInlineMap& Fun2Extract, Module& M) {
      auto Globals = getReferencedGlobals(Fun2Extract);

      if(!ValidForExtraction(Globals))
        return nullptr;

      auto Clone = llvm::CloneModule(&M);

      auto Functions = GetFunctions(Fun2Extract);

      //collect the referenced globals in the clone
      std::vector<GlobalValue*> GlobalsToKeep(Globals.size() + Fun2Extract.size());
      auto GetGlobalInClone = [&Clone](auto* G) { return Clone->getNamedValue(G->getName()); };
      std::transform(Globals.begin(), Globals.end(), GlobalsToKeep.begin(), GetGlobalInClone);
      std::transform(Functions.begin(), Functions.end(), GlobalsToKeep.begin() + Globals.size(), GetGlobalInClone);

      //clean the clonned module
      llvm::legacy::PassManager Passes;
      Passes.add(createGVExtractionPass(GlobalsToKeep));
      Passes.add(createGlobalDCEPass());
      Passes.add(createStripDeadDebugInfoPass());
      Passes.add(createStripDeadPrototypesPass());
      Passes.run(*Clone);

      //transform the globals to external declarations
      std::for_each(GlobalsToKeep.begin(), GlobalsToKeep.begin() + Globals.size(),
                    GVMakeExternalDeclaration);

      //rename the functions
      std::for_each(GlobalsToKeep.begin() + Globals.size(), GlobalsToKeep.end(),
                    [](GlobalValue *GV) { GV->setName(GV->getName() + "__"); });

      return Clone;
  }

  std::string ModuleToString(Module &M) {
    std::string s;
    raw_string_ostream so(s);
    WriteBitcodeToFile(&M, so);
    so.flush();
    return s;
  }

  GlobalVariable* WriteModuleToGlobal(Module& M, Module& JitM) {
    std::string ModuleAsStr = ModuleToString(JitM);
    LLVMContext& C = M.getContext();

    Constant* Init = ConstantDataArray::getString(C, ModuleAsStr, true);
    GlobalVariable* BitcodeGV =
        new GlobalVariable(M, Init->getType(), true, GlobalValue::InternalLinkage, Init, EmbeddedModuleName);

    if(Debug)
      errs() << "Extracted module written to " << BitcodeGV->getName() << "\n";

    return BitcodeGV;
  }

  bool ExtractAndEmbed::runOnModule(llvm::Module& M) {
    if(M.getNamedGlobal(EmbeddedModuleName)) {
      errs() << "WARNING: Compilation unit already contains an extracted module.\n";
      return false;
    }

    auto Fun2Extract = GetFunctionsToJit(M);
    if(Fun2Extract.empty())
      return false;

    auto JitM = GetModuleForJITCompilation(Fun2Extract, M);
    if(!JitM)
      return false;

    GlobalVariable* BitcodeGV = WriteModuleToGlobal(M, *JitM);

    for (auto const &KV : Fun2Extract) {
      CreateJITHook(KV.first, KV.second, BitcodeGV);
    }

    return true;
  }
}
