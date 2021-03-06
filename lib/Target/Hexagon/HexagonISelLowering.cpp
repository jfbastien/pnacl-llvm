//===-- HexagonISelLowering.cpp - Hexagon DAG Lowering Implementation -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces that Hexagon uses to lower LLVM code
// into a selection DAG.
//
//===----------------------------------------------------------------------===//

#include "HexagonISelLowering.h"
#include "HexagonMachineFunctionInfo.h"
#include "HexagonSubtarget.h"
#include "HexagonTargetMachine.h"
#include "HexagonTargetObjectFile.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "hexagon-lowering"

static cl::opt<bool>
EmitJumpTables("hexagon-emit-jump-tables", cl::init(true), cl::Hidden,
               cl::desc("Control jump table emission on Hexagon target"));

namespace {
class HexagonCCState : public CCState {
  int NumNamedVarArgParams;

public:
  HexagonCCState(CallingConv::ID CC, bool isVarArg, MachineFunction &MF,
                 SmallVectorImpl<CCValAssign> &locs, LLVMContext &C,
                 int NumNamedVarArgParams)
      : CCState(CC, isVarArg, MF, locs, C),
        NumNamedVarArgParams(NumNamedVarArgParams) {}

  int getNumNamedVarArgParams() const { return NumNamedVarArgParams; }
};
}

// Implement calling convention for Hexagon.
static bool
CC_Hexagon(unsigned ValNo, MVT ValVT,
           MVT LocVT, CCValAssign::LocInfo LocInfo,
           ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
CC_Hexagon32(unsigned ValNo, MVT ValVT,
             MVT LocVT, CCValAssign::LocInfo LocInfo,
             ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
CC_Hexagon64(unsigned ValNo, MVT ValVT,
             MVT LocVT, CCValAssign::LocInfo LocInfo,
             ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
RetCC_Hexagon(unsigned ValNo, MVT ValVT,
              MVT LocVT, CCValAssign::LocInfo LocInfo,
              ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
RetCC_Hexagon32(unsigned ValNo, MVT ValVT,
                MVT LocVT, CCValAssign::LocInfo LocInfo,
                ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
RetCC_Hexagon64(unsigned ValNo, MVT ValVT,
                MVT LocVT, CCValAssign::LocInfo LocInfo,
                ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
CC_Hexagon_VarArg (unsigned ValNo, MVT ValVT,
            MVT LocVT, CCValAssign::LocInfo LocInfo,
            ISD::ArgFlagsTy ArgFlags, CCState &State) {
  HexagonCCState &HState = static_cast<HexagonCCState &>(State);

  // NumNamedVarArgParams can not be zero for a VarArg function.
  assert((HState.getNumNamedVarArgParams() > 0) &&
         "NumNamedVarArgParams is not bigger than zero.");

  if ((int)ValNo < HState.getNumNamedVarArgParams()) {
    // Deal with named arguments.
    return CC_Hexagon(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State);
  }

  // Deal with un-named arguments.
  unsigned ofst;
  if (ArgFlags.isByVal()) {
    // If pass-by-value, the size allocated on stack is decided
    // by ArgFlags.getByValSize(), not by the size of LocVT.
    assert ((ArgFlags.getByValSize() > 8) &&
            "ByValSize must be bigger than 8 bytes");
    ofst = State.AllocateStack(ArgFlags.getByValSize(), 4);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, ofst, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::i1 || LocVT == MVT::i8 || LocVT == MVT::i16) {
    LocVT = MVT::i32;
    ValVT = MVT::i32;
    if (ArgFlags.isSExt())
      LocInfo = CCValAssign::SExt;
    else if (ArgFlags.isZExt())
      LocInfo = CCValAssign::ZExt;
    else
      LocInfo = CCValAssign::AExt;
  }
  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    ofst = State.AllocateStack(4, 4);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, ofst, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    ofst = State.AllocateStack(8, 8);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, ofst, LocVT, LocInfo));
    return false;
  }
  llvm_unreachable(nullptr);
}


static bool
CC_Hexagon (unsigned ValNo, MVT ValVT,
            MVT LocVT, CCValAssign::LocInfo LocInfo,
            ISD::ArgFlagsTy ArgFlags, CCState &State) {

  if (ArgFlags.isByVal()) {
    // Passed on stack.
    assert ((ArgFlags.getByValSize() > 8) &&
            "ByValSize must be bigger than 8 bytes");
    unsigned Offset = State.AllocateStack(ArgFlags.getByValSize(), 4);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }

  if (LocVT == MVT::i1 || LocVT == MVT::i8 || LocVT == MVT::i16) {
    LocVT = MVT::i32;
    ValVT = MVT::i32;
    if (ArgFlags.isSExt())
      LocInfo = CCValAssign::SExt;
    else if (ArgFlags.isZExt())
      LocInfo = CCValAssign::ZExt;
    else
      LocInfo = CCValAssign::AExt;
  } else if (LocVT == MVT::v4i8 || LocVT == MVT::v2i16) {
    LocVT = MVT::i32;
    LocInfo = CCValAssign::BCvt;
  } else if (LocVT == MVT::v8i8 || LocVT == MVT::v4i16 || LocVT == MVT::v2i32) {
    LocVT = MVT::i64;
    LocInfo = CCValAssign::BCvt;
  }

  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    if (!CC_Hexagon32(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }

  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    if (!CC_Hexagon64(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }

  return true;  // CC didn't match.
}


static bool CC_Hexagon32(unsigned ValNo, MVT ValVT,
                         MVT LocVT, CCValAssign::LocInfo LocInfo,
                         ISD::ArgFlagsTy ArgFlags, CCState &State) {

  static const MCPhysReg RegList[] = {
    Hexagon::R0, Hexagon::R1, Hexagon::R2, Hexagon::R3, Hexagon::R4,
    Hexagon::R5
  };
  if (unsigned Reg = State.AllocateReg(RegList)) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }

  unsigned Offset = State.AllocateStack(4, 4);
  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
  return false;
}

static bool CC_Hexagon64(unsigned ValNo, MVT ValVT,
                         MVT LocVT, CCValAssign::LocInfo LocInfo,
                         ISD::ArgFlagsTy ArgFlags, CCState &State) {

  if (unsigned Reg = State.AllocateReg(Hexagon::D0)) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }

  static const MCPhysReg RegList1[] = {
    Hexagon::D1, Hexagon::D2
  };
  static const MCPhysReg RegList2[] = {
    Hexagon::R1, Hexagon::R3
  };
  if (unsigned Reg = State.AllocateReg(RegList1, RegList2)) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }

  unsigned Offset = State.AllocateStack(8, 8, Hexagon::D2);
  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
  return false;
}

static bool RetCC_Hexagon(unsigned ValNo, MVT ValVT,
                          MVT LocVT, CCValAssign::LocInfo LocInfo,
                          ISD::ArgFlagsTy ArgFlags, CCState &State) {


  if (LocVT == MVT::i1 ||
      LocVT == MVT::i8 ||
      LocVT == MVT::i16) {
    LocVT = MVT::i32;
    ValVT = MVT::i32;
    if (ArgFlags.isSExt())
      LocInfo = CCValAssign::SExt;
    else if (ArgFlags.isZExt())
      LocInfo = CCValAssign::ZExt;
    else
      LocInfo = CCValAssign::AExt;
  } else if (LocVT == MVT::v4i8 || LocVT == MVT::v2i16) {
    LocVT = MVT::i32;
    LocInfo = CCValAssign::BCvt;
  } else if (LocVT == MVT::v8i8 || LocVT == MVT::v4i16 || LocVT == MVT::v2i32) {
    LocVT = MVT::i64;
    LocInfo = CCValAssign::BCvt;
  }

  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    if (!RetCC_Hexagon32(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
    return false;
  }

  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    if (!RetCC_Hexagon64(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
    return false;
  }

  return true;  // CC didn't match.
}

static bool RetCC_Hexagon32(unsigned ValNo, MVT ValVT,
                            MVT LocVT, CCValAssign::LocInfo LocInfo,
                            ISD::ArgFlagsTy ArgFlags, CCState &State) {

  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    if (unsigned Reg = State.AllocateReg(Hexagon::R0)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  unsigned Offset = State.AllocateStack(4, 4);
  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
  return false;
}

static bool RetCC_Hexagon64(unsigned ValNo, MVT ValVT,
                            MVT LocVT, CCValAssign::LocInfo LocInfo,
                            ISD::ArgFlagsTy ArgFlags, CCState &State) {
  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    if (unsigned Reg = State.AllocateReg(Hexagon::D0)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  unsigned Offset = State.AllocateStack(8, 8);
  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
  return false;
}

SDValue
HexagonTargetLowering::LowerINTRINSIC_WO_CHAIN(SDValue Op, SelectionDAG &DAG)
const {
  return SDValue();
}

/// CreateCopyOfByValArgument - Make a copy of an aggregate at address specified
/// by "Src" to address "Dst" of size "Size".  Alignment information is
/// specified by the specific parameter attribute. The copy will be passed as
/// a byval function parameter.  Sometimes what we are copying is the end of a
/// larger object, the part that does not fit in registers.
static SDValue
CreateCopyOfByValArgument(SDValue Src, SDValue Dst, SDValue Chain,
                          ISD::ArgFlagsTy Flags, SelectionDAG &DAG,
                          SDLoc dl) {

  SDValue SizeNode = DAG.getConstant(Flags.getByValSize(), MVT::i32);
  return DAG.getMemcpy(Chain, dl, Dst, Src, SizeNode, Flags.getByValAlign(),
                       /*isVolatile=*/false, /*AlwaysInline=*/false,
                       /*isTailCall=*/false,
                       MachinePointerInfo(), MachinePointerInfo());
}


// LowerReturn - Lower ISD::RET. If a struct is larger than 8 bytes and is
// passed by value, the function prototype is modified to return void and
// the value is stored in memory pointed by a pointer passed by caller.
SDValue
HexagonTargetLowering::LowerReturn(SDValue Chain,
                                   CallingConv::ID CallConv, bool isVarArg,
                                   const SmallVectorImpl<ISD::OutputArg> &Outs,
                                   const SmallVectorImpl<SDValue> &OutVals,
                                   SDLoc dl, SelectionDAG &DAG) const {

  // CCValAssign - represent the assignment of the return value to locations.
  SmallVector<CCValAssign, 16> RVLocs;

  // CCState - Info about the registers and stack slot.
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  // Analyze return values of ISD::RET
  CCInfo.AnalyzeReturn(Outs, RetCC_Hexagon);

  SDValue Flag;
  SmallVector<SDValue, 4> RetOps(1, Chain);

  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];

    Chain = DAG.getCopyToReg(Chain, dl, VA.getLocReg(), OutVals[i], Flag);

    // Guarantee that all emitted copies are stuck together with flags.
    Flag = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;  // Update chain.

  // Add the flag if we have it.
  if (Flag.getNode())
    RetOps.push_back(Flag);

  return DAG.getNode(HexagonISD::RET_FLAG, dl, MVT::Other, RetOps);
}




/// LowerCallResult - Lower the result values of an ISD::CALL into the
/// appropriate copies out of appropriate physical registers.  This assumes that
/// Chain/InFlag are the input chain/flag to use, and that TheCall is the call
/// being lowered. Returns a SDNode with the same number of values as the
/// ISD::CALL.
SDValue
HexagonTargetLowering::LowerCallResult(SDValue Chain, SDValue InFlag,
                                       CallingConv::ID CallConv, bool isVarArg,
                                       const
                                       SmallVectorImpl<ISD::InputArg> &Ins,
                                       SDLoc dl, SelectionDAG &DAG,
                                       SmallVectorImpl<SDValue> &InVals,
                                       const SmallVectorImpl<SDValue> &OutVals,
                                       SDValue Callee) const {

  // Assign locations to each value returned by this call.
  SmallVector<CCValAssign, 16> RVLocs;

  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  CCInfo.AnalyzeCallResult(Ins, RetCC_Hexagon);

  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    Chain = DAG.getCopyFromReg(Chain, dl,
                               RVLocs[i].getLocReg(),
                               RVLocs[i].getValVT(), InFlag).getValue(1);
    InFlag = Chain.getValue(2);
    InVals.push_back(Chain.getValue(0));
  }

  return Chain;
}

/// LowerCall - Functions arguments are copied from virtual regs to
/// (physical regs)/(stack frame), CALLSEQ_START and CALLSEQ_END are emitted.
SDValue
HexagonTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                 SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG                     = CLI.DAG;
  SDLoc &dl                             = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals     = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins   = CLI.Ins;
  SDValue Chain                         = CLI.Chain;
  SDValue Callee                        = CLI.Callee;
  bool &isTailCall                      = CLI.IsTailCall;
  CallingConv::ID CallConv              = CLI.CallConv;
  bool isVarArg                         = CLI.IsVarArg;
  bool doesNotReturn                    = CLI.DoesNotReturn;

  bool IsStructRet    = (Outs.empty()) ? false : Outs[0].Flags.isSRet();

  // Check for varargs.
  int NumNamedVarArgParams = -1;
  if (GlobalAddressSDNode *GA = dyn_cast<GlobalAddressSDNode>(Callee))
  {
    const Function* CalleeFn = nullptr;
    Callee = DAG.getTargetGlobalAddress(GA->getGlobal(), dl, MVT::i32);
    if ((CalleeFn = dyn_cast<Function>(GA->getGlobal())))
    {
      // If a function has zero args and is a vararg function, that's
      // disallowed so it must be an undeclared function.  Do not assume
      // varargs if the callee is undefined.
      if (CalleeFn->isVarArg() &&
          CalleeFn->getFunctionType()->getNumParams() != 0) {
        NumNamedVarArgParams = CalleeFn->getFunctionType()->getNumParams();
      }
    }
  }

  // Analyze operands of the call, assigning locations to each operand.
  SmallVector<CCValAssign, 16> ArgLocs;
  HexagonCCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                        *DAG.getContext(), NumNamedVarArgParams);

  if (NumNamedVarArgParams > 0)
    CCInfo.AnalyzeCallOperands(Outs, CC_Hexagon_VarArg);
  else
    CCInfo.AnalyzeCallOperands(Outs, CC_Hexagon);


  if(isTailCall) {
    bool StructAttrFlag =
      DAG.getMachineFunction().getFunction()->hasStructRetAttr();
    isTailCall = IsEligibleForTailCallOptimization(Callee, CallConv,
                                                   isVarArg, IsStructRet,
                                                   StructAttrFlag,
                                                   Outs, OutVals, Ins, DAG);
    for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i){
      CCValAssign &VA = ArgLocs[i];
      if (VA.isMemLoc()) {
        isTailCall = false;
        break;
      }
    }
    if (isTailCall) {
      DEBUG(dbgs () << "Eligible for Tail Call\n");
    } else {
      DEBUG(dbgs () <<
            "Argument must be passed on stack. Not eligible for Tail Call\n");
    }
  }
  // Get a count of how many bytes are to be pushed on the stack.
  unsigned NumBytes = CCInfo.getNextStackOffset();
  SmallVector<std::pair<unsigned, SDValue>, 16> RegsToPass;
  SmallVector<SDValue, 8> MemOpChains;

  const HexagonRegisterInfo *QRI = Subtarget->getRegisterInfo();
  SDValue StackPtr =
      DAG.getCopyFromReg(Chain, dl, QRI->getStackRegister(), getPointerTy());

  // Walk the register/memloc assignments, inserting copies/loads.
  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    SDValue Arg = OutVals[i];
    ISD::ArgFlagsTy Flags = Outs[i].Flags;

    // Promote the value if needed.
    switch (VA.getLocInfo()) {
      default:
        // Loc info must be one of Full, SExt, ZExt, or AExt.
        llvm_unreachable("Unknown loc info!");
      case CCValAssign::Full:
        break;
      case CCValAssign::SExt:
        Arg = DAG.getNode(ISD::SIGN_EXTEND, dl, VA.getLocVT(), Arg);
        break;
      case CCValAssign::ZExt:
        Arg = DAG.getNode(ISD::ZERO_EXTEND, dl, VA.getLocVT(), Arg);
        break;
      case CCValAssign::AExt:
        Arg = DAG.getNode(ISD::ANY_EXTEND, dl, VA.getLocVT(), Arg);
        break;
    }

    if (VA.isMemLoc()) {
      unsigned LocMemOffset = VA.getLocMemOffset();
      SDValue PtrOff = DAG.getConstant(LocMemOffset, StackPtr.getValueType());
      PtrOff = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, PtrOff);

      if (Flags.isByVal()) {
        // The argument is a struct passed by value. According to LLVM, "Arg"
        // is is pointer.
        MemOpChains.push_back(CreateCopyOfByValArgument(Arg, PtrOff, Chain,
                                                        Flags, DAG, dl));
      } else {
        // The argument is not passed by value. "Arg" is a buildin type. It is
        // not a pointer.
        MemOpChains.push_back(DAG.getStore(Chain, dl, Arg, PtrOff,
                                           MachinePointerInfo(),false, false,
                                           0));
      }
      continue;
    }

    // Arguments that can be passed on register must be kept at RegsToPass
    // vector.
    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
    }
  }

  // Transform all store nodes into one single node because all store
  // nodes are independent of each other.
  if (!MemOpChains.empty()) {
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOpChains);
  }

  if (!isTailCall)
    Chain = DAG.getCALLSEQ_START(Chain, DAG.getConstant(NumBytes,
                                                        getPointerTy(), true),
                                 dl);

  // Build a sequence of copy-to-reg nodes chained together with token
  // chain and flag operands which copy the outgoing args into registers.
  // The InFlag in necessary since all emitted instructions must be
  // stuck together.
  SDValue InFlag;
  if (!isTailCall) {
    for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
      Chain = DAG.getCopyToReg(Chain, dl, RegsToPass[i].first,
                               RegsToPass[i].second, InFlag);
      InFlag = Chain.getValue(1);
    }
  }

  // For tail calls lower the arguments to the 'real' stack slot.
  if (isTailCall) {
    // Force all the incoming stack arguments to be loaded from the stack
    // before any new outgoing arguments are stored to the stack, because the
    // outgoing stack slots may alias the incoming argument stack slots, and
    // the alias isn't otherwise explicit. This is slightly more conservative
    // than necessary, because it means that each store effectively depends
    // on every argument instead of just those arguments it would clobber.
    //
    // Do not flag preceding copytoreg stuff together with the following stuff.
    InFlag = SDValue();
    for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
      Chain = DAG.getCopyToReg(Chain, dl, RegsToPass[i].first,
                               RegsToPass[i].second, InFlag);
      InFlag = Chain.getValue(1);
    }
    InFlag =SDValue();
  }

  // If the callee is a GlobalAddress/ExternalSymbol node (quite common, every
  // direct call is) turn it into a TargetGlobalAddress/TargetExternalSymbol
  // node so that legalize doesn't hack it.
  if (flag_aligned_memcpy) {
    const char *MemcpyName =
      "__hexagon_memcpy_likely_aligned_min32bytes_mult8bytes";
    Callee =
      DAG.getTargetExternalSymbol(MemcpyName, getPointerTy());
    flag_aligned_memcpy = false;
  } else if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee)) {
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), dl, getPointerTy());
  } else if (ExternalSymbolSDNode *S =
             dyn_cast<ExternalSymbolSDNode>(Callee)) {
    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), getPointerTy());
  }

  // Returns a chain & a flag for retval copy to use.
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);

  // Add argument registers to the end of the list so that they are
  // known live into the call.
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
    Ops.push_back(DAG.getRegister(RegsToPass[i].first,
                                  RegsToPass[i].second.getValueType()));
  }

  if (InFlag.getNode()) {
    Ops.push_back(InFlag);
  }

  if (isTailCall)
    return DAG.getNode(HexagonISD::TC_RETURN, dl, NodeTys, Ops);

  int OpCode = doesNotReturn ? HexagonISD::CALLv3nr : HexagonISD::CALLv3;
  Chain = DAG.getNode(OpCode, dl, NodeTys, Ops);
  InFlag = Chain.getValue(1);

  // Create the CALLSEQ_END node.
  Chain = DAG.getCALLSEQ_END(Chain, DAG.getIntPtrConstant(NumBytes, true),
                             DAG.getIntPtrConstant(0, true), InFlag, dl);
  InFlag = Chain.getValue(1);

  // Handle result values, copying them out of physregs into vregs that we
  // return.
  return LowerCallResult(Chain, InFlag, CallConv, isVarArg, Ins, dl, DAG,
                         InVals, OutVals, Callee);
}

