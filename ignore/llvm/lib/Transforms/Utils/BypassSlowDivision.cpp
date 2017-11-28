//===-- BypassSlowDivision.cpp - Bypass slow division ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains an optimization for div and rem on architectures that
// execute short instructions significantly faster than longer instructions.
// For example, on Intel Atom 32-bit divides are slow enough that during
// runtime it is profitable to check the value of the operands, and if they are
// positive and less than 256 use an unsigned 8-bit divide.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/BypassSlowDivision.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "bypass-slow-division"

namespace {
  struct DivOpInfo {
    bool SignedOp;
    Value *Dividend;
    Value *Divisor;

    DivOpInfo(bool InSignedOp, Value *InDividend, Value *InDivisor)
      : SignedOp(InSignedOp), Dividend(InDividend), Divisor(InDivisor) {}
  };

  struct QuotRemPair {
    Value *Quotient;
    Value *Remainder;

    QuotRemPair(Value *InQuotient, Value *InRemainder)
        : Quotient(InQuotient), Remainder(InRemainder) {}
  };

  /// A quotient and remainder, plus a BB from which they logically "originate".
  /// If you use Quotient or Remainder in a Phi node, you should use BB as its
  /// corresponding predecessor.
  struct QuotRemWithBB {
    BasicBlock *BB = nullptr;
    Value *Quotient = nullptr;
    Value *Remainder = nullptr;
  };
}

namespace llvm {
  template<>
  struct DenseMapInfo<DivOpInfo> {
    static bool isEqual(const DivOpInfo &Val1, const DivOpInfo &Val2) {
      return Val1.SignedOp == Val2.SignedOp &&
             Val1.Dividend == Val2.Dividend &&
             Val1.Divisor == Val2.Divisor;
    }

    static DivOpInfo getEmptyKey() {
      return DivOpInfo(false, nullptr, nullptr);
    }

    static DivOpInfo getTombstoneKey() {
      return DivOpInfo(true, nullptr, nullptr);
    }

    static unsigned getHashValue(const DivOpInfo &Val) {
      return (unsigned)(reinterpret_cast<uintptr_t>(Val.Dividend) ^
                        reinterpret_cast<uintptr_t>(Val.Divisor)) ^
                        (unsigned)Val.SignedOp;
    }
  };

  typedef DenseMap<DivOpInfo, QuotRemPair> DivCacheTy;
  typedef DenseMap<unsigned, unsigned> BypassWidthsTy;
}

namespace {
enum ValueRange {
  /// Operand definitely fits into BypassType. No runtime checks are needed.
  VALRNG_SHORT,
  /// A runtime check is required, as value range is unknown.
  VALRNG_UNKNOWN,
  /// Operand is unlikely to fit into BypassType. The bypassing should be
  /// disabled.
  VALRNG_LONG
};

class FastDivInsertionTask {
  bool IsValidTask = false;
  Instruction *SlowDivOrRem = nullptr;
  IntegerType *BypassType = nullptr;
  BasicBlock *MainBB = nullptr;

  ValueRange getValueRange(Value *Op);
  QuotRemWithBB createSlowBB(BasicBlock *Successor);
  QuotRemWithBB createFastBB(BasicBlock *Successor);
  QuotRemPair createDivRemPhiNodes(QuotRemWithBB &LHS, QuotRemWithBB &RHS,
                                   BasicBlock *PhiBB);
  Value *insertOperandRuntimeCheck(Value *Op1, Value *Op2);
  Optional<QuotRemPair> insertFastDivAndRem();

  bool isSignedOp() {
    return SlowDivOrRem->getOpcode() == Instruction::SDiv ||
           SlowDivOrRem->getOpcode() == Instruction::SRem;
  }
  bool isDivisionOp() {
    return SlowDivOrRem->getOpcode() == Instruction::SDiv ||
           SlowDivOrRem->getOpcode() == Instruction::UDiv;
  }
  Type *getSlowType() { return SlowDivOrRem->getType(); }

public:
  FastDivInsertionTask(Instruction *I, const BypassWidthsTy &BypassWidths);
  Value *getReplacement(DivCacheTy &Cache);
};
} // anonymous namespace

