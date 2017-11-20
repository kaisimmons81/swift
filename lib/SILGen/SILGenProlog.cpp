//===--- SILGenProlog.cpp - Function prologue emission --------------------===//
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

#include "SILGenFunction.h"
#include "Initialization.h"
#include "ManagedValue.h"
#include "Scope.h"
#include "swift/SIL/SILArgument.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"

using namespace swift;
using namespace Lowering;

SILValue SILGenFunction::emitSelfDecl(VarDecl *selfDecl) {
  // Emit the implicit 'self' argument.
  SILType selfType = getLoweredLoadableType(selfDecl->getType());
  SILValue selfValue = F.begin()->createFunctionArgument(selfType, selfDecl);
  VarLocs[selfDecl] = VarLoc::get(selfValue);
  SILLocation PrologueLoc(selfDecl);
  PrologueLoc.markAsPrologue();
  unsigned ArgNo = 1; // Hardcoded for destructors.
  B.createDebugValue(PrologueLoc, selfValue, {selfDecl->isLet(), ArgNo});
  return selfValue;
}

namespace {

/// Cleanup that writes back to an inout argument on function exit.
class CleanupWriteBackToInOut : public Cleanup {
  VarDecl *var;
  SILValue inoutAddr;

public:
  CleanupWriteBackToInOut(VarDecl *var, SILValue inoutAddr)
    : var(var), inoutAddr(inoutAddr) {}

  void emit(SILGenFunction &SGF, CleanupLocation l) override {
    // Assign from the local variable to the inout address with an
    // 'autogenerated' copyaddr.
    l.markAutoGenerated();
    SGF.B.createCopyAddr(l, SGF.VarLocs[var].value, inoutAddr,
                         IsNotTake, IsNotInitialization);
  }
};
} // end anonymous namespace

  
namespace {
class StrongReleaseCleanup : public Cleanup {
  SILValue box;
public:
  StrongReleaseCleanup(SILValue box) : box(box) {}
  void emit(SILGenFunction &SGF, CleanupLocation l) override {
    SGF.B.emitDestroyValueOperation(l, box);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "DeallocateValueBuffer\n"
                 << "State: " << getState() << "box: " << box << "\n";
#endif
  }
};
} // end anonymous namespace


