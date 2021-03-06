//===- MipsCallLowering.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the lowering of LLVM calls to machine code calls for
/// GlobalISel.
//
//===----------------------------------------------------------------------===//

#include "MipsCallLowering.h"
#include "MipsCCState.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"

using namespace llvm;

MipsCallLowering::MipsCallLowering(const MipsTargetLowering &TLI)
    : CallLowering(&TLI) {}

bool MipsCallLowering::MipsHandler::assign(const CCValAssign &VA,
                                           unsigned vreg) {
  if (VA.isRegLoc()) {
    assignValueToReg(vreg, VA.getLocReg());
  } else {
    return false;
  }
  return true;
}

namespace {
class IncomingValueHandler : public MipsCallLowering::MipsHandler {
public:
  IncomingValueHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI)
      : MipsHandler(MIRBuilder, MRI) {}

  bool handle(ArrayRef<CCValAssign> ArgLocs,
              ArrayRef<CallLowering::ArgInfo> Args);

private:
  virtual void assignValueToReg(unsigned ValVReg, unsigned PhysReg) override;

  void markPhysRegUsed(unsigned PhysReg) {
    MIRBuilder.getMBB().addLiveIn(PhysReg);
  }
};
} // end anonymous namespace

void IncomingValueHandler::assignValueToReg(unsigned ValVReg,
                                            unsigned PhysReg) {
  MIRBuilder.buildCopy(ValVReg, PhysReg);
  markPhysRegUsed(PhysReg);
}

bool IncomingValueHandler::handle(ArrayRef<CCValAssign> ArgLocs,
                                  ArrayRef<CallLowering::ArgInfo> Args) {
  for (unsigned i = 0, ArgsSize = Args.size(); i < ArgsSize; ++i) {
    if (!assign(ArgLocs[i], Args[i].Reg))
      return false;
  }
  return true;
}

namespace {
class OutgoingValueHandler : public MipsCallLowering::MipsHandler {
public:
  OutgoingValueHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                       MachineInstrBuilder &MIB)
      : MipsHandler(MIRBuilder, MRI), MIB(MIB) {}

  bool handle(ArrayRef<CCValAssign> ArgLocs,
              ArrayRef<CallLowering::ArgInfo> Args);

private:
  virtual void assignValueToReg(unsigned ValVReg, unsigned PhysReg) override;

  MachineInstrBuilder &MIB;
};
} // end anonymous namespace

void OutgoingValueHandler::assignValueToReg(unsigned ValVReg,
                                            unsigned PhysReg) {
  MIRBuilder.buildCopy(PhysReg, ValVReg);
  MIB.addUse(PhysReg, RegState::Implicit);
}

bool OutgoingValueHandler::handle(ArrayRef<CCValAssign> ArgLocs,
                                  ArrayRef<CallLowering::ArgInfo> Args) {
  for (unsigned i = 0; i < Args.size(); ++i) {
    if (!assign(ArgLocs[i], Args[i].Reg))
      return false;
  }
  return true;
}

static bool isSupportedType(Type *T) {
  if (T->isIntegerTy() && T->getScalarSizeInBits() == 32)
    return true;
  return false;
}

bool MipsCallLowering::lowerReturn(MachineIRBuilder &MIRBuilder,
                                   const Value *Val, unsigned VReg) const {

  MachineInstrBuilder Ret = MIRBuilder.buildInstrNoInsert(Mips::RetRA);

  if (Val != nullptr) {
    if (!isSupportedType(Val->getType()))
      return false;

    MachineFunction &MF = MIRBuilder.getMF();
    const Function &F = MF.getFunction();
    const DataLayout &DL = MF.getDataLayout();
    const MipsTargetLowering &TLI = *getTLI<MipsTargetLowering>();

    SmallVector<ArgInfo, 8> RetInfos;
    SmallVector<unsigned, 8> OrigArgIndices;

    ArgInfo ArgRetInfo(VReg, Val->getType());
    setArgFlags(ArgRetInfo, AttributeList::ReturnIndex, DL, F);
    splitToValueTypes(ArgRetInfo, 0, RetInfos, OrigArgIndices);

    SmallVector<ISD::OutputArg, 8> Outs;
    subTargetRegTypeForCallingConv(
        MIRBuilder, RetInfos, OrigArgIndices,
        [&](ISD::ArgFlagsTy flags, EVT vt, EVT argvt, bool used,
            unsigned origIdx, unsigned partOffs) {
          Outs.emplace_back(flags, vt, argvt, used, origIdx, partOffs);
        });

    SmallVector<CCValAssign, 16> ArgLocs;
    MipsCCState CCInfo(F.getCallingConv(), F.isVarArg(), MF, ArgLocs,
                       F.getContext());
    CCInfo.AnalyzeReturn(Outs, TLI.CCAssignFnForReturn());

    OutgoingValueHandler RetHandler(MIRBuilder, MF.getRegInfo(), Ret);
    if (!RetHandler.handle(ArgLocs, RetInfos)) {
      return false;
    }
  }
  MIRBuilder.insertInstr(Ret);
  return true;
}

