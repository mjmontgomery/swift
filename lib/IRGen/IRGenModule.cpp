//===--- IRGenModule.cpp - Swift Global LLVM IR Generation ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for global declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"

#include "GenType.h"
#include "IRGenModule.h"
#include "IRGenDebugInfo.h"
#include "Linking.h"

#include <initializer_list>

using namespace swift;
using namespace irgen;
using clang::CodeGen::CodeGenABITypes;
using llvm::Attribute;

const unsigned DefaultAS = 0;

/// A helper for creating LLVM struct types.
static llvm::StructType *createStructType(IRGenModule &IGM,
                                          StringRef name,
                                  std::initializer_list<llvm::Type*> types) {
  return llvm::StructType::create(IGM.getLLVMContext(),
                                  ArrayRef<llvm::Type*>(types.begin(),
                                                        types.size()),
                                  name);
};

/// A helper for creating pointer-to-struct types.
static llvm::PointerType *createStructPointerType(IRGenModule &IGM,
                                                  StringRef name,
                                  std::initializer_list<llvm::Type*> types) {
  return createStructType(IGM, name, types)->getPointerTo(DefaultAS);
};

static clang::CodeGenerator *createClangCodeGenerator(ASTContext &Context,
                                                      IRGenOptions &Opts,
                                                      StringRef ModuleName) {
  auto Loader = Context.getClangModuleLoader();
  auto *Importer = static_cast<ClangImporter*>(&*Loader);
  assert(Importer && "No clang module loader!");
  auto &ClangContext = Importer->getClangASTContext();

  auto *CGO = new clang::CodeGenOptions;
  CGO->OptimizationLevel = Opts.OptLevel;
  CGO->DisableFPElim = Opts.DisableFPElim;
  auto &TO = ClangContext.getTargetInfo().getTargetOpts();
  auto *ClangCodeGen = clang::CreateLLVMCodeGen(ClangContext.getDiagnostics(),
                                                ModuleName, *CGO, TO,
                                                llvm::getGlobalContext());
  ClangCodeGen->Initialize(ClangContext);

  return ClangCodeGen;
}