namespace {
class EmitBBArguments : public CanTypeVisitor<EmitBBArguments,
                                              /*RetTy*/ ManagedValue>
{
public:
  SILGenFunction &SGF;
  SILBasicBlock *parent;
  SILLocation loc;
  bool functionArgs;
  ArrayRef<SILParameterInfo> &parameters;

  EmitBBArguments(SILGenFunction &sgf, SILBasicBlock *parent,
                  SILLocation l, bool functionArgs,
                  ArrayRef<SILParameterInfo> &parameters)
    : SGF(sgf), parent(parent), loc(l), functionArgs(functionArgs),
      parameters(parameters) {}

  ManagedValue getManagedValue(SILValue arg, CanType t,
                               SILParameterInfo parameterInfo) const {
    switch (parameterInfo.getConvention()) {
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Indirect_In_Guaranteed:
      // If we have a guaranteed parameter, it is passed in at +0, and its
      // lifetime is guaranteed. We can potentially use the argument as-is
      // if the parameter is bound as a 'let' without cleaning up.
      return ManagedValue::forUnmanaged(arg);

    case ParameterConvention::Direct_Unowned:
      // An unowned parameter is passed at +0, like guaranteed, but it isn't
      // kept alive by the caller, so we need to retain and manage it
      // regardless.
      return SGF.emitManagedRetain(loc, arg);

    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
      // An inout parameter is +0 and guaranteed, but represents an lvalue.
      return ManagedValue::forLValue(arg);

    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Indirect_In:
      // An owned or 'in' parameter is passed in at +1. We can claim ownership
      // of the parameter and clean it up when it goes out of scope.
      return SGF.emitManagedRValueWithCleanup(arg);

    case ParameterConvention::Indirect_In_Constant:
      break;
    }
    llvm_unreachable("bad parameter convention");
  }

  ManagedValue visitType(CanType t) {
    auto argType = SGF.getLoweredType(t);
    // Pop the next parameter info.
    auto parameterInfo = parameters.front();
    parameters = parameters.slice(1);
    assert(
        argType
            == parent->getParent()->mapTypeIntoContext(
                   SGF.getSILType(parameterInfo))
        && "argument does not have same type as specified by parameter info");

    SILValue arg =
        parent->createFunctionArgument(argType, loc.getAsASTNode<ValueDecl>());
    ManagedValue mv = getManagedValue(arg, t, parameterInfo);

    // If the value is a (possibly optional) ObjC block passed into the entry
    // point of the function, then copy it so we can treat the value reliably
    // as a heap object. Escape analysis can eliminate this copy if it's
    // unneeded during optimization.
    CanType objectType = t;
    if (auto theObjTy = t.getAnyOptionalObjectType())
      objectType = theObjTy;
    if (functionArgs
        && isa<FunctionType>(objectType)
        && cast<FunctionType>(objectType)->getRepresentation()
              == FunctionType::Representation::Block) {
      SILValue blockCopy = SGF.B.createCopyBlock(loc, mv.getValue());
      mv = SGF.emitManagedRValueWithCleanup(blockCopy);
    }
    return mv;
  }

  ManagedValue visitTupleType(CanTupleType t) {
    SmallVector<ManagedValue, 4> elements;

    auto &tl = SGF.getTypeLowering(t);
    bool canBeGuaranteed = tl.isLoadable();

    // Collect the exploded elements.
    for (auto fieldType : t.getElementTypes()) {
      auto elt = visit(fieldType);
      // If we can't borrow one of the elements as a guaranteed parameter, then
      // we have to +1 the tuple.
      if (elt.hasCleanup())
        canBeGuaranteed = false;
      elements.push_back(elt);
    }

    if (tl.isLoadable() || !SGF.silConv.useLoweredAddresses()) {
      SmallVector<SILValue, 4> elementValues;
      if (canBeGuaranteed) {
        // If all of the elements were guaranteed, we can form a guaranteed tuple.
        for (auto element : elements)
          elementValues.push_back(element.getUnmanagedValue());
      } else {
        // Otherwise, we need to move or copy values into a +1 tuple.
        for (auto element : elements) {
          SILValue value = element.hasCleanup()
            ? element.forward(SGF)
            : element.copyUnmanaged(SGF, loc).forward(SGF);
          elementValues.push_back(value);
        }
      }
      auto tupleValue = SGF.B.createTuple(loc, tl.getLoweredType(),
                                          elementValues);
      return canBeGuaranteed
        ? ManagedValue::forUnmanaged(tupleValue)
        : SGF.emitManagedRValueWithCleanup(tupleValue);
    } else {
      // If the type is address-only, we need to move or copy the elements into
      // a tuple in memory.
      // TODO: It would be a bit more efficient to use a preallocated buffer
      // in this case.
      auto buffer = SGF.emitTemporaryAllocation(loc, tl.getLoweredType());
      for (auto i : indices(elements)) {
        auto element = elements[i];
        auto elementBuffer = SGF.B.createTupleElementAddr(loc, buffer,
                                        i, element.getType().getAddressType());
        if (element.hasCleanup())
          element.forwardInto(SGF, loc, elementBuffer);
        else
          element.copyInto(SGF, elementBuffer, loc);
      }
      return SGF.emitManagedRValueWithCleanup(buffer);
    }
  }
};
} // end anonymous namespace

  
namespace {

/// A helper for creating SILArguments and binding variables to the argument
/// names.
struct ArgumentInitHelper {
  SILGenFunction &SGF;
  SILFunction &f;
  SILGenBuilder &initB;

  /// An ArrayRef that we use in our SILParameterList queue. Parameters are
  /// sliced off of the front as they're emitted.
  ArrayRef<SILParameterInfo> parameters;
  unsigned ArgNo = 0;

  ArgumentInitHelper(SILGenFunction &SGF, SILFunction &f)
    : SGF(SGF), f(f), initB(SGF.B),
      parameters(f.getLoweredFunctionType()->getParameters()) {
  }

  unsigned getNumArgs() const { return ArgNo; }

