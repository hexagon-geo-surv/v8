// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/flush-instruction-cache.h"
#include "src/codegen/macro-assembler.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/isolate-data.h"

namespace v8 {
namespace internal {

// The deopt exit sizes below depend on the following IsolateData layout
// guarantees:
#define ASSERT_OFFSET(BuiltinName)                                       \
  static_assert(IsolateData::builtin_tier0_entry_table_offset() +        \
                    Builtins::ToInt(BuiltinName) * kSystemPointerSize <= \
                0x1000)
ASSERT_OFFSET(Builtin::kDeoptimizationEntry_Eager);
ASSERT_OFFSET(Builtin::kDeoptimizationEntry_Lazy);
#undef ASSERT_OFFSET

const int Deoptimizer::kEagerDeoptExitSize = 2 * kInstrSize;
const int Deoptimizer::kLazyDeoptExitSize = 2 * kInstrSize;

const int Deoptimizer::kAdaptShadowStackOffsetToSubtract = 0;

// static
void Deoptimizer::ZapCode(Address start, Address end, RelocIterator& it) {
  // TODO(422364570): Support this platform.
}

// static
void Deoptimizer::PatchToJump(Address pc, Address new_pc) {
  int offset = new_pc - (pc + Instruction::kPcLoadDelta);
  // We'll overwrite only one instruction of 4-bytes. Give enough
  // space not to try to grow the buffer.
  constexpr int kSize = 64;

  Assembler masm(
      AssemblerOptions{},
      ExternalAssemblerBuffer(reinterpret_cast<uint8_t*>(pc), kSize));
  masm.b(offset);
  FlushInstructionCache(pc, kSize);
}

Float32 RegisterValues::GetFloatRegister(unsigned n) const {
  const Address start = reinterpret_cast<Address>(simd128_registers_);
  const size_t offset = n * sizeof(Float32);
  return base::ReadUnalignedValue<Float32>(start + offset);
}

Float64 RegisterValues::GetDoubleRegister(unsigned n) const {
  const Address start = reinterpret_cast<Address>(simd128_registers_);
  const size_t offset = n * sizeof(Float64);
  return base::ReadUnalignedValue<Float64>(start + offset);
}

void RegisterValues::SetDoubleRegister(unsigned n, Float64 value) {
  V8_ASSUME(n < 2 * arraysize(simd128_registers_));
  const Address start = reinterpret_cast<Address>(simd128_registers_);
  const size_t offset = n * sizeof(Float64);
  base::WriteUnalignedValue(start + offset, value);
}

void FrameDescription::SetCallerPc(unsigned offset, intptr_t value) {
  SetFrameSlot(offset, value);
}

void FrameDescription::SetCallerFp(unsigned offset, intptr_t value) {
  SetFrameSlot(offset, value);
}

void FrameDescription::SetCallerConstantPool(unsigned offset, intptr_t value) {
  // No embedded constant pool support.
  UNREACHABLE();
}

void FrameDescription::SetPc(intptr_t pc) { pc_ = pc; }

}  // namespace internal
}  // namespace v8