IRGenModule::IRGenModule(ASTContext &Context,
                         IRGenOptions &Opts, StringRef ModuleName,
                         const llvm::DataLayout &DataLayout,
                         SILModule *SILMod)
  : Context(Context), Opts(Opts),
    ClangCodeGen(createClangCodeGenerator(Context, Opts, ModuleName)),
    Module(*ClangCodeGen->GetModule()),
    LLVMContext(Module.getContext()), DataLayout(DataLayout),
    SILMod(SILMod), TargetInfo(SwiftTargetInfo::get(*this)),
    DebugInfo(0), Types(*new TypeConverter(*this))
{
  VoidTy = llvm::Type::getVoidTy(getLLVMContext());
  Int1Ty = llvm::Type::getInt1Ty(getLLVMContext());
  Int8Ty = llvm::Type::getInt8Ty(getLLVMContext());
  Int16Ty = llvm::Type::getInt16Ty(getLLVMContext());
  Int32Ty = llvm::Type::getInt32Ty(getLLVMContext());
  Int64Ty = llvm::Type::getInt64Ty(getLLVMContext());
  Int8PtrTy = llvm::Type::getInt8PtrTy(getLLVMContext());
  Int8PtrPtrTy = Int8PtrTy->getPointerTo(0);
  SizeTy = DataLayout.getIntPtrType(getLLVMContext(), /*addrspace*/ 0);

  RefCountedStructTy =
    llvm::StructType::create(getLLVMContext(), "swift.refcounted");
  RefCountedPtrTy = RefCountedStructTy->getPointerTo(/*addrspace*/ 0);
  RefCountedNull = llvm::ConstantPointerNull::get(RefCountedPtrTy);

  // For now, native weak references are just a pointer.
  WeakReferencePtrTy =
    createStructPointerType(*this, "swift.weak", { RefCountedPtrTy });

  // A type metadata record is the structure pointed to by the canonical
  // address point of a type metadata.  This is at least one word, and
  // potentially more than that, past the start of the actual global
  // structure.
  TypeMetadataStructTy = createStructType(*this, "swift.type", {
    MetadataKindTy          // MetadataKind Kind;
  });
  TypeMetadataPtrTy = TypeMetadataStructTy->getPointerTo(DefaultAS);

  // A protocol descriptor describes a protocol. It is not type metadata in
  // and of itself, but is referenced in the structure of existential type
  // metadata records.
  ProtocolDescriptorStructTy = createStructType(*this, "swift.protocol", {
    Int8PtrTy,              // objc isa
    Int8PtrTy,              // name
    Int8PtrTy,              // inherited protocols
    Int8PtrTy,              // required objc instance methods
    Int8PtrTy,              // required objc class methods
    Int8PtrTy,              // optional objc instance methods
    Int8PtrTy,              // optional objc class methods
    Int8PtrTy,              // objc properties
    Int32Ty,                // size
    Int32Ty                 // flags
  });
  
  ProtocolDescriptorPtrTy = ProtocolDescriptorStructTy->getPointerTo();
  
  // A tuple type metadata record has a couple extra fields.
  auto tupleElementTy = createStructType(*this, "swift.tuple_element_type", {
    TypeMetadataPtrTy,      // Metadata *Type;
    SizeTy                  // size_t Offset;
  });
  TupleTypeMetadataPtrTy = createStructPointerType(*this, "swift.tuple_type", {
    TypeMetadataStructTy,   // (base)
    SizeTy,                 // size_t NumElements;
    Int8PtrTy,              // const char *Labels;
    llvm::ArrayType::get(tupleElementTy, 0) // Element Elements[];
  });

  // A full type metadata record is basically just an adjustment to the
  // address point of a type metadata.  Resilience may cause
  // additional data to be laid out prior to this address point.
  FullTypeMetadataStructTy = createStructType(*this, "swift.full_type", {
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });
  FullTypeMetadataPtrTy = FullTypeMetadataStructTy->getPointerTo(DefaultAS);

  // A metadata pattern is a structure from which generic type
  // metadata are allocated.  We leave this struct type intentionally
  // opaque, because the compiler basically never needs to access
  // anything from one.
  TypeMetadataPatternStructTy =
    llvm::StructType::create(getLLVMContext(), "swift.type_pattern");
  TypeMetadataPatternPtrTy =
    TypeMetadataPatternStructTy->getPointerTo(DefaultAS);

  DeallocatingDtorTy = llvm::FunctionType::get(VoidTy, RefCountedPtrTy, false);
  llvm::Type *dtorPtrTy = DeallocatingDtorTy->getPointerTo();

  // A full heap metadata is basically just an additional small prefix
  // on a full metadata, used for metadata corresponding to heap
  // allocations.
  FullHeapMetadataStructTy =
                  createStructType(*this, "swift.full_heapmetadata", {
    dtorPtrTy,
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });
  FullHeapMetadataPtrTy = FullHeapMetadataStructTy->getPointerTo(DefaultAS);

  llvm::Type *refCountedElts[] = { TypeMetadataPtrTy, Int32Ty, Int32Ty };
  RefCountedStructTy->setBody(refCountedElts);

  PtrSize = Size(DataLayout.getPointerSize(DefaultAS));

  FunctionPairTy = createStructType(*this, "swift.function", {
    FunctionPtrTy,
    RefCountedPtrTy,
  });
  
  WitnessFunctionPairTy = createStructType(*this, "swift.witness_function",  {
    FunctionPtrTy,
    TypeMetadataPtrTy,
  });
  
  OpaquePtrTy = llvm::StructType::create(LLVMContext, "swift.opaque")
                  ->getPointerTo(DefaultAS);

  FixedBufferTy = nullptr;
  for (unsigned i = 0; i != MaxNumValueWitnesses; ++i)
    ValueWitnessTys[i] = nullptr;

  ObjCPtrTy = llvm::StructType::create(getLLVMContext(), "objc_object")
                ->getPointerTo(DefaultAS);

  ObjCClassStructTy = llvm::StructType::create(LLVMContext, "objc_class");
  ObjCClassPtrTy = ObjCClassStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcClassElts[] = {
    ObjCClassPtrTy,
    ObjCClassPtrTy,
    OpaquePtrTy,
    OpaquePtrTy,
    IntPtrTy
  };
  ObjCClassStructTy->setBody(objcClassElts);

  ObjCSuperStructTy = llvm::StructType::create(LLVMContext, "objc_super");
  ObjCSuperPtrTy = ObjCSuperStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcSuperElts[] = {
    ObjCPtrTy,
    ObjCClassPtrTy
  };
  ObjCSuperStructTy->setBody(objcSuperElts);
      
  // TODO: use "tinycc" on platforms that support it
  RuntimeCC = llvm::CallingConv::C;

  auto CI = static_cast<ClangImporter*>(&*Context.getClangModuleLoader());
  assert(CI && "no clang module loader");

  auto &clangASTContext = CI->getClangASTContext();
  ABITypes = new CodeGenABITypes(clangASTContext, Module, DataLayout);

  if (Opts.DebugInfo) {
    DebugInfo = new IRGenDebugInfo(Opts, *CI, *this, Module);
  }
}

