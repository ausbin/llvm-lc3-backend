//===- MLRegAllocEvictAdvisor.cpp - ML eviction advisor -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Function declarations of utilities related to feature extraction for unit
// testing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_MLREGALLOCEVICTIONADVISOR_H
#define LLVM_CODEGEN_MLREGALLOCEVICTIONADVISOR_H

#include "llvm/Analysis/MLModelRunner.h"
#include "llvm/CodeGen/SlotIndexes.h"

using namespace llvm;

// LRStartEndInfo contains the start and end of a specific live range as
// slot indices as well as storing the index of the physical register it
// is assigned to (or 1 above the phys reg count if its the candidate).
// Used when extracting per-instruction features in the context of a
// specific eviction problem.
struct LRStartEndInfo {
  SlotIndex Begin;
  SlotIndex End;
  size_t Pos = 0;
};

void extractInstructionFeatures(
    llvm::SmallVectorImpl<LRStartEndInfo> &LRPosInfo,
    MLModelRunner *RegallocRunner, function_ref<int(SlotIndex)> GetOpcode,
    const int InstructionsIndex, const int InstructionsMappingIndex,
    const SlotIndex LastIndex);

// This is the maximum number of interfererring ranges. That's the number of
// distinct AllocationOrder values, which comes from MCRegisterClass::RegsSize.
// For X86, that's 32.
// TODO: find a way to get this, statically, in a programmatic way.
static const int64_t MaxInterferences = 32;

// Logically, we can think of the feature set given to the evaluator as a 2D
// matrix. The rows are the features (see next). The columns correspond to the
// interferences. We treat the candidate virt reg as an 'interference', too, as
// its feature set is the same as that of the interferring ranges. So we'll have
// MaxInterferences + 1 columns and by convention, we will use the last column
// for the virt reg seeking allocation.
static const int64_t CandidateVirtRegPos = MaxInterferences;
static const int64_t NumberOfInterferences = CandidateVirtRegPos + 1;

// The number of instructions that a specific live range might have is variable,
// but we're passing in a single matrix of instructions and tensorflow saved
// models only support a fixed input size, so we have to cap the number of
// instructions that can be passed along. The specific value was derived from
// experimentation such that the majority of eviction problems would be
// completely covered.
static const int ModelMaxSupportedInstructionCount = 300;

// When extracting per-instruction features, the advisor will currently create
// a vector of size ModelMaxSupportedInstructionCount to hold the opcodes of the
// instructions relevant to the eviction problem, and a NumberOfInterferences *
// ModelMaxSupportedInstructionCount matrix that maps LRs to the instructions
// that they span.
static const std::vector<int64_t> InstructionsShape{
    1, ModelMaxSupportedInstructionCount};
static const std::vector<int64_t> InstructionsMappingShape{
    1, NumberOfInterferences, ModelMaxSupportedInstructionCount};

#endif // LLVM_CODEGEN_MLREGALLOCEVICTIONADVISOR_H