FastDivInsertionTask::FastDivInsertionTask(Instruction *I,
                                           const BypassWidthsTy &BypassWidths) {
  switch (I->getOpcode()) {
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::URem:
  case Instruction::SRem:
    SlowDivOrRem = I;
    break;
  default:
    // I is not a div/rem operation.
    return;
  }

  // Skip division on vector types. Only optimize integer instructions.
  IntegerType *SlowType = dyn_cast<IntegerType>(SlowDivOrRem->getType());
  if (!SlowType)
    return;

  // Skip if this bitwidth is not bypassed.
  auto BI = BypassWidths.find(SlowType->getBitWidth());
  if (BI == BypassWidths.end())
    return;

  // Get type for div/rem instruction with bypass bitwidth.
  IntegerType *BT = IntegerType::get(I->getContext(), BI->second);
  BypassType = BT;

  // The original basic block.
  MainBB = I->getParent();

  // The instruction is indeed a slow div or rem operation.
  IsValidTask = true;
}

/// Reuses previously-computed dividend or remainder from the current BB if
/// operands and operation are identical. Otherwise calls insertFastDivAndRem to
/// perform the optimization and caches the resulting dividend and remainder.
/// If no replacement can be generated, nullptr is returned.
Value *FastDivInsertionTask::getReplacement(DivCacheTy &Cache) {
  // First, make sure that the task is valid.
  if (!IsValidTask)
    return nullptr;

  // Then, look for a value in Cache.
  Value *Dividend = SlowDivOrRem->getOperand(0);
  Value *Divisor = SlowDivOrRem->getOperand(1);
  DivOpInfo Key(isSignedOp(), Dividend, Divisor);
  auto CacheI = Cache.find(Key);

  if (CacheI == Cache.end()) {
    // If previous instance does not exist, try to insert fast div.
    Optional<QuotRemPair> OptResult = insertFastDivAndRem();
    // Bail out if insertFastDivAndRem has failed.
    if (!OptResult)
      return nullptr;
    CacheI = Cache.insert({Key, *OptResult}).first;
  }

  QuotRemPair &Value = CacheI->second;
  return isDivisionOp() ? Value.Quotient : Value.Remainder;
}

/// Check if an integer value fits into our bypass type.
ValueRange FastDivInsertionTask::getValueRange(Value *V) {
  unsigned ShortLen = BypassType->getBitWidth();
  unsigned LongLen = V->getType()->getIntegerBitWidth();

  assert(LongLen > ShortLen && "Value type must be wider than BypassType");
  unsigned HiBits = LongLen - ShortLen;

  const DataLayout &DL = SlowDivOrRem->getModule()->getDataLayout();
  APInt Zeros(LongLen, 0), Ones(LongLen, 0);

  computeKnownBits(V, Zeros, Ones, DL);

  if (Zeros.countLeadingOnes() >= HiBits)
    return VALRNG_SHORT;

  if (Ones.countLeadingZeros() < HiBits)
    return VALRNG_LONG;

  return VALRNG_UNKNOWN;
}

/// Add new basic block for slow div and rem operations and put it before
/// SuccessorBB.
QuotRemWithBB FastDivInsertionTask::createSlowBB(BasicBlock *SuccessorBB) {
  QuotRemWithBB DivRemPair;
  DivRemPair.BB = BasicBlock::Create(MainBB->getParent()->getContext(), "",
                                     MainBB->getParent(), SuccessorBB);
  IRBuilder<> Builder(DivRemPair.BB, DivRemPair.BB->begin());

  Value *Dividend = SlowDivOrRem->getOperand(0);
  Value *Divisor = SlowDivOrRem->getOperand(1);

  if (isSignedOp()) {
    DivRemPair.Quotient = Builder.CreateSDiv(Dividend, Divisor);
    DivRemPair.Remainder = Builder.CreateSRem(Dividend, Divisor);
  } else {
    DivRemPair.Quotient = Builder.CreateUDiv(Dividend, Divisor);
    DivRemPair.Remainder = Builder.CreateURem(Dividend, Divisor);
  }

  Builder.CreateBr(SuccessorBB);
  return DivRemPair;
}