IRGenModule::~IRGenModule() {
  delete &Types;
  if (DebugInfo)
    delete DebugInfo;
  delete ABITypes;
}

static llvm::Constant *getRuntimeFn(IRGenModule &IGM,
                      llvm::Constant *&cache,
                      char const *name,
                      llvm::CallingConv::ID cc,
                      std::initializer_list<llvm::Type*> retTypes,
                      std::initializer_list<llvm::Type*> argTypes,
                      std::initializer_list<Attribute::AttrKind> attrs
                         = std::initializer_list<Attribute::AttrKind>()) {
  if (cache)
    return cache;
  
  llvm::Type *retTy;
  if (retTypes.size() == 1)
    retTy = *retTypes.begin();
  else
    retTy = llvm::StructType::get(IGM.LLVMContext,
                                  {retTypes.begin(), retTypes.end()},
                                  /*packed*/ false);
  auto fnTy = llvm::FunctionType::get(retTy,
                                      {argTypes.begin(), argTypes.end()},
                                      /*isVararg*/ false);

  cache = IGM.Module.getOrInsertFunction(name, fnTy);

  // Add any function attributes and set the calling convention.
  if (auto fn = dyn_cast<llvm::Function>(cache)) {
    fn->setCallingConv(cc);

    llvm::AttrBuilder b;

    for (auto Attr : attrs)
      b.addAttribute(Attr);

    fn->getAttributes().
      addAttributes(IGM.LLVMContext,
                    llvm::AttributeSet::FunctionIndex,
                    llvm::AttributeSet::get(IGM.LLVMContext,
                                            llvm::AttributeSet::FunctionIndex,
                                            b));
  }

  return cache;
}


// Explicitly listing these constants is an unfortunate compromise for
// making the database file much more compact.
//
// They have to be non-local because otherwise we'll get warnings when
// a particular x-macro expansion doesn't use one.
namespace RuntimeConstants {
  const auto ReadNone = llvm::Attribute::ReadNone;
  const auto ReadOnly = llvm::Attribute::ReadOnly;
  const auto NoUnwind = llvm::Attribute::NoUnwind;
  const auto C_CC = llvm::CallingConv::C;
}

#define RETURNS(...) { __VA_ARGS__ }
#define ARGS(...) { __VA_ARGS__ }
#define NO_ARGS {}
#define ATTRS(...) { __VA_ARGS__ }
#define NO_ATTRS {}
#define FUNCTION(ID, NAME, CC, RETURNS, ARGS, ATTRS)       \
llvm::Constant *IRGenModule::get##ID##Fn() {               \
  using namespace RuntimeConstants;                        \
  return getRuntimeFn(*this, ID##Fn, #NAME, CC,            \
                      RETURNS, ARGS, ATTRS);               \
}
#include "RuntimeFunctions.def"

llvm::Constant *IRGenModule::getEmptyTupleMetadata() {
  if (EmptyTupleMetadata)
    return EmptyTupleMetadata;

  return EmptyTupleMetadata =
    Module.getOrInsertGlobal("_TMdT_", FullTypeMetadataStructTy);
}

llvm::Constant *IRGenModule::getObjCEmptyCachePtr() {
  if (ObjCEmptyCachePtr) return ObjCEmptyCachePtr;

  // struct objc_cache _objc_empty_cache;
  ObjCEmptyCachePtr = Module.getOrInsertGlobal("_objc_empty_cache",
                                               OpaquePtrTy->getElementType());
  return ObjCEmptyCachePtr;
}