  ManagedValue makeArgument(Type ty, SILBasicBlock *parent, SILLocation l) {
    assert(ty && "no type?!");

    // Create an RValue by emitting destructured arguments into a basic block.
    CanType canTy = ty->eraseDynamicSelfType()->getCanonicalType();
    return EmitBBArguments(SGF, parent, l, /*functionArgs*/ true,
                           parameters).visit(canTy);
  }

  /// Create a SILArgument and store its value into the given Initialization,
  /// if not null.
  void makeArgumentIntoBinding(Type ty, SILBasicBlock *parent, VarDecl *vd) {
    SILLocation loc(vd);
    loc.markAsPrologue();

    ManagedValue argrv = makeArgument(ty, parent, loc);

    // Create a shadow copy of inout parameters so they can be captured
    // by closures. The InOutDeshadowing guaranteed optimization will
    // eliminate the variable if it is not needed.
    if (vd->isInOut()) {
      SILValue address = argrv.getUnmanagedValue();

      CanType objectType = vd->getType()->getInOutObjectType()->getCanonicalType();

      // As a special case, don't introduce a local variable for
      // Builtin.UnsafeValueBuffer, which is not copyable.
      if (isa<BuiltinUnsafeValueBufferType>(objectType)) {
        // FIXME: mark a debug location?
        SGF.VarLocs[vd] = SILGenFunction::VarLoc::get(address);
        SGF.B.createDebugValueAddr(loc, address, {vd->isLet(), ArgNo});
        return;
      }
      assert(argrv.getType().isAddress() && "expected inout to be address");
    } else if (auto *metatypeTy = ty->getAs<MetatypeType>()) {
      // This is a hack to deal with the fact that Self.Type comes in as a
      // static metatype, but we have to downcast it to a dynamic Self
      // metatype to get the right semantics.
      if (metatypeTy->getInstanceType()->is<DynamicSelfType>()) {
        auto loweredTy = SGF.getLoweredType(ty);
        if (loweredTy != argrv.getType()) {
          argrv = ManagedValue::forUnmanaged(
            SGF.B.createUncheckedBitCast(loc, argrv.getValue(), loweredTy));
        }
      }
    } else {
      assert((vd->isLet() || vd->isShared())
             && "expected parameter to be immutable!");
      // If the variable is immutable, we can bind the value as is.
      // Leave the cleanup on the argument, if any, in place to consume the
      // argument if we're responsible for it.
    }
    SGF.VarLocs[vd] = SILGenFunction::VarLoc::get(argrv.getValue());
    if (argrv.getType().isAddress())
      SGF.B.createDebugValueAddr(loc, argrv.getValue(), {vd->isLet(), ArgNo});
    else
      SGF.B.createDebugValue(loc, argrv.getValue(), {vd->isLet(), ArgNo});
  }

  void emitParam(ParamDecl *PD) {
    auto type = PD->getType();

    ++ArgNo;
    if (PD->hasName()) {
      makeArgumentIntoBinding(type, &*f.begin(), PD);
      return;
    }

    emitAnonymousParam(type, PD, PD);
  }

  void emitAnonymousParam(Type type, SILLocation paramLoc, ParamDecl *PD) {
    // Allow non-materializable tuples to be bound to anonymous parameters.
    if (!type->isMaterializable()) {
      if (auto tupleType = type->getAs<TupleType>()) {
        for (auto eltType : tupleType->getElementTypes()) {
          emitAnonymousParam(eltType, paramLoc, nullptr);
        }
        return;
      }
    }

    // A value bound to _ is unused and can be immediately released.
    Scope discardScope(SGF.Cleanups, CleanupLocation(PD));

    // Manage the parameter.
    ManagedValue argrv = makeArgument(type, &*f.begin(), paramLoc);

    // Don't do anything else if we don't have a parameter.
    if (!PD) return;

    // Emit debug information for the argument.
    SILLocation loc(PD);
    loc.markAsPrologue();
    if (argrv.getType().isAddress())
      SGF.B.createDebugValueAddr(loc, argrv.getValue(), {PD->isLet(), ArgNo});
    else
      SGF.B.createDebugValue(loc, argrv.getValue(), {PD->isLet(), ArgNo});
  }
};
} // end anonymous namespace

  
static void makeArgument(Type ty, ParamDecl *decl,
                         SmallVectorImpl<SILValue> &args, SILGenFunction &SGF) {
  assert(ty && "no type?!");
  
  // Destructure tuple arguments.
  if (TupleType *tupleTy = ty->getAs<TupleType>()) {
    for (auto fieldType : tupleTy->getElementTypes())
      makeArgument(fieldType, decl, args, SGF);
  } else {
    auto arg =
        SGF.F.begin()->createFunctionArgument(SGF.getLoweredType(ty), decl);
    args.push_back(arg);
  }
}