/// Add new basic block for fast div and rem operations and put it before
/// SuccessorBB.
QuotRemWithBB FastDivInsertionTask::createFastBB(BasicBlock *SuccessorBB) {
  QuotRemWithBB DivRemPair;
  DivRemPair.BB = BasicBlock::Create(MainBB->getParent()->getContext(), "",
                                     MainBB->getParent(), SuccessorBB);
  IRBuilder<> Builder(DivRemPair.BB, DivRemPair.BB->begin());

  Value *Dividend = SlowDivOrRem->getOperand(0);
  Value *Divisor = SlowDivOrRem->getOperand(1);
  Value *ShortDivisorV =
      Builder.CreateCast(Instruction::Trunc, Divisor, BypassType);
  Value *ShortDividendV =
      Builder.CreateCast(Instruction::Trunc, Dividend, BypassType);

  // udiv/urem because this optimization only handles positive numbers.
  Value *ShortQV = Builder.CreateUDiv(ShortDividendV, ShortDivisorV);
  Value *ShortRV = Builder.CreateURem(ShortDividendV, ShortDivisorV);
  DivRemPair.Quotient =
      Builder.CreateCast(Instruction::ZExt, ShortQV, getSlowType());
  DivRemPair.Remainder =
      Builder.CreateCast(Instruction::ZExt, ShortRV, getSlowType());
  Builder.CreateBr(SuccessorBB);

  return DivRemPair;
}

/// Creates Phi nodes for result of Div and Rem.
QuotRemPair FastDivInsertionTask::createDivRemPhiNodes(QuotRemWithBB &LHS,
                                                       QuotRemWithBB &RHS,
                                                       BasicBlock *PhiBB) {
  IRBuilder<> Builder(PhiBB, PhiBB->begin());
  PHINode *QuoPhi = Builder.CreatePHI(getSlowType(), 2);
  QuoPhi->addIncoming(LHS.Quotient, LHS.BB);
  QuoPhi->addIncoming(RHS.Quotient, RHS.BB);
  PHINode *RemPhi = Builder.CreatePHI(getSlowType(), 2);
  RemPhi->addIncoming(LHS.Remainder, LHS.BB);
  RemPhi->addIncoming(RHS.Remainder, RHS.BB);
  return QuotRemPair(QuoPhi, RemPhi);
}

/// Creates a runtime check to test whether both the divisor and dividend fit
/// into BypassType. The check is inserted at the end of MainBB. True return
/// value means that the operands fit. Either of the operands may be NULL if it
/// doesn't need a runtime check.
Value *FastDivInsertionTask::insertOperandRuntimeCheck(Value *Op1, Value *Op2) {
  assert((Op1 || Op2) && "Nothing to check");
  IRBuilder<> Builder(MainBB, MainBB->end());

  Value *OrV;
  if (Op1 && Op2)
    OrV = Builder.CreateOr(Op1, Op2);
  else
    OrV = Op1 ? Op1 : Op2;

  // BitMask is inverted to check if the operands are
  // larger than the bypass type
  uint64_t BitMask = ~BypassType->getBitMask();
  Value *AndV = Builder.CreateAnd(OrV, BitMask);

  // Compare operand values
  Value *ZeroV = ConstantInt::getSigned(getSlowType(), 0);
  return Builder.CreateICmpEQ(AndV, ZeroV);
}