llvm::Constant *IRGenModule::getObjCEmptyVTablePtr() {
  if (ObjCEmptyVTablePtr) return ObjCEmptyVTablePtr;

  // IMP _objc_empty_vtable;

  // On recent Darwin platforms, this symbol is actually defined at
  // runtime as an absolute symbol with the value of null.  On some
  // older platforms, that wasn't true, and it isn't clear that the
  // ObjC runtime is willing to make a *guarantee* that it's true, so
  // in general we still use the symbol.  However, there are a number
  // of (non-ABI) environments that don't actually support absolute
  // symbols correctly, such as the iOS simulator, and for these we
  // have to fill in null directly.

  if (TargetInfo.ObjCUseNullForEmptyVTable) {
    ObjCEmptyVTablePtr = llvm::ConstantPointerNull::get(OpaquePtrTy);
  } else {
    ObjCEmptyVTablePtr = Module.getOrInsertGlobal("_objc_empty_vtable",
                                                  OpaquePtrTy->getElementType());
  }
  return ObjCEmptyVTablePtr;
}

llvm::Module *IRGenModule::getModule() const {
  return ClangCodeGen->GetModule();
}

llvm::Module *IRGenModule::releaseModule() {
  return ClangCodeGen->ReleaseModule();
}

llvm::Constant *IRGenModule::getSize(Size size) {
  return llvm::ConstantInt::get(SizeTy, size.getValue());
}

void IRGenModule::addLinkLibrary(const LinkLibrary &linkLib) {
  llvm::LLVMContext &ctx = Module.getContext();

  switch (linkLib.getKind()) {
  case LibraryKind::Library: {
    // FIXME: Use target-independent linker option.
    // Clang uses CGM.getTargetCodeGenInfo().getDependentLibraryOption(...).
    llvm::SmallString<32> buf;
    buf += "-l";
    buf += linkLib.getName();
    auto flag = llvm::MDString::get(ctx, buf);
    AutolinkEntries.push_back(llvm::MDNode::get(ctx, flag));
    break;
  }
  case LibraryKind::Framework:
    llvm::Value *args[] = {
      llvm::MDString::get(ctx, "-framework"),
      llvm::MDString::get(ctx, linkLib.getName())
    };
    AutolinkEntries.push_back(llvm::MDNode::get(ctx, args));
    break;
  }
}

// FIXME: This should just be the implementation of
// llvm::array_pod_sort_comparator. The only difference is that it uses
// std::less instead of operator<.
template <typename T>
static int pointerPODSortComparator(T * const *lhs, T * const *rhs) {
  std::less<T *> lt;
  if (lt(*lhs, *rhs))
    return -1;
  if (lt(*rhs, *lhs))
    return -1;
  return 0;
}

void IRGenModule::emitAutolinkInfo() {
  // FIXME: This constant should be vended by LLVM somewhere.
  static const char * const LinkerOptionsFlagName = "Linker Options";

  // Remove duplicates.
  llvm::array_pod_sort(AutolinkEntries.begin(), AutolinkEntries.end(),
                       pointerPODSortComparator);
  auto newEnd = std::unique(AutolinkEntries.begin(), AutolinkEntries.end());
  AutolinkEntries.erase(newEnd, AutolinkEntries.end());

  llvm::LLVMContext &ctx = Module.getContext();
  Module.addModuleFlag(llvm::Module::AppendUnique, LinkerOptionsFlagName,
                       llvm::MDNode::get(ctx, AutolinkEntries));
}

void IRGenModule::finalize() {
  emitGlobalLists();
  emitAutolinkInfo();
  if (DebugInfo)
    DebugInfo->finalize();
}

void IRGenModule::unimplemented(SourceLoc loc, StringRef message) {
  Context.Diags.diagnose(loc, diag::irgen_unimplemented, message);
}

void IRGenModule::error(SourceLoc loc, const Twine &message) {
  SmallVector<char, 128> buffer;
  Context.Diags.diagnose(loc, diag::irgen_failure,
                         message.toStringRef(buffer));
}
