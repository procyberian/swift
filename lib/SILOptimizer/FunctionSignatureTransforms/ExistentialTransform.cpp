//===------- ExistentialTransform.cpp - Transform Existential Args -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Transform existential parameters to generic ones.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-existential-transform"
#include "ExistentialTransform.h"
#include "swift/AST/ConformanceLookup.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/Existential.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;

using llvm::SmallDenseMap;
using llvm::SmallPtrSet;
using llvm::SmallVector;
using llvm::SmallVectorImpl;

/// Create a SILCloner for Existential Specilizer.
namespace {
class ExistentialSpecializerCloner
    : public TypeSubstCloner<ExistentialSpecializerCloner,
                             SILOptFunctionBuilder> {
  using SuperTy =
      TypeSubstCloner<ExistentialSpecializerCloner, SILOptFunctionBuilder>;
  friend class SILInstructionVisitor<ExistentialSpecializerCloner>;
  friend class SILCloner<ExistentialSpecializerCloner>;

  SILFunction *OrigF;
  SmallVector<ArgumentDescriptor, 4> &ArgumentDescList;
  SmallDenseMap<int, GenericTypeParamType *> &ArgToGenericTypeMap;
  SmallDenseMap<int, ExistentialTransformArgumentDescriptor>
      &ExistentialArgDescriptor;

  // AllocStack instructions introduced in the new prolog that require cleanup.
  SmallVector<AllocStackInst *, 4> AllocStackInsts;
  // Temporary values introduced in the new prolog that require cleanup.
  SmallVector<SILValue, 4> CleanupValues;

protected:
  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    SILClonerWithScopes<ExistentialSpecializerCloner>::postProcess(Orig,
                                                                   Cloned);
  }

  void cloneArguments(SmallVectorImpl<SILValue> &entryArgs);

public:
  ExistentialSpecializerCloner(
      SILFunction *OrigF, SILFunction *NewF, SubstitutionMap Subs,
      SmallVector<ArgumentDescriptor, 4> &ArgumentDescList,
      SmallDenseMap<int, GenericTypeParamType *> &ArgToGenericTypeMap,
      SmallDenseMap<int, ExistentialTransformArgumentDescriptor>
          &ExistentialArgDescriptor)
      : SuperTy(*NewF, *OrigF, Subs), OrigF(OrigF),
        ArgumentDescList(ArgumentDescList),
        ArgToGenericTypeMap(ArgToGenericTypeMap),
        ExistentialArgDescriptor(ExistentialArgDescriptor) {}

  void cloneAndPopulateFunction();
};
} // end anonymous namespace

/// This function will create the generic version.
void ExistentialSpecializerCloner::cloneAndPopulateFunction() {
  SmallVector<SILValue, 4> entryArgs;
  entryArgs.reserve(OrigF->getArguments().size());
  cloneArguments(entryArgs);

  // Visit original BBs in depth-first preorder, starting with the
  // entry block, cloning all instructions and terminators.
  auto *NewEntryBB = getBuilder().getFunction().getEntryBlock();
  cloneFunctionBody(&Original, NewEntryBB, entryArgs);

  // Cleanup allocations created in the new prolog.
  SmallVector<SILBasicBlock *, 4> exitingBlocks;
  getBuilder().getFunction().findExitingBlocks(exitingBlocks);
  for (auto *exitBB : exitingBlocks) {
    SILBuilderWithScope Builder(exitBB->getTerminator());
    // A return location can't be used for a non-return instruction.
    auto loc = RegularLocation::getAutoGeneratedLocation();
    for (SILValue cleanupVal : CleanupValues) {
      assert(cleanupVal->getOwnershipKind() != OwnershipKind::Guaranteed);
      Builder.emitDestroyOperation(loc, cleanupVal);
    }

    for (auto *ASI : llvm::reverse(AllocStackInsts))
      Builder.createDeallocStack(loc, ASI);
  }
}

