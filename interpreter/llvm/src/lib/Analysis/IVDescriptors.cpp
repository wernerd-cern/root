//===- llvm/Analysis/IVDescriptors.cpp - IndVar Descriptors -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file "describes" induction and recurrence variables.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MustExecute.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/KnownBits.h"

#include <set>

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "iv-descriptors"

bool RecurrenceDescriptor::areAllUsesIn(Instruction *I,
                                        SmallPtrSetImpl<Instruction *> &Set) {
  for (User::op_iterator Use = I->op_begin(), E = I->op_end(); Use != E; ++Use)
    if (!Set.count(dyn_cast<Instruction>(*Use)))
      return false;
  return true;
}

bool RecurrenceDescriptor::isIntegerRecurrenceKind(RecurKind Kind) {
  switch (Kind) {
  default:
    break;
  case RecurKind::Add:
  case RecurKind::Mul:
  case RecurKind::Or:
  case RecurKind::And:
  case RecurKind::Xor:
  case RecurKind::SMax:
  case RecurKind::SMin:
  case RecurKind::UMax:
  case RecurKind::UMin:
    return true;
  }
  return false;
}

bool RecurrenceDescriptor::isFloatingPointRecurrenceKind(RecurKind Kind) {
  return (Kind != RecurKind::None) && !isIntegerRecurrenceKind(Kind);
}

bool RecurrenceDescriptor::isArithmeticRecurrenceKind(RecurKind Kind) {
  switch (Kind) {
  default:
    break;
  case RecurKind::Add:
  case RecurKind::Mul:
  case RecurKind::FAdd:
  case RecurKind::FMul:
    return true;
  }
  return false;
}

/// Determines if Phi may have been type-promoted. If Phi has a single user
/// that ANDs the Phi with a type mask, return the user. RT is updated to
/// account for the narrower bit width represented by the mask, and the AND
/// instruction is added to CI.
static Instruction *lookThroughAnd(PHINode *Phi, Type *&RT,
                                   SmallPtrSetImpl<Instruction *> &Visited,
                                   SmallPtrSetImpl<Instruction *> &CI) {
  if (!Phi->hasOneUse())
    return Phi;

  const APInt *M = nullptr;
  Instruction *I, *J = cast<Instruction>(Phi->use_begin()->getUser());

  // Matches either I & 2^x-1 or 2^x-1 & I. If we find a match, we update RT
  // with a new integer type of the corresponding bit width.
  if (match(J, m_c_And(m_Instruction(I), m_APInt(M)))) {
    int32_t Bits = (*M + 1).exactLogBase2();
    if (Bits > 0) {
      RT = IntegerType::get(Phi->getContext(), Bits);
      Visited.insert(Phi);
      CI.insert(J);
      return J;
    }
  }
  return Phi;
}

/// Compute the minimal bit width needed to represent a reduction whose exit
/// instruction is given by Exit.
static std::pair<Type *, bool> computeRecurrenceType(Instruction *Exit,
                                                     DemandedBits *DB,
                                                     AssumptionCache *AC,
                                                     DominatorTree *DT) {
  bool IsSigned = false;
  const DataLayout &DL = Exit->getModule()->getDataLayout();
  uint64_t MaxBitWidth = DL.getTypeSizeInBits(Exit->getType());

  if (DB) {
    // Use the demanded bits analysis to determine the bits that are live out
    // of the exit instruction, rounding up to the nearest power of two. If the
    // use of demanded bits results in a smaller bit width, we know the value
    // must be positive (i.e., IsSigned = false), because if this were not the
    // case, the sign bit would have been demanded.
    auto Mask = DB->getDemandedBits(Exit);
    MaxBitWidth = Mask.getBitWidth() - Mask.countLeadingZeros();
  }

  if (MaxBitWidth == DL.getTypeSizeInBits(Exit->getType()) && AC && DT) {
    // If demanded bits wasn't able to limit the bit width, we can try to use
    // value tracking instead. This can be the case, for example, if the value
    // may be negative.
    auto NumSignBits = ComputeNumSignBits(Exit, DL, 0, AC, nullptr, DT);
    auto NumTypeBits = DL.getTypeSizeInBits(Exit->getType());
    MaxBitWidth = NumTypeBits - NumSignBits;
    KnownBits Bits = computeKnownBits(Exit, DL);
    if (!Bits.isNonNegative()) {
      // If the value is not known to be non-negative, we set IsSigned to true,
      // meaning that we will use sext instructions instead of zext
      // instructions to restore the original type.
      IsSigned = true;
      if (!Bits.isNegative())
        // If the value is not known to be negative, we don't known what the
        // upper bit is, and therefore, we don't know what kind of extend we
        // will need. In this case, just increase the bit width by one bit and
        // use sext.
        ++MaxBitWidth;
    }
  }
  if (!isPowerOf2_64(MaxBitWidth))
    MaxBitWidth = NextPowerOf2(MaxBitWidth);

  return std::make_pair(Type::getIntNTy(Exit->getContext(), MaxBitWidth),
                        IsSigned);
}

/// Collect cast instructions that can be ignored in the vectorizer's cost
/// model, given a reduction exit value and the minimal type in which the
/// reduction can be represented.
static void collectCastsToIgnore(Loop *TheLoop, Instruction *Exit,
                                 Type *RecurrenceType,
                                 SmallPtrSetImpl<Instruction *> &Casts) {

  SmallVector<Instruction *, 8> Worklist;
  SmallPtrSet<Instruction *, 8> Visited;
  Worklist.push_back(Exit);

  while (!Worklist.empty()) {
    Instruction *Val = Worklist.pop_back_val();
    Visited.insert(Val);
    if (auto *Cast = dyn_cast<CastInst>(Val))
      if (Cast->getSrcTy() == RecurrenceType) {
        // If the source type of a cast instruction is equal to the recurrence
        // type, it will be eliminated, and should be ignored in the vectorizer
        // cost model.
        Casts.insert(Cast);
        continue;
      }

    // Add all operands to the work list if they are loop-varying values that
    // we haven't yet visited.
    for (Value *O : cast<User>(Val)->operands())
      if (auto *I = dyn_cast<Instruction>(O))
        if (TheLoop->contains(I) && !Visited.count(I))
          Worklist.push_back(I);
  }
}