void SILGenFunction::bindParametersForForwarding(const ParameterList *params,
                                     SmallVectorImpl<SILValue> &parameters) {
  for (auto param : *params) {
    Type type = (param->hasType()
                 ? param->getType()
                 : F.mapTypeIntoContext(param->getInterfaceType()));
    makeArgument(type->eraseDynamicSelfType(), param, parameters, *this);
  }
}

static void emitCaptureArguments(SILGenFunction &SGF,
                                 AnyFunctionRef closure,
                                 CapturedValue capture,
                                 unsigned ArgNo) {

  auto *VD = capture.getDecl();
  SILLocation Loc(VD);
  Loc.markAsPrologue();

  // Local function to get the captured variable type within the capturing
  // context.
  auto getVarTypeInCaptureContext = [&]() -> Type {
    auto interfaceType = VD->getInterfaceType();
    return GenericEnvironment::mapTypeIntoContext(
      closure.getGenericEnvironment(), interfaceType);
  };

  switch (SGF.SGM.Types.getDeclCaptureKind(capture)) {
  case CaptureKind::None:
    break;

  case CaptureKind::Constant: {
    auto type = getVarTypeInCaptureContext();
    auto &lowering = SGF.getTypeLowering(type);
    // Constant decls are captured by value.
    SILType ty = lowering.getLoweredType();
    SILValue val = SGF.F.begin()->createFunctionArgument(ty, VD);

    bool NeedToDestroyValueAtExit =
        !SGF.SGM.M.getOptions().EnableGuaranteedClosureContexts;

    // If the original variable was settable, then Sema will have treated the
    // VarDecl as an lvalue, even in the closure's use.  As such, we need to
    // allow formation of the address for this captured value.  Create a
    // temporary within the closure to provide this address.
    if (VD->isSettable(VD->getDeclContext())) {
      auto addr = SGF.emitTemporaryAllocation(VD, ty);
      if (SGF.SGM.M.getOptions().EnableGuaranteedClosureContexts) {
        // We have created a copy that needs to be destroyed.
        val = SGF.B.createCopyValue(Loc, val);
        NeedToDestroyValueAtExit = true;
      }
      lowering.emitStore(SGF.B, VD, val, addr, StoreOwnershipQualifier::Init);
      val = addr;
    }

    SGF.VarLocs[VD] = SILGenFunction::VarLoc::get(val);
    if (auto *AllocStack = dyn_cast<AllocStackInst>(val))
      AllocStack->setArgNo(ArgNo);
    else 
      SGF.B.createDebugValue(Loc, val, {/*Constant*/true, ArgNo});

    // TODO: Closure contexts should always be guaranteed.
    if (NeedToDestroyValueAtExit && !lowering.isTrivial())
      SGF.enterDestroyCleanup(val);
    break;
  }

  case CaptureKind::Box: {
    // LValues are captured as a retained @box that owns
    // the captured value.
    auto type = getVarTypeInCaptureContext();
    auto boxTy = SGF.SGM.Types.getContextBoxTypeForCapture(VD,
                               SGF.getLoweredType(type).getSwiftRValueType(),
                               SGF.F.getGenericEnvironment(), /*mutable*/ true);
    SILValue box = SGF.F.begin()->createFunctionArgument(
        SILType::getPrimitiveObjectType(boxTy), VD);
    SILValue addr = SGF.B.createProjectBox(VD, box, 0);
    SGF.VarLocs[VD] = SILGenFunction::VarLoc::get(addr, box);
    SGF.B.createDebugValueAddr(Loc, addr, {/*Constant*/false, ArgNo});
    if (!SGF.SGM.M.getOptions().EnableGuaranteedClosureContexts)
      SGF.Cleanups.pushCleanup<StrongReleaseCleanup>(box);
    break;
  }
  case CaptureKind::StorageAddress: {
    // Non-escaping stored decls are captured as the address of the value.
    auto type = getVarTypeInCaptureContext();
    SILType ty = SGF.getLoweredType(type).getAddressType();
    SILValue addr = SGF.F.begin()->createFunctionArgument(ty, VD);
    SGF.VarLocs[VD] = SILGenFunction::VarLoc::get(addr);
    SGF.B.createDebugValueAddr(Loc, addr, {/*Constant*/true, ArgNo});
    break;
  }
  }
}