// Create the entry basic block with the function arguments.
void ExistentialSpecializerCloner::cloneArguments(
    SmallVectorImpl<SILValue> &entryArgs) {
  auto &M = OrigF->getModule();

  // Create the new entry block.
  SILFunction &NewF = getBuilder().getFunction();
  SILBasicBlock *ClonedEntryBB = NewF.createBasicBlock();

  /// Builder will have a ScopeClone with a debugscope that is inherited from
  /// the F.
  ScopeCloner SC(NewF);
  auto DebugScope = SC.getOrCreateClonedScope(OrigF->getDebugScope());

  // Setup a NewFBuilder for the new entry block, reusing the cloner's
  // SILBuilderContext.
  SILBuilder NewFBuilder(ClonedEntryBB, getBuilder().getBuilderContext(),
                         DebugScope);
  auto InsertLoc = RegularLocation::getAutoGeneratedLocation();

  auto NewFTy = NewF.getLoweredFunctionType();
  SmallVector<SILParameterInfo, 4> params;
  params.append(NewFTy->getParameters().begin(), NewFTy->getParameters().end());

  for (auto &ArgDesc : ArgumentDescList) {
    auto iter = ArgToGenericTypeMap.find(ArgDesc.Index);
    if (iter == ArgToGenericTypeMap.end()) {
      // Clone arguments that are not rewritten.
      auto Ty = params[ArgDesc.Index].getArgumentType(
          M, NewFTy, NewF.getTypeExpansionContext());
      auto LoweredTy = NewF.getLoweredType(NewF.mapTypeIntoContext(Ty));
      auto MappedTy =
          LoweredTy.getCategoryType(ArgDesc.Arg->getType().getCategory());
      auto *NewArg =
          ClonedEntryBB->createFunctionArgument(MappedTy, ArgDesc.Decl);
      NewArg->copyFlags(ArgDesc.Arg);
      entryArgs.push_back(NewArg);
      continue;
    }
    // Create the generic argument.
    GenericTypeParamType *GenericParam = iter->second;
    SILType GenericSILType =
        NewF.getLoweredType(NewF.mapTypeIntoContext(GenericParam));
    GenericSILType = GenericSILType.getCategoryType(
                                          ArgDesc.Arg->getType().getCategory());
    auto *NewArg = ClonedEntryBB->createFunctionArgument(
        GenericSILType, ArgDesc.Decl,
        ValueOwnershipKind(NewF, GenericSILType,
                           ArgDesc.Arg->getArgumentConvention()));
    NewArg->copyFlags(ArgDesc.Arg);

    // Gather the conformances needed for an existential value based on an
    // opened archetype. This adds any conformances inherited from superclass
    // constraints.
    SILType ExistentialType = ArgDesc.Arg->getType().getObjectType();
    CanType OpenedType = NewArg->getType().getASTType();
    assert(!OpenedType.isAnyExistentialType());
    auto Conformances = collectExistentialConformances(
        OpenedType, ExistentialType.getASTType());

    auto ExistentialRepr =
        ArgDesc.Arg->getType().getPreferredExistentialRepresentation();
    auto &EAD = ExistentialArgDescriptor[ArgDesc.Index];
    switch (ExistentialRepr) {
    case ExistentialRepresentation::Opaque: {
      /// Create this sequence for init_existential_addr.:
      /// bb0(%0 : $*T):
      /// %3 = alloc_stack $P
      /// %4 = init_existential_addr %3 : $*P, $T
      /// copy_addr [take] %0 to [init] %4 : $*T
      /// %7 = open_existential_addr immutable_access %3 : $*P to
      /// $*@opened P
      auto *ASI =
          NewFBuilder.createAllocStack(InsertLoc, ArgDesc.Arg->getType());
      AllocStackInsts.push_back(ASI);

      auto *EAI = NewFBuilder.createInitExistentialAddr(
          InsertLoc, ASI, NewArg->getType().getASTType(), NewArg->getType(),
          Conformances);

      bool origConsumed = EAD.isConsumed;
      // If the existential is not consumed in the function body, then the one
      // we introduce here needs cleanup.
      if (!origConsumed)
        CleanupValues.push_back(ASI);

      NewFBuilder.createCopyAddr(InsertLoc, NewArg, EAI,
                                 origConsumed ? IsTake_t::IsTake
                                              : IsTake_t::IsNotTake,
                                 IsInitialization_t::IsInitialization);
      entryArgs.push_back(ASI);
      break;
    }
    case ExistentialRepresentation::Class: {
      SILValue NewArgValue = NewArg;
      bool origConsumed = EAD.isConsumed;

      // Load our object if needed and if our original value was not consumed,
      // make a copy in ossa. Do not perturb code-gen in non-ossa code though.
      if (!NewArg->getType().isObject()) {
        auto qual = LoadOwnershipQualifier::Take;
        if (NewFBuilder.hasOwnership() && !origConsumed) {
          qual = LoadOwnershipQualifier::Copy;
        }
        NewArgValue =
            NewFBuilder.emitLoadValueOperation(InsertLoc, NewArg, qual);
      }

      if (NewFBuilder.hasOwnership() &&
          NewArg->getOwnershipKind() == OwnershipKind::Unowned) {
        NewArgValue = NewFBuilder.emitCopyValueOperation(InsertLoc, NewArg);
      }
      ///  Simple case: Create an init_existential.
      /// %5 = init_existential_ref %0 : $T : $T, $P
      SILValue InitRef = NewFBuilder.createInitExistentialRef(
          InsertLoc, ArgDesc.Arg->getType().getObjectType(),
          NewArg->getType().getASTType(),
          NewArgValue, Conformances);

      if (NewFBuilder.hasOwnership() &&
          NewArg->getOwnershipKind() == OwnershipKind::Unowned) {
        CleanupValues.push_back(InitRef);
      }
      // If we don't have an object and we are in ossa, the store will consume
      // the InitRef.
      if (!NewArg->getType().isObject()) {
        auto alloc = NewFBuilder.createAllocStack(InsertLoc,
                                                  InitRef->getType());
        NewFBuilder.emitStoreValueOperation(InsertLoc, InitRef, alloc,
                                            StoreOwnershipQualifier::Init);
        InitRef = alloc;
        AllocStackInsts.push_back(alloc);
      }

      entryArgs.push_back(InitRef);
      break;
    }

    default: {
      llvm_unreachable("Unhandled existential type in ExistentialTransform!");
      break;
    }
    };
  }
}

