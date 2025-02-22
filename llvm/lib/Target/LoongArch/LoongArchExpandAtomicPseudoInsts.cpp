//==- LoongArchExpandAtomicPseudoInsts.cpp - Expand atomic pseudo instrs. -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that expands atomic pseudo instructions into
// target instructions. This pass should be run at the last possible moment,
// avoiding the possibility for other passes to break the requirements for
// forward progress in the LL/SC block.
//
//===----------------------------------------------------------------------===//

#include "LoongArch.h"
#include "LoongArchInstrInfo.h"
#include "LoongArchTargetMachine.h"

#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

using namespace llvm;

#define LoongArch_EXPAND_ATOMIC_PSEUDO_NAME                                    \
  "LoongArch atomic pseudo instruction expansion pass"

namespace {

class LoongArchExpandAtomicPseudo : public MachineFunctionPass {
public:
  const LoongArchInstrInfo *TII;
  static char ID;

  LoongArchExpandAtomicPseudo() : MachineFunctionPass(ID) {
    initializeLoongArchExpandAtomicPseudoPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return LoongArch_EXPAND_ATOMIC_PSEUDO_NAME;
  }

private:
  bool expandMBB(MachineBasicBlock &MBB);
  bool expandMI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                MachineBasicBlock::iterator &NextMBBI);
  bool expandAtomicBinOp(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator MBBI, AtomicRMWInst::BinOp,
                         bool IsMasked, int Width,
                         MachineBasicBlock::iterator &NextMBBI);
};

char LoongArchExpandAtomicPseudo::ID = 0;

bool LoongArchExpandAtomicPseudo::runOnMachineFunction(MachineFunction &MF) {
  TII =
      static_cast<const LoongArchInstrInfo *>(MF.getSubtarget().getInstrInfo());
  bool Modified = false;
  for (auto &MBB : MF)
    Modified |= expandMBB(MBB);
  return Modified;
}

bool LoongArchExpandAtomicPseudo::expandMBB(MachineBasicBlock &MBB) {
  bool Modified = false;

  MachineBasicBlock::iterator MBBI = MBB.begin(), E = MBB.end();
  while (MBBI != E) {
    MachineBasicBlock::iterator NMBBI = std::next(MBBI);
    Modified |= expandMI(MBB, MBBI, NMBBI);
    MBBI = NMBBI;
  }

  return Modified;
}

bool LoongArchExpandAtomicPseudo::expandMI(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    MachineBasicBlock::iterator &NextMBBI) {
  switch (MBBI->getOpcode()) {
  case LoongArch::PseudoMaskedAtomicSwap32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Xchg, true, 32,
                             NextMBBI);
  case LoongArch::PseudoAtomicSwap32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Xchg, false, 32,
                             NextMBBI);
  case LoongArch::PseudoMaskedAtomicLoadAdd32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Add, true, 32, NextMBBI);
  case LoongArch::PseudoMaskedAtomicLoadSub32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Sub, true, 32, NextMBBI);
  case LoongArch::PseudoAtomicLoadNand32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Nand, false, 32,
                             NextMBBI);
  case LoongArch::PseudoAtomicLoadNand64:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Nand, false, 64,
                             NextMBBI);
  case LoongArch::PseudoMaskedAtomicLoadNand32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Nand, true, 32,
                             NextMBBI);
  case LoongArch::PseudoAtomicLoadAdd32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Add, false, 32,
                             NextMBBI);
  case LoongArch::PseudoAtomicLoadSub32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Sub, false, 32,
                             NextMBBI);
  case LoongArch::PseudoAtomicLoadAnd32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::And, false, 32,
                             NextMBBI);
  case LoongArch::PseudoAtomicLoadOr32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Or, false, 32, NextMBBI);
  case LoongArch::PseudoAtomicLoadXor32:
    return expandAtomicBinOp(MBB, MBBI, AtomicRMWInst::Xor, false, 32,
                             NextMBBI);
  }
  return false;
}