static bool getIndexedAddressParts(SDNode *Ptr, EVT VT,
                                   bool isSEXTLoad, SDValue &Base,
                                   SDValue &Offset, bool &isInc,
                                   SelectionDAG &DAG) {
  if (Ptr->getOpcode() != ISD::ADD)
  return false;

  if (VT == MVT::i64 || VT == MVT::i32 || VT == MVT::i16 || VT == MVT::i8) {
    isInc = (Ptr->getOpcode() == ISD::ADD);
    Base = Ptr->getOperand(0);
    Offset = Ptr->getOperand(1);
    // Ensure that Offset is a constant.
    return (isa<ConstantSDNode>(Offset));
  }

  return false;
}

// TODO: Put this function along with the other isS* functions in
// HexagonISelDAGToDAG.cpp into a common file. Or better still, use the
// functions defined in HexagonOperands.td.
static bool Is_PostInc_S4_Offset(SDNode * S, int ShiftAmount) {
  ConstantSDNode *N = cast<ConstantSDNode>(S);

  // immS4 predicate - True if the immediate fits in a 4-bit sign extended.
  // field.
  int64_t v = (int64_t)N->getSExtValue();
  int64_t m = 0;
  if (ShiftAmount > 0) {
    m = v % ShiftAmount;
    v = v >> ShiftAmount;
  }
  return (v <= 7) && (v >= -8) && (m == 0);
}

/// getPostIndexedAddressParts - returns true by value, base pointer and
/// offset pointer and addressing mode by reference if this node can be
/// combined with a load / store to form a post-indexed load / store.
bool HexagonTargetLowering::getPostIndexedAddressParts(SDNode *N, SDNode *Op,
                                                       SDValue &Base,
                                                       SDValue &Offset,
                                                       ISD::MemIndexedMode &AM,
                                                       SelectionDAG &DAG) const
{
  EVT VT;
  SDValue Ptr;
  bool isSEXTLoad = false;

  if (LoadSDNode *LD = dyn_cast<LoadSDNode>(N)) {
    VT  = LD->getMemoryVT();
    isSEXTLoad = LD->getExtensionType() == ISD::SEXTLOAD;
  } else if (StoreSDNode *ST = dyn_cast<StoreSDNode>(N)) {
    VT  = ST->getMemoryVT();
    if (ST->getValue().getValueType() == MVT::i64 && ST->isTruncatingStore()) {
      return false;
    }
  } else {
    return false;
  }

  bool isInc = false;
  bool isLegal = getIndexedAddressParts(Op, VT, isSEXTLoad, Base, Offset,
                                        isInc, DAG);
  // ShiftAmount = number of left-shifted bits in the Hexagon instruction.
  int ShiftAmount = VT.getSizeInBits() / 16;
  if (isLegal && Is_PostInc_S4_Offset(Offset.getNode(), ShiftAmount)) {
    AM = isInc ? ISD::POST_INC : ISD::POST_DEC;
    return true;
  }

  return false;
}

SDValue HexagonTargetLowering::LowerINLINEASM(SDValue Op,
                                              SelectionDAG &DAG) const {
  SDNode *Node = Op.getNode();
  MachineFunction &MF = DAG.getMachineFunction();
  HexagonMachineFunctionInfo *FuncInfo =
    MF.getInfo<HexagonMachineFunctionInfo>();
  switch (Node->getOpcode()) {
    case ISD::INLINEASM: {
      unsigned NumOps = Node->getNumOperands();
      if (Node->getOperand(NumOps-1).getValueType() == MVT::Glue)
        --NumOps;  // Ignore the flag operand.

      for (unsigned i = InlineAsm::Op_FirstOperand; i != NumOps;) {
        if (FuncInfo->hasClobberLR())
          break;
        unsigned Flags =
          cast<ConstantSDNode>(Node->getOperand(i))->getZExtValue();
        unsigned NumVals = InlineAsm::getNumOperandRegisters(Flags);
        ++i;  // Skip the ID value.

        switch (InlineAsm::getKind(Flags)) {
        default: llvm_unreachable("Bad flags!");
          case InlineAsm::Kind_RegDef:
          case InlineAsm::Kind_RegUse:
          case InlineAsm::Kind_Imm:
          case InlineAsm::Kind_Clobber:
          case InlineAsm::Kind_Mem: {
            for (; NumVals; --NumVals, ++i) {}
            break;
          }
          case InlineAsm::Kind_RegDefEarlyClobber: {
            for (; NumVals; --NumVals, ++i) {
              unsigned Reg =
                cast<RegisterSDNode>(Node->getOperand(i))->getReg();

              // Check it to be lr
              const HexagonRegisterInfo *QRI = Subtarget->getRegisterInfo();
              if (Reg == QRI->getRARegister()) {
                FuncInfo->setHasClobberLR(true);
                break;
              }
            }
            break;
          }
        }
      }
    }
  } // Node->getOpcode
  return Op;
}


//
// Taken from the XCore backend.
//
SDValue HexagonTargetLowering::
LowerBR_JT(SDValue Op, SelectionDAG &DAG) const
{
  SDValue Chain = Op.getOperand(0);
  SDValue Table = Op.getOperand(1);
  SDValue Index = Op.getOperand(2);
  SDLoc dl(Op);
  JumpTableSDNode *JT = cast<JumpTableSDNode>(Table);
  unsigned JTI = JT->getIndex();
  MachineFunction &MF = DAG.getMachineFunction();
  const MachineJumpTableInfo *MJTI = MF.getJumpTableInfo();
  SDValue TargetJT = DAG.getTargetJumpTable(JT->getIndex(), MVT::i32);

  // Mark all jump table targets as address taken.
  const std::vector<MachineJumpTableEntry> &JTE = MJTI->getJumpTables();
  const std::vector<MachineBasicBlock*> &JTBBs = JTE[JTI].MBBs;
  for (unsigned i = 0, e = JTBBs.size(); i != e; ++i) {
    MachineBasicBlock *MBB = JTBBs[i];
    MBB->setHasAddressTaken();
    // This line is needed to set the hasAddressTaken flag on the BasicBlock
    // object.
    BlockAddress::get(const_cast<BasicBlock *>(MBB->getBasicBlock()));
  }

  SDValue JumpTableBase = DAG.getNode(HexagonISD::JT, dl,
                                      getPointerTy(), TargetJT);
  SDValue ShiftIndex = DAG.getNode(ISD::SHL, dl, MVT::i32, Index,
                                   DAG.getConstant(2, MVT::i32));
  SDValue JTAddress = DAG.getNode(ISD::ADD, dl, MVT::i32, JumpTableBase,
                                  ShiftIndex);
  SDValue LoadTarget = DAG.getLoad(MVT::i32, dl, Chain, JTAddress,
                                   MachinePointerInfo(), false, false, false,
                                   0);
  return DAG.getNode(HexagonISD::BR_JT, dl, MVT::Other, Chain, LoadTarget);
}