bool MipsCallLowering::lowerFormalArguments(MachineIRBuilder &MIRBuilder,
                                            const Function &F,
                                            ArrayRef<unsigned> VRegs) const {

  // Quick exit if there aren't any args.
  if (F.arg_empty())
    return true;

  if (F.isVarArg()) {
    return false;
  }

  for (auto &Arg : F.args()) {
    if (!isSupportedType(Arg.getType()))
      return false;
  }

  MachineFunction &MF = MIRBuilder.getMF();
  const DataLayout &DL = MF.getDataLayout();
  const MipsTargetLowering &TLI = *getTLI<MipsTargetLowering>();

  SmallVector<ArgInfo, 8> ArgInfos;
  SmallVector<unsigned, 8> OrigArgIndices;
  unsigned i = 0;
  for (auto &Arg : F.args()) {
    ArgInfo AInfo(VRegs[i], Arg.getType());
    setArgFlags(AInfo, i + AttributeList::FirstArgIndex, DL, F);
    splitToValueTypes(AInfo, i, ArgInfos, OrigArgIndices);
    ++i;
  }

  SmallVector<ISD::InputArg, 8> Ins;
  subTargetRegTypeForCallingConv(
      MIRBuilder, ArgInfos, OrigArgIndices,
      [&](ISD::ArgFlagsTy flags, EVT vt, EVT argvt, bool used, unsigned origIdx,
          unsigned partOffs) {
        Ins.emplace_back(flags, vt, argvt, used, origIdx, partOffs);
      });

  SmallVector<CCValAssign, 16> ArgLocs;
  MipsCCState CCInfo(F.getCallingConv(), F.isVarArg(), MF, ArgLocs,
                     F.getContext());

  CCInfo.AnalyzeFormalArguments(Ins, TLI.CCAssignFnForCall());

  IncomingValueHandler Handler(MIRBuilder, MF.getRegInfo());
  if (!Handler.handle(ArgLocs, ArgInfos))
    return false;

  return true;
}

void MipsCallLowering::subTargetRegTypeForCallingConv(
    MachineIRBuilder &MIRBuilder, ArrayRef<ArgInfo> Args,
    ArrayRef<unsigned> OrigArgIndices, const FunTy &PushBack) const {
  MachineFunction &MF = MIRBuilder.getMF();
  const Function &F = MF.getFunction();
  const DataLayout &DL = F.getParent()->getDataLayout();
  const MipsTargetLowering &TLI = *getTLI<MipsTargetLowering>();

  unsigned ArgNo = 0;
  for (auto &Arg : Args) {

    EVT VT = TLI.getValueType(DL, Arg.Ty);
    MVT RegisterVT = TLI.getRegisterTypeForCallingConv(F.getContext(), VT);

    ISD::ArgFlagsTy Flags = Arg.Flags;
    Flags.setOrigAlign(TLI.getABIAlignmentForCallingConv(Arg.Ty, DL));

    PushBack(Flags, RegisterVT, VT, true, OrigArgIndices[ArgNo], 0);

    ++ArgNo;
  }
}

void MipsCallLowering::splitToValueTypes(
    const ArgInfo &OrigArg, unsigned OriginalIndex,
    SmallVectorImpl<ArgInfo> &SplitArgs,
    SmallVectorImpl<unsigned> &SplitArgsOrigIndices) const {

  // TODO : perform structure and array split. For now we only deal with
  // types that pass isSupportedType check.
  SplitArgs.push_back(OrigArg);
  SplitArgsOrigIndices.push_back(OriginalIndex);
}