static void doAtomicBinOpExpansion(const LoongArchInstrInfo *TII,
                                   MachineInstr &MI, DebugLoc DL,
                                   MachineBasicBlock *ThisMBB,
                                   MachineBasicBlock *LoopMBB,
                                   MachineBasicBlock *DoneMBB,
                                   AtomicRMWInst::BinOp BinOp, int Width) {
  Register DestReg = MI.getOperand(0).getReg();
  Register ScratchReg = MI.getOperand(1).getReg();
  Register AddrReg = MI.getOperand(2).getReg();
  Register IncrReg = MI.getOperand(3).getReg();

  // .loop:
  //   dbar 0
  //   ll.[w|d] dest, (addr)
  //   binop scratch, dest, val
  //   sc.[w|d] scratch, scratch, (addr)
  //   beq scratch, zero, loop
  BuildMI(LoopMBB, DL, TII->get(LoongArch::DBAR)).addImm(0);
  BuildMI(LoopMBB, DL,
          TII->get(Width == 32 ? LoongArch::LL_W : LoongArch::LL_D), DestReg)
      .addReg(AddrReg)
      .addImm(0);
  switch (BinOp) {
  default:
    llvm_unreachable("Unexpected AtomicRMW BinOp");
  case AtomicRMWInst::Xchg:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::OR), ScratchReg)
        .addReg(IncrReg)
        .addReg(LoongArch::R0);
    break;
  case AtomicRMWInst::Nand:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::AND), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    BuildMI(LoopMBB, DL, TII->get(LoongArch::XORI), ScratchReg)
        .addReg(ScratchReg)
        .addImm(-1);
    break;
  case AtomicRMWInst::Add:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::ADD_W), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    break;
  case AtomicRMWInst::Sub:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::SUB_W), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    break;
  case AtomicRMWInst::And:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::AND), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    break;
  case AtomicRMWInst::Or:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::OR), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    break;
  case AtomicRMWInst::Xor:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::XOR), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    break;
  }
  BuildMI(LoopMBB, DL,
          TII->get(Width == 32 ? LoongArch::SC_W : LoongArch::SC_D), ScratchReg)
      .addReg(ScratchReg)
      .addReg(AddrReg)
      .addImm(0);
  BuildMI(LoopMBB, DL, TII->get(LoongArch::BEQ))
      .addReg(ScratchReg)
      .addReg(LoongArch::R0)
      .addMBB(LoopMBB);
}

static void insertMaskedMerge(const LoongArchInstrInfo *TII, DebugLoc DL,
                              MachineBasicBlock *MBB, Register DestReg,
                              Register OldValReg, Register NewValReg,
                              Register MaskReg, Register ScratchReg) {
  assert(OldValReg != ScratchReg && "OldValReg and ScratchReg must be unique");
  assert(OldValReg != MaskReg && "OldValReg and MaskReg must be unique");
  assert(ScratchReg != MaskReg && "ScratchReg and MaskReg must be unique");

  // res = oldval ^ ((oldval ^ newval) & masktargetdata);
  BuildMI(MBB, DL, TII->get(LoongArch::XOR), ScratchReg)
      .addReg(OldValReg)
      .addReg(NewValReg);
  BuildMI(MBB, DL, TII->get(LoongArch::AND), ScratchReg)
      .addReg(ScratchReg)
      .addReg(MaskReg);
  BuildMI(MBB, DL, TII->get(LoongArch::XOR), DestReg)
      .addReg(OldValReg)
      .addReg(ScratchReg);
}