/// Create a new function name for the newly generated protocol constrained
/// generic function.
std::string ExistentialTransform::createExistentialSpecializedFunctionName() {
  for (auto const &IdxIt : ExistentialArgDescriptor) {
    int Idx = IdxIt.first;
    Mangler.setArgumentExistentialToGeneric(Idx);
  }
  return Mangler.mangle();
}

/// Convert all existential argument types to generic argument type.
void ExistentialTransform::convertExistentialArgTypesToGenericArgTypes(
    SmallVectorImpl<GenericTypeParamType *> &genericParams,
    SmallVectorImpl<Requirement> &requirements) {

  SILModule &M = F->getModule();
  auto &Ctx = M.getASTContext();
  auto FTy = F->getLoweredFunctionType();

  /// If the original function is generic, then maintain the same.
  auto OrigGenericSig = FTy->getInvocationGenericSignature();

  /// Original list of parameters
  SmallVector<SILParameterInfo, 4> params;
  params.append(FTy->getParameters().begin(), FTy->getParameters().end());

  /// Determine the existing generic parameter depth.
  int Depth = OrigGenericSig.getNextDepth();

  /// Index of the Generic Parameter.
  int GPIdx = 0;

  /// Convert the protocol arguments of F to generic ones.
  for (auto const &IdxIt : ExistentialArgDescriptor) {
    int Idx = IdxIt.first;
    auto &param = params[Idx];
    auto PType = param.getArgumentType(M, FTy, F->getTypeExpansionContext());
    assert(PType.isExistentialType());

    CanType constraint = PType;
    if (auto existential = PType->getAs<ExistentialType>())
      constraint = existential->getConstraintType()->getCanonicalType();

    /// Generate new generic parameter.
    auto *NewGenericParam = GenericTypeParamType::getType(Depth, GPIdx++, Ctx);
    genericParams.push_back(NewGenericParam);
    Requirement NewRequirement(RequirementKind::Conformance, NewGenericParam,
                               constraint);
    requirements.push_back(NewRequirement);
    ArgToGenericTypeMap.insert(
        std::pair<int, GenericTypeParamType *>(Idx, NewGenericParam));
    assert(ArgToGenericTypeMap.find(Idx) != ArgToGenericTypeMap.end());
  }
}