void SILGenFunction::emitProlog(AnyFunctionRef TheClosure,
                                ArrayRef<ParameterList*> paramPatterns,
                                Type resultType, bool throws) {
  unsigned ArgNo = emitProlog(paramPatterns, resultType,
                              TheClosure.getAsDeclContext(), throws);

  // Emit the capture argument variables. These are placed last because they
  // become the first curry level of the SIL function.
  auto captureInfo = SGM.Types.getLoweredLocalCaptures(TheClosure);
  for (auto capture : captureInfo.getCaptures()) {
    if (capture.isDynamicSelfMetadata()) {
      auto selfMetatype = MetatypeType::get(
        captureInfo.getDynamicSelfType());
      SILType ty = getLoweredType(selfMetatype);
      SILValue val = F.begin()->createFunctionArgument(ty);
      (void) val;

      return;
    }

    emitCaptureArguments(*this, TheClosure, capture, ++ArgNo);
  }
}

static void emitIndirectResultParameters(SILGenFunction &SGF, Type resultType,
                                         DeclContext *DC) {
  // Expand tuples.
  if (auto tupleType = resultType->getAs<TupleType>()) {
    for (auto eltType : tupleType->getElementTypes()) {
      emitIndirectResultParameters(SGF, eltType, DC);
    }
    return;
  }

  // If the return type is address-only, emit the indirect return argument.

  const TypeLowering &resultTI =
      SGF.getTypeLowering(DC->mapTypeIntoContext(resultType));
  if (!SILModuleConventions::isReturnedIndirectlyInSIL(
          resultTI.getLoweredType(), SGF.SGM.M)) {
    return;
  }
  auto &ctx = SGF.getASTContext();
  auto var = new (ctx) ParamDecl(VarDecl::Specifier::InOut,
                                 SourceLoc(), SourceLoc(),
                                 ctx.getIdentifier("$return_value"), SourceLoc(),
                                 ctx.getIdentifier("$return_value"), Type(),
                                 DC);
  var->setInterfaceType(resultType);

  auto *arg =
      SGF.F.begin()->createFunctionArgument(resultTI.getLoweredType(), var);
  (void)arg;
}

unsigned SILGenFunction::emitProlog(ArrayRef<ParameterList *> paramLists,
                                    Type resultType, DeclContext *DC,
                                    bool throws) {
  // Create the indirect result parameters.
  auto *genericSig = DC->getGenericSignatureOfContext();
  resultType = resultType->getCanonicalType(genericSig);

  emitIndirectResultParameters(*this, resultType, DC);

  // Emit the argument variables in calling convention order.
  ArgumentInitHelper emitter(*this, F);

  for (ParameterList *paramList : reversed(paramLists)) {
    // Add the SILArguments and use them to initialize the local argument
    // values.
    for (auto &param : *paramList)
      emitter.emitParam(param);
  }

  // Record the ArgNo of the artificial $error inout argument. 
  unsigned ArgNo = emitter.getNumArgs();
  if (throws) {
    RegularLocation Loc{SourceLoc()};
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC))
      Loc = AFD->getThrowsLoc();
    else if (auto *ACE = dyn_cast<AbstractClosureExpr>(DC))
      Loc = ACE->getLoc();
    auto NativeErrorTy = SILType::getExceptionType(getASTContext());
    ManagedValue Undef = emitUndef(Loc, NativeErrorTy);
    B.createDebugValue(Loc, Undef.getValue(),
                       {"$error", /*Constant*/ false, ++ArgNo});
  }

  return ArgNo;
}