static void doMaskedAtomicBinOpExpansion(
    const LoongArchInstrInfo *TII, MachineInstr &MI, DebugLoc DL,
    MachineBasicBlock *ThisMBB, MachineBasicBlock *LoopMBB,
    MachineBasicBlock *DoneMBB, AtomicRMWInst::BinOp BinOp, int Width) {
  assert(Width == 32 && "Should never need to expand masked 64-bit operations");
  Register DestReg = MI.getOperand(0).getReg();
  Register ScratchReg = MI.getOperand(1).getReg();
  Register AddrReg = MI.getOperand(2).getReg();
  Register IncrReg = MI.getOperand(3).getReg();
  Register MaskReg = MI.getOperand(4).getReg();

  // .loop:
  //   dbar 0
  //   ll.w destreg, (alignedaddr)
  //   binop scratch, destreg, incr
  //   xor scratch, destreg, scratch
  //   and scratch, scratch, masktargetdata
  //   xor scratch, destreg, scratch
  //   sc.w scratch, scratch, (alignedaddr)
  //   beq scratch, zero, loop
  BuildMI(LoopMBB, DL, TII->get(LoongArch::DBAR)).addImm(0);
  BuildMI(LoopMBB, DL, TII->get(LoongArch::LL_W), DestReg)
      .addReg(AddrReg)
      .addImm(0);
  switch (BinOp) {
  default:
    llvm_unreachable("Unexpected AtomicRMW BinOp");
  case AtomicRMWInst::Xchg:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::ADDI_W), ScratchReg)
        .addReg(IncrReg)
        .addImm(0);
    break;
  case AtomicRMWInst::Add:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::ADD_W), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    break;
  case AtomicRMWInst::Sub:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::SUB_W), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    break;
  case AtomicRMWInst::Nand:
    BuildMI(LoopMBB, DL, TII->get(LoongArch::AND), ScratchReg)
        .addReg(DestReg)
        .addReg(IncrReg);
    BuildMI(LoopMBB, DL, TII->get(LoongArch::XORI), ScratchReg)
        .addReg(ScratchReg)
        .addImm(-1);
    // TODO: support other AtomicRMWInst.
  }

  insertMaskedMerge(TII, DL, LoopMBB, ScratchReg, DestReg, ScratchReg, MaskReg,
                    ScratchReg);

  BuildMI(LoopMBB, DL, TII->get(LoongArch::SC_W), ScratchReg)
      .addReg(ScratchReg)
      .addReg(AddrReg)
      .addImm(0);
  BuildMI(LoopMBB, DL, TII->get(LoongArch::BEQ))
      .addReg(ScratchReg)
      .addReg(LoongArch::R0)
      .addMBB(LoopMBB);
}

bool LoongArchExpandAtomicPseudo::expandAtomicBinOp(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
    AtomicRMWInst::BinOp BinOp, bool IsMasked, int Width,
    MachineBasicBlock::iterator &NextMBBI) {
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();

  MachineFunction *MF = MBB.getParent();
  auto LoopMBB = MF->CreateMachineBasicBlock(MBB.getBasicBlock());
  auto DoneMBB = MF->CreateMachineBasicBlock(MBB.getBasicBlock());

  // Insert new MBBs.
  MF->insert(++MBB.getIterator(), LoopMBB);
  MF->insert(++LoopMBB->getIterator(), DoneMBB);

  // Set up successors and transfer remaining instructions to DoneMBB.
  LoopMBB->addSuccessor(LoopMBB);
  LoopMBB->addSuccessor(DoneMBB);
  DoneMBB->splice(DoneMBB->end(), &MBB, MI, MBB.end());
  DoneMBB->transferSuccessors(&MBB);
  MBB.addSuccessor(LoopMBB);

  if (IsMasked)
    doMaskedAtomicBinOpExpansion(TII, MI, DL, &MBB, LoopMBB, DoneMBB, BinOp,
                                 Width);
  else
    doAtomicBinOpExpansion(TII, MI, DL, &MBB, LoopMBB, DoneMBB, BinOp, Width);

  NextMBBI = MBB.end();
  MI.eraseFromParent();

  LivePhysRegs LiveRegs;
  computeAndAddLiveIns(LiveRegs, *LoopMBB);
  computeAndAddLiveIns(LiveRegs, *DoneMBB);

  return true;
}

} // end namespace

INITIALIZE_PASS(LoongArchExpandAtomicPseudo, "loongarch-expand-atomic-pseudo",
                LoongArch_EXPAND_ATOMIC_PSEUDO_NAME, false, false)

namespace llvm {

FunctionPass *createLoongArchExpandAtomicPseudoPass() {
  return new LoongArchExpandAtomicPseudo();
}

} // end namespace llvm