SDValue
HexagonTargetLowering::LowerDYNAMIC_STACKALLOC(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Size = Op.getOperand(1);
  SDLoc dl(Op);

  unsigned SPReg = getStackPointerRegisterToSaveRestore();

  // Get a reference to the stack pointer.
  SDValue StackPointer = DAG.getCopyFromReg(Chain, dl, SPReg, MVT::i32);

  // Subtract the dynamic size from the actual stack size to
  // obtain the new stack size.
  SDValue Sub = DAG.getNode(ISD::SUB, dl, MVT::i32, StackPointer, Size);

  //
  // For Hexagon, the outgoing memory arguments area should be on top of the
  // alloca area on the stack i.e., the outgoing memory arguments should be
  // at a lower address than the alloca area. Move the alloca area down the
  // stack by adding back the space reserved for outgoing arguments to SP
  // here.
  //
  // We do not know what the size of the outgoing args is at this point.
  // So, we add a pseudo instruction ADJDYNALLOC that will adjust the
  // stack pointer. We patch this instruction with the correct, known
  // offset in emitPrologue().
  //
  // Use a placeholder immediate (zero) for now. This will be patched up
  // by emitPrologue().
  SDValue ArgAdjust = DAG.getNode(HexagonISD::ADJDYNALLOC, dl,
                                  MVT::i32,
                                  Sub,
                                  DAG.getConstant(0, MVT::i32));

  // The Sub result contains the new stack start address, so it
  // must be placed in the stack pointer register.
  const HexagonRegisterInfo *QRI = Subtarget->getRegisterInfo();
  SDValue CopyChain = DAG.getCopyToReg(Chain, dl, QRI->getStackRegister(), Sub);

  SDValue Ops[2] = { ArgAdjust, CopyChain };
  return DAG.getMergeValues(Ops, dl);
}

SDValue
HexagonTargetLowering::LowerFormalArguments(SDValue Chain,
                                            CallingConv::ID CallConv,
                                            bool isVarArg,
                                            const
                                            SmallVectorImpl<ISD::InputArg> &Ins,
                                            SDLoc dl, SelectionDAG &DAG,
                                            SmallVectorImpl<SDValue> &InVals)
const {

  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  HexagonMachineFunctionInfo *FuncInfo =
    MF.getInfo<HexagonMachineFunctionInfo>();


  // Assign locations to all of the incoming arguments.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());

  CCInfo.AnalyzeFormalArguments(Ins, CC_Hexagon);

  // For LLVM, in the case when returning a struct by value (>8byte),
  // the first argument is a pointer that points to the location on caller's
  // stack where the return value will be stored. For Hexagon, the location on
  // caller's stack is passed only when the struct size is smaller than (and
  // equal to) 8 bytes. If not, no address will be passed into callee and
  // callee return the result direclty through R0/R1.

  SmallVector<SDValue, 4> MemOps;

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    ISD::ArgFlagsTy Flags = Ins[i].Flags;
    unsigned ObjSize;
    unsigned StackLocation;
    int FI;

    if (   (VA.isRegLoc() && !Flags.isByVal())
        || (VA.isRegLoc() && Flags.isByVal() && Flags.getByValSize() > 8)) {
      // Arguments passed in registers
      // 1. int, long long, ptr args that get allocated in register.
      // 2. Large struct that gets an register to put its address in.
      EVT RegVT = VA.getLocVT();
      if (RegVT == MVT::i8 || RegVT == MVT::i16 ||
          RegVT == MVT::i32 || RegVT == MVT::f32) {
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::IntRegsRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));
      } else if (RegVT == MVT::i64 || RegVT == MVT::f64) {
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::DoubleRegsRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));
      } else {
        assert (0);
      }
    } else if (VA.isRegLoc() && Flags.isByVal() && Flags.getByValSize() <= 8) {
      assert (0 && "ByValSize must be bigger than 8 bytes");
    } else {
      // Sanity check.
      assert(VA.isMemLoc());

      if (Flags.isByVal()) {
        // If it's a byval parameter, then we need to compute the
        // "real" size, not the size of the pointer.
        ObjSize = Flags.getByValSize();
      } else {
        ObjSize = VA.getLocVT().getStoreSizeInBits() >> 3;
      }

      StackLocation = HEXAGON_LRFP_SIZE + VA.getLocMemOffset();
      // Create the frame index object for this incoming parameter...
      FI = MFI->CreateFixedObject(ObjSize, StackLocation, true);

      // Create the SelectionDAG nodes cordl, responding to a load
      // from this parameter.
      SDValue FIN = DAG.getFrameIndex(FI, MVT::i32);

      if (Flags.isByVal()) {
        // If it's a pass-by-value aggregate, then do not dereference the stack
        // location. Instead, we should generate a reference to the stack
        // location.
        InVals.push_back(FIN);
      } else {
        InVals.push_back(DAG.getLoad(VA.getLocVT(), dl, Chain, FIN,
                                     MachinePointerInfo(), false, false,
                                     false, 0));
      }
    }
  }

  if (!MemOps.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOps);

  if (isVarArg) {
    // This will point to the next argument passed via stack.
    int FrameIndex = MFI->CreateFixedObject(Hexagon_PointerSize,
                                            HEXAGON_LRFP_SIZE +
                                            CCInfo.getNextStackOffset(),
                                            true);
    FuncInfo->setVarArgsFrameIndex(FrameIndex);
  }

  return Chain;
}

SDValue
HexagonTargetLowering::LowerVASTART(SDValue Op, SelectionDAG &DAG) const {
  // VASTART stores the address of the VarArgsFrameIndex slot into the
  // memory location argument.
  MachineFunction &MF = DAG.getMachineFunction();
  HexagonMachineFunctionInfo *QFI = MF.getInfo<HexagonMachineFunctionInfo>();
  SDValue Addr = DAG.getFrameIndex(QFI->getVarArgsFrameIndex(), MVT::i32);
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Op.getOperand(0), SDLoc(Op), Addr,
                      Op.getOperand(1), MachinePointerInfo(SV), false,
                      false, 0);
}

// Creates a SPLAT instruction for a constant value VAL.
static SDValue createSplat(SelectionDAG &DAG, SDLoc dl, EVT VT, SDValue Val) {
  if (VT.getSimpleVT() == MVT::v4i8)
    return DAG.getNode(HexagonISD::VSPLATB, dl, VT, Val);

  if (VT.getSimpleVT() == MVT::v4i16)
    return DAG.getNode(HexagonISD::VSPLATH, dl, VT, Val);

  return SDValue();
}

static bool isSExtFree(SDValue N) {
  // A sign-extend of a truncate of a sign-extend is free.
  if (N.getOpcode() == ISD::TRUNCATE &&
      N.getOperand(0).getOpcode() == ISD::AssertSext)
    return true;
  // We have sign-extended loads.
  if (N.getOpcode() == ISD::LOAD)
    return true;
  return false;
}

SDValue HexagonTargetLowering::LowerCTPOP(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDValue InpVal = Op.getOperand(0);
  if (isa<ConstantSDNode>(InpVal)) {
    uint64_t V = cast<ConstantSDNode>(InpVal)->getZExtValue();
    return DAG.getTargetConstant(countPopulation(V), MVT::i64);
  }
  SDValue PopOut = DAG.getNode(HexagonISD::POPCOUNT, dl, MVT::i32, InpVal);
  return DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i64, PopOut);
}

SDValue HexagonTargetLowering::LowerSETCC(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);

  SDValue LHS = Op.getOperand(0);
  SDValue RHS = Op.getOperand(1);
  SDValue Cmp = Op.getOperand(2);
  ISD::CondCode CC = cast<CondCodeSDNode>(Cmp)->get();

  EVT VT = Op.getValueType();
  EVT LHSVT = LHS.getValueType();
  EVT RHSVT = RHS.getValueType();

  if (LHSVT == MVT::v2i16) {
    assert(ISD::isSignedIntSetCC(CC) || ISD::isUnsignedIntSetCC(CC));
    unsigned ExtOpc = ISD::isSignedIntSetCC(CC) ? ISD::SIGN_EXTEND
                                                : ISD::ZERO_EXTEND;
    SDValue LX = DAG.getNode(ExtOpc, dl, MVT::v2i32, LHS);
    SDValue RX = DAG.getNode(ExtOpc, dl, MVT::v2i32, RHS);
    SDValue SC = DAG.getNode(ISD::SETCC, dl, MVT::v2i1, LX, RX, Cmp);
    return SC;
  }

  // Treat all other vector types as legal.
  if (VT.isVector())
    return Op;

  // Equals and not equals should use sign-extend, not zero-extend, since
  // we can represent small negative values in the compare instructions.
  // The LLVM default is to use zero-extend arbitrarily in these cases.
  if ((CC == ISD::SETEQ || CC == ISD::SETNE) &&
      (RHSVT == MVT::i8 || RHSVT == MVT::i16) &&
      (LHSVT == MVT::i8 || LHSVT == MVT::i16)) {
    ConstantSDNode *C = dyn_cast<ConstantSDNode>(RHS);
    if (C && C->getAPIntValue().isNegative()) {
      LHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, LHS);
      RHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, RHS);
      return DAG.getNode(ISD::SETCC, dl, Op.getValueType(),
                         LHS, RHS, Op.getOperand(2));
    }
    if (isSExtFree(LHS) || isSExtFree(RHS)) {
      LHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, LHS);
      RHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, RHS);
      return DAG.getNode(ISD::SETCC, dl, Op.getValueType(),
                         LHS, RHS, Op.getOperand(2));
    }
  }
  return SDValue();
}

SDValue HexagonTargetLowering::LowerVSELECT(SDValue Op, SelectionDAG &DAG)
      const {
  SDValue PredOp = Op.getOperand(0);
  SDValue Op1 = Op.getOperand(1), Op2 = Op.getOperand(2);
  EVT OpVT = Op1.getValueType();
  SDLoc DL(Op);

  if (OpVT == MVT::v2i16) {
    SDValue X1 = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::v2i32, Op1);
    SDValue X2 = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::v2i32, Op2);
    SDValue SL = DAG.getNode(ISD::VSELECT, DL, MVT::v2i32, PredOp, X1, X2);
    SDValue TR = DAG.getNode(ISD::TRUNCATE, DL, MVT::v2i16, SL);
    return TR;
  }

  return SDValue();
}

// Handle only specific vector loads.
SDValue HexagonTargetLowering::LowerLOAD(SDValue Op, SelectionDAG &DAG) const {
  EVT VT = Op.getValueType();
  SDLoc DL(Op);
  LoadSDNode *LoadNode = cast<LoadSDNode>(Op);
  SDValue Chain = LoadNode->getChain();
  SDValue Ptr = Op.getOperand(1);
  SDValue LoweredLoad;
  SDValue Result;
  SDValue Base = LoadNode->getBasePtr();
  ISD::LoadExtType Ext = LoadNode->getExtensionType();
  unsigned Alignment = LoadNode->getAlignment();
  SDValue LoadChain;

  if(Ext == ISD::NON_EXTLOAD)
    Ext = ISD::ZEXTLOAD;

  if (VT == MVT::v4i16) {
    if (Alignment == 2) {
      SDValue Loads[4];
      // Base load.
      Loads[0] = DAG.getExtLoad(Ext, DL, MVT::i32, Chain, Base,
                                LoadNode->getPointerInfo(), MVT::i16,
                                LoadNode->isVolatile(),
                                LoadNode->isNonTemporal(),
                                LoadNode->isInvariant(),
                                Alignment);
      // Base+2 load.
      SDValue Increment = DAG.getConstant(2, MVT::i32);
      Ptr = DAG.getNode(ISD::ADD, DL, Base.getValueType(), Base, Increment);
      Loads[1] = DAG.getExtLoad(Ext, DL, MVT::i32, Chain, Ptr,
                                LoadNode->getPointerInfo(), MVT::i16,
                                LoadNode->isVolatile(),
                                LoadNode->isNonTemporal(),
                                LoadNode->isInvariant(),
                                Alignment);
      // SHL 16, then OR base and base+2.
      SDValue ShiftAmount = DAG.getConstant(16, MVT::i32);
      SDValue Tmp1 = DAG.getNode(ISD::SHL, DL, MVT::i32, Loads[1], ShiftAmount);
      SDValue Tmp2 = DAG.getNode(ISD::OR, DL, MVT::i32, Tmp1, Loads[0]);
      // Base + 4.
      Increment = DAG.getConstant(4, MVT::i32);
      Ptr = DAG.getNode(ISD::ADD, DL, Base.getValueType(), Base, Increment);
      Loads[2] = DAG.getExtLoad(Ext, DL, MVT::i32, Chain, Ptr,
                                LoadNode->getPointerInfo(), MVT::i16,
                                LoadNode->isVolatile(),
                                LoadNode->isNonTemporal(),
                                LoadNode->isInvariant(),
                                Alignment);
      // Base + 6.
      Increment = DAG.getConstant(6, MVT::i32);
      Ptr = DAG.getNode(ISD::ADD, DL, Base.getValueType(), Base, Increment);
      Loads[3] = DAG.getExtLoad(Ext, DL, MVT::i32, Chain, Ptr,
                                LoadNode->getPointerInfo(), MVT::i16,
                                LoadNode->isVolatile(),
                                LoadNode->isNonTemporal(),
                                LoadNode->isInvariant(),
                                Alignment);
      // SHL 16, then OR base+4 and base+6.
      Tmp1 = DAG.getNode(ISD::SHL, DL, MVT::i32, Loads[3], ShiftAmount);
      SDValue Tmp4 = DAG.getNode(ISD::OR, DL, MVT::i32, Tmp1, Loads[2]);
      // Combine to i64. This could be optimised out later if we can
      // affect reg allocation of this code.
      Result = DAG.getNode(HexagonISD::COMBINE, DL, MVT::i64, Tmp4, Tmp2);
      LoadChain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other,
                              Loads[0].getValue(1), Loads[1].getValue(1),
                              Loads[2].getValue(1), Loads[3].getValue(1));
    } else {
      // Perform default type expansion.
      Result = DAG.getLoad(MVT::i64, DL, Chain, Ptr, LoadNode->getPointerInfo(),
                           LoadNode->isVolatile(), LoadNode->isNonTemporal(),
                          LoadNode->isInvariant(), LoadNode->getAlignment());
      LoadChain = Result.getValue(1);
    }
  } else
    llvm_unreachable("Custom lowering unsupported load");

  Result = DAG.getNode(ISD::BITCAST, DL, VT, Result);
  // Since we pretend to lower a load, we need the original chain
  // info attached to the result.
  SDValue Ops[] = { Result, LoadChain };

  return DAG.getMergeValues(Ops, DL);
}