/// Substitutes the div/rem instruction with code that checks the value of the
/// operands and uses a shorter-faster div/rem instruction when possible.
Optional<QuotRemPair> FastDivInsertionTask::insertFastDivAndRem() {
  Value *Dividend = SlowDivOrRem->getOperand(0);
  Value *Divisor = SlowDivOrRem->getOperand(1);

  if (isa<ConstantInt>(Divisor)) {
    // Keep division by a constant for DAGCombiner.
    return None;
  }

  ValueRange DividendRange = getValueRange(Dividend);
  if (DividendRange == VALRNG_LONG)
    return None;

  ValueRange DivisorRange = getValueRange(Divisor);
  if (DivisorRange == VALRNG_LONG)
    return None;

  bool DividendShort = (DividendRange == VALRNG_SHORT);
  bool DivisorShort = (DivisorRange == VALRNG_SHORT);

  if (DividendShort && DivisorShort) {
    // If both operands are known to be short then just replace the long
    // division with a short one in-place.

    IRBuilder<> Builder(SlowDivOrRem);
    Value *TruncDividend = Builder.CreateTrunc(Dividend, BypassType);
    Value *TruncDivisor = Builder.CreateTrunc(Divisor, BypassType);
    Value *TruncDiv = Builder.CreateUDiv(TruncDividend, TruncDivisor);
    Value *TruncRem = Builder.CreateURem(TruncDividend, TruncDivisor);
    Value *ExtDiv = Builder.CreateZExt(TruncDiv, getSlowType());
    Value *ExtRem = Builder.CreateZExt(TruncRem, getSlowType());
    return QuotRemPair(ExtDiv, ExtRem);
  } else if (DividendShort && !isSignedOp()) {
    // If the division is unsigned and Dividend is known to be short, then
    // either
    // 1) Divisor is less or equal to Dividend, and the result can be computed
    //    with a short division.
    // 2) Divisor is greater than Dividend. In this case, no division is needed
    //    at all: The quotient is 0 and the remainder is equal to Dividend.
    //
    // So instead of checking at runtime whether Divisor fits into BypassType,
    // we emit a runtime check to differentiate between these two cases. This
    // lets us entirely avoid a long div.

    // Split the basic block before the div/rem.
    BasicBlock *SuccessorBB = MainBB->splitBasicBlock(SlowDivOrRem);
    // Remove the unconditional branch from MainBB to SuccessorBB.
    MainBB->getInstList().back().eraseFromParent();
    QuotRemWithBB Long;
    Long.BB = MainBB;
    Long.Quotient = ConstantInt::get(getSlowType(), 0);
    Long.Remainder = Dividend;
    QuotRemWithBB Fast = createFastBB(SuccessorBB);
    QuotRemPair Result = createDivRemPhiNodes(Fast, Long, SuccessorBB);
    IRBuilder<> Builder(MainBB, MainBB->end());
    Value *CmpV = Builder.CreateICmpUGE(Dividend, Divisor);
    Builder.CreateCondBr(CmpV, Fast.BB, SuccessorBB);
    return Result;
  } else {
    // General case. Create both slow and fast div/rem pairs and choose one of
    // them at runtime.

    // Split the basic block before the div/rem.
    BasicBlock *SuccessorBB = MainBB->splitBasicBlock(SlowDivOrRem);
    // Remove the unconditional branch from MainBB to SuccessorBB.
    MainBB->getInstList().back().eraseFromParent();
    QuotRemWithBB Fast = createFastBB(SuccessorBB);
    QuotRemWithBB Slow = createSlowBB(SuccessorBB);
    QuotRemPair Result = createDivRemPhiNodes(Fast, Slow, SuccessorBB);
    Value *CmpV = insertOperandRuntimeCheck(DividendShort ? nullptr : Dividend,
                                            DivisorShort ? nullptr : Divisor);
    IRBuilder<> Builder(MainBB, MainBB->end());
    Builder.CreateCondBr(CmpV, Fast.BB, Slow.BB);
    return Result;
  }
}

/// This optimization identifies DIV/REM instructions in a BB that can be
/// profitably bypassed and carried out with a shorter, faster divide.
bool llvm::bypassSlowDivision(BasicBlock *BB,
                              const BypassWidthsTy &BypassWidths) {
  DivCacheTy PerBBDivCache;

  bool MadeChange = false;
  Instruction* Next = &*BB->begin();
  while (Next != nullptr) {
    // We may add instructions immediately after I, but we want to skip over
    // them.
    Instruction* I = Next;
    Next = Next->getNextNode();

    FastDivInsertionTask Task(I, BypassWidths);
    if (Value *Replacement = Task.getReplacement(PerBBDivCache)) {
      I->replaceAllUsesWith(Replacement);
      I->eraseFromParent();
      MadeChange = true;
    }
  }

  // Above we eagerly create divs and rems, as pairs, so that we can efficiently
  // create divrem machine instructions.  Now erase any unused divs / rems so we
  // don't leave extra instructions sitting around.
  for (auto &KV : PerBBDivCache)
    for (Value *V : {KV.second.Quotient, KV.second.Remainder})
      RecursivelyDeleteTriviallyDeadInstructions(V);

  return MadeChange;
}