// Check if a given Phi node can be recognized as an ordered reduction for
// vectorizing floating point operations without unsafe math.
static bool checkOrderedReduction(RecurKind Kind, Instruction *ExactFPMathInst,
                                  Instruction *Exit, PHINode *Phi) {
  // Currently only FAdd is supported
  if (Kind != RecurKind::FAdd)
    return false;

  if (Exit->getOpcode() != Instruction::FAdd || Exit != ExactFPMathInst)
    return false;

  // The only pattern accepted is the one in which the reduction PHI
  // is used as one of the operands of the exit instruction
  auto *LHS = Exit->getOperand(0);
  auto *RHS = Exit->getOperand(1);
  if (LHS != Phi && RHS != Phi)
    return false;

  LLVM_DEBUG(dbgs() << "LV: Found an ordered reduction: Phi: " << *Phi
                    << ", ExitInst: " << *Exit << "\n");

  return true;
}

bool RecurrenceDescriptor::AddReductionVar(PHINode *Phi, RecurKind Kind,
                                           Loop *TheLoop, FastMathFlags FuncFMF,
                                           RecurrenceDescriptor &RedDes,
                                           DemandedBits *DB,
                                           AssumptionCache *AC,
                                           DominatorTree *DT) {
  if (Phi->getNumIncomingValues() != 2)
    return false;

  // Reduction variables are only found in the loop header block.
  if (Phi->getParent() != TheLoop->getHeader())
    return false;

  // Obtain the reduction start value from the value that comes from the loop
  // preheader.
  Value *RdxStart = Phi->getIncomingValueForBlock(TheLoop->getLoopPreheader());

  // ExitInstruction is the single value which is used outside the loop.
  // We only allow for a single reduction value to be used outside the loop.
  // This includes users of the reduction, variables (which form a cycle
  // which ends in the phi node).
  Instruction *ExitInstruction = nullptr;
  // Indicates that we found a reduction operation in our scan.
  bool FoundReduxOp = false;

  // We start with the PHI node and scan for all of the users of this
  // instruction. All users must be instructions that can be used as reduction
  // variables (such as ADD). We must have a single out-of-block user. The cycle
  // must include the original PHI.
  bool FoundStartPHI = false;

  // To recognize min/max patterns formed by a icmp select sequence, we store
  // the number of instruction we saw from the recognized min/max pattern,
  //  to make sure we only see exactly the two instructions.
  unsigned NumCmpSelectPatternInst = 0;
  InstDesc ReduxDesc(false, nullptr);

  // Data used for determining if the recurrence has been type-promoted.
  Type *RecurrenceType = Phi->getType();
  SmallPtrSet<Instruction *, 4> CastInsts;
  Instruction *Start = Phi;
  bool IsSigned = false;

  SmallPtrSet<Instruction *, 8> VisitedInsts;
  SmallVector<Instruction *, 8> Worklist;

  // Return early if the recurrence kind does not match the type of Phi. If the
  // recurrence kind is arithmetic, we attempt to look through AND operations
  // resulting from the type promotion performed by InstCombine.  Vector
  // operations are not limited to the legal integer widths, so we may be able
  // to evaluate the reduction in the narrower width.
  if (RecurrenceType->isFloatingPointTy()) {
    if (!isFloatingPointRecurrenceKind(Kind))
      return false;
  } else if (RecurrenceType->isIntegerTy()) {
    if (!isIntegerRecurrenceKind(Kind))
      return false;
    if (isArithmeticRecurrenceKind(Kind))
      Start = lookThroughAnd(Phi, RecurrenceType, VisitedInsts, CastInsts);
  } else {
    // Pointer min/max may exist, but it is not supported as a reduction op.
    return false;
  }

  Worklist.push_back(Start);
  VisitedInsts.insert(Start);

  // Start with all flags set because we will intersect this with the reduction
  // flags from all the reduction operations.
  FastMathFlags FMF = FastMathFlags::getFast();

  // A value in the reduction can be used:
  //  - By the reduction:
  //      - Reduction operation:
  //        - One use of reduction value (safe).
  //        - Multiple use of reduction value (not safe).
  //      - PHI:
  //        - All uses of the PHI must be the reduction (safe).
  //        - Otherwise, not safe.
  //  - By instructions outside of the loop (safe).
  //      * One value may have several outside users, but all outside
  //        uses must be of the same value.
  //  - By an instruction that is not part of the reduction (not safe).
  //    This is either:
  //      * An instruction type other than PHI or the reduction operation.
  //      * A PHI in the header other than the initial PHI.
  while (!Worklist.empty()) {
    Instruction *Cur = Worklist.pop_back_val();

    // No Users.
    // If the instruction has no users then this is a broken chain and can't be
    // a reduction variable.
    if (Cur->use_empty())
      return false;

    bool IsAPhi = isa<PHINode>(Cur);

    // A header PHI use other than the original PHI.
    if (Cur != Phi && IsAPhi && Cur->getParent() == Phi->getParent())
      return false;

    // Reductions of instructions such as Div, and Sub is only possible if the
    // LHS is the reduction variable.
    if (!Cur->isCommutative() && !IsAPhi && !isa<SelectInst>(Cur) &&
        !isa<ICmpInst>(Cur) && !isa<FCmpInst>(Cur) &&
        !VisitedInsts.count(dyn_cast<Instruction>(Cur->getOperand(0))))
      return false;

    // Any reduction instruction must be of one of the allowed kinds. We ignore
    // the starting value (the Phi or an AND instruction if the Phi has been
    // type-promoted).
    if (Cur != Start) {
      ReduxDesc = isRecurrenceInstr(Cur, Kind, ReduxDesc, FuncFMF);
      if (!ReduxDesc.isRecurrence())
        return false;
      // FIXME: FMF is allowed on phi, but propagation is not handled correctly.
      if (isa<FPMathOperator>(ReduxDesc.getPatternInst()) && !IsAPhi) {
        FastMathFlags CurFMF = ReduxDesc.getPatternInst()->getFastMathFlags();
        if (auto *Sel = dyn_cast<SelectInst>(ReduxDesc.getPatternInst())) {
          // Accept FMF on either fcmp or select of a min/max idiom.
          // TODO: This is a hack to work-around the fact that FMF may not be
          //       assigned/propagated correctly. If that problem is fixed or we
          //       standardize on fmin/fmax via intrinsics, this can be removed.
          if (auto *FCmp = dyn_cast<FCmpInst>(Sel->getCondition()))
            CurFMF |= FCmp->getFastMathFlags();
        }
        FMF &= CurFMF;
      }
      // Update this reduction kind if we matched a new instruction.
      // TODO: Can we eliminate the need for a 2nd InstDesc by keeping 'Kind'
      //       state accurate while processing the worklist?
      if (ReduxDesc.getRecKind() != RecurKind::None)
        Kind = ReduxDesc.getRecKind();
    }

    bool IsASelect = isa<SelectInst>(Cur);

    // A conditional reduction operation must only have 2 or less uses in
    // VisitedInsts.
    if (IsASelect && (Kind == RecurKind::FAdd || Kind == RecurKind::FMul) &&
        hasMultipleUsesOf(Cur, VisitedInsts, 2))
      return false;

    // A reduction operation must only have one use of the reduction value.
    if (!IsAPhi && !IsASelect && !isMinMaxRecurrenceKind(Kind) &&
        hasMultipleUsesOf(Cur, VisitedInsts, 1))
      return false;

    // All inputs to a PHI node must be a reduction value.
    if (IsAPhi && Cur != Phi && !areAllUsesIn(Cur, VisitedInsts))
      return false;

    if (isIntMinMaxRecurrenceKind(Kind) &&
        (isa<ICmpInst>(Cur) || isa<SelectInst>(Cur)))
      ++NumCmpSelectPatternInst;
    if (isFPMinMaxRecurrenceKind(Kind) &&
        (isa<FCmpInst>(Cur) || isa<SelectInst>(Cur)))
      ++NumCmpSelectPatternInst;

    // Check  whether we found a reduction operator.
    FoundReduxOp |= !IsAPhi && Cur != Start;

    // Process users of current instruction. Push non-PHI nodes after PHI nodes
    // onto the stack. This way we are going to have seen all inputs to PHI
    // nodes once we get to them.
    SmallVector<Instruction *, 8> NonPHIs;
    SmallVector<Instruction *, 8> PHIs;
    for (User *U : Cur->users()) {
      Instruction *UI = cast<Instruction>(U);

      // Check if we found the exit user.
      BasicBlock *Parent = UI->getParent();
      if (!TheLoop->contains(Parent)) {
        // If we already know this instruction is used externally, move on to
        // the next user.
        if (ExitInstruction == Cur)
          continue;

        // Exit if you find multiple values used outside or if the header phi
        // node is being used. In this case the user uses the value of the
        // previous iteration, in which case we would loose "VF-1" iterations of
        // the reduction operation if we vectorize.
        if (ExitInstruction != nullptr || Cur == Phi)
          return false;

        // The instruction used by an outside user must be the last instruction
        // before we feed back to the reduction phi. Otherwise, we loose VF-1
        // operations on the value.
        if (!is_contained(Phi->operands(), Cur))
          return false;

        ExitInstruction = Cur;
        continue;
      }

      // Process instructions only once (termination). Each reduction cycle
      // value must only be used once, except by phi nodes and min/max
      // reductions which are represented as a cmp followed by a select.
      InstDesc IgnoredVal(false, nullptr);
      if (VisitedInsts.insert(UI).second) {
        if (isa<PHINode>(UI))
          PHIs.push_back(UI);
        else
          NonPHIs.push_back(UI);
      } else if (!isa<PHINode>(UI) &&
                 ((!isa<FCmpInst>(UI) && !isa<ICmpInst>(UI) &&
                   !isa<SelectInst>(UI)) ||
                  (!isConditionalRdxPattern(Kind, UI).isRecurrence() &&
                   !isMinMaxSelectCmpPattern(UI, IgnoredVal).isRecurrence())))
        return false;

      // Remember that we completed the cycle.
      if (UI == Phi)
        FoundStartPHI = true;
    }
    Worklist.append(PHIs.begin(), PHIs.end());
    Worklist.append(NonPHIs.begin(), NonPHIs.end());
  }

  // This means we have seen one but not the other instruction of the
  // pattern or more than just a select and cmp.
  if (isMinMaxRecurrenceKind(Kind) && NumCmpSelectPatternInst != 2)
    return false;

  if (!FoundStartPHI || !FoundReduxOp || !ExitInstruction)
    return false;

  const bool IsOrdered = checkOrderedReduction(
      Kind, ReduxDesc.getExactFPMathInst(), ExitInstruction, Phi);

  if (Start != Phi) {
    // If the starting value is not the same as the phi node, we speculatively
    // looked through an 'and' instruction when evaluating a potential
    // arithmetic reduction to determine if it may have been type-promoted.
    //
    // We now compute the minimal bit width that is required to represent the
    // reduction. If this is the same width that was indicated by the 'and', we
    // can represent the reduction in the smaller type. The 'and' instruction
    // will be eliminated since it will essentially be a cast instruction that
    // can be ignore in the cost model. If we compute a different type than we
    // did when evaluating the 'and', the 'and' will not be eliminated, and we
    // will end up with different kinds of operations in the recurrence
    // expression (e.g., IntegerAND, IntegerADD). We give up if this is
    // the case.
    //
    // The vectorizer relies on InstCombine to perform the actual
    // type-shrinking. It does this by inserting instructions to truncate the
    // exit value of the reduction to the width indicated by RecurrenceType and
    // then extend this value back to the original width. If IsSigned is false,
    // a 'zext' instruction will be generated; otherwise, a 'sext' will be
    // used.
    //
    // TODO: We should not rely on InstCombine to rewrite the reduction in the
    //       smaller type. We should just generate a correctly typed expression
    //       to begin with.
    Type *ComputedType;
    std::tie(ComputedType, IsSigned) =
        computeRecurrenceType(ExitInstruction, DB, AC, DT);
    if (ComputedType != RecurrenceType)
      return false;

    // The recurrence expression will be represented in a narrower type. If
    // there are any cast instructions that will be unnecessary, collect them
    // in CastInsts. Note that the 'and' instruction was already included in
    // this list.
    //
    // TODO: A better way to represent this may be to tag in some way all the
    //       instructions that are a part of the reduction. The vectorizer cost
    //       model could then apply the recurrence type to these instructions,
    //       without needing a white list of instructions to ignore.
    //       This may also be useful for the inloop reductions, if it can be
    //       kept simple enough.
    collectCastsToIgnore(TheLoop, ExitInstruction, RecurrenceType, CastInsts);
  }

  // We found a reduction var if we have reached the original phi node and we
  // only have a single instruction with out-of-loop users.

  // The ExitInstruction(Instruction which is allowed to have out-of-loop users)
  // is saved as part of the RecurrenceDescriptor.

  // Save the description of this reduction variable.
  RecurrenceDescriptor RD(RdxStart, ExitInstruction, Kind, FMF,
                          ReduxDesc.getExactFPMathInst(), RecurrenceType,
                          IsSigned, IsOrdered, CastInsts);
  RedDes = RD;

  return true;
}