SDValue
HexagonTargetLowering::LowerConstantPool(SDValue Op, SelectionDAG &DAG) const {
  EVT ValTy = Op.getValueType();
  SDLoc dl(Op);
  ConstantPoolSDNode *CP = cast<ConstantPoolSDNode>(Op);
  SDValue Res;
  if (CP->isMachineConstantPoolEntry())
    Res = DAG.getTargetConstantPool(CP->getMachineCPVal(), ValTy,
                                    CP->getAlignment());
  else
    Res = DAG.getTargetConstantPool(CP->getConstVal(), ValTy,
                                    CP->getAlignment());
  return DAG.getNode(HexagonISD::CONST32, dl, ValTy, Res);
}

SDValue
HexagonTargetLowering::LowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const {
  const TargetRegisterInfo *TRI = Subtarget->getRegisterInfo();
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MFI->setReturnAddressIsTaken(true);

  if (verifyReturnAddressArgumentIsConstant(Op, DAG))
    return SDValue();

  EVT VT = Op.getValueType();
  SDLoc dl(Op);
  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
  if (Depth) {
    SDValue FrameAddr = LowerFRAMEADDR(Op, DAG);
    SDValue Offset = DAG.getConstant(4, MVT::i32);
    return DAG.getLoad(VT, dl, DAG.getEntryNode(),
                       DAG.getNode(ISD::ADD, dl, VT, FrameAddr, Offset),
                       MachinePointerInfo(), false, false, false, 0);
  }

  // Return LR, which contains the return address. Mark it an implicit live-in.
  unsigned Reg = MF.addLiveIn(TRI->getRARegister(), getRegClassFor(MVT::i32));
  return DAG.getCopyFromReg(DAG.getEntryNode(), dl, Reg, VT);
}

SDValue
HexagonTargetLowering::LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const {
  const HexagonRegisterInfo *TRI = Subtarget->getRegisterInfo();
  MachineFrameInfo *MFI = DAG.getMachineFunction().getFrameInfo();
  MFI->setFrameAddressIsTaken(true);

  EVT VT = Op.getValueType();
  SDLoc dl(Op);
  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
  SDValue FrameAddr = DAG.getCopyFromReg(DAG.getEntryNode(), dl,
                                         TRI->getFrameRegister(), VT);
  while (Depth--)
    FrameAddr = DAG.getLoad(VT, dl, DAG.getEntryNode(), FrameAddr,
                            MachinePointerInfo(),
                            false, false, false, 0);
  return FrameAddr;
}

SDValue HexagonTargetLowering::LowerATOMIC_FENCE(SDValue Op,
                                                 SelectionDAG& DAG) const {
  SDLoc dl(Op);
  return DAG.getNode(HexagonISD::BARRIER, dl, MVT::Other, Op.getOperand(0));
}


SDValue HexagonTargetLowering::LowerGLOBALADDRESS(SDValue Op,
                                                  SelectionDAG &DAG) const {
  SDValue Result;
  const GlobalValue *GV = cast<GlobalAddressSDNode>(Op)->getGlobal();
  int64_t Offset = cast<GlobalAddressSDNode>(Op)->getOffset();
  SDLoc dl(Op);
  Result = DAG.getTargetGlobalAddress(GV, dl, getPointerTy(), Offset);

  const HexagonTargetObjectFile *TLOF =
      static_cast<const HexagonTargetObjectFile *>(
          getTargetMachine().getObjFileLowering());
  if (TLOF->IsGlobalInSmallSection(GV, getTargetMachine())) {
    return DAG.getNode(HexagonISD::CONST32_GP, dl, getPointerTy(), Result);
  }

  return DAG.getNode(HexagonISD::CONST32, dl, getPointerTy(), Result);
}

// Specifies that for loads and stores VT can be promoted to PromotedLdStVT.
void HexagonTargetLowering::promoteLdStType(EVT VT, EVT PromotedLdStVT) {
  if (VT != PromotedLdStVT) {
    setOperationAction(ISD::LOAD, VT.getSimpleVT(), Promote);
    AddPromotedToType(ISD::LOAD, VT.getSimpleVT(),
                      PromotedLdStVT.getSimpleVT());

    setOperationAction(ISD::STORE, VT.getSimpleVT(), Promote);
    AddPromotedToType(ISD::STORE, VT.getSimpleVT(),
                      PromotedLdStVT.getSimpleVT());
  }
}

SDValue
HexagonTargetLowering::LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const {
  const BlockAddress *BA = cast<BlockAddressSDNode>(Op)->getBlockAddress();
  SDValue BA_SD =  DAG.getTargetBlockAddress(BA, MVT::i32);
  SDLoc dl(Op);
  return DAG.getNode(HexagonISD::CONST32_GP, dl, getPointerTy(), BA_SD);
}

//===----------------------------------------------------------------------===//
// TargetLowering Implementation
//===----------------------------------------------------------------------===//