/// Create the signature for the newly generated protocol constrained generic
/// function.
CanSILFunctionType
ExistentialTransform::createExistentialSpecializedFunctionType() {
  auto FTy = F->getLoweredFunctionType();
  SILModule &M = F->getModule();
  auto &Ctx = M.getASTContext();
  GenericSignature NewGenericSig;

  /// If the original function is generic, then maintain the same.
  auto OrigGenericSig = FTy->getInvocationGenericSignature();

  SmallVector<GenericTypeParamType *, 2> GenericParams;
  SmallVector<Requirement, 2> Requirements;

  /// Convert existential argument types to generic argument types.
  convertExistentialArgTypesToGenericArgTypes(GenericParams, Requirements);

  /// Compute the updated generic signature.
  NewGenericSig = buildGenericSignature(Ctx, OrigGenericSig,
                                        std::move(GenericParams),
                                        std::move(Requirements),
                                        /*allowInverses=*/true);

  /// Original list of parameters
  SmallVector<SILParameterInfo, 4> params;
  params.append(FTy->getParameters().begin(), FTy->getParameters().end());

  /// Create the complete list of parameters.
  int Idx = 0;
  SmallVector<SILParameterInfo, 8> InterfaceParams;
  InterfaceParams.reserve(params.size());
  for (auto &param : params) {
    auto iter = ArgToGenericTypeMap.find(Idx);
    if (iter != ArgToGenericTypeMap.end()) {
      auto GenericParam = iter->second;
      InterfaceParams.push_back(SILParameterInfo(GenericParam->getReducedType(NewGenericSig),
                                                 param.getConvention()));
    } else {
      InterfaceParams.push_back(param);
    }
    Idx++;
  }

  // Add error results.
  std::optional<SILResultInfo> InterfaceErrorResult;
  if (FTy->hasErrorResult()) {
    InterfaceErrorResult = FTy->getErrorResult();
  }

  /// Finally the ExtInfo.
  auto ExtInfo = FTy->getExtInfo();
  ExtInfo = ExtInfo.withRepresentation(SILFunctionTypeRepresentation::Thin);

  /// Return the new signature.
  return SILFunctionType::get(
      NewGenericSig, ExtInfo, FTy->getCoroutineKind(),
      FTy->getCalleeConvention(), InterfaceParams, FTy->getYields(),
      FTy->getResults(), InterfaceErrorResult,
      SubstitutionMap(), SubstitutionMap(),
      Ctx);
}

