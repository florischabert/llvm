//===-- MCS51BranchSelector.cpp - Emit long conditional branches ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that scans a machine function to determine which
// conditional branches need more than 10 bits of displacement to reach their
// target basic block.  It does this in two passes; a calculation of basic block
// positions pass, and a branch pseudo op to machine branch opcode pass.  This
// pass should be run last, just before the assembly printer.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "MCS51-branch-select"
#include "MCS51.h"
#include "MCS51InstrInfo.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetMachine.h"
using namespace llvm;

STATISTIC(NumExpanded, "Number of branches expanded to long format");

namespace {
  struct MCS51BSel : public MachineFunctionPass {
    static char ID;
    MCS51BSel() : MachineFunctionPass(ID) {}

    /// BlockSizes - The sizes of the basic blocks in the function.
    std::vector<unsigned> BlockSizes;

    virtual bool runOnMachineFunction(MachineFunction &Fn);

    virtual const char *getPassName() const {
      return "MCS51 Branch Selector";
    }
  };
  char MCS51BSel::ID = 0;
}

/// createMCS51BranchSelectionPass - returns an instance of the Branch
/// Selection Pass
///
FunctionPass *llvm::createMCS51BranchSelectionPass() {
  return new MCS51BSel();
}

bool MCS51BSel::runOnMachineFunction(MachineFunction &Fn) {
  const MCS51InstrInfo *TII =
             static_cast<const MCS51InstrInfo*>(Fn.getTarget().getInstrInfo());
  // Give the blocks of the function a dense, in-order, numbering.
  Fn.RenumberBlocks();
  BlockSizes.resize(Fn.getNumBlockIDs());

  // Measure each MBB and compute a size for the entire function.
  unsigned FuncSize = 0;
  for (MachineFunction::iterator MFI = Fn.begin(), E = Fn.end(); MFI != E;
       ++MFI) {
    MachineBasicBlock *MBB = MFI;

    unsigned BlockSize = 0;
    for (MachineBasicBlock::iterator MBBI = MBB->begin(), EE = MBB->end();
         MBBI != EE; ++MBBI)
      BlockSize += TII->GetInstSizeInBytes(MBBI);

    BlockSizes[MBB->getNumber()] = BlockSize;
    FuncSize += BlockSize;
  }

  // If the entire function is smaller than the displacement of a branch field,
  // we know we don't need to shrink any branches in this function.  This is a
  // common case.
  if (FuncSize < (1 << 9)) {
    BlockSizes.clear();
    return false;
  }

  // For each conditional branch, if the offset to its destination is larger
  // than the offset field allows, transform it into a long branch sequence
  // like this:
  //   short branch:
  //     bCC MBB
  //   long branch:
  //     b!CC $PC+6
  //     b MBB
  //
  bool MadeChange = true;
  bool EverMadeChange = false;
  while (MadeChange) {
    // Iteratively expand branches until we reach a fixed point.
    MadeChange = false;

    for (MachineFunction::iterator MFI = Fn.begin(), E = Fn.end(); MFI != E;
         ++MFI) {
      MachineBasicBlock &MBB = *MFI;
      unsigned MBBStartOffset = 0;
      for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end();
           I != E; ++I) {
        if ((I->getOpcode() != MCS51::JCC || I->getOperand(0).isImm()) &&
            I->getOpcode() != MCS51::JMP) {
          MBBStartOffset += TII->GetInstSizeInBytes(I);
          continue;
        }

        // Determine the offset from the current branch to the destination
        // block.
        MachineBasicBlock *Dest = I->getOperand(0).getMBB();

        int BranchSize;
        if (Dest->getNumber() <= MBB.getNumber()) {
          // If this is a backwards branch, the delta is the offset from the
          // start of this block to this branch, plus the sizes of all blocks
          // from this block to the dest.
          BranchSize = MBBStartOffset;

          for (unsigned i = Dest->getNumber(), e = MBB.getNumber(); i != e; ++i)
            BranchSize += BlockSizes[i];
        } else {
          // Otherwise, add the size of the blocks between this block and the
          // dest to the number of bytes left in this block.
          BranchSize = -MBBStartOffset;

          for (unsigned i = MBB.getNumber(), e = Dest->getNumber(); i != e; ++i)
            BranchSize += BlockSizes[i];
        }

        // If this branch is in range, ignore it.
        if (isInt<10>(BranchSize)) {
          MBBStartOffset += 2;
          continue;
        }

        // Otherwise, we have to expand it to a long branch.
        unsigned NewSize;
        MachineInstr *OldBranch = I;
        DebugLoc dl = OldBranch->getDebugLoc();

        if (I->getOpcode() == MCS51::JMP) {
          NewSize = 4;
        } else {
          // The BCC operands are:
          // 0. MCS51 branch predicate
          // 1. Target MBB
          SmallVector<MachineOperand, 1> Cond;
          Cond.push_back(I->getOperand(1));

          // Jump over the uncond branch inst (i.e. $+6) on opposite condition.
          TII->ReverseBranchCondition(Cond);
          BuildMI(MBB, I, dl, TII->get(MCS51::JCC))
            .addImm(4).addOperand(Cond[0]);

          NewSize = 6;
        }
        // Uncond branch to the real destination.
        I = BuildMI(MBB, I, dl, TII->get(MCS51::Bi)).addMBB(Dest);

        // Remove the old branch from the function.
        OldBranch->eraseFromParent();

        // Remember that this instruction is NewSize bytes, increase the size of the
        // block by NewSize-2, remember to iterate.
        BlockSizes[MBB.getNumber()] += NewSize-2;
        MBBStartOffset += NewSize;

        ++NumExpanded;
        MadeChange = true;
      }
    }
    EverMadeChange |= MadeChange;
  }

  BlockSizes.clear();
  return true;
}