HexagonTargetLowering::HexagonTargetLowering(const TargetMachine &TM,
                                             const HexagonSubtarget &STI)
    : TargetLowering(TM), Subtarget(&STI) {

  // Set up the register classes.
  addRegisterClass(MVT::v2i1, &Hexagon::PredRegsRegClass);  // bbbbaaaa
  addRegisterClass(MVT::v4i1, &Hexagon::PredRegsRegClass);  // ddccbbaa
  addRegisterClass(MVT::v8i1, &Hexagon::PredRegsRegClass);  // hgfedcba
  addRegisterClass(MVT::i32, &Hexagon::IntRegsRegClass);
  addRegisterClass(MVT::v4i8, &Hexagon::IntRegsRegClass);
  addRegisterClass(MVT::v2i16, &Hexagon::IntRegsRegClass);
  promoteLdStType(MVT::v4i8, MVT::i32);
  promoteLdStType(MVT::v2i16, MVT::i32);

  if (Subtarget->hasV5TOps()) {
    addRegisterClass(MVT::f32, &Hexagon::IntRegsRegClass);
    addRegisterClass(MVT::f64, &Hexagon::DoubleRegsRegClass);
  }

  addRegisterClass(MVT::i64, &Hexagon::DoubleRegsRegClass);
  addRegisterClass(MVT::v8i8, &Hexagon::DoubleRegsRegClass);
  addRegisterClass(MVT::v4i16, &Hexagon::DoubleRegsRegClass);
  addRegisterClass(MVT::v2i32, &Hexagon::DoubleRegsRegClass);
  promoteLdStType(MVT::v8i8, MVT::i64);

  // Custom lower v4i16 load only. Let v4i16 store to be
  // promoted for now.
  setOperationAction(ISD::LOAD, MVT::v4i16, Custom);
  AddPromotedToType(ISD::LOAD, MVT::v4i16, MVT::i64);
  setOperationAction(ISD::STORE, MVT::v4i16, Promote);
  AddPromotedToType(ISD::STORE, MVT::v4i16, MVT::i64);
  promoteLdStType(MVT::v2i32, MVT::i64);

  for (unsigned i = (unsigned) MVT::FIRST_VECTOR_VALUETYPE;
       i <= (unsigned) MVT::LAST_VECTOR_VALUETYPE; ++i) {
    MVT::SimpleValueType VT = (MVT::SimpleValueType) i;

    // Hexagon does not have support for the following operations,
    // so they need to be expanded.
    setOperationAction(ISD::SELECT, VT, Expand);
    setOperationAction(ISD::SDIV, VT, Expand);
    setOperationAction(ISD::SREM, VT, Expand);
    setOperationAction(ISD::UDIV, VT, Expand);
    setOperationAction(ISD::UREM, VT, Expand);
    setOperationAction(ISD::ROTL, VT, Expand);
    setOperationAction(ISD::ROTR, VT, Expand);
    setOperationAction(ISD::FDIV, VT, Expand);
    setOperationAction(ISD::FNEG, VT, Expand);
    setOperationAction(ISD::UMUL_LOHI, VT, Expand);
    setOperationAction(ISD::SMUL_LOHI, VT, Expand);
    setOperationAction(ISD::UDIVREM, VT, Expand);
    setOperationAction(ISD::SDIVREM, VT, Expand);
    setOperationAction(ISD::FPOW, VT, Expand);
    setOperationAction(ISD::CTPOP, VT, Expand);
    setOperationAction(ISD::CTLZ, VT, Expand);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, VT, Expand);
    setOperationAction(ISD::CTTZ, VT, Expand);
    setOperationAction(ISD::CTTZ_ZERO_UNDEF, VT, Expand);

    // Expand all any extend loads.
    for (unsigned j = (unsigned) MVT::FIRST_VECTOR_VALUETYPE;
                  j <= (unsigned) MVT::LAST_VECTOR_VALUETYPE; ++j)
      setLoadExtAction(ISD::EXTLOAD, (MVT::SimpleValueType) j, VT, Expand);

    // Expand all trunc stores.
    for (unsigned TargetVT = (unsigned) MVT::FIRST_VECTOR_VALUETYPE;
         TargetVT <= (unsigned) MVT::LAST_VECTOR_VALUETYPE; ++TargetVT)
      setTruncStoreAction(VT, (MVT::SimpleValueType) TargetVT, Expand);

    setOperationAction(ISD::VECTOR_SHUFFLE, VT, Expand);
    setOperationAction(ISD::ConstantPool, VT, Expand);
    setOperationAction(ISD::SCALAR_TO_VECTOR, VT, Expand);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, VT, Expand);
    setOperationAction(ISD::INSERT_VECTOR_ELT, VT, Expand);
    setOperationAction(ISD::BUILD_VECTOR, VT, Expand);
    setOperationAction(ISD::EXTRACT_SUBVECTOR, VT, Expand);
    setOperationAction(ISD::INSERT_SUBVECTOR, VT, Expand);
    setOperationAction(ISD::CONCAT_VECTORS, VT, Expand);
    setOperationAction(ISD::SRA, VT, Custom);
    setOperationAction(ISD::SHL, VT, Custom);
    setOperationAction(ISD::SRL, VT, Custom);

    if (!isTypeLegal(VT))
      continue;

    setOperationAction(ISD::ADD, VT, Legal);
    setOperationAction(ISD::SUB, VT, Legal);
    setOperationAction(ISD::MUL, VT, Legal);

    setOperationAction(ISD::BUILD_VECTOR, VT, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, VT, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT, VT, Custom);
    setOperationAction(ISD::EXTRACT_SUBVECTOR, VT, Custom);
    setOperationAction(ISD::INSERT_SUBVECTOR, VT, Custom);
    setOperationAction(ISD::CONCAT_VECTORS, VT, Custom);
  }

  setOperationAction(ISD::SETCC, MVT::v2i16, Custom);
  setOperationAction(ISD::VSELECT, MVT::v2i16, Custom);
  setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v8i8, Custom);
  setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v4i16, Custom);

  setOperationAction(ISD::ConstantPool, MVT::i32, Custom);

  addRegisterClass(MVT::i1, &Hexagon::PredRegsRegClass);

  computeRegisterProperties(Subtarget->getRegisterInfo());

  // Align loop entry
  setPrefLoopAlignment(4);

  // Limits for inline expansion of memcpy/memmove
  MaxStoresPerMemcpy = 6;
  MaxStoresPerMemmove = 6;

  //
  // Library calls for unsupported operations
  //

  setLibcallName(RTLIB::SINTTOFP_I128_F64, "__hexagon_floattidf");
  setLibcallName(RTLIB::SINTTOFP_I128_F32, "__hexagon_floattisf");

  setLibcallName(RTLIB::FPTOUINT_F32_I128, "__hexagon_fixunssfti");
  setLibcallName(RTLIB::FPTOUINT_F64_I128, "__hexagon_fixunsdfti");

  setLibcallName(RTLIB::FPTOSINT_F32_I128, "__hexagon_fixsfti");
  setLibcallName(RTLIB::FPTOSINT_F64_I128, "__hexagon_fixdfti");

  setLibcallName(RTLIB::SDIV_I32, "__hexagon_divsi3");
  setOperationAction(ISD::SDIV, MVT::i32, Expand);
  setLibcallName(RTLIB::SREM_I32, "__hexagon_umodsi3");
  setOperationAction(ISD::SREM, MVT::i32, Expand);

  setLibcallName(RTLIB::SDIV_I64, "__hexagon_divdi3");
  setOperationAction(ISD::SDIV, MVT::i64, Expand);
  setLibcallName(RTLIB::SREM_I64, "__hexagon_moddi3");
  setOperationAction(ISD::SREM, MVT::i64, Expand);

  setLibcallName(RTLIB::UDIV_I32, "__hexagon_udivsi3");
  setOperationAction(ISD::UDIV, MVT::i32, Expand);

  setLibcallName(RTLIB::UDIV_I64, "__hexagon_udivdi3");
  setOperationAction(ISD::UDIV, MVT::i64, Expand);

  setLibcallName(RTLIB::UREM_I32, "__hexagon_umodsi3");
  setOperationAction(ISD::UREM, MVT::i32, Expand);

  setLibcallName(RTLIB::UREM_I64, "__hexagon_umoddi3");
  setOperationAction(ISD::UREM, MVT::i64, Expand);

  setLibcallName(RTLIB::DIV_F32, "__hexagon_divsf3");
  setOperationAction(ISD::FDIV, MVT::f32, Expand);

  setLibcallName(RTLIB::DIV_F64, "__hexagon_divdf3");
  setOperationAction(ISD::FDIV, MVT::f64, Expand);

  setLibcallName(RTLIB::ADD_F64, "__hexagon_adddf3");
  setLibcallName(RTLIB::SUB_F64, "__hexagon_subdf3");
  setLibcallName(RTLIB::MUL_F64, "__hexagon_muldf3");

  setOperationAction(ISD::FSQRT, MVT::f32, Expand);
  setOperationAction(ISD::FSQRT, MVT::f64, Expand);
  setOperationAction(ISD::FSIN, MVT::f32, Expand);
  setOperationAction(ISD::FSIN, MVT::f64, Expand);

  if (Subtarget->hasV5TOps()) {
    // Hexagon V5 Support.
    setOperationAction(ISD::FADD, MVT::f32, Legal);
    setOperationAction(ISD::FADD, MVT::f64, Expand);
    setOperationAction(ISD::FSUB, MVT::f32, Legal);
    setOperationAction(ISD::FSUB, MVT::f64, Expand);
    setOperationAction(ISD::FMUL, MVT::f64, Expand);
    setOperationAction(ISD::FP_EXTEND, MVT::f32, Legal);
    setCondCodeAction(ISD::SETOEQ, MVT::f32, Legal);
    setCondCodeAction(ISD::SETOEQ, MVT::f64, Legal);
    setCondCodeAction(ISD::SETUEQ, MVT::f32, Legal);
    setCondCodeAction(ISD::SETUEQ, MVT::f64, Legal);

    setCondCodeAction(ISD::SETOGE, MVT::f32, Legal);
    setCondCodeAction(ISD::SETOGE, MVT::f64, Legal);
    setCondCodeAction(ISD::SETUGE, MVT::f32, Legal);
    setCondCodeAction(ISD::SETUGE, MVT::f64, Legal);

    setCondCodeAction(ISD::SETOGT, MVT::f32, Legal);
    setCondCodeAction(ISD::SETOGT, MVT::f64, Legal);
    setCondCodeAction(ISD::SETUGT, MVT::f32, Legal);
    setCondCodeAction(ISD::SETUGT, MVT::f64, Legal);

    setCondCodeAction(ISD::SETOLE, MVT::f32, Legal);
    setCondCodeAction(ISD::SETOLE, MVT::f64, Legal);
    setCondCodeAction(ISD::SETOLT, MVT::f32, Legal);
    setCondCodeAction(ISD::SETOLT, MVT::f64, Legal);

    setOperationAction(ISD::ConstantFP, MVT::f32, Legal);
    setOperationAction(ISD::ConstantFP, MVT::f64, Legal);

    setOperationAction(ISD::FP_TO_UINT, MVT::i1, Promote);
    setOperationAction(ISD::FP_TO_SINT, MVT::i1, Promote);
    setOperationAction(ISD::UINT_TO_FP, MVT::i1, Promote);
    setOperationAction(ISD::SINT_TO_FP, MVT::i1, Promote);

    setOperationAction(ISD::FP_TO_UINT, MVT::i8, Promote);
    setOperationAction(ISD::FP_TO_SINT, MVT::i8, Promote);
    setOperationAction(ISD::UINT_TO_FP, MVT::i8, Promote);
    setOperationAction(ISD::SINT_TO_FP, MVT::i8, Promote);

    setOperationAction(ISD::FP_TO_UINT, MVT::i16, Promote);
    setOperationAction(ISD::FP_TO_SINT, MVT::i16, Promote);
    setOperationAction(ISD::UINT_TO_FP, MVT::i16, Promote);
    setOperationAction(ISD::SINT_TO_FP, MVT::i16, Promote);

    setOperationAction(ISD::FP_TO_UINT, MVT::i32, Legal);
    setOperationAction(ISD::FP_TO_SINT, MVT::i32, Legal);
    setOperationAction(ISD::UINT_TO_FP, MVT::i32, Legal);
    setOperationAction(ISD::SINT_TO_FP, MVT::i32, Legal);

    setOperationAction(ISD::FP_TO_UINT, MVT::i64, Legal);
    setOperationAction(ISD::FP_TO_SINT, MVT::i64, Legal);
    setOperationAction(ISD::UINT_TO_FP, MVT::i64, Legal);
    setOperationAction(ISD::SINT_TO_FP, MVT::i64, Legal);

    setOperationAction(ISD::FABS, MVT::f32, Legal);
    setOperationAction(ISD::FABS, MVT::f64, Expand);

    setOperationAction(ISD::FNEG, MVT::f32, Legal);
    setOperationAction(ISD::FNEG, MVT::f64, Expand);
  } else {

    // Expand fp<->uint.
    setOperationAction(ISD::FP_TO_SINT, MVT::i32, Expand);
    setOperationAction(ISD::FP_TO_UINT, MVT::i32, Expand);

    setOperationAction(ISD::SINT_TO_FP, MVT::i32, Expand);
    setOperationAction(ISD::UINT_TO_FP, MVT::i32, Expand);

    setLibcallName(RTLIB::SINTTOFP_I64_F32, "__hexagon_floatdisf");
    setLibcallName(RTLIB::UINTTOFP_I64_F32, "__hexagon_floatundisf");

    setLibcallName(RTLIB::UINTTOFP_I32_F32, "__hexagon_floatunsisf");
    setLibcallName(RTLIB::SINTTOFP_I32_F32, "__hexagon_floatsisf");

    setLibcallName(RTLIB::SINTTOFP_I64_F64, "__hexagon_floatdidf");
    setLibcallName(RTLIB::UINTTOFP_I64_F64, "__hexagon_floatundidf");

    setLibcallName(RTLIB::UINTTOFP_I32_F64, "__hexagon_floatunsidf");
    setLibcallName(RTLIB::SINTTOFP_I32_F64, "__hexagon_floatsidf");

    setLibcallName(RTLIB::FPTOUINT_F32_I32, "__hexagon_fixunssfsi");
    setLibcallName(RTLIB::FPTOUINT_F32_I64, "__hexagon_fixunssfdi");

    setLibcallName(RTLIB::FPTOSINT_F64_I64, "__hexagon_fixdfdi");
    setLibcallName(RTLIB::FPTOSINT_F32_I64, "__hexagon_fixsfdi");

    setLibcallName(RTLIB::FPTOUINT_F64_I32, "__hexagon_fixunsdfsi");
    setLibcallName(RTLIB::FPTOUINT_F64_I64, "__hexagon_fixunsdfdi");


    setLibcallName(RTLIB::ADD_F32, "__hexagon_addsf3");
    setOperationAction(ISD::FADD, MVT::f32, Expand);
    setOperationAction(ISD::FADD, MVT::f64, Expand);

    setLibcallName(RTLIB::SUB_F32, "__hexagon_subsf3");
    setOperationAction(ISD::FSUB, MVT::f32, Expand);
    setOperationAction(ISD::FSUB, MVT::f64, Expand);

    setLibcallName(RTLIB::FPEXT_F32_F64, "__hexagon_extendsfdf2");
    setOperationAction(ISD::FP_EXTEND, MVT::f32, Expand);

    setLibcallName(RTLIB::OEQ_F32, "__hexagon_eqsf2");
    setCondCodeAction(ISD::SETOEQ, MVT::f32, Expand);

    setLibcallName(RTLIB::OEQ_F64, "__hexagon_eqdf2");
    setCondCodeAction(ISD::SETOEQ, MVT::f64, Expand);

    setLibcallName(RTLIB::OGE_F32, "__hexagon_gesf2");
    setCondCodeAction(ISD::SETOGE, MVT::f32, Expand);

    setLibcallName(RTLIB::OGE_F64, "__hexagon_gedf2");
    setCondCodeAction(ISD::SETOGE, MVT::f64, Expand);

    setLibcallName(RTLIB::OGT_F32, "__hexagon_gtsf2");
    setCondCodeAction(ISD::SETOGT, MVT::f32, Expand);

    setLibcallName(RTLIB::OGT_F64, "__hexagon_gtdf2");
    setCondCodeAction(ISD::SETOGT, MVT::f64, Expand);

    setLibcallName(RTLIB::FPTOSINT_F64_I32, "__hexagon_fixdfsi");
    setOperationAction(ISD::FP_TO_SINT, MVT::f64, Expand);

    setLibcallName(RTLIB::FPTOSINT_F32_I32, "__hexagon_fixsfsi");
    setOperationAction(ISD::FP_TO_SINT, MVT::f32, Expand);

    setLibcallName(RTLIB::OLE_F64, "__hexagon_ledf2");
    setCondCodeAction(ISD::SETOLE, MVT::f64, Expand);

    setLibcallName(RTLIB::OLE_F32, "__hexagon_lesf2");
    setCondCodeAction(ISD::SETOLE, MVT::f32, Expand);

    setLibcallName(RTLIB::OLT_F64, "__hexagon_ltdf2");
    setCondCodeAction(ISD::SETOLT, MVT::f64, Expand);

    setLibcallName(RTLIB::OLT_F32, "__hexagon_ltsf2");
    setCondCodeAction(ISD::SETOLT, MVT::f32, Expand);

    setOperationAction(ISD::FMUL, MVT::f64, Expand);

    setLibcallName(RTLIB::MUL_F32, "__hexagon_mulsf3");
    setOperationAction(ISD::MUL, MVT::f32, Expand);

    setLibcallName(RTLIB::UNE_F64, "__hexagon_nedf2");
    setCondCodeAction(ISD::SETUNE, MVT::f64, Expand);

    setLibcallName(RTLIB::UNE_F32, "__hexagon_nesf2");

    setLibcallName(RTLIB::SUB_F64, "__hexagon_subdf3");
    setOperationAction(ISD::SUB, MVT::f64, Expand);

    setLibcallName(RTLIB::SUB_F32, "__hexagon_subsf3");
    setOperationAction(ISD::SUB, MVT::f32, Expand);

    setLibcallName(RTLIB::FPROUND_F64_F32, "__hexagon_truncdfsf2");
    setOperationAction(ISD::FP_ROUND, MVT::f64, Expand);

    setLibcallName(RTLIB::UO_F64, "__hexagon_unorddf2");
    setCondCodeAction(ISD::SETUO, MVT::f64, Expand);

    setLibcallName(RTLIB::O_F64, "__hexagon_unorddf2");
    setCondCodeAction(ISD::SETO, MVT::f64, Expand);

    setLibcallName(RTLIB::O_F32, "__hexagon_unordsf2");
    setCondCodeAction(ISD::SETO, MVT::f32, Expand);

    setLibcallName(RTLIB::UO_F32, "__hexagon_unordsf2");
    setCondCodeAction(ISD::SETUO, MVT::f32, Expand);

    setOperationAction(ISD::FABS, MVT::f32, Expand);
    setOperationAction(ISD::FABS, MVT::f64, Expand);
    setOperationAction(ISD::FNEG, MVT::f32, Expand);
    setOperationAction(ISD::FNEG, MVT::f64, Expand);
  }

  setLibcallName(RTLIB::SREM_I32, "__hexagon_modsi3");
  setOperationAction(ISD::SREM, MVT::i32, Expand);

  setIndexedLoadAction(ISD::POST_INC, MVT::i8, Legal);
  setIndexedLoadAction(ISD::POST_INC, MVT::i16, Legal);
  setIndexedLoadAction(ISD::POST_INC, MVT::i32, Legal);
  setIndexedLoadAction(ISD::POST_INC, MVT::i64, Legal);

  setIndexedStoreAction(ISD::POST_INC, MVT::i8, Legal);
  setIndexedStoreAction(ISD::POST_INC, MVT::i16, Legal);
  setIndexedStoreAction(ISD::POST_INC, MVT::i32, Legal);
  setIndexedStoreAction(ISD::POST_INC, MVT::i64, Legal);

  setOperationAction(ISD::BUILD_PAIR, MVT::i64, Expand);

  // Turn FP extload into load/fextend.
  for (MVT VT : MVT::fp_valuetypes())
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::f32, Expand);

  // No extending loads from i32.
  for (MVT VT : MVT::integer_valuetypes()) {
    setLoadExtAction(ISD::ZEXTLOAD, VT, MVT::i32, Expand);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i32, Expand);
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::i32, Expand);
  }

  // Turn FP truncstore into trunc + store.
  setTruncStoreAction(MVT::f64, MVT::f32, Expand);

  // Custom legalize GlobalAddress nodes into CONST32.
  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::GlobalAddress, MVT::i8, Custom);
  setOperationAction(ISD::BlockAddress, MVT::i32, Custom);
  // Truncate action?
  setOperationAction(ISD::TRUNCATE, MVT::i64, Expand);

  // Hexagon doesn't have sext_inreg, replace them with shl/sra.
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);

  // Hexagon has no REM or DIVREM operations.
  setOperationAction(ISD::UREM, MVT::i32, Expand);
  setOperationAction(ISD::SREM, MVT::i32, Expand);
  setOperationAction(ISD::SDIVREM, MVT::i32, Expand);
  setOperationAction(ISD::UDIVREM, MVT::i32, Expand);
  setOperationAction(ISD::SREM, MVT::i64, Expand);
  setOperationAction(ISD::SDIVREM, MVT::i64, Expand);
  setOperationAction(ISD::UDIVREM, MVT::i64, Expand);

  setOperationAction(ISD::BSWAP, MVT::i64, Expand);

  // Lower SELECT_CC to SETCC and SELECT.
  setOperationAction(ISD::SELECT_CC, MVT::i1, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::i32, Expand);
  setOperationAction(ISD::SELECT_CC, MVT::i64, Expand);

  if (Subtarget->hasV5TOps()) {

    // We need to make the operation type of SELECT node to be Custom,
    // such that we don't go into the infinite loop of
    // select ->  setcc -> select_cc -> select loop.
    setOperationAction(ISD::SELECT, MVT::f32, Custom);
    setOperationAction(ISD::SELECT, MVT::f64, Custom);

    setOperationAction(ISD::SELECT_CC, MVT::f32, Expand);
    setOperationAction(ISD::SELECT_CC, MVT::f64, Expand);

  } else {

    // Hexagon has no select or setcc: expand to SELECT_CC.
    setOperationAction(ISD::SELECT, MVT::f32, Expand);
    setOperationAction(ISD::SELECT, MVT::f64, Expand);
  }

  // Hexagon needs to optimize cases with negative constants.
  setOperationAction(ISD::SETCC, MVT::i16, Custom);
  setOperationAction(ISD::SETCC, MVT::i8, Custom);

  if (EmitJumpTables) {
    setOperationAction(ISD::BR_JT, MVT::Other, Custom);
  } else {
    setOperationAction(ISD::BR_JT, MVT::Other, Expand);
  }
  // Increase jump tables cutover to 5, was 4.
  setMinimumJumpTableEntries(5);

  setOperationAction(ISD::BR_CC, MVT::f32, Expand);
  setOperationAction(ISD::BR_CC, MVT::f64, Expand);
  setOperationAction(ISD::BR_CC, MVT::i1, Expand);
  setOperationAction(ISD::BR_CC, MVT::i32, Expand);
  setOperationAction(ISD::BR_CC, MVT::i64, Expand);

  setOperationAction(ISD::ATOMIC_FENCE, MVT::Other, Custom);

  setOperationAction(ISD::FSIN, MVT::f64, Expand);
  setOperationAction(ISD::FCOS, MVT::f64, Expand);
  setOperationAction(ISD::FREM, MVT::f64, Expand);
  setOperationAction(ISD::FSIN, MVT::f32, Expand);
  setOperationAction(ISD::FCOS, MVT::f32, Expand);
  setOperationAction(ISD::FREM, MVT::f32, Expand);
  setOperationAction(ISD::FSINCOS, MVT::f64, Expand);
  setOperationAction(ISD::FSINCOS, MVT::f32, Expand);

  // In V4, we have double word add/sub with carry. The problem with
  // modelling this instruction is that it produces 2 results - Rdd and Px.
  // To model update of Px, we will have to use Defs[p0..p3] which will
  // cause any predicate live range to spill. So, we pretend we dont't
  // have these instructions.
  setOperationAction(ISD::ADDE, MVT::i8, Expand);
  setOperationAction(ISD::ADDE, MVT::i16, Expand);
  setOperationAction(ISD::ADDE, MVT::i32, Expand);
  setOperationAction(ISD::ADDE, MVT::i64, Expand);
  setOperationAction(ISD::SUBE, MVT::i8, Expand);
  setOperationAction(ISD::SUBE, MVT::i16, Expand);
  setOperationAction(ISD::SUBE, MVT::i32, Expand);
  setOperationAction(ISD::SUBE, MVT::i64, Expand);
  setOperationAction(ISD::ADDC, MVT::i8, Expand);
  setOperationAction(ISD::ADDC, MVT::i16, Expand);
  setOperationAction(ISD::ADDC, MVT::i32, Expand);
  setOperationAction(ISD::ADDC, MVT::i64, Expand);
  setOperationAction(ISD::SUBC, MVT::i8, Expand);
  setOperationAction(ISD::SUBC, MVT::i16, Expand);
  setOperationAction(ISD::SUBC, MVT::i32, Expand);
  setOperationAction(ISD::SUBC, MVT::i64, Expand);

  // Only add and sub that detect overflow are the saturating ones.
  for (MVT VT : MVT::integer_valuetypes()) {
    setOperationAction(ISD::UADDO, VT, Expand);
    setOperationAction(ISD::SADDO, VT, Expand);
    setOperationAction(ISD::USUBO, VT, Expand);
    setOperationAction(ISD::SSUBO, VT, Expand);
  }

  setOperationAction(ISD::CTPOP, MVT::i32, Expand);
  setOperationAction(ISD::CTPOP, MVT::i64, Expand);
  setOperationAction(ISD::CTTZ, MVT::i32, Expand);
  setOperationAction(ISD::CTTZ, MVT::i64, Expand);
  setOperationAction(ISD::CTTZ_ZERO_UNDEF, MVT::i32, Expand);
  setOperationAction(ISD::CTTZ_ZERO_UNDEF, MVT::i64, Expand);
  setOperationAction(ISD::CTLZ, MVT::i32, Expand);
  setOperationAction(ISD::CTLZ, MVT::i64, Expand);
  setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i32, Expand);
  setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i64, Expand);

  setOperationAction(ISD::ROTL, MVT::i32, Expand);
  setOperationAction(ISD::ROTR, MVT::i32, Expand);
  setOperationAction(ISD::BSWAP, MVT::i32, Expand);
  setOperationAction(ISD::ROTL, MVT::i64, Expand);
  setOperationAction(ISD::ROTR, MVT::i64, Expand);
  setOperationAction(ISD::SHL_PARTS, MVT::i64, Expand);
  setOperationAction(ISD::SRA_PARTS, MVT::i64, Expand);
  setOperationAction(ISD::SRL_PARTS, MVT::i64, Expand);
  setOperationAction(ISD::BR_CC, MVT::i64, Expand);

  setOperationAction(ISD::FCOPYSIGN, MVT::f64, Expand);
  setOperationAction(ISD::FCOPYSIGN, MVT::f32, Expand);
  setOperationAction(ISD::FPOW, MVT::f64, Expand);
  setOperationAction(ISD::FPOW, MVT::f32, Expand);

  setOperationAction(ISD::SHL_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRA_PARTS, MVT::i32, Expand);
  setOperationAction(ISD::SRL_PARTS, MVT::i32, Expand);

  setOperationAction(ISD::UMUL_LOHI, MVT::i32, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i32, Expand);

  setOperationAction(ISD::MULHS, MVT::i64, Expand);
  setOperationAction(ISD::SMUL_LOHI, MVT::i64, Expand);
  setOperationAction(ISD::UMUL_LOHI, MVT::i64, Expand);

  setOperationAction(ISD::EH_RETURN, MVT::Other, Custom);

  setExceptionPointerRegister(Hexagon::R0);
  setExceptionSelectorRegister(Hexagon::R1);

  // VASTART needs to be custom lowered to use the VarArgsFrameIndex.
  setOperationAction(ISD::VASTART, MVT::Other, Custom);

  // Use the default implementation.
  setOperationAction(ISD::VAARG, MVT::Other, Expand);
  setOperationAction(ISD::VACOPY, MVT::Other, Expand);
  setOperationAction(ISD::VAEND, MVT::Other, Expand);
  setOperationAction(ISD::STACKSAVE, MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE, MVT::Other, Expand);

  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Custom);
  setOperationAction(ISD::INLINEASM, MVT::Other, Custom);

  setMinFunctionAlignment(2);

  // Needed for DYNAMIC_STACKALLOC expansion.
  const HexagonRegisterInfo *QRI = Subtarget->getRegisterInfo();
  setStackPointerRegisterToSaveRestore(QRI->getStackRegister());
  setSchedulingPreference(Sched::VLIW);
}