/// Create the Thunk Body with always_inline attribute.
void ExistentialTransform::populateThunkBody() {

  SILModule &M = F->getModule();

  F->setThunk(IsSignatureOptimizedThunk);
  F->setInlineStrategy(AlwaysInline);

  /// Remove original body of F.
  for (auto It = F->begin(), End = F->end(); It != End;) {
    auto *BB = &*It++;
    BB->removeDeadBlock();
  }

  /// Create a basic block and the function arguments.
  auto *ThunkBody = F->createBasicBlock();
  for (auto &ArgDesc : ArgumentDescList) {
    auto argumentType = ArgDesc.Arg->getType();
    auto *NewArg =
        ThunkBody->createFunctionArgument(argumentType, ArgDesc.Decl);
    NewArg->copyFlags(ArgDesc.Arg);
  }

  /// Builder to add new instructions in the Thunk.
  SILBuilder Builder(ThunkBody);
  Builder.setCurrentDebugScope(ThunkBody->getParent()->getDebugScope());

  /// Location to insert new instructions.
  auto Loc = ThunkBody->getParent()->getLocation();

  /// Create the function_ref instruction to the NewF.
  auto *FRI = Builder.createFunctionRefFor(Loc, NewF);

  auto GenCalleeType = NewF->getLoweredFunctionType();
  auto CalleeGenericSig = GenCalleeType->getInvocationGenericSignature();
  auto OrigGenCalleeType = F->getLoweredFunctionType();
  auto OrigCalleeGenericSig =
    OrigGenCalleeType->getInvocationGenericSignature();

  /// Determine arguments to Apply.
  /// Generate opened existentials for generics.
  SmallVector<SILValue, 8> ApplyArgs;
  // Maintain a list of arg values to be destroyed. These are consumed by the
  // convention and require a copy.
  struct Temp {
    SILValue DeallocStackEntry;
    SILValue DestroyValue;
  };
  SmallVector<Temp, 8> Temps;
  SmallDenseMap<GenericTypeParamType *, Type> GenericToOpenedTypeMap;
  for (auto &ArgDesc : ArgumentDescList) {
    auto iter = ArgToGenericTypeMap.find(ArgDesc.Index);
    auto it = ExistentialArgDescriptor.find(ArgDesc.Index);
    if (iter != ArgToGenericTypeMap.end() &&
        it != ExistentialArgDescriptor.end()) {
      ExistentialTransformArgumentDescriptor &ETAD = it->second;
      auto OrigOperand = ThunkBody->getArgument(ArgDesc.Index);
      auto SwiftType = ArgDesc.Arg->getType().getASTType();
      auto OpenedType = ExistentialArchetypeType::getAny(SwiftType)
              ->getCanonicalType();
      auto OpenedSILType = NewF->getLoweredType(OpenedType);
      SILValue archetypeValue;
      auto ExistentialRepr =
          ArgDesc.Arg->getType().getPreferredExistentialRepresentation();
      bool OriginallyConsumed = ETAD.isConsumed;
      switch (ExistentialRepr) {
      case ExistentialRepresentation::Opaque: {
        archetypeValue = Builder.createOpenExistentialAddr(
            Loc, OrigOperand, OpenedSILType, it->second.AccessType);
        SILValue calleeArg = archetypeValue;
        if (OriginallyConsumed) {
          // open_existential_addr projects a borrowed address into the
          // existential box. Since the callee consumes the generic value, we
          // must pass in a copy.
          auto *ASI =
            Builder.createAllocStack(Loc, OpenedSILType);
          Builder.createCopyAddr(Loc, archetypeValue, ASI, IsNotTake,
                                 IsInitialization_t::IsInitialization);
          Temps.push_back({ASI, OrigOperand});
          calleeArg = ASI;
        }
        ApplyArgs.push_back(calleeArg);
        break;
      }
      case ExistentialRepresentation::Class: {
        // If the operand is not object type, we need an explicit load.
        SILValue OrigValue = OrigOperand;
        if (!OrigOperand->getType().isObject()) {
          auto qual = LoadOwnershipQualifier::Take;
          if (Builder.hasOwnership() && !OriginallyConsumed) {
            qual = LoadOwnershipQualifier::Copy;
          }
          OrigValue = Builder.emitLoadValueOperation(Loc, OrigValue, qual);
        } else {
          if (Builder.hasOwnership() && !OriginallyConsumed) {
            OrigValue = Builder.emitCopyValueOperation(Loc, OrigValue);
          }
        }

        // OpenExistentialRef forwards ownership, so it does the right thing
        // regardless of whether the argument is borrowed or consumed.
        archetypeValue =
            Builder.createOpenExistentialRef(Loc, OrigValue, OpenedSILType);

        // If we don't have an object and we are in ossa, the store will consume
        // the open_existential_ref.
        if (!OrigOperand->getType().isObject()) {
          SILValue ASI = Builder.createAllocStack(Loc, OpenedSILType);
          Builder.emitStoreValueOperation(Loc, archetypeValue, ASI,
                                          StoreOwnershipQualifier::Init);
          Temps.push_back({ASI, SILValue()});
          archetypeValue = ASI;
        } else {
          // Otherwise in ossa, we need to add open_existential_ref as something
          // to be cleaned up. In non-ossa, we do not insert the copies, so we
          // do not need to do it then.
          //
          // TODO: This would be simpler if we had managed value/cleanup scopes.
          if (Builder.hasOwnership() && !OriginallyConsumed) {
            Temps.push_back({SILValue(), archetypeValue});
          }
        }
        ApplyArgs.push_back(archetypeValue);
        break;
      }
      default: {
        llvm_unreachable("Unhandled existential type in ExistentialTransform!");
        break;
      }
      };
      GenericToOpenedTypeMap.insert(
          std::pair<GenericTypeParamType *, Type>(iter->second, OpenedType));
      assert(GenericToOpenedTypeMap.find(iter->second) !=
             GenericToOpenedTypeMap.end());
    } else {
      ApplyArgs.push_back(ThunkBody->getArgument(ArgDesc.Index));
    }
  }

  unsigned int OrigDepth = OrigCalleeGenericSig.getNextDepth();
  SubstitutionMap OrigSubMap = F->getForwardingSubstitutionMap();

  /// Create substitutions for Apply instructions.
  auto SubMap = SubstitutionMap::get(
      CalleeGenericSig,
      [&](SubstitutableType *type) -> Type {
        if (auto *GP = dyn_cast<GenericTypeParamType>(type)) {
          if (GP->getDepth() < OrigDepth) {
            return Type(GP).subst(OrigSubMap);
          } else {
            auto iter = GenericToOpenedTypeMap.find(GP);
            assert(iter != GenericToOpenedTypeMap.end());
            return iter->second;
          }
        } else {
          return type;
        }
      },
      MakeAbstractConformanceForGenericType());

  /// Perform the substitutions.
  auto SubstCalleeType = GenCalleeType->substGenericArgs(
      M, SubMap, Builder.getTypeExpansionContext());

  /// Obtain the Result Type.
  SILValue ReturnValue;
  auto FunctionTy = NewF->getLoweredFunctionType();
  SILFunctionConventions Conv(SubstCalleeType, M);
  SILType ResultType = Conv.getSILResultType(Builder.getTypeExpansionContext());

  /// If the original function has error results,  we need to generate a
  /// try_apply to call a function with an error result.
  if (FunctionTy->hasErrorResult()) {
    SILFunction *Thunk = ThunkBody->getParent();
    SILBasicBlock *NormalBlock = Thunk->createBasicBlock();
    ReturnValue =
        NormalBlock->createPhiArgument(ResultType, OwnershipKind::Owned);
    SILBasicBlock *ErrorBlock = Thunk->createBasicBlock();

    SILType Error = Conv.getSILType(FunctionTy->getErrorResult(),
                                    Builder.getTypeExpansionContext());
    auto *ErrorArg = ErrorBlock->createPhiArgument(Error, OwnershipKind::Owned);
    Builder.createTryApply(Loc, FRI, SubMap, ApplyArgs, NormalBlock,
                           ErrorBlock);

    Builder.setInsertionPoint(ErrorBlock);
    Builder.createThrow(Loc, ErrorArg);
    Builder.setInsertionPoint(NormalBlock);
  } else {
    /// Create the Apply with substitutions
    ReturnValue = Builder.createApply(Loc, FRI, SubMap, ApplyArgs);
  }
  auto cleanupLoc = RegularLocation::getAutoGeneratedLocation();
  for (auto &Temp : llvm::reverse(Temps)) {
    // The original argument was copied into a temporary and consumed by the
    // callee as such:
    //   bb (%consumedExistential : $*Protocol)
    //     %valAdr = open_existential_addr %consumedExistential
    //     %temp = alloc_stack $T
    //     copy_addr %valAdr to %temp // <== Temp CopyAddr
    //     apply(%temp)               // <== Temp is consumed by the apply
    //
    // Destroy the original argument and deallocation the temporary. If we have
    // an address this becomes:
    //     destroy_addr %consumedExistential : $*Protocol
    //     dealloc_stack %temp : $*T
    //
    // Otherwise, if we had an object, we just emit a destroy_value.
    if (Temp.DestroyValue)
      Builder.emitDestroyOperation(cleanupLoc, Temp.DestroyValue);
    if (Temp.DeallocStackEntry)
      Builder.createDeallocStack(cleanupLoc, Temp.DeallocStackEntry);
  }
  /// Set up the return results.
  if (NewF->isNoReturnFunction(Builder.getTypeExpansionContext())) {
    Builder.createUnreachable(Loc);
  } else {
    Builder.createReturn(Loc, ReturnValue);
  }
}