RecurrenceDescriptor::InstDesc
RecurrenceDescriptor::isMinMaxSelectCmpPattern(Instruction *I,
                                               const InstDesc &Prev) {
  assert((isa<CmpInst>(I) || isa<SelectInst>(I)) &&
         "Expected a cmp or select instruction");

  // We must handle the select(cmp()) as a single instruction. Advance to the
  // select.
  CmpInst::Predicate Pred;
  if (match(I, m_OneUse(m_Cmp(Pred, m_Value(), m_Value())))) {
    if (auto *Select = dyn_cast<SelectInst>(*I->user_begin()))
      return InstDesc(Select, Prev.getRecKind());
  }

  // Only match select with single use cmp condition.
  if (!match(I, m_Select(m_OneUse(m_Cmp(Pred, m_Value(), m_Value())), m_Value(),
                         m_Value())))
    return InstDesc(false, I);

  // Look for a min/max pattern.
  if (match(I, m_UMin(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::UMin);
  if (match(I, m_UMax(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::UMax);
  if (match(I, m_SMax(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::SMax);
  if (match(I, m_SMin(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::SMin);
  if (match(I, m_OrdFMin(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::FMin);
  if (match(I, m_OrdFMax(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::FMax);
  if (match(I, m_UnordFMin(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::FMin);
  if (match(I, m_UnordFMax(m_Value(), m_Value())))
    return InstDesc(I, RecurKind::FMax);

  return InstDesc(false, I);
}

/// Returns true if the select instruction has users in the compare-and-add
/// reduction pattern below. The select instruction argument is the last one
/// in the sequence.
///
/// %sum.1 = phi ...
/// ...
/// %cmp = fcmp pred %0, %CFP
/// %add = fadd %0, %sum.1
/// %sum.2 = select %cmp, %add, %sum.1
RecurrenceDescriptor::InstDesc
RecurrenceDescriptor::isConditionalRdxPattern(RecurKind Kind, Instruction *I) {
  SelectInst *SI = dyn_cast<SelectInst>(I);
  if (!SI)
    return InstDesc(false, I);

  CmpInst *CI = dyn_cast<CmpInst>(SI->getCondition());
  // Only handle single use cases for now.
  if (!CI || !CI->hasOneUse())
    return InstDesc(false, I);

  Value *TrueVal = SI->getTrueValue();
  Value *FalseVal = SI->getFalseValue();
  // Handle only when either of operands of select instruction is a PHI
  // node for now.
  if ((isa<PHINode>(*TrueVal) && isa<PHINode>(*FalseVal)) ||
      (!isa<PHINode>(*TrueVal) && !isa<PHINode>(*FalseVal)))
    return InstDesc(false, I);

  Instruction *I1 =
      isa<PHINode>(*TrueVal) ? dyn_cast<Instruction>(FalseVal)
                             : dyn_cast<Instruction>(TrueVal);
  if (!I1 || !I1->isBinaryOp())
    return InstDesc(false, I);

  Value *Op1, *Op2;
  if ((m_FAdd(m_Value(Op1), m_Value(Op2)).match(I1)  ||
       m_FSub(m_Value(Op1), m_Value(Op2)).match(I1)) &&
      I1->isFast())
    return InstDesc(Kind == RecurKind::FAdd, SI);

  if (m_FMul(m_Value(Op1), m_Value(Op2)).match(I1) && (I1->isFast()))
    return InstDesc(Kind == RecurKind::FMul, SI);

  return InstDesc(false, I);
}

RecurrenceDescriptor::InstDesc
RecurrenceDescriptor::isRecurrenceInstr(Instruction *I, RecurKind Kind,
                                        InstDesc &Prev, FastMathFlags FMF) {
  switch (I->getOpcode()) {
  default:
    return InstDesc(false, I);
  case Instruction::PHI:
    return InstDesc(I, Prev.getRecKind(), Prev.getExactFPMathInst());
  case Instruction::Sub:
  case Instruction::Add:
    return InstDesc(Kind == RecurKind::Add, I);
  case Instruction::Mul:
    return InstDesc(Kind == RecurKind::Mul, I);
  case Instruction::And:
    return InstDesc(Kind == RecurKind::And, I);
  case Instruction::Or:
    return InstDesc(Kind == RecurKind::Or, I);
  case Instruction::Xor:
    return InstDesc(Kind == RecurKind::Xor, I);
  case Instruction::FDiv:
  case Instruction::FMul:
    return InstDesc(Kind == RecurKind::FMul, I,
                    I->hasAllowReassoc() ? nullptr : I);
  case Instruction::FSub:
  case Instruction::FAdd:
    return InstDesc(Kind == RecurKind::FAdd, I,
                    I->hasAllowReassoc() ? nullptr : I);
  case Instruction::Select:
    if (Kind == RecurKind::FAdd || Kind == RecurKind::FMul)
      return isConditionalRdxPattern(Kind, I);
    LLVM_FALLTHROUGH;
  case Instruction::FCmp:
  case Instruction::ICmp:
    if (isIntMinMaxRecurrenceKind(Kind) ||
        (FMF.noNaNs() && FMF.noSignedZeros() && isFPMinMaxRecurrenceKind(Kind)))
      return isMinMaxSelectCmpPattern(I, Prev);
    return InstDesc(false, I);
  }
}

bool RecurrenceDescriptor::hasMultipleUsesOf(
    Instruction *I, SmallPtrSetImpl<Instruction *> &Insts,
    unsigned MaxNumUses) {
  unsigned NumUses = 0;
  for (const Use &U : I->operands()) {
    if (Insts.count(dyn_cast<Instruction>(U)))
      ++NumUses;
    if (NumUses > MaxNumUses)
      return true;
  }

  return false;
}

bool RecurrenceDescriptor::isReductionPHI(PHINode *Phi, Loop *TheLoop,
                                          RecurrenceDescriptor &RedDes,
                                          DemandedBits *DB, AssumptionCache *AC,
                                          DominatorTree *DT) {

  BasicBlock *Header = TheLoop->getHeader();
  Function &F = *Header->getParent();
  FastMathFlags FMF;
  FMF.setNoNaNs(
      F.getFnAttribute("no-nans-fp-math").getValueAsBool());
  FMF.setNoSignedZeros(
      F.getFnAttribute("no-signed-zeros-fp-math").getValueAsBool());

  if (AddReductionVar(Phi, RecurKind::Add, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found an ADD reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::Mul, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a MUL reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::Or, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found an OR reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::And, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found an AND reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::Xor, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a XOR reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::SMax, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a SMAX reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::SMin, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a SMIN reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::UMax, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a UMAX reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::UMin, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a UMIN reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::FMul, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found an FMult reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::FAdd, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found an FAdd reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::FMax, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a float MAX reduction PHI." << *Phi << "\n");
    return true;
  }
  if (AddReductionVar(Phi, RecurKind::FMin, TheLoop, FMF, RedDes, DB, AC, DT)) {
    LLVM_DEBUG(dbgs() << "Found a float MIN reduction PHI." << *Phi << "\n");
    return true;
  }
  // Not a reduction of known type.
  return false;
}

bool RecurrenceDescriptor::isFirstOrderRecurrence(
    PHINode *Phi, Loop *TheLoop,
    MapVector<Instruction *, Instruction *> &SinkAfter, DominatorTree *DT) {

  // Ensure the phi node is in the loop header and has two incoming values.
  if (Phi->getParent() != TheLoop->getHeader() ||
      Phi->getNumIncomingValues() != 2)
    return false;

  // Ensure the loop has a preheader and a single latch block. The loop
  // vectorizer will need the latch to set up the next iteration of the loop.
  auto *Preheader = TheLoop->getLoopPreheader();
  auto *Latch = TheLoop->getLoopLatch();
  if (!Preheader || !Latch)
    return false;

  // Ensure the phi node's incoming blocks are the loop preheader and latch.
  if (Phi->getBasicBlockIndex(Preheader) < 0 ||
      Phi->getBasicBlockIndex(Latch) < 0)
    return false;

  // Get the previous value. The previous value comes from the latch edge while
  // the initial value comes form the preheader edge.
  auto *Previous = dyn_cast<Instruction>(Phi->getIncomingValueForBlock(Latch));
  if (!Previous || !TheLoop->contains(Previous) || isa<PHINode>(Previous) ||
      SinkAfter.count(Previous)) // Cannot rely on dominance due to motion.
    return false;

  // Ensure every user of the phi node (recursively) is dominated by the
  // previous value. The dominance requirement ensures the loop vectorizer will
  // not need to vectorize the initial value prior to the first iteration of the
  // loop.
  // TODO: Consider extending this sinking to handle memory instructions.

  // We optimistically assume we can sink all users after Previous. Keep a set
  // of instructions to sink after Previous ordered by dominance in the common
  // basic block. It will be applied to SinkAfter if all users can be sunk.
  auto CompareByComesBefore = [](const Instruction *A, const Instruction *B) {
    return A->comesBefore(B);
  };
  std::set<Instruction *, decltype(CompareByComesBefore)> InstrsToSink(
      CompareByComesBefore);

  BasicBlock *PhiBB = Phi->getParent();
  SmallVector<Instruction *, 8> WorkList;
  auto TryToPushSinkCandidate = [&](Instruction *SinkCandidate) {
    // Already sunk SinkCandidate.
    if (SinkCandidate->getParent() == PhiBB &&
        InstrsToSink.find(SinkCandidate) != InstrsToSink.end())
      return true;

    // Cyclic dependence.
    if (Previous == SinkCandidate)
      return false;

    if (DT->dominates(Previous,
                      SinkCandidate)) // We already are good w/o sinking.
      return true;

    if (SinkCandidate->getParent() != PhiBB ||
        SinkCandidate->mayHaveSideEffects() ||
        SinkCandidate->mayReadFromMemory() || SinkCandidate->isTerminator())
      return false;

    // Do not try to sink an instruction multiple times (if multiple operands
    // are first order recurrences).
    // TODO: We can support this case, by sinking the instruction after the
    // 'deepest' previous instruction.
    if (SinkAfter.find(SinkCandidate) != SinkAfter.end())
      return false;

    // If we reach a PHI node that is not dominated by Previous, we reached a
    // header PHI. No need for sinking.
    if (isa<PHINode>(SinkCandidate))
      return true;

    // Sink User tentatively and check its users
    InstrsToSink.insert(SinkCandidate);
    WorkList.push_back(SinkCandidate);
    return true;
  };

  WorkList.push_back(Phi);
  // Try to recursively sink instructions and their users after Previous.
  while (!WorkList.empty()) {
    Instruction *Current = WorkList.pop_back_val();
    for (User *User : Current->users()) {
      if (!TryToPushSinkCandidate(cast<Instruction>(User)))
        return false;
    }
  }

  // We can sink all users of Phi. Update the mapping.
  for (Instruction *I : InstrsToSink) {
    SinkAfter[I] = Previous;
    Previous = I;
  }
  return true;
}

/// This function returns the identity element (or neutral element) for
/// the operation K.
Constant *RecurrenceDescriptor::getRecurrenceIdentity(RecurKind K, Type *Tp,
                                                      FastMathFlags FMF) {
  switch (K) {
  case RecurKind::Xor:
  case RecurKind::Add:
  case RecurKind::Or:
    // Adding, Xoring, Oring zero to a number does not change it.
    return ConstantInt::get(Tp, 0);
  case RecurKind::Mul:
    // Multiplying a number by 1 does not change it.
    return ConstantInt::get(Tp, 1);
  case RecurKind::And:
    // AND-ing a number with an all-1 value does not change it.
    return ConstantInt::get(Tp, -1, true);
  case RecurKind::FMul:
    // Multiplying a number by 1 does not change it.
    return ConstantFP::get(Tp, 1.0L);
  case RecurKind::FAdd:
    // Adding zero to a number does not change it.
    // FIXME: Ideally we should not need to check FMF for FAdd and should always
    // use -0.0. However, this will currently result in mixed vectors of 0.0/-0.0.
    // Instead, we should ensure that 1) the FMF from FAdd are propagated to the PHI
    // nodes where possible, and 2) PHIs with the nsz flag + -0.0 use 0.0. This would
    // mean we can then remove the check for noSignedZeros() below (see D98963).
    if (FMF.noSignedZeros())
      return ConstantFP::get(Tp, 0.0L);
    return ConstantFP::get(Tp, -0.0L);
  case RecurKind::UMin:
    return ConstantInt::get(Tp, -1);
  case RecurKind::UMax:
    return ConstantInt::get(Tp, 0);
  case RecurKind::SMin:
    return ConstantInt::get(Tp,
                            APInt::getSignedMaxValue(Tp->getIntegerBitWidth()));
  case RecurKind::SMax:
    return ConstantInt::get(Tp,
                            APInt::getSignedMinValue(Tp->getIntegerBitWidth()));
  case RecurKind::FMin:
    return ConstantFP::getInfinity(Tp, true);
  case RecurKind::FMax:
    return ConstantFP::getInfinity(Tp, false);
  default:
    llvm_unreachable("Unknown recurrence kind");
  }
}

unsigned RecurrenceDescriptor::getOpcode(RecurKind Kind) {
  switch (Kind) {
  case RecurKind::Add:
    return Instruction::Add;
  case RecurKind::Mul:
    return Instruction::Mul;
  case RecurKind::Or:
    return Instruction::Or;
  case RecurKind::And:
    return Instruction::And;
  case RecurKind::Xor:
    return Instruction::Xor;
  case RecurKind::FMul:
    return Instruction::FMul;
  case RecurKind::FAdd:
    return Instruction::FAdd;
  case RecurKind::SMax:
  case RecurKind::SMin:
  case RecurKind::UMax:
  case RecurKind::UMin:
    return Instruction::ICmp;
  case RecurKind::FMax:
  case RecurKind::FMin:
    return Instruction::FCmp;
  default:
    llvm_unreachable("Unknown recurrence operation");
  }
}

SmallVector<Instruction *, 4>
RecurrenceDescriptor::getReductionOpChain(PHINode *Phi, Loop *L) const {
  SmallVector<Instruction *, 4> ReductionOperations;
  unsigned RedOp = getOpcode(Kind);

  // Search down from the Phi to the LoopExitInstr, looking for instructions
  // with a single user of the correct type for the reduction.

  // Note that we check that the type of the operand is correct for each item in
  // the chain, including the last (the loop exit value). This can come up from
  // sub, which would otherwise be treated as an add reduction. MinMax also need
  // to check for a pair of icmp/select, for which we use getNextInstruction and
  // isCorrectOpcode functions to step the right number of instruction, and
  // check the icmp/select pair.
  // FIXME: We also do not attempt to look through Phi/Select's yet, which might
  // be part of the reduction chain, or attempt to looks through And's to find a
  // smaller bitwidth. Subs are also currently not allowed (which are usually
  // treated as part of a add reduction) as they are expected to generally be
  // more expensive than out-of-loop reductions, and need to be costed more
  // carefully.
  unsigned ExpectedUses = 1;
  if (RedOp == Instruction::ICmp || RedOp == Instruction::FCmp)
    ExpectedUses = 2;

  auto getNextInstruction = [&](Instruction *Cur) {
    if (RedOp == Instruction::ICmp || RedOp == Instruction::FCmp) {
      // We are expecting a icmp/select pair, which we go to the next select
      // instruction if we can. We already know that Cur has 2 uses.
      if (isa<SelectInst>(*Cur->user_begin()))
        return cast<Instruction>(*Cur->user_begin());
      else
        return cast<Instruction>(*std::next(Cur->user_begin()));
    }
    return cast<Instruction>(*Cur->user_begin());
  };
  auto isCorrectOpcode = [&](Instruction *Cur) {
    if (RedOp == Instruction::ICmp || RedOp == Instruction::FCmp) {
      Value *LHS, *RHS;
      return SelectPatternResult::isMinOrMax(
          matchSelectPattern(Cur, LHS, RHS).Flavor);
    }
    return Cur->getOpcode() == RedOp;
  };

  // The loop exit instruction we check first (as a quick test) but add last. We
  // check the opcode is correct (and dont allow them to be Subs) and that they
  // have expected to have the expected number of uses. They will have one use
  // from the phi and one from a LCSSA value, no matter the type.
  if (!isCorrectOpcode(LoopExitInstr) || !LoopExitInstr->hasNUses(2))
    return {};

  // Check that the Phi has one (or two for min/max) uses.
  if (!Phi->hasNUses(ExpectedUses))
    return {};
  Instruction *Cur = getNextInstruction(Phi);

  // Each other instruction in the chain should have the expected number of uses
  // and be the correct opcode.
  while (Cur != LoopExitInstr) {
    if (!isCorrectOpcode(Cur) || !Cur->hasNUses(ExpectedUses))
      return {};

    ReductionOperations.push_back(Cur);
    Cur = getNextInstruction(Cur);
  }

  ReductionOperations.push_back(Cur);
  return ReductionOperations;
}

InductionDescriptor::InductionDescriptor(Value *Start, InductionKind K,
                                         const SCEV *Step, BinaryOperator *BOp,
                                         SmallVectorImpl<Instruction *> *Casts)
    : StartValue(Start), IK(K), Step(Step), InductionBinOp(BOp) {
  assert(IK != IK_NoInduction && "Not an induction");

  // Start value type should match the induction kind and the value
  // itself should not be null.
  assert(StartValue && "StartValue is null");
  assert((IK != IK_PtrInduction || StartValue->getType()->isPointerTy()) &&
         "StartValue is not a pointer for pointer induction");
  assert((IK != IK_IntInduction || StartValue->getType()->isIntegerTy()) &&
         "StartValue is not an integer for integer induction");

  // Check the Step Value. It should be non-zero integer value.
  assert((!getConstIntStepValue() || !getConstIntStepValue()->isZero()) &&
         "Step value is zero");

  assert((IK != IK_PtrInduction || getConstIntStepValue()) &&
         "Step value should be constant for pointer induction");
  assert((IK == IK_FpInduction || Step->getType()->isIntegerTy()) &&
         "StepValue is not an integer");

  assert((IK != IK_FpInduction || Step->getType()->isFloatingPointTy()) &&
         "StepValue is not FP for FpInduction");
  assert((IK != IK_FpInduction ||
          (InductionBinOp &&
           (InductionBinOp->getOpcode() == Instruction::FAdd ||
            InductionBinOp->getOpcode() == Instruction::FSub))) &&
         "Binary opcode should be specified for FP induction");

  if (Casts) {
    for (auto &Inst : *Casts) {
      RedundantCasts.push_back(Inst);
    }
  }
}

ConstantInt *InductionDescriptor::getConstIntStepValue() const {
  if (isa<SCEVConstant>(Step))
    return dyn_cast<ConstantInt>(cast<SCEVConstant>(Step)->getValue());
  return nullptr;
}

bool InductionDescriptor::isFPInductionPHI(PHINode *Phi, const Loop *TheLoop,
                                           ScalarEvolution *SE,
                                           InductionDescriptor &D) {

  // Here we only handle FP induction variables.
  assert(Phi->getType()->isFloatingPointTy() && "Unexpected Phi type");

  if (TheLoop->getHeader() != Phi->getParent())
    return false;

  // The loop may have multiple entrances or multiple exits; we can analyze
  // this phi if it has a unique entry value and a unique backedge value.
  if (Phi->getNumIncomingValues() != 2)
    return false;
  Value *BEValue = nullptr, *StartValue = nullptr;
  if (TheLoop->contains(Phi->getIncomingBlock(0))) {
    BEValue = Phi->getIncomingValue(0);
    StartValue = Phi->getIncomingValue(1);
  } else {
    assert(TheLoop->contains(Phi->getIncomingBlock(1)) &&
           "Unexpected Phi node in the loop");
    BEValue = Phi->getIncomingValue(1);
    StartValue = Phi->getIncomingValue(0);
  }

  BinaryOperator *BOp = dyn_cast<BinaryOperator>(BEValue);
  if (!BOp)
    return false;

  Value *Addend = nullptr;
  if (BOp->getOpcode() == Instruction::FAdd) {
    if (BOp->getOperand(0) == Phi)
      Addend = BOp->getOperand(1);
    else if (BOp->getOperand(1) == Phi)
      Addend = BOp->getOperand(0);
  } else if (BOp->getOpcode() == Instruction::FSub)
    if (BOp->getOperand(0) == Phi)
      Addend = BOp->getOperand(1);

  if (!Addend)
    return false;

  // The addend should be loop invariant
  if (auto *I = dyn_cast<Instruction>(Addend))
    if (TheLoop->contains(I))
      return false;

  // FP Step has unknown SCEV
  const SCEV *Step = SE->getUnknown(Addend);
  D = InductionDescriptor(StartValue, IK_FpInduction, Step, BOp);
  return true;
}

/// This function is called when we suspect that the update-chain of a phi node
/// (whose symbolic SCEV expression sin \p PhiScev) contains redundant casts,
/// that can be ignored. (This can happen when the PSCEV rewriter adds a runtime
/// predicate P under which the SCEV expression for the phi can be the
/// AddRecurrence \p AR; See createAddRecFromPHIWithCast). We want to find the
/// cast instructions that are involved in the update-chain of this induction.
/// A caller that adds the required runtime predicate can be free to drop these
/// cast instructions, and compute the phi using \p AR (instead of some scev
/// expression with casts).
///
/// For example, without a predicate the scev expression can take the following
/// form:
///      (Ext ix (Trunc iy ( Start + i*Step ) to ix) to iy)
///
/// It corresponds to the following IR sequence:
/// %for.body:
///   %x = phi i64 [ 0, %ph ], [ %add, %for.body ]
///   %casted_phi = "ExtTrunc i64 %x"
///   %add = add i64 %casted_phi, %step
///
/// where %x is given in \p PN,
/// PSE.getSCEV(%x) is equal to PSE.getSCEV(%casted_phi) under a predicate,
/// and the IR sequence that "ExtTrunc i64 %x" represents can take one of
/// several forms, for example, such as:
///   ExtTrunc1:    %casted_phi = and  %x, 2^n-1
/// or:
///   ExtTrunc2:    %t = shl %x, m
///                 %casted_phi = ashr %t, m
///
/// If we are able to find such sequence, we return the instructions
/// we found, namely %casted_phi and the instructions on its use-def chain up
/// to the phi (not including the phi).
static bool getCastsForInductionPHI(PredicatedScalarEvolution &PSE,
                                    const SCEVUnknown *PhiScev,
                                    const SCEVAddRecExpr *AR,
                                    SmallVectorImpl<Instruction *> &CastInsts) {

  assert(CastInsts.empty() && "CastInsts is expected to be empty.");
  auto *PN = cast<PHINode>(PhiScev->getValue());
  assert(PSE.getSCEV(PN) == AR && "Unexpected phi node SCEV expression");
  const Loop *L = AR->getLoop();

  // Find any cast instructions that participate in the def-use chain of
  // PhiScev in the loop.
  // FORNOW/TODO: We currently expect the def-use chain to include only
  // two-operand instructions, where one of the operands is an invariant.
  // createAddRecFromPHIWithCasts() currently does not support anything more
  // involved than that, so we keep the search simple. This can be
  // extended/generalized as needed.

  auto getDef = [&](const Value *Val) -> Value * {
    const BinaryOperator *BinOp = dyn_cast<BinaryOperator>(Val);
    if (!BinOp)
      return nullptr;
    Value *Op0 = BinOp->getOperand(0);
    Value *Op1 = BinOp->getOperand(1);
    Value *Def = nullptr;
    if (L->isLoopInvariant(Op0))
      Def = Op1;
    else if (L->isLoopInvariant(Op1))
      Def = Op0;
    return Def;
  };

  // Look for the instruction that defines the induction via the
  // loop backedge.
  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return false;
  Value *Val = PN->getIncomingValueForBlock(Latch);
  if (!Val)
    return false;

  // Follow the def-use chain until the induction phi is reached.
  // If on the way we encounter a Value that has the same SCEV Expr as the
  // phi node, we can consider the instructions we visit from that point
  // as part of the cast-sequence that can be ignored.
  bool InCastSequence = false;
  auto *Inst = dyn_cast<Instruction>(Val);
  while (Val != PN) {
    // If we encountered a phi node other than PN, or if we left the loop,
    // we bail out.
    if (!Inst || !L->contains(Inst)) {
      return false;
    }
    auto *AddRec = dyn_cast<SCEVAddRecExpr>(PSE.getSCEV(Val));
    if (AddRec && PSE.areAddRecsEqualWithPreds(AddRec, AR))
      InCastSequence = true;
    if (InCastSequence) {
      // Only the last instruction in the cast sequence is expected to have
      // uses outside the induction def-use chain.
      if (!CastInsts.empty())
        if (!Inst->hasOneUse())
          return false;
      CastInsts.push_back(Inst);
    }
    Val = getDef(Val);
    if (!Val)
      return false;
    Inst = dyn_cast<Instruction>(Val);
  }

  return InCastSequence;
}

bool InductionDescriptor::isInductionPHI(PHINode *Phi, const Loop *TheLoop,
                                         PredicatedScalarEvolution &PSE,
                                         InductionDescriptor &D, bool Assume) {
  Type *PhiTy = Phi->getType();

  // Handle integer and pointer inductions variables.
  // Now we handle also FP induction but not trying to make a
  // recurrent expression from the PHI node in-place.

  if (!PhiTy->isIntegerTy() && !PhiTy->isPointerTy() && !PhiTy->isFloatTy() &&
      !PhiTy->isDoubleTy() && !PhiTy->isHalfTy())
    return false;

  if (PhiTy->isFloatingPointTy())
    return isFPInductionPHI(Phi, TheLoop, PSE.getSE(), D);

  const SCEV *PhiScev = PSE.getSCEV(Phi);
  const auto *AR = dyn_cast<SCEVAddRecExpr>(PhiScev);

  // We need this expression to be an AddRecExpr.
  if (Assume && !AR)
    AR = PSE.getAsAddRec(Phi);

  if (!AR) {
    LLVM_DEBUG(dbgs() << "LV: PHI is not a poly recurrence.\n");
    return false;
  }

  // Record any Cast instructions that participate in the induction update
  const auto *SymbolicPhi = dyn_cast<SCEVUnknown>(PhiScev);
  // If we started from an UnknownSCEV, and managed to build an addRecurrence
  // only after enabling Assume with PSCEV, this means we may have encountered
  // cast instructions that required adding a runtime check in order to
  // guarantee the correctness of the AddRecurrence respresentation of the
  // induction.
  if (PhiScev != AR && SymbolicPhi) {
    SmallVector<Instruction *, 2> Casts;
    if (getCastsForInductionPHI(PSE, SymbolicPhi, AR, Casts))
      return isInductionPHI(Phi, TheLoop, PSE.getSE(), D, AR, &Casts);
  }

  return isInductionPHI(Phi, TheLoop, PSE.getSE(), D, AR);
}

bool InductionDescriptor::isInductionPHI(
    PHINode *Phi, const Loop *TheLoop, ScalarEvolution *SE,
    InductionDescriptor &D, const SCEV *Expr,
    SmallVectorImpl<Instruction *> *CastsToIgnore) {
  Type *PhiTy = Phi->getType();
  // We only handle integer and pointer inductions variables.
  if (!PhiTy->isIntegerTy() && !PhiTy->isPointerTy())
    return false;

  // Check that the PHI is consecutive.
  const SCEV *PhiScev = Expr ? Expr : SE->getSCEV(Phi);
  const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PhiScev);

  if (!AR) {
    LLVM_DEBUG(dbgs() << "LV: PHI is not a poly recurrence.\n");
    return false;
  }

  if (AR->getLoop() != TheLoop) {
    // FIXME: We should treat this as a uniform. Unfortunately, we
    // don't currently know how to handled uniform PHIs.
    LLVM_DEBUG(
        dbgs() << "LV: PHI is a recurrence with respect to an outer loop.\n");
    return false;
  }

  Value *StartValue =
      Phi->getIncomingValueForBlock(AR->getLoop()->getLoopPreheader());

  BasicBlock *Latch = AR->getLoop()->getLoopLatch();
  if (!Latch)
    return false;
  BinaryOperator *BOp =
      dyn_cast<BinaryOperator>(Phi->getIncomingValueForBlock(Latch));

  const SCEV *Step = AR->getStepRecurrence(*SE);
  // Calculate the pointer stride and check if it is consecutive.
  // The stride may be a constant or a loop invariant integer value.
  const SCEVConstant *ConstStep = dyn_cast<SCEVConstant>(Step);
  if (!ConstStep && !SE->isLoopInvariant(Step, TheLoop))
    return false;

  if (PhiTy->isIntegerTy()) {
    D = InductionDescriptor(StartValue, IK_IntInduction, Step, BOp,
                            CastsToIgnore);
    return true;
  }

  assert(PhiTy->isPointerTy() && "The PHI must be a pointer");
  // Pointer induction should be a constant.
  if (!ConstStep)
    return false;

  ConstantInt *CV = ConstStep->getValue();
  Type *PointerElementType = PhiTy->getPointerElementType();
  // The pointer stride cannot be determined if the pointer element type is not
  // sized.
  if (!PointerElementType->isSized())
    return false;

  const DataLayout &DL = Phi->getModule()->getDataLayout();
  int64_t Size = static_cast<int64_t>(DL.getTypeAllocSize(PointerElementType));
  if (!Size)
    return false;

  int64_t CVSize = CV->getSExtValue();
  if (CVSize % Size)
    return false;
  auto *StepValue =
      SE->getConstant(CV->getType(), CVSize / Size, true /* signed */);
  D = InductionDescriptor(StartValue, IK_PtrInduction, StepValue, BOp);
  return true;
}