const char*
HexagonTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  default: return nullptr;
  case HexagonISD::CONST32:     return "HexagonISD::CONST32";
  case HexagonISD::CONST32_GP: return "HexagonISD::CONST32_GP";
  case HexagonISD::CONST32_Int_Real: return "HexagonISD::CONST32_Int_Real";
  case HexagonISD::ADJDYNALLOC: return "HexagonISD::ADJDYNALLOC";
  case HexagonISD::CMPICC:      return "HexagonISD::CMPICC";
  case HexagonISD::CMPFCC:      return "HexagonISD::CMPFCC";
  case HexagonISD::BRICC:       return "HexagonISD::BRICC";
  case HexagonISD::BRFCC:       return "HexagonISD::BRFCC";
  case HexagonISD::SELECT_ICC:  return "HexagonISD::SELECT_ICC";
  case HexagonISD::SELECT_FCC:  return "HexagonISD::SELECT_FCC";
  case HexagonISD::Hi:          return "HexagonISD::Hi";
  case HexagonISD::Lo:          return "HexagonISD::Lo";
  case HexagonISD::JT: return "HexagonISD::JT";
  case HexagonISD::CP: return "HexagonISD::CP";
  case HexagonISD::POPCOUNT: return "HexagonISD::POPCOUNT";
  case HexagonISD::COMBINE: return "HexagonISD::COMBINE";
  case HexagonISD::PACKHL: return "HexagonISD::PACKHL";
  case HexagonISD::VSPLATB: return "HexagonISD::VSPLTB";
  case HexagonISD::VSPLATH: return "HexagonISD::VSPLATH";
  case HexagonISD::SHUFFEB: return "HexagonISD::SHUFFEB";
  case HexagonISD::SHUFFEH: return "HexagonISD::SHUFFEH";
  case HexagonISD::SHUFFOB: return "HexagonISD::SHUFFOB";
  case HexagonISD::SHUFFOH: return "HexagonISD::SHUFFOH";
  case HexagonISD::VSXTBH: return "HexagonISD::VSXTBH";
  case HexagonISD::VSXTBW: return "HexagonISD::VSXTBW";
  case HexagonISD::VSRAW: return "HexagonISD::VSRAW";
  case HexagonISD::VSRAH: return "HexagonISD::VSRAH";
  case HexagonISD::VSRLW: return "HexagonISD::VSRLW";
  case HexagonISD::VSRLH: return "HexagonISD::VSRLH";
  case HexagonISD::VSHLW: return "HexagonISD::VSHLW";
  case HexagonISD::VSHLH: return "HexagonISD::VSHLH";
  case HexagonISD::VCMPBEQ: return "HexagonISD::VCMPBEQ";
  case HexagonISD::VCMPBGT: return "HexagonISD::VCMPBGT";
  case HexagonISD::VCMPBGTU: return "HexagonISD::VCMPBGTU";
  case HexagonISD::VCMPHEQ: return "HexagonISD::VCMPHEQ";
  case HexagonISD::VCMPHGT: return "HexagonISD::VCMPHGT";
  case HexagonISD::VCMPHGTU: return "HexagonISD::VCMPHGTU";
  case HexagonISD::VCMPWEQ: return "HexagonISD::VCMPWEQ";
  case HexagonISD::VCMPWGT: return "HexagonISD::VCMPWGT";
  case HexagonISD::VCMPWGTU: return "HexagonISD::VCMPWGTU";
  case HexagonISD::INSERT_ri: return "HexagonISD::INSERT_ri";
  case HexagonISD::INSERT_rd: return "HexagonISD::INSERT_rd";
  case HexagonISD::INSERT_riv: return "HexagonISD::INSERT_riv";
  case HexagonISD::INSERT_rdv: return "HexagonISD::INSERT_rdv";
  case HexagonISD::EXTRACTU_ri: return "HexagonISD::EXTRACTU_ri";
  case HexagonISD::EXTRACTU_rd: return "HexagonISD::EXTRACTU_rd";
  case HexagonISD::EXTRACTU_riv: return "HexagonISD::EXTRACTU_riv";
  case HexagonISD::EXTRACTU_rdv: return "HexagonISD::EXTRACTU_rdv";
  case HexagonISD::FTOI:        return "HexagonISD::FTOI";
  case HexagonISD::ITOF:        return "HexagonISD::ITOF";
  case HexagonISD::CALLv3:      return "HexagonISD::CALLv3";
  case HexagonISD::CALLv3nr:    return "HexagonISD::CALLv3nr";
  case HexagonISD::CALLR:       return "HexagonISD::CALLR";
  case HexagonISD::RET_FLAG:    return "HexagonISD::RET_FLAG";
  case HexagonISD::BR_JT:       return "HexagonISD::BR_JT";
  case HexagonISD::TC_RETURN:   return "HexagonISD::TC_RETURN";
  case HexagonISD::EH_RETURN: return "HexagonISD::EH_RETURN";
  }
}

bool
HexagonTargetLowering::isTruncateFree(Type *Ty1, Type *Ty2) const {
  EVT MTy1 = EVT::getEVT(Ty1);
  EVT MTy2 = EVT::getEVT(Ty2);
  if (!MTy1.isSimple() || !MTy2.isSimple()) {
    return false;
  }
  return ((MTy1.getSimpleVT() == MVT::i64) && (MTy2.getSimpleVT() == MVT::i32));
}

bool HexagonTargetLowering::isTruncateFree(EVT VT1, EVT VT2) const {
  if (!VT1.isSimple() || !VT2.isSimple()) {
    return false;
  }
  return ((VT1.getSimpleVT() == MVT::i64) && (VT2.getSimpleVT() == MVT::i32));
}

// shouldExpandBuildVectorWithShuffles
// Should we expand the build vector with shuffles?
bool
HexagonTargetLowering::shouldExpandBuildVectorWithShuffles(EVT VT,
                                  unsigned DefinedValues) const {

  // Hexagon vector shuffle operates on element sizes of bytes or halfwords
  EVT EltVT = VT.getVectorElementType();
  int EltBits = EltVT.getSizeInBits();
  if ((EltBits != 8) && (EltBits != 16))
    return false;

  return TargetLowering::shouldExpandBuildVectorWithShuffles(VT, DefinedValues);
}

// LowerVECTOR_SHUFFLE - Lower a vector shuffle (V1, V2, V3).  V1 and
// V2 are the two vectors to select data from, V3 is the permutation.
static SDValue LowerVECTOR_SHUFFLE(SDValue Op, SelectionDAG &DAG) {
  const ShuffleVectorSDNode *SVN = cast<ShuffleVectorSDNode>(Op);
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  SDLoc dl(Op);
  EVT VT = Op.getValueType();

  if (V2.getOpcode() == ISD::UNDEF)
    V2 = V1;

  if (SVN->isSplat()) {
    int Lane = SVN->getSplatIndex();
    if (Lane == -1) Lane = 0;

    // Test if V1 is a SCALAR_TO_VECTOR.
    if (Lane == 0 && V1.getOpcode() == ISD::SCALAR_TO_VECTOR)
      return createSplat(DAG, dl, VT, V1.getOperand(0));

    // Test if V1 is a BUILD_VECTOR which is equivalent to a SCALAR_TO_VECTOR
    // (and probably will turn into a SCALAR_TO_VECTOR once legalization
    // reaches it).
    if (Lane == 0 && V1.getOpcode() == ISD::BUILD_VECTOR &&
        !isa<ConstantSDNode>(V1.getOperand(0))) {
      bool IsScalarToVector = true;
      for (unsigned i = 1, e = V1.getNumOperands(); i != e; ++i)
        if (V1.getOperand(i).getOpcode() != ISD::UNDEF) {
          IsScalarToVector = false;
          break;
        }
      if (IsScalarToVector)
        return createSplat(DAG, dl, VT, V1.getOperand(0));
    }
    return createSplat(DAG, dl, VT, DAG.getConstant(Lane, MVT::i32));
  }

  // FIXME: We need to support more general vector shuffles.  See
  // below the comment from the ARM backend that deals in the general
  // case with the vector shuffles.  For now, let expand handle these.
  return SDValue();

  // If the shuffle is not directly supported and it has 4 elements, use
  // the PerfectShuffle-generated table to synthesize it from other shuffles.
}