/// Strategy to specialize existential arguments:
/// (1) Create a protocol constrained generic function from the old function;
/// (2) Create a thunk for the original function that invokes (1) including
/// setting
///     its inline strategy as always inline.
void ExistentialTransform::createExistentialSpecializedFunction() {
  std::string Name = createExistentialSpecializedFunctionName();

  /// Create devirtualized function type and populate ArgToGenericTypeMap.
  auto NewFTy = createExistentialSpecializedFunctionType();

  /// Step 1: Create the new protocol constrained generic function.
  if (auto *CachedFn = F->getModule().lookUpFunction(Name)) {
    // The specialized body still exists (because it is now called directly),
    // but the thunk has been dead-code eliminated.
    assert(CachedFn->getLoweredFunctionType() == NewFTy);
    NewF = CachedFn;
  } else {
    auto NewFGenericSig = NewFTy->getInvocationGenericSignature();
    auto NewFGenericEnv = NewFGenericSig.getGenericEnvironment();
    SILLinkage linkage = getSpecializedLinkage(F, F->getLinkage());

    NewF = FunctionBuilder.createFunction(
        linkage, Name, NewFTy, NewFGenericEnv, F->getLocation(), F->isBare(),
        F->isTransparent(), F->getSerializedKind(), IsNotDynamic,
        IsNotDistributed, IsNotRuntimeAccessible, F->getEntryCount(),
        F->isThunk(), F->getClassSubclassScope(), F->getInlineStrategy(),
        F->getEffectsKind(), nullptr, F->getDebugScope());

    /// Set the semantics attributes for the new function.
    for (auto &Attr : F->getSemanticsAttrs())
      NewF->addSemanticsAttr(Attr);

    /// Set Unqualified ownership, if any.
    if (!F->hasOwnership()) {
      NewF->setOwnershipEliminated();
    }
    /// Step 1a: Populate the body of NewF.
    SubstitutionMap Subs = SubstitutionMap::get(
      NewFGenericSig,
      [&](SubstitutableType *type) -> Type {
        return NewFGenericEnv->mapTypeIntoContext(type);
      },
      LookUpConformanceInModule());
    ExistentialSpecializerCloner cloner(F, NewF, Subs, ArgumentDescList,
                                        ArgToGenericTypeMap,
                                        ExistentialArgDescriptor);
    cloner.cloneAndPopulateFunction();
  }
  /// Step 2: Create the thunk with always_inline and populate its body.
  populateThunkBody();

  assert(F->getDebugScope()->Parent != NewF->getDebugScope()->Parent);

  LLVM_DEBUG(llvm::dbgs() << "After ExistentialSpecializer Pass\n"; F->dump();
             NewF->dump(););
}
