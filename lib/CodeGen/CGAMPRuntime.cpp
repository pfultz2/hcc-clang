//===----- CGAMPRuntime.cpp - Interface to C++ AMP Runtime ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides an abstract class for C++ AMP code generation.  Concrete
// subclasses of this implement code generation for specific C++ AMP
// runtime libraries.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CGAMPRuntime.h"
#include "clang/AST/Decl.h"
#include "clang/AST/ExprCXX.h"
#include "CGCall.h"

namespace clang {
namespace CodeGen {

CGAMPRuntime::~CGAMPRuntime() {}

/// Creates an instance of a C++ AMP runtime class.
CGAMPRuntime *CreateAMPRuntime(CodeGenModule &CGM) {
  return new CGAMPRuntime(CGM);
}
static CXXMethodDecl *findValidIndexType(QualType IndexTy) {
  CXXRecordDecl *IndexClass = (*IndexTy).getAsCXXRecordDecl();
  CXXMethodDecl *IndexConstructor = NULL;
  if (IndexClass) {
    for (CXXRecordDecl::method_iterator CtorIt = IndexClass->method_begin(),
        CtorE = IndexClass->method_end();
        CtorIt != CtorE; ++CtorIt) {
      if (CtorIt->hasAttr<AnnotateAttr>() &&
          CtorIt->getAttr<AnnotateAttr>()->getAnnotation() ==
            "__cxxamp_opencl_index") {
        IndexConstructor = *CtorIt;
      }
    }
  }
  return IndexConstructor;
}
/// Operations:
/// For each reference-typed members, construct temporary object
/// Invoke constructor of index
/// Invoke constructor of the class
/// Invoke operator(index)
void CGAMPRuntime::EmitTrampolineBody(CodeGenFunction &CGF,
  const FunctionDecl *Trampoline, FunctionArgList& Args) {
  const CXXRecordDecl *ClassDecl = dyn_cast<CXXMethodDecl>(Trampoline)->getParent();
  assert(ClassDecl);
  // Allocate "this"
  llvm::AllocaInst *ai = CGF.CreateMemTemp(QualType(ClassDecl->getTypeForDecl(),0));
  // Locate the constructor to call
  if(ClassDecl->getCXXAMPDeserializationConstructor()==NULL) {
    return;
  }
  CXXConstructorDecl *DeserializeConstructor =
    dyn_cast<CXXConstructorDecl>(
      ClassDecl->getCXXAMPDeserializationConstructor());
  assert(DeserializeConstructor);
  CallArgList DeserializerArgs;
  // this
  DeserializerArgs.add(RValue::get(ai),
    DeserializeConstructor ->getThisType(CGF.getContext()));
  // the rest of constructor args. Create temporary objects for references
  // on stack
  CXXConstructorDecl::param_iterator CPI = DeserializeConstructor->param_begin(),
    CPE = DeserializeConstructor->param_end();
  for (FunctionArgList::iterator I = Args.begin();
       I != Args.end() && CPI != CPE; ++CPI) {
    // Reference types are only allowed to have one level; i.e. no
    // class base {&int}; class foo { bar &base; };
    QualType MemberType = (*CPI)->getType().getNonReferenceType();
    if (MemberType != (*CPI)->getType()) {
      if (!CGM.getLangOpts().HSAExtension) {
        assert(MemberType.getTypePtr()->isClassType() == true &&
               "Only supporting taking reference of classes");
        CXXRecordDecl *MemberClass = MemberType.getTypePtr()->getAsCXXRecordDecl();
        CXXConstructorDecl *MemberDeserializer =
  	dyn_cast<CXXConstructorDecl>(
            MemberClass->getCXXAMPDeserializationConstructor());
        assert(MemberDeserializer);
        std::vector<Stmt *>MemberArgDeclRefs;
        for (CXXMethodDecl::param_iterator MCPI = 
  	      MemberDeserializer->param_begin(),
  	    MCPE = MemberDeserializer->param_end(); MCPI!=MCPE; ++MCPI, ++I) {
          Expr *ArgDeclRef = DeclRefExpr::Create(CGM.getContext(),
              NestedNameSpecifierLoc(),
              SourceLocation(),
              const_cast<VarDecl *>(*I),
              false,
              SourceLocation(),
              (*MCPI)->getType(), VK_RValue);
           MemberArgDeclRefs.push_back(ArgDeclRef);
        }
        // Allocate "this" for member referenced objects
        llvm::AllocaInst *mai =
  	CGF.CreateMemTemp(MemberType);
        // Emit code to call the deserializing constructor of temp objects
        CGF.EmitCXXConstructorCall(MemberDeserializer,
  	  Ctor_Complete, false, false,
  	  mai, ConstExprIterator(&*MemberArgDeclRefs.begin()),
  	  ConstExprIterator(&*MemberArgDeclRefs.end()));
        DeserializerArgs.add(RValue::get(mai), (*CPI)->getType());
      } else { // HSA extension check
        if (MemberType.getTypePtr()->isClassType()) {
          std::string Info = MemberType.getAsString();

          // hc::array should still be serialized as traditional C++AMP objects
          if (Info.find("hc::array<") != std::string::npos) {
            CXXRecordDecl *MemberClass = MemberType.getTypePtr()->getAsCXXRecordDecl();
            CXXConstructorDecl *MemberDeserializer =
            dyn_cast<CXXConstructorDecl>(
                MemberClass->getCXXAMPDeserializationConstructor());
            assert(MemberDeserializer);
            std::vector<Stmt *>MemberArgDeclRefs;
            for (CXXMethodDecl::param_iterator MCPI =
                  MemberDeserializer->param_begin(),
                MCPE = MemberDeserializer->param_end(); MCPI!=MCPE; ++MCPI, ++I) {
              Expr *ArgDeclRef = DeclRefExpr::Create(CGM.getContext(),
                  NestedNameSpecifierLoc(),
                  SourceLocation(),
                  const_cast<VarDecl *>(*I),
                  false,
                  SourceLocation(),
                  (*MCPI)->getType(), VK_RValue);
               MemberArgDeclRefs.push_back(ArgDeclRef);
            }
            // Allocate "this" for member referenced objects
            llvm::AllocaInst *mai =
            CGF.CreateMemTemp(MemberType);
            // Emit code to call the deserializing constructor of temp objects
            CGF.EmitCXXConstructorCall(MemberDeserializer,
              Ctor_Complete, false, false,
              mai, ConstExprIterator(&*MemberArgDeclRefs.begin()),
              ConstExprIterator(&*MemberArgDeclRefs.end()));
            DeserializerArgs.add(RValue::get(mai), (*CPI)->getType());

          } else {
            // capture by refernce for HSA
            Expr *ArgDeclRef = DeclRefExpr::Create(CGM.getContext(),
                NestedNameSpecifierLoc(),
                SourceLocation(),
                const_cast<VarDecl *>(*I), false,
                SourceLocation(),
                (*I)->getType(), VK_RValue);
            RValue ArgRV = CGF.EmitAnyExpr(ArgDeclRef);
            DeserializerArgs.add(ArgRV, CGM.getContext().getPointerType(MemberType));
            ++I;
          }
        } else {
          // capture by refernce for HSA
          Expr *ArgDeclRef = DeclRefExpr::Create(CGM.getContext(),
              NestedNameSpecifierLoc(),
              SourceLocation(),
              const_cast<VarDecl *>(*I), false,
              SourceLocation(),
              (*I)->getType(), VK_RValue);
          RValue ArgRV = CGF.EmitAnyExpr(ArgDeclRef);
          DeserializerArgs.add(ArgRV, CGM.getContext().getPointerType(MemberType));
          ++I;
        }
      } // HSA extension check
    } else {
      Expr *ArgDeclRef = DeclRefExpr::Create(CGM.getContext(),
	  NestedNameSpecifierLoc(),
	  SourceLocation(),
	  const_cast<VarDecl *>(*I), false,
	  SourceLocation(),
	  (*I)->getType(), VK_RValue);
      RValue ArgRV = CGF.EmitAnyExpr(ArgDeclRef);
      DeserializerArgs.add(ArgRV, (*CPI)->getType());
      ++I;
    }
  }
  // Emit code to call the deserializing constructor
  {
    llvm::Value *Callee = CGM.GetAddrOfCXXConstructor(
      DeserializeConstructor, Ctor_Complete);
    const FunctionProtoType *FPT = 
      DeserializeConstructor->getType()->castAs<FunctionProtoType>();
    RequiredArgs required = RequiredArgs::forPrototypePlus(FPT,
      DeserializerArgs.size());

    const CGFunctionInfo &DesFnInfo =
      CGM.getTypes().arrangeCXXMethodCall(
	  DeserializerArgs, FPT, required);
    CGF.EmitCall(DesFnInfo, Callee, ReturnValueSlot(), DeserializerArgs, 0);
  }
  // Locate the type of Concurrency::index<1>
  // Locate the operator to call
  CXXMethodDecl *KernelDecl = NULL;
  const FunctionType *MT = NULL;
  QualType IndexTy;
  for (CXXRecordDecl::method_iterator Method = ClassDecl->method_begin(),
      MethodEnd = ClassDecl->method_end();
      Method != MethodEnd; ++Method) {
    CXXMethodDecl *MethodDecl = *Method;
    if (MethodDecl->isOverloadedOperator() &&
        MethodDecl->getOverloadedOperator() == OO_Call &&
        MethodDecl->hasAttr<CXXAMPRestrictAMPAttr>()) {
      //Check types.
      if(MethodDecl->getNumParams() != 1)
	continue;
      ParmVarDecl *P = MethodDecl->getParamDecl(0);
      IndexTy = P->getType().getNonReferenceType();
      if (!findValidIndexType(IndexTy))
        continue;
      MT = dyn_cast<FunctionType>(MethodDecl->getType().getTypePtr());
      assert(MT);
      KernelDecl = MethodDecl;
      break;
    }
  }

  // in case we couldn't find any kernel declarator
  // raise error
  if (!KernelDecl) {
    CGF.CGM.getDiags().Report(ClassDecl->getLocation(), diag::err_amp_ill_formed_functor);
    return;
  }
  // Allocate Index
  llvm::AllocaInst *index = CGF.CreateMemTemp(IndexTy);

  // Locate the constructor to call
  CXXMethodDecl *IndexConstructor = findValidIndexType(IndexTy); 
  assert(IndexConstructor);
  // Emit code to call the Concurrency::index<1>::__cxxamp_opencl_index()
  if (!CGF.getLangOpts().AMPCPU) {
    if (CXXConstructorDecl *Constructor =
        dyn_cast <CXXConstructorDecl>(IndexConstructor)) {
      CGF.EmitCXXConstructorCall(Constructor,
          Ctor_Complete, false, false,
          index, ConstExprIterator(NULL), ConstExprIterator(NULL));
    } else {
      llvm::FunctionType *indexInitType =
        CGM.getTypes().GetFunctionType(
          CGM.getTypes().arrangeCXXMethodDeclaration(IndexConstructor));
      llvm::Value *indexInitAddr = CGM.GetAddrOfFunction(
        IndexConstructor, indexInitType);

      CGF.EmitCXXMemberCall(IndexConstructor, SourceLocation(), indexInitAddr,
        ReturnValueSlot(), index, /*ImplicitParam=*/0, QualType(),
        ConstExprIterator(NULL), ConstExprIterator(NULL));
    }
  }
  // Invoke this->operator(index)
  // Prepate the operator() to call
  llvm::FunctionType *fnType =
    CGM.getTypes().GetFunctionType(CGM.getTypes().arrangeCXXMethodDeclaration(KernelDecl));
  llvm::Value *fnAddr = CGM.GetAddrOfFunction(KernelDecl, fnType);
  // Prepare argument
  CallArgList KArgs;
  // this
  KArgs.add(RValue::get(ai), KernelDecl ->getThisType(CGF.getContext()));
  // *index
  KArgs.add(RValue::getAggregate(index), IndexTy);

  const CGFunctionInfo &FnInfo = CGM.getTypes().arrangeFreeFunctionCall(KArgs, MT);
  CGF.EmitCall(FnInfo, fnAddr, ReturnValueSlot(), KArgs, 0);
}

void CGAMPRuntime::EmitTrampolineNameBody(CodeGenFunction &CGF,
  const FunctionDecl *Trampoline, FunctionArgList& Args) {
  const CXXRecordDecl *ClassDecl = dyn_cast<CXXMethodDecl>(Trampoline)->getParent();
  assert(ClassDecl);
  // Locate the trampoline
  // Locate the operator to call
  CXXMethodDecl *TrampolineDecl = NULL; 
  for (CXXRecordDecl::method_iterator Method = ClassDecl->method_begin(),
      MethodEnd = ClassDecl->method_end();
      Method != MethodEnd; ++Method) {
    CXXMethodDecl *MethodDecl = *Method;
    if (Method->hasAttr<AnnotateAttr>() &&
        Method->getAttr<AnnotateAttr>()->getAnnotation() == "__cxxamp_trampoline") {
      TrampolineDecl = MethodDecl;
      break;
    }
  }
  assert(TrampolineDecl && "Trampoline not declared!");
  GlobalDecl GD(TrampolineDecl);
  llvm::Constant *S = llvm::ConstantDataArray::getString(CGM.getLLVMContext(),
    CGM.getMangledName(GD));
  llvm::GlobalVariable *GV = new llvm::GlobalVariable(CGM.getModule(), S->getType(),
    true, llvm::GlobalValue::PrivateLinkage, S, "__cxxamp_trampoline.kernelname");
  GV->setUnnamedAddr(true);
  
  //Create GetElementPtr(0, 0)
  std::vector<llvm::Constant*> indices;
  llvm::ConstantInt *zero = llvm::ConstantInt::get(CGM.getLLVMContext(), llvm::APInt(32, StringRef("0"), 10));
  indices.push_back(zero);
  indices.push_back(zero);
  llvm::Constant *const_ptr = llvm::ConstantExpr::getGetElementPtr(GV, indices);
  CGF.Builder.CreateStore(const_ptr, CGF.ReturnValue);

}
}
}