// If BUILD_VECTOR has same base element repeated several times,
// report true.
static bool isCommonSplatElement(BuildVectorSDNode *BVN) {
  unsigned NElts = BVN->getNumOperands();
  SDValue V0 = BVN->getOperand(0);

  for (unsigned i = 1, e = NElts; i != e; ++i) {
    if (BVN->getOperand(i) != V0)
      return false;
  }
  return true;
}

// LowerVECTOR_SHIFT - Lower a vector shift. Try to convert
// <VT> = SHL/SRA/SRL <VT> by <VT> to Hexagon specific
// <VT> = SHL/SRA/SRL <VT> by <IT/i32>.
static SDValue LowerVECTOR_SHIFT(SDValue Op, SelectionDAG &DAG) {
  BuildVectorSDNode *BVN = 0;
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  SDValue V3;
  SDLoc dl(Op);
  EVT VT = Op.getValueType();

  if ((BVN = dyn_cast<BuildVectorSDNode>(V1.getNode())) &&
      isCommonSplatElement(BVN))
    V3 = V2;
  else if ((BVN = dyn_cast<BuildVectorSDNode>(V2.getNode())) &&
           isCommonSplatElement(BVN))
    V3 = V1;
  else
    return SDValue();

  SDValue CommonSplat = BVN->getOperand(0);
  SDValue Result;

  if (VT.getSimpleVT() == MVT::v4i16) {
    switch (Op.getOpcode()) {
    case ISD::SRA:
      Result = DAG.getNode(HexagonISD::VSRAH, dl, VT, V3, CommonSplat);
      break;
    case ISD::SHL:
      Result = DAG.getNode(HexagonISD::VSHLH, dl, VT, V3, CommonSplat);
      break;
    case ISD::SRL:
      Result = DAG.getNode(HexagonISD::VSRLH, dl, VT, V3, CommonSplat);
      break;
    default:
      return SDValue();
    }
  } else if (VT.getSimpleVT() == MVT::v2i32) {
    switch (Op.getOpcode()) {
    case ISD::SRA:
      Result = DAG.getNode(HexagonISD::VSRAW, dl, VT, V3, CommonSplat);
      break;
    case ISD::SHL:
      Result = DAG.getNode(HexagonISD::VSHLW, dl, VT, V3, CommonSplat);
      break;
    case ISD::SRL:
      Result = DAG.getNode(HexagonISD::VSRLW, dl, VT, V3, CommonSplat);
      break;
    default:
      return SDValue();
    }
  } else {
    return SDValue();
  }

  return DAG.getNode(ISD::BITCAST, dl, VT, Result);
}

SDValue
HexagonTargetLowering::LowerBUILD_VECTOR(SDValue Op, SelectionDAG &DAG) const {
  BuildVectorSDNode *BVN = cast<BuildVectorSDNode>(Op.getNode());
  SDLoc dl(Op);
  EVT VT = Op.getValueType();

  unsigned Size = VT.getSizeInBits();

  // A vector larger than 64 bits cannot be represented in Hexagon.
  // Expand will split the vector.
  if (Size > 64)
    return SDValue();

  APInt APSplatBits, APSplatUndef;
  unsigned SplatBitSize;
  bool HasAnyUndefs;
  unsigned NElts = BVN->getNumOperands();

  // Try to generate a SPLAT instruction.
  if ((VT.getSimpleVT() == MVT::v4i8 || VT.getSimpleVT() == MVT::v4i16) &&
      (BVN->isConstantSplat(APSplatBits, APSplatUndef, SplatBitSize,
                            HasAnyUndefs, 0, true) && SplatBitSize <= 16)) {
    unsigned SplatBits = APSplatBits.getZExtValue();
    int32_t SextVal = ((int32_t) (SplatBits << (32 - SplatBitSize)) >>
                       (32 - SplatBitSize));
    return createSplat(DAG, dl, VT, DAG.getConstant(SextVal, MVT::i32));
  }

  // Try to generate COMBINE to build v2i32 vectors.
  if (VT.getSimpleVT() == MVT::v2i32) {
    SDValue V0 = BVN->getOperand(0);
    SDValue V1 = BVN->getOperand(1);

    if (V0.getOpcode() == ISD::UNDEF)
      V0 = DAG.getConstant(0, MVT::i32);
    if (V1.getOpcode() == ISD::UNDEF)
      V1 = DAG.getConstant(0, MVT::i32);

    ConstantSDNode *C0 = dyn_cast<ConstantSDNode>(V0);
    ConstantSDNode *C1 = dyn_cast<ConstantSDNode>(V1);
    // If the element isn't a constant, it is in a register:
    // generate a COMBINE Register Register instruction.
    if (!C0 || !C1)
      return DAG.getNode(HexagonISD::COMBINE, dl, VT, V1, V0);

    // If one of the operands is an 8 bit integer constant, generate
    // a COMBINE Immediate Immediate instruction.
    if (isInt<8>(C0->getSExtValue()) ||
        isInt<8>(C1->getSExtValue()))
      return DAG.getNode(HexagonISD::COMBINE, dl, VT, V1, V0);
  }

  // Try to generate a S2_packhl to build v2i16 vectors.
  if (VT.getSimpleVT() == MVT::v2i16) {
    for (unsigned i = 0, e = NElts; i != e; ++i) {
      if (BVN->getOperand(i).getOpcode() == ISD::UNDEF)
        continue;
      ConstantSDNode *Cst = dyn_cast<ConstantSDNode>(BVN->getOperand(i));
      // If the element isn't a constant, it is in a register:
      // generate a S2_packhl instruction.
      if (!Cst) {
        SDValue pack = DAG.getNode(HexagonISD::PACKHL, dl, MVT::v4i16,
                                   BVN->getOperand(1), BVN->getOperand(0));

        return DAG.getTargetExtractSubreg(Hexagon::subreg_loreg, dl, MVT::v2i16,
                                          pack);
      }
    }
  }

  // In the general case, generate a CONST32 or a CONST64 for constant vectors,
  // and insert_vector_elt for all the other cases.
  uint64_t Res = 0;
  unsigned EltSize = Size / NElts;
  SDValue ConstVal;
  uint64_t Mask = ~uint64_t(0ULL) >> (64 - EltSize);
  bool HasNonConstantElements = false;

  for (unsigned i = 0, e = NElts; i != e; ++i) {
    // LLVM's BUILD_VECTOR operands are in Little Endian mode, whereas Hexagon's
    // combine, const64, etc. are Big Endian.
    unsigned OpIdx = NElts - i - 1;
    SDValue Operand = BVN->getOperand(OpIdx);
    if (Operand.getOpcode() == ISD::UNDEF)
      continue;

    int64_t Val = 0;
    if (ConstantSDNode *Cst = dyn_cast<ConstantSDNode>(Operand))
      Val = Cst->getSExtValue();
    else
      HasNonConstantElements = true;

    Val &= Mask;
    Res = (Res << EltSize) | Val;
  }

  if (Size == 64)
    ConstVal = DAG.getConstant(Res, MVT::i64);
  else
    ConstVal = DAG.getConstant(Res, MVT::i32);

  // When there are non constant operands, add them with INSERT_VECTOR_ELT to
  // ConstVal, the constant part of the vector.
  if (HasNonConstantElements) {
    EVT EltVT = VT.getVectorElementType();
    SDValue Width = DAG.getConstant(EltVT.getSizeInBits(), MVT::i64);
    SDValue Shifted = DAG.getNode(ISD::SHL, dl, MVT::i64, Width,
                                  DAG.getConstant(32, MVT::i64));

    for (unsigned i = 0, e = NElts; i != e; ++i) {
      // LLVM's BUILD_VECTOR operands are in Little Endian mode, whereas Hexagon
      // is Big Endian.
      unsigned OpIdx = NElts - i - 1;
      SDValue Operand = BVN->getOperand(OpIdx);
      if (isa<ConstantSDNode>(Operand))
        // This operand is already in ConstVal.
        continue;

      if (VT.getSizeInBits() == 64 &&
          Operand.getValueType().getSizeInBits() == 32) {
        SDValue C = DAG.getConstant(0, MVT::i32);
        Operand = DAG.getNode(HexagonISD::COMBINE, dl, VT, C, Operand);
      }

      SDValue Idx = DAG.getConstant(OpIdx, MVT::i64);
      SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i64, Idx, Width);
      SDValue Combined = DAG.getNode(ISD::OR, dl, MVT::i64, Shifted, Offset);
      const SDValue Ops[] = {ConstVal, Operand, Combined};

      if (VT.getSizeInBits() == 32)
        ConstVal = DAG.getNode(HexagonISD::INSERT_riv, dl, MVT::i32, Ops);
      else
        ConstVal = DAG.getNode(HexagonISD::INSERT_rdv, dl, MVT::i64, Ops);
    }
  }

  return DAG.getNode(ISD::BITCAST, dl, VT, ConstVal);
}

SDValue
HexagonTargetLowering::LowerCONCAT_VECTORS(SDValue Op,
                                           SelectionDAG &DAG) const {
  SDLoc dl(Op);
  EVT VT = Op.getValueType();
  unsigned NElts = Op.getNumOperands();
  SDValue Vec = Op.getOperand(0);
  EVT VecVT = Vec.getValueType();
  SDValue Width = DAG.getConstant(VecVT.getSizeInBits(), MVT::i64);
  SDValue Shifted = DAG.getNode(ISD::SHL, dl, MVT::i64, Width,
                                DAG.getConstant(32, MVT::i64));
  SDValue ConstVal = DAG.getConstant(0, MVT::i64);

  ConstantSDNode *W = dyn_cast<ConstantSDNode>(Width);
  ConstantSDNode *S = dyn_cast<ConstantSDNode>(Shifted);

  if ((VecVT.getSimpleVT() == MVT::v2i16) && (NElts == 2) && W && S) {
    if ((W->getZExtValue() == 32) && ((S->getZExtValue() >> 32) == 32)) {
      // We are trying to concat two v2i16 to a single v4i16.
      SDValue Vec0 = Op.getOperand(1);
      SDValue Combined  = DAG.getNode(HexagonISD::COMBINE, dl, VT, Vec0, Vec);
      return DAG.getNode(ISD::BITCAST, dl, VT, Combined);
    }
  }

  if ((VecVT.getSimpleVT() == MVT::v4i8) && (NElts == 2) && W && S) {
    if ((W->getZExtValue() == 32) && ((S->getZExtValue() >> 32) == 32)) {
      // We are trying to concat two v4i8 to a single v8i8.
      SDValue Vec0 = Op.getOperand(1);
      SDValue Combined  = DAG.getNode(HexagonISD::COMBINE, dl, VT, Vec0, Vec);
      return DAG.getNode(ISD::BITCAST, dl, VT, Combined);
    }
  }

  for (unsigned i = 0, e = NElts; i != e; ++i) {
    unsigned OpIdx = NElts - i - 1;
    SDValue Operand = Op.getOperand(OpIdx);

    if (VT.getSizeInBits() == 64 &&
        Operand.getValueType().getSizeInBits() == 32) {
      SDValue C = DAG.getConstant(0, MVT::i32);
      Operand = DAG.getNode(HexagonISD::COMBINE, dl, VT, C, Operand);
    }

    SDValue Idx = DAG.getConstant(OpIdx, MVT::i64);
    SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i64, Idx, Width);
    SDValue Combined = DAG.getNode(ISD::OR, dl, MVT::i64, Shifted, Offset);
    const SDValue Ops[] = {ConstVal, Operand, Combined};

    if (VT.getSizeInBits() == 32)
      ConstVal = DAG.getNode(HexagonISD::INSERT_riv, dl, MVT::i32, Ops);
    else
      ConstVal = DAG.getNode(HexagonISD::INSERT_rdv, dl, MVT::i64, Ops);
  }

  return DAG.getNode(ISD::BITCAST, dl, VT, ConstVal);
}

SDValue
HexagonTargetLowering::LowerEXTRACT_VECTOR(SDValue Op,
                                           SelectionDAG &DAG) const {
  EVT VT = Op.getValueType();
  int VTN = VT.isVector() ? VT.getVectorNumElements() : 1;
  SDLoc dl(Op);
  SDValue Idx = Op.getOperand(1);
  SDValue Vec = Op.getOperand(0);
  EVT VecVT = Vec.getValueType();
  EVT EltVT = VecVT.getVectorElementType();
  int EltSize = EltVT.getSizeInBits();
  SDValue Width = DAG.getConstant(Op.getOpcode() == ISD::EXTRACT_VECTOR_ELT ?
                                  EltSize : VTN * EltSize, MVT::i64);

  // Constant element number.
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Idx)) {
    SDValue Offset = DAG.getConstant(C->getZExtValue() * EltSize, MVT::i32);
    const SDValue Ops[] = {Vec, Width, Offset};

    ConstantSDNode *W = dyn_cast<ConstantSDNode>(Width);
    assert(W && "Non constant width in LowerEXTRACT_VECTOR");

    SDValue N;
    // For certain extracts, it is a simple _hi/_lo subreg.
    if (VecVT.getSimpleVT() == MVT::v2i32) {
      // v2i32 -> i32 vselect.
      if (C->getZExtValue() == 0)
        N = DAG.getTargetExtractSubreg(Hexagon::subreg_loreg, dl,
                                       MVT::i32, Vec);
      else if (C->getZExtValue() == 1)
        N = DAG.getTargetExtractSubreg(Hexagon::subreg_hireg, dl,
                                       MVT::i32, Vec);
      else
        llvm_unreachable("Bad offset");
    } else if ((VecVT.getSimpleVT() == MVT::v4i16) &&
               (W->getZExtValue() == 32)) {
      // v4i16 -> v2i16/i32 vselect.
      if (C->getZExtValue() == 0)
        N = DAG.getTargetExtractSubreg(Hexagon::subreg_loreg, dl,
                                       MVT::i32, Vec);
      else if (C->getZExtValue() == 2)
        N = DAG.getTargetExtractSubreg(Hexagon::subreg_hireg, dl,
                                       MVT::i32, Vec);
      else
        llvm_unreachable("Bad offset");
    }  else if ((VecVT.getSimpleVT() == MVT::v8i8) &&
               (W->getZExtValue() == 32)) {
      // v8i8 -> v4i8/i32 vselect.
      if (C->getZExtValue() == 0)
        N = DAG.getTargetExtractSubreg(Hexagon::subreg_loreg, dl,
                                       MVT::i32, Vec);
      else if (C->getZExtValue() == 4)
        N = DAG.getTargetExtractSubreg(Hexagon::subreg_hireg, dl,
                                       MVT::i32, Vec);
      else
        llvm_unreachable("Bad offset");
    } else if (VecVT.getSizeInBits() == 32) {
        N = DAG.getNode(HexagonISD::EXTRACTU_ri, dl, MVT::i32, Ops);
    } else {
      N = DAG.getNode(HexagonISD::EXTRACTU_rd, dl, MVT::i64, Ops);
      if (VT.getSizeInBits() == 32)
        N = DAG.getTargetExtractSubreg(Hexagon::subreg_loreg, dl, MVT::i32, N);
    }

    return DAG.getNode(ISD::BITCAST, dl, VT, N);
  }

  // Variable element number.
  SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i32, Idx,
                               DAG.getConstant(EltSize, MVT::i32));
  SDValue Shifted = DAG.getNode(ISD::SHL, dl, MVT::i64, Width,
                                DAG.getConstant(32, MVT::i64));
  SDValue Combined = DAG.getNode(ISD::OR, dl, MVT::i64, Shifted, Offset);

  const SDValue Ops[] = {Vec, Combined};

  SDValue N;
  if (VecVT.getSizeInBits() == 32) {
    N = DAG.getNode(HexagonISD::EXTRACTU_riv, dl, MVT::i32, Ops);
  } else {
    N = DAG.getNode(HexagonISD::EXTRACTU_rdv, dl, MVT::i64, Ops);
    if (VT.getSizeInBits() == 32)
      N = DAG.getTargetExtractSubreg(Hexagon::subreg_loreg, dl, MVT::i32, N);
  }
  return DAG.getNode(ISD::BITCAST, dl, VT, N);
}

SDValue
HexagonTargetLowering::LowerINSERT_VECTOR(SDValue Op,
                                          SelectionDAG &DAG) const {
  EVT VT = Op.getValueType();
  int VTN = VT.isVector() ? VT.getVectorNumElements() : 1;
  SDLoc dl(Op);
  SDValue Vec = Op.getOperand(0);
  SDValue Val = Op.getOperand(1);
  SDValue Idx = Op.getOperand(2);
  EVT VecVT = Vec.getValueType();
  EVT EltVT = VecVT.getVectorElementType();
  int EltSize = EltVT.getSizeInBits();
  SDValue Width = DAG.getConstant(Op.getOpcode() == ISD::INSERT_VECTOR_ELT ?
                                  EltSize : VTN * EltSize, MVT::i64);

  if (ConstantSDNode *C = cast<ConstantSDNode>(Idx)) {
    SDValue Offset = DAG.getConstant(C->getSExtValue() * EltSize, MVT::i32);
    const SDValue Ops[] = {Vec, Val, Width, Offset};

    SDValue N;
    if (VT.getSizeInBits() == 32)
      N = DAG.getNode(HexagonISD::INSERT_ri, dl, MVT::i32, Ops);
    else
      N = DAG.getNode(HexagonISD::INSERT_rd, dl, MVT::i64, Ops);

    return DAG.getNode(ISD::BITCAST, dl, VT, N);
  }

  // Variable element number.
  SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i32, Idx,
                               DAG.getConstant(EltSize, MVT::i32));
  SDValue Shifted = DAG.getNode(ISD::SHL, dl, MVT::i64, Width,
                                DAG.getConstant(32, MVT::i64));
  SDValue Combined = DAG.getNode(ISD::OR, dl, MVT::i64, Shifted, Offset);

  if (VT.getSizeInBits() == 64 &&
      Val.getValueType().getSizeInBits() == 32) {
    SDValue C = DAG.getConstant(0, MVT::i32);
    Val = DAG.getNode(HexagonISD::COMBINE, dl, VT, C, Val);
  }

  const SDValue Ops[] = {Vec, Val, Combined};

  SDValue N;
  if (VT.getSizeInBits() == 32)
    N = DAG.getNode(HexagonISD::INSERT_riv, dl, MVT::i32, Ops);
  else
    N = DAG.getNode(HexagonISD::INSERT_rdv, dl, MVT::i64, Ops);

  return DAG.getNode(ISD::BITCAST, dl, VT, N);
}

bool
HexagonTargetLowering::allowTruncateForTailCall(Type *Ty1, Type *Ty2) const {
  // Assuming the caller does not have either a signext or zeroext modifier, and
  // only one value is accepted, any reasonable truncation is allowed.
  if (!Ty1->isIntegerTy() || !Ty2->isIntegerTy())
    return false;

  // FIXME: in principle up to 64-bit could be made safe, but it would be very
  // fragile at the moment: any support for multiple value returns would be
  // liable to disallow tail calls involving i64 -> iN truncation in many cases.
  return Ty1->getPrimitiveSizeInBits() <= 32;
}

SDValue
HexagonTargetLowering::LowerEH_RETURN(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain     = Op.getOperand(0);
  SDValue Offset    = Op.getOperand(1);
  SDValue Handler   = Op.getOperand(2);
  SDLoc dl(Op);

  // Mark function as containing a call to EH_RETURN.
  HexagonMachineFunctionInfo *FuncInfo =
    DAG.getMachineFunction().getInfo<HexagonMachineFunctionInfo>();
  FuncInfo->setHasEHReturn();

  unsigned OffsetReg = Hexagon::R28;

  SDValue StoreAddr = DAG.getNode(ISD::ADD, dl, getPointerTy(),
                                  DAG.getRegister(Hexagon::R30, getPointerTy()),
                                  DAG.getIntPtrConstant(4));
  Chain = DAG.getStore(Chain, dl, Handler, StoreAddr, MachinePointerInfo(),
                       false, false, 0);
  Chain = DAG.getCopyToReg(Chain, dl, OffsetReg, Offset);

  // Not needed we already use it as explict input to EH_RETURN.
  // MF.getRegInfo().addLiveOut(OffsetReg);

  return DAG.getNode(HexagonISD::EH_RETURN, dl, MVT::Other, Chain);
}

SDValue
HexagonTargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
    default: llvm_unreachable("Should not custom lower this!");
    case ISD::CONCAT_VECTORS:     return LowerCONCAT_VECTORS(Op, DAG);
    case ISD::INSERT_SUBVECTOR:   return LowerINSERT_VECTOR(Op, DAG);
    case ISD::INSERT_VECTOR_ELT:  return LowerINSERT_VECTOR(Op, DAG);
    case ISD::EXTRACT_SUBVECTOR:  return LowerEXTRACT_VECTOR(Op, DAG);
    case ISD::EXTRACT_VECTOR_ELT: return LowerEXTRACT_VECTOR(Op, DAG);
    case ISD::BUILD_VECTOR:       return LowerBUILD_VECTOR(Op, DAG);
    case ISD::VECTOR_SHUFFLE:     return LowerVECTOR_SHUFFLE(Op, DAG);
    case ISD::SRA:
    case ISD::SHL:
    case ISD::SRL:
      return LowerVECTOR_SHIFT(Op, DAG);
    case ISD::ConstantPool:
      return LowerConstantPool(Op, DAG);
    case ISD::EH_RETURN:          return LowerEH_RETURN(Op, DAG);
      // Frame & Return address.  Currently unimplemented.
    case ISD::RETURNADDR:         return LowerRETURNADDR(Op, DAG);
    case ISD::FRAMEADDR:          return LowerFRAMEADDR(Op, DAG);
    case ISD::GlobalTLSAddress:
                          llvm_unreachable("TLS not implemented for Hexagon.");
    case ISD::ATOMIC_FENCE:       return LowerATOMIC_FENCE(Op, DAG);
    case ISD::GlobalAddress:      return LowerGLOBALADDRESS(Op, DAG);
    case ISD::BlockAddress:       return LowerBlockAddress(Op, DAG);
    case ISD::VASTART:            return LowerVASTART(Op, DAG);
    case ISD::BR_JT:              return LowerBR_JT(Op, DAG);
    // Custom lower some vector loads.
    case ISD::LOAD:               return LowerLOAD(Op, DAG);

    case ISD::DYNAMIC_STACKALLOC: return LowerDYNAMIC_STACKALLOC(Op, DAG);
    case ISD::SELECT:             return Op;
    case ISD::SETCC:              return LowerSETCC(Op, DAG);
    case ISD::VSELECT:            return LowerVSELECT(Op, DAG);
    case ISD::CTPOP:              return LowerCTPOP(Op, DAG);
    case ISD::INTRINSIC_WO_CHAIN: return LowerINTRINSIC_WO_CHAIN(Op, DAG);
    case ISD::INLINEASM:          return LowerINLINEASM(Op, DAG);

  }
}



//===----------------------------------------------------------------------===//
//                           Hexagon Scheduler Hooks
//===----------------------------------------------------------------------===//
MachineBasicBlock *
HexagonTargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                                   MachineBasicBlock *BB)
const {
  switch (MI->getOpcode()) {
    case Hexagon::ADJDYNALLOC: {
      MachineFunction *MF = BB->getParent();
      HexagonMachineFunctionInfo *FuncInfo =
        MF->getInfo<HexagonMachineFunctionInfo>();
      FuncInfo->addAllocaAdjustInst(MI);
      return BB;
    }
    default: llvm_unreachable("Unexpected instr type to insert");
  } // switch
}

//===----------------------------------------------------------------------===//
// Inline Assembly Support
//===----------------------------------------------------------------------===//

std::pair<unsigned, const TargetRegisterClass *>
HexagonTargetLowering::getRegForInlineAsmConstraint(
    const TargetRegisterInfo *TRI, const std::string &Constraint,
    MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':   // R0-R31
       switch (VT.SimpleTy) {
       default:
         llvm_unreachable("getRegForInlineAsmConstraint Unhandled data type");
       case MVT::i32:
       case MVT::i16:
       case MVT::i8:
       case MVT::f32:
         return std::make_pair(0U, &Hexagon::IntRegsRegClass);
       case MVT::i64:
       case MVT::f64:
         return std::make_pair(0U, &Hexagon::DoubleRegsRegClass);
      }
    default:
      llvm_unreachable("Unknown asm register class");
    }
  }

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

/// isFPImmLegal - Returns true if the target can instruction select the
/// specified FP immediate natively. If false, the legalizer will
/// materialize the FP immediate as a load from a constant pool.
bool HexagonTargetLowering::isFPImmLegal(const APFloat &Imm, EVT VT) const {
  return Subtarget->hasV5TOps();
}

/// isLegalAddressingMode - Return true if the addressing mode represented by
/// AM is legal for this target, for a load/store of the specified type.
bool HexagonTargetLowering::isLegalAddressingMode(const AddrMode &AM,
                                                  Type *Ty) const {
  // Allows a signed-extended 11-bit immediate field.
  if (AM.BaseOffs <= -(1LL << 13) || AM.BaseOffs >= (1LL << 13)-1) {
    return false;
  }

  // No global is ever allowed as a base.
  if (AM.BaseGV) {
    return false;
  }

  int Scale = AM.Scale;
  if (Scale < 0) Scale = -Scale;
  switch (Scale) {
  case 0:  // No scale reg, "r+i", "r", or just "i".
    break;
  default: // No scaled addressing mode.
    return false;
  }
  return true;
}

/// isLegalICmpImmediate - Return true if the specified immediate is legal
/// icmp immediate, that is the target has icmp instructions which can compare
/// a register against the immediate without having to materialize the
/// immediate into a register.
bool HexagonTargetLowering::isLegalICmpImmediate(int64_t Imm) const {
  return Imm >= -512 && Imm <= 511;
}

/// IsEligibleForTailCallOptimization - Check whether the call is eligible
/// for tail call optimization. Targets which want to do tail call
/// optimization should implement this function.
bool HexagonTargetLowering::IsEligibleForTailCallOptimization(
                                 SDValue Callee,
                                 CallingConv::ID CalleeCC,
                                 bool isVarArg,
                                 bool isCalleeStructRet,
                                 bool isCallerStructRet,
                                 const SmallVectorImpl<ISD::OutputArg> &Outs,
                                 const SmallVectorImpl<SDValue> &OutVals,
                                 const SmallVectorImpl<ISD::InputArg> &Ins,
                                 SelectionDAG& DAG) const {
  const Function *CallerF = DAG.getMachineFunction().getFunction();
  CallingConv::ID CallerCC = CallerF->getCallingConv();
  bool CCMatch = CallerCC == CalleeCC;

  // ***************************************************************************
  //  Look for obvious safe cases to perform tail call optimization that do not
  //  require ABI changes.
  // ***************************************************************************

  // If this is a tail call via a function pointer, then don't do it!
  if (!(dyn_cast<GlobalAddressSDNode>(Callee))
      && !(dyn_cast<ExternalSymbolSDNode>(Callee))) {
    return false;
  }

  // Do not optimize if the calling conventions do not match.
  if (!CCMatch)
    return false;

  // Do not tail call optimize vararg calls.
  if (isVarArg)
    return false;

  // Also avoid tail call optimization if either caller or callee uses struct
  // return semantics.
  if (isCalleeStructRet || isCallerStructRet)
    return false;

  // In addition to the cases above, we also disable Tail Call Optimization if
  // the calling convention code that at least one outgoing argument needs to
  // go on the stack. We cannot check that here because at this point that
  // information is not available.
  return true;
}

// Return true when the given node fits in a positive half word.
bool llvm::isPositiveHalfWord(SDNode *N) {
  ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N);
  if (CN && CN->getSExtValue() > 0 && isInt<16>(CN->getSExtValue()))
    return true;

  switch (N->getOpcode()) {
  default:
    return false;
  case ISD::SIGN_EXTEND_INREG:
    return true;
  }
}
