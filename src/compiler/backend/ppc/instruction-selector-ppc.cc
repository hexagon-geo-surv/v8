// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <optional>

#include "src/base/iterator.h"
#include "src/compiler/backend/instruction-selector-impl.h"
#include "src/compiler/turboshaft/opmasks.h"
#include "src/execution/ppc/frame-constants-ppc.h"
#include "src/roots/roots-inl.h"

namespace v8 {
namespace internal {
namespace compiler {

using namespace turboshaft;  // NOLINT(build/namespaces)

enum ImmediateMode {
  kInt16Imm,
  kInt16Imm_Unsigned,
  kInt16Imm_Negate,
  kInt16Imm_4ByteAligned,
  kShift32Imm,
  kInt34Imm,
  kShift64Imm,
  kNoImmediate
};

// Adds PPC-specific methods for generating operands.
class PPCOperandGenerator final : public OperandGenerator {
 public:
  explicit PPCOperandGenerator(InstructionSelector* selector)
      : OperandGenerator(selector) {}

  InstructionOperand UseOperand(OpIndex node, ImmediateMode mode) {
    if (CanBeImmediate(node, mode)) {
      return UseImmediate(node);
    }
    return UseRegister(node);
  }

  bool CanBeImmediate(OpIndex node, ImmediateMode mode) {
    const ConstantOp* constant = selector()->Get(node).TryCast<ConstantOp>();
    if (!constant) return false;
    if (constant->kind == ConstantOp::Kind::kCompressedHeapObject) {
      if (!COMPRESS_POINTERS_BOOL) return false;
      // For builtin code we need static roots
      if (selector()->isolate()->bootstrapper() && !V8_STATIC_ROOTS_BOOL) {
        return false;
      }
      const RootsTable& roots_table = selector()->isolate()->roots_table();
      RootIndex root_index;
      Handle<HeapObject> value = constant->handle();
      if (roots_table.IsRootHandle(value, &root_index)) {
        if (!RootsTable::IsReadOnly(root_index)) return false;
        return CanBeImmediate(MacroAssemblerBase::ReadOnlyRootPtr(
                                  root_index, selector()->isolate()),
                              mode);
      }
      return false;
    }

    int64_t value;
    if (!selector()->MatchSignedIntegralConstant(node, &value)) return false;
    return CanBeImmediate(value, mode);
  }

  bool CanBeImmediate(int64_t value, ImmediateMode mode) {
    switch (mode) {
      case kInt16Imm:
        return is_int16(value);
      case kInt16Imm_Unsigned:
        return is_uint16(value);
      case kInt16Imm_Negate:
        return is_int16(-value);
      case kInt16Imm_4ByteAligned:
        return is_int16(value) && !(value & 3);
      case kShift32Imm:
        return 0 <= value && value < 32;
      case kInt34Imm:
        return is_int34(value);
      case kShift64Imm:
        return 0 <= value && value < 64;
      case kNoImmediate:
        return false;
    }
    return false;
  }
};

namespace {

void VisitRR(InstructionSelector* selector, InstructionCode opcode,
             OpIndex node) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  DCHECK_EQ(op.input_count, 1);
  selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void VisitRRR(InstructionSelector* selector, InstructionCode opcode,
              OpIndex node) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  DCHECK_EQ(op.input_count, 2);
  selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
                 g.UseRegister(op.input(1)));
}

void VisitRRO(InstructionSelector* selector, InstructionCode opcode,
              OpIndex node, ImmediateMode operand_mode) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
                 g.UseOperand(op.input(1), operand_mode));
}

void VisitTryTruncateDouble(InstructionSelector* selector,
                            InstructionCode opcode, OpIndex node) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  InstructionOperand inputs[] = {g.UseRegister(op.input(0))};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  OptionalOpIndex success_output = selector->FindProjection(node, 1);
  if (success_output.valid()) {
    outputs[output_count++] = g.DefineAsRegister(success_output.value());
  }

  selector->Emit(opcode, output_count, outputs, 1, inputs);
}

// Shared routine for multiple binary operations.
void VisitBinop(InstructionSelector* selector, OpIndex node,
                InstructionCode opcode, ImmediateMode operand_mode,
                FlagsContinuation* cont) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  InstructionOperand inputs[4];
  size_t input_count = 0;
  InstructionOperand outputs[2];
  size_t output_count = 0;

  inputs[input_count++] = g.UseRegister(op.input(0));
  inputs[input_count++] = g.UseOperand(op.input(1), operand_mode);

  if (cont->IsDeoptimize()) {
    // If we can deoptimize as a result of the binop, we need to make sure that
    // the deopt inputs are not overwritten by the binop result. One way
    // to achieve that is to declare the output register as same-as-first.
    outputs[output_count++] = g.DefineSameAsFirst(node);
  } else {
    outputs[output_count++] = g.DefineAsRegister(node);
  }

  DCHECK_NE(0u, input_count);
  DCHECK_NE(0u, output_count);
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

  selector->EmitWithContinuation(opcode, output_count, outputs, input_count,
                                 inputs, cont);
}

// Shared routine for multiple binary operations.
void VisitBinop(InstructionSelector* selector, OpIndex node,
                InstructionCode opcode, ImmediateMode operand_mode) {
  FlagsContinuation cont;
  VisitBinop(selector, node, opcode, operand_mode, &cont);
}

}  // namespace

void InstructionSelector::VisitStackSlot(OpIndex node) {
  const StackSlotOp& stack_slot = Cast<StackSlotOp>(node);
  int slot = frame_->AllocateSpillSlot(stack_slot.size, stack_slot.alignment,
                                       stack_slot.is_tagged);
  OperandGenerator g(this);

  Emit(kArchStackSlot, g.DefineAsRegister(node),
       sequence()->AddImmediate(Constant(slot)), 0, nullptr);
}

void InstructionSelector::VisitAbortCSADcheck(OpIndex node) {
  PPCOperandGenerator g(this);
  const AbortCSADcheckOp& op = Cast<AbortCSADcheckOp>(node);
  Emit(kArchAbortCSADcheck, g.NoOutput(), g.UseFixed(op.input(0), r4));
}

ArchOpcode SelectLoadOpcode(MemoryRepresentation loaded_rep,
                            RegisterRepresentation result_rep,
                            ImmediateMode* mode) {
  // NOTE: The meaning of `loaded_rep` = `MemoryRepresentation::AnyTagged()` is
  // we are loading a compressed tagged field, while `result_rep` =
  // `RegisterRepresentation::Tagged()` refers to an uncompressed tagged value.
  if (CpuFeatures::IsSupported(PPC_10_PLUS)) {
    *mode = kInt34Imm;
  } else {
    *mode = kInt16Imm;
  }
  switch (loaded_rep) {
    case MemoryRepresentation::Int8():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kPPC_LoadWordS8;
    case MemoryRepresentation::Uint8():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kPPC_LoadWordU8;
    case MemoryRepresentation::Int16():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kPPC_LoadWordS16;
    case MemoryRepresentation::Uint16():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kPPC_LoadWordU16;
    case MemoryRepresentation::Int32():
    case MemoryRepresentation::Uint32():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kPPC_LoadWordU32;
    case MemoryRepresentation::Int64():
    case MemoryRepresentation::Uint64():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word64());
      if (*mode != kInt34Imm) *mode = kInt16Imm_4ByteAligned;
      return kPPC_LoadWord64;
    case MemoryRepresentation::Float16():
      UNIMPLEMENTED();
    case MemoryRepresentation::Float32():
      DCHECK_EQ(result_rep, RegisterRepresentation::Float32());
      return kPPC_LoadFloat32;
    case MemoryRepresentation::Float64():
      DCHECK_EQ(result_rep, RegisterRepresentation::Float64());
      return kPPC_LoadDouble;
#ifdef V8_COMPRESS_POINTERS
    case MemoryRepresentation::AnyTagged():
    case MemoryRepresentation::TaggedPointer():
      if (result_rep == RegisterRepresentation::Compressed()) {
        if (*mode != kInt34Imm) *mode = kInt16Imm_4ByteAligned;
        return kPPC_LoadWordS32;
      }
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      return kPPC_LoadDecompressTagged;
    case MemoryRepresentation::TaggedSigned():
      if (result_rep == RegisterRepresentation::Compressed()) {
        if (*mode != kInt34Imm) *mode = kInt16Imm_4ByteAligned;
        return kPPC_LoadWordS32;
      }
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      return kPPC_LoadDecompressTaggedSigned;
#else
      USE(result_rep);
    case MemoryRepresentation::AnyTagged():
    case MemoryRepresentation::TaggedPointer():
    case MemoryRepresentation::TaggedSigned():
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      if (*mode != kInt34Imm) *mode = kInt16Imm_4ByteAligned;
      return kPPC_LoadWord64;
#endif
    case MemoryRepresentation::AnyUncompressedTagged():
    case MemoryRepresentation::UncompressedTaggedPointer():
    case MemoryRepresentation::UncompressedTaggedSigned():
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      if (*mode != kInt34Imm) *mode = kInt16Imm_4ByteAligned;
      return kPPC_LoadWord64;
    case MemoryRepresentation::SandboxedPointer():
      return kPPC_LoadDecodeSandboxedPointer;
    case MemoryRepresentation::Simd128():
      DCHECK_EQ(result_rep, RegisterRepresentation::Simd128());
      // Vectors do not support MRI mode, only MRR is available.
      *mode = kNoImmediate;
      return kPPC_LoadSimd128;
    case MemoryRepresentation::ProtectedPointer():
    case MemoryRepresentation::IndirectPointer():
    case MemoryRepresentation::Simd256():
      UNREACHABLE();
  }
}

ArchOpcode SelectLoadOpcode(LoadRepresentation load_rep, ImmediateMode* mode) {
  if (CpuFeatures::IsSupported(PPC_10_PLUS)) {
    *mode = kInt34Imm;
  } else {
    *mode = kInt16Imm;
  }
  switch (load_rep.representation()) {
    case MachineRepresentation::kFloat32:
      return kPPC_LoadFloat32;
    case MachineRepresentation::kFloat64:
      return kPPC_LoadDouble;
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      return load_rep.IsSigned() ? kPPC_LoadWordS8 : kPPC_LoadWordU8;
    case MachineRepresentation::kWord16:
      return load_rep.IsSigned() ? kPPC_LoadWordS16 : kPPC_LoadWordU16;
    case MachineRepresentation::kWord32:
      return kPPC_LoadWordU32;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
      if (*mode != kInt34Imm) *mode = kInt16Imm_4ByteAligned;
      return kPPC_LoadWordS32;
#else
      UNREACHABLE();
#endif
      case MachineRepresentation::kIndirectPointer:
        UNREACHABLE();
      case MachineRepresentation::kSandboxedPointer:
        return kPPC_LoadDecodeSandboxedPointer;
#ifdef V8_COMPRESS_POINTERS
      case MachineRepresentation::kTaggedSigned:
        return kPPC_LoadDecompressTaggedSigned;
      case MachineRepresentation::kTaggedPointer:
        return kPPC_LoadDecompressTagged;
      case MachineRepresentation::kTagged:
        return kPPC_LoadDecompressTagged;
#else
      case MachineRepresentation::kTaggedSigned:   // Fall through.
      case MachineRepresentation::kTaggedPointer:  // Fall through.
      case MachineRepresentation::kTagged:         // Fall through.
#endif
      case MachineRepresentation::kWord64:
        if (*mode != kInt34Imm) *mode = kInt16Imm_4ByteAligned;
        return kPPC_LoadWord64;
      case MachineRepresentation::kSimd128:
        // Vectors do not support MRI mode, only MRR is available.
        *mode = kNoImmediate;
        return kPPC_LoadSimd128;
      case MachineRepresentation::kFloat16:
        UNIMPLEMENTED();
      case MachineRepresentation::kProtectedPointer:  // Fall through.
      case MachineRepresentation::kSimd256:  // Fall through.
      case MachineRepresentation::kMapWord:  // Fall through.
      case MachineRepresentation::kFloat16RawBits:  // Fall through.
      case MachineRepresentation::kNone:
        UNREACHABLE();
  }
}

static void VisitLoadCommon(InstructionSelector* selector, OpIndex node,
                            ImmediateMode mode, InstructionCode opcode) {
  PPCOperandGenerator g(selector);
  auto load = selector->load_view(node);
  OpIndex base = load.base();
  OpIndex offset = load.index();

  bool is_atomic = load.is_atomic();

  if (selector->Is<LoadRootRegisterOp>(base)) {
    selector->Emit(opcode |= AddressingModeField::encode(kMode_Root),
                   g.DefineAsRegister(node), g.UseRegister(offset),
                   g.UseImmediate(is_atomic));
  } else if (g.CanBeImmediate(offset, mode)) {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                   g.DefineAsRegister(node), g.UseRegister(base),
                   g.UseImmediate(offset), g.UseImmediate(is_atomic));
  } else if (g.CanBeImmediate(base, mode)) {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                   g.DefineAsRegister(node), g.UseRegister(offset),
                   g.UseImmediate(base), g.UseImmediate(is_atomic));
  } else {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRR),
                   g.DefineAsRegister(node), g.UseRegister(base),
                   g.UseRegister(offset), g.UseImmediate(is_atomic));
  }
}

void InstructionSelector::VisitLoad(OpIndex node) {
  LoadView load = load_view(node);
  ImmediateMode mode;
  InstructionCode opcode =
      SelectLoadOpcode(load.ts_loaded_rep(), load.ts_result_rep(), &mode);
  VisitLoadCommon(this, node, mode, opcode);
}

void InstructionSelector::VisitProtectedLoad(OpIndex node) {
  // TODO(eholk)
  UNIMPLEMENTED();
}

void VisitStoreCommon(InstructionSelector* selector, OpIndex node,
                      StoreRepresentation store_rep,
                      std::optional<AtomicMemoryOrder> atomic_order) {
  PPCOperandGenerator g(selector);
  auto store = selector->store_view(node);
  OpIndex base = store.base();
  OpIndex offset = store.index().value();
  OpIndex value = store.value();
  bool is_atomic = store.is_atomic();

  MachineRepresentation rep = store_rep.representation();
  WriteBarrierKind write_barrier_kind = kNoWriteBarrier;

  if (!is_atomic) {
    write_barrier_kind = store_rep.write_barrier_kind();
  }

  if (v8_flags.enable_unconditional_write_barriers &&
      CanBeTaggedOrCompressedOrIndirectPointer(rep)) {
    write_barrier_kind = kFullWriteBarrier;
  }

  if (write_barrier_kind != kNoWriteBarrier &&
      !v8_flags.disable_write_barriers) {
    DCHECK(CanBeTaggedOrCompressedOrIndirectPointer(rep));
    // Uncompressed stores should not happen if we need a write barrier.
    CHECK((store.ts_stored_rep() !=
           MemoryRepresentation::AnyUncompressedTagged()) &&
          (store.ts_stored_rep() !=
           MemoryRepresentation::UncompressedTaggedPointer()) &&
          (store.ts_stored_rep() !=
           MemoryRepresentation::UncompressedTaggedPointer()));
    AddressingMode addressing_mode;
    InstructionOperand inputs[4];
    size_t input_count = 0;
    inputs[input_count++] = g.UseUniqueRegister(base);
    // OutOfLineRecordWrite uses the offset in an 'add' instruction as well as
    // for the store itself, so we must check compatibility with both.
    if (g.CanBeImmediate(offset, kInt16Imm)
        && g.CanBeImmediate(offset, kInt16Imm_4ByteAligned)
    ) {
      inputs[input_count++] = g.UseImmediate(offset);
      addressing_mode = kMode_MRI;
    } else {
      inputs[input_count++] = g.UseUniqueRegister(offset);
      addressing_mode = kMode_MRR;
    }
    inputs[input_count++] = g.UseUniqueRegister(value);
    RecordWriteMode record_write_mode =
        WriteBarrierKindToRecordWriteMode(write_barrier_kind);
    InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
    size_t const temp_count = arraysize(temps);
    InstructionCode code;
    if (rep == MachineRepresentation::kIndirectPointer) {
      DCHECK_EQ(write_barrier_kind, kIndirectPointerWriteBarrier);
      // In this case we need to add the IndirectPointerTag as additional input.
      code = kArchStoreIndirectWithWriteBarrier;
      IndirectPointerTag tag = store.indirect_pointer_tag();
      inputs[input_count++] = g.UseImmediate(static_cast<int64_t>(tag));
    } else {
      code = kArchStoreWithWriteBarrier;
    }
    code |= AddressingModeField::encode(addressing_mode);
    code |= RecordWriteModeField::encode(record_write_mode);
    CHECK_EQ(is_atomic, false);
    selector->Emit(code, 0, nullptr, input_count, inputs, temp_count, temps);
  } else {
    ArchOpcode opcode;
    ImmediateMode mode;
    if (CpuFeatures::IsSupported(PPC_10_PLUS)) {
      mode = kInt34Imm;
    } else {
      mode = kInt16Imm;
    }
    switch (store.ts_stored_rep()) {
      case MemoryRepresentation::Int8():
      case MemoryRepresentation::Uint8():
        opcode = kPPC_StoreWord8;
        break;
      case MemoryRepresentation::Int16():
      case MemoryRepresentation::Uint16():
        opcode = kPPC_StoreWord16;
        break;
      case MemoryRepresentation::Int32():
      case MemoryRepresentation::Uint32(): {
        opcode = kPPC_StoreWord32;
        const Operation& reverse_op = selector->Get(value);
        if (reverse_op.Is<Opmask::kWord32ReverseBytes>()) {
          opcode = kPPC_StoreByteRev32;
          value = reverse_op.input(0);
          mode = kNoImmediate;
        }
        break;
      }
      case MemoryRepresentation::Int64():
      case MemoryRepresentation::Uint64(): {
        if (mode != kInt34Imm) mode = kInt16Imm_4ByteAligned;
        opcode = kPPC_StoreWord64;
        const Operation& reverse_op = selector->Get(value);
        if (reverse_op.Is<Opmask::kWord64ReverseBytes>()) {
          opcode = kPPC_StoreByteRev64;
          value = reverse_op.input(0);
          mode = kNoImmediate;
        }
        break;
      }
      case MemoryRepresentation::Float16():
        UNIMPLEMENTED();
      case MemoryRepresentation::Float32():
        opcode = kPPC_StoreFloat32;
        break;
      case MemoryRepresentation::Float64():
        opcode = kPPC_StoreDouble;
        break;
      case MemoryRepresentation::AnyTagged():
      case MemoryRepresentation::TaggedPointer():
      case MemoryRepresentation::TaggedSigned():
        if (mode != kInt34Imm) mode = kInt16Imm;
        opcode = kPPC_StoreCompressTagged;
        break;
      case MemoryRepresentation::AnyUncompressedTagged():
      case MemoryRepresentation::UncompressedTaggedPointer():
      case MemoryRepresentation::UncompressedTaggedSigned():
        if (mode != kInt34Imm) mode = kInt16Imm_4ByteAligned;
        opcode = kPPC_StoreWord64;
        break;
      case MemoryRepresentation::ProtectedPointer():
        // We never store directly to protected pointers from generated code.
        UNREACHABLE();
      case MemoryRepresentation::IndirectPointer():
        if (mode != kInt34Imm) mode = kInt16Imm_4ByteAligned;
        opcode = kPPC_StoreIndirectPointer;
        break;
      case MemoryRepresentation::SandboxedPointer():
        if (mode != kInt34Imm) mode = kInt16Imm_4ByteAligned;
        opcode = kPPC_StoreEncodeSandboxedPointer;
        break;
      case MemoryRepresentation::Simd128():
        opcode = kPPC_StoreSimd128;
        // Vectors do not support MRI mode, only MRR is available.
        mode = kNoImmediate;
        break;
      case MemoryRepresentation::Simd256():
        UNREACHABLE();
    }

    if (selector->Is<LoadRootRegisterOp>(base)) {
      selector->Emit(opcode | AddressingModeField::encode(kMode_Root),
                     g.NoOutput(), g.UseRegister(offset), g.UseRegister(value),
                     g.UseImmediate(is_atomic));
    } else if (g.CanBeImmediate(offset, mode)) {
      selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                     g.NoOutput(), g.UseRegister(base), g.UseImmediate(offset),
                     g.UseRegister(value), g.UseImmediate(is_atomic));
    } else if (g.CanBeImmediate(base, mode)) {
      selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                     g.NoOutput(), g.UseRegister(offset), g.UseImmediate(base),
                     g.UseRegister(value), g.UseImmediate(is_atomic));
    } else {
      selector->Emit(opcode | AddressingModeField::encode(kMode_MRR),
                     g.NoOutput(), g.UseRegister(base), g.UseRegister(offset),
                     g.UseRegister(value), g.UseImmediate(is_atomic));
    }
  }
}

void InstructionSelector::VisitStorePair(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitStore(OpIndex node) {
  VisitStoreCommon(this, node, store_view(node).stored_rep(), std::nullopt);
}

void InstructionSelector::VisitProtectedStore(OpIndex node) {
  // TODO(eholk)
  UNIMPLEMENTED();
}

// Architecture supports unaligned access, therefore VisitLoad is used instead
void InstructionSelector::VisitUnalignedLoad(OpIndex node) { UNREACHABLE(); }

// Architecture supports unaligned access, therefore VisitStore is used instead
void InstructionSelector::VisitUnalignedStore(OpIndex node) { UNREACHABLE(); }

static void VisitLogical(InstructionSelector* selector, OpIndex node,
                         ArchOpcode opcode, bool left_can_cover,
                         bool right_can_cover, ImmediateMode imm_mode) {
  PPCOperandGenerator g(selector);
  const WordBinopOp& logical_op = selector->Get(node).Cast<WordBinopOp>();
  const Operation& lhs = selector->Get(logical_op.left());
  const Operation& rhs = selector->Get(logical_op.right());

  // Map instruction to equivalent operation with inverted right input.
  ArchOpcode inv_opcode = opcode;
  switch (opcode) {
    case kPPC_And:
      inv_opcode = kPPC_AndComplement;
      break;
    case kPPC_Or:
      inv_opcode = kPPC_OrComplement;
      break;
    default:
      UNREACHABLE();
  }

  // Select Logical(y, ~x) for Logical(Xor(x, -1), y).
  if (lhs.Is<Opmask::kBitwiseXor>() && left_can_cover) {
    const WordBinopOp& xor_op = lhs.Cast<WordBinopOp>();
    int64_t xor_rhs_val;
    if (selector->MatchSignedIntegralConstant(xor_op.right(), &xor_rhs_val) &&
        xor_rhs_val == -1) {
      // TODO(all): support shifted operand on right.
      selector->Emit(inv_opcode, g.DefineAsRegister(node),
                     g.UseRegister(logical_op.right()),
                     g.UseRegister(xor_op.left()));
      return;
    }
  }

  // Select Logical(x, ~y) for Logical(x, Xor(y, -1)).
  if (rhs.Is<Opmask::kBitwiseXor>() && right_can_cover) {
    const WordBinopOp& xor_op = rhs.Cast<WordBinopOp>();
    int64_t xor_rhs_val;
    if (selector->MatchSignedIntegralConstant(xor_op.right(), &xor_rhs_val) &&
        xor_rhs_val == -1) {
      // TODO(all): support shifted operand on right.
      selector->Emit(inv_opcode, g.DefineAsRegister(node),
                     g.UseRegister(logical_op.left()),
                     g.UseRegister(xor_op.left()));
      return;
    }
  }

  VisitBinop(selector, node, opcode, imm_mode);
}

static inline bool IsContiguousMask32(uint32_t value, int* mb, int* me) {
  int mask_width = base::bits::CountPopulation(value);
  int mask_msb = base::bits::CountLeadingZeros32(value);
  int mask_lsb = base::bits::CountTrailingZeros32(value);
  if ((mask_width == 0) || (mask_msb + mask_width + mask_lsb != 32))
    return false;
  *mb = mask_lsb + mask_width - 1;
  *me = mask_lsb;
  return true;
}

static inline bool IsContiguousMask64(uint64_t value, int* mb, int* me) {
  int mask_width = base::bits::CountPopulation(value);
  int mask_msb = base::bits::CountLeadingZeros64(value);
  int mask_lsb = base::bits::CountTrailingZeros64(value);
  if ((mask_width == 0) || (mask_msb + mask_width + mask_lsb != 64))
    return false;
  *mb = mask_lsb + mask_width - 1;
  *me = mask_lsb;
  return true;
}

// TODO(mbrandy): Absorb rotate-right into rlwinm?
void InstructionSelector::VisitWord32And(OpIndex node) {
  PPCOperandGenerator g(this);

  const WordBinopOp& bitwise_and = Get(node).Cast<WordBinopOp>();
  int mb = 0;
  int me = 0;
  int64_t value;
  if (MatchSignedIntegralConstant(bitwise_and.right(), &value) &&
      IsContiguousMask32(value, &mb, &me)) {
    int sh = 0;
    OpIndex left = bitwise_and.left();
    const Operation& lhs = Get(left);
    if ((lhs.Is<Opmask::kWord32ShiftRightLogical>() ||
         lhs.Is<Opmask::kWord32ShiftLeft>()) &&
        CanCover(node, left)) {
      // Try to absorb left/right shift into rlwinm
      int32_t shift_by;
      const ShiftOp& shift_op = lhs.Cast<ShiftOp>();
      if (MatchIntegralWord32Constant(shift_op.right(), &shift_by) &&
          base::IsInRange(shift_by, 0, 31)) {
        left = shift_op.left();
        sh = shift_by;
        if (lhs.Is<Opmask::kWord32ShiftRightLogical>()) {
          // Adjust the mask such that it doesn't include any rotated bits.
          if (mb > 31 - sh) mb = 31 - sh;
          sh = (32 - sh) & 0x1F;
        } else {
          // Adjust the mask such that it doesn't include any rotated bits.
          if (me < sh) me = sh;
        }
      }
    }
    if (mb >= me) {
      Emit(kPPC_RotLeftAndMask32, g.DefineAsRegister(node), g.UseRegister(left),
           g.TempImmediate(sh), g.TempImmediate(mb), g.TempImmediate(me));
      return;
    }
  }
  VisitLogical(this, node, kPPC_And, CanCover(node, bitwise_and.left()),
               CanCover(node, bitwise_and.right()), kInt16Imm_Unsigned);
}

// TODO(mbrandy): Absorb rotate-right into rldic?
void InstructionSelector::VisitWord64And(OpIndex node) {
  PPCOperandGenerator g(this);

  const WordBinopOp& bitwise_and = Get(node).Cast<WordBinopOp>();
  int mb = 0;
  int me = 0;
  int64_t value;
  if (MatchSignedIntegralConstant(bitwise_and.right(), &value) &&
      IsContiguousMask64(value, &mb, &me)) {
    int sh = 0;
    OpIndex left = bitwise_and.left();
    const Operation& lhs = Get(left);
    if ((lhs.Is<Opmask::kWord64ShiftRightLogical>() ||
         lhs.Is<Opmask::kWord64ShiftLeft>()) &&
        CanCover(node, left)) {
      // Try to absorb left/right shift into rldic
      int64_t shift_by;
      const ShiftOp& shift_op = lhs.Cast<ShiftOp>();
      if (MatchIntegralWord64Constant(shift_op.right(), &shift_by) &&
          base::IsInRange(shift_by, 0, 63)) {
        left = shift_op.left();
        sh = shift_by;
        if (lhs.Is<Opmask::kWord64ShiftRightLogical>()) {
          // Adjust the mask such that it doesn't include any rotated bits.
          if (mb > 63 - sh) mb = 63 - sh;
          sh = (64 - sh) & 0x3F;
        } else {
          // Adjust the mask such that it doesn't include any rotated bits.
          if (me < sh) me = sh;
        }
      }
    }
    if (mb >= me) {
      bool match = false;
      ArchOpcode opcode;
      int mask;
      if (me == 0) {
        match = true;
        opcode = kPPC_RotLeftAndClearLeft64;
        mask = mb;
      } else if (mb == 63) {
        match = true;
        opcode = kPPC_RotLeftAndClearRight64;
        mask = me;
      } else if (sh && me <= sh && lhs.Is<Opmask::kWord64ShiftLeft>()) {
        match = true;
        opcode = kPPC_RotLeftAndClear64;
        mask = mb;
      }
      if (match) {
        Emit(opcode, g.DefineAsRegister(node), g.UseRegister(left),
             g.TempImmediate(sh), g.TempImmediate(mask));
        return;
      }
    }
  }
  VisitLogical(this, node, kPPC_And, CanCover(node, bitwise_and.left()),
               CanCover(node, bitwise_and.right()), kInt16Imm_Unsigned);
}

void InstructionSelector::VisitWord32Or(OpIndex node) {
  const WordBinopOp& op = this->Get(node).template Cast<WordBinopOp>();
  VisitLogical(this, node, kPPC_Or, CanCover(node, op.left()),
               CanCover(node, op.right()), kInt16Imm_Unsigned);
}

void InstructionSelector::VisitWord64Or(OpIndex node) {
  const WordBinopOp& op = this->Get(node).template Cast<WordBinopOp>();
  VisitLogical(this, node, kPPC_Or, CanCover(node, op.left()),
               CanCover(node, op.right()), kInt16Imm_Unsigned);
}

void InstructionSelector::VisitWord32Xor(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& bitwise_xor = this->Get(node).template Cast<WordBinopOp>();
  int32_t mask;
  if (this->MatchIntegralWord32Constant(bitwise_xor.right(), &mask) &&
      mask == -1) {
    Emit(kPPC_Not, g.DefineAsRegister(node), g.UseRegister(bitwise_xor.left()));
  } else {
    VisitBinop(this, node, kPPC_Xor, kInt16Imm_Unsigned);
  }
}

void InstructionSelector::VisitStackPointerGreaterThan(
    OpIndex node, FlagsContinuation* cont) {
  StackCheckKind kind;
  OpIndex value;
  const auto& op = this->turboshaft_graph()
                       ->Get(node)
                       .template Cast<StackPointerGreaterThanOp>();
  kind = op.kind;
  value = op.stack_limit();
  InstructionCode opcode =
      kArchStackPointerGreaterThan |
      StackCheckField::encode(static_cast<StackCheckKind>(kind));

  PPCOperandGenerator g(this);

  // No outputs.
  InstructionOperand* const outputs = nullptr;
  const int output_count = 0;

  // Applying an offset to this stack check requires a temp register. Offsets
  // are only applied to the first stack check. If applying an offset, we must
  // ensure the input and temp registers do not alias, thus kUniqueRegister.
  InstructionOperand temps[] = {g.TempRegister()};
  const int temp_count = (kind == StackCheckKind::kJSFunctionEntry) ? 1 : 0;
  const auto register_mode = (kind == StackCheckKind::kJSFunctionEntry)
                                 ? OperandGenerator::kUniqueRegister
                                 : OperandGenerator::kRegister;

  InstructionOperand inputs[] = {g.UseRegisterWithMode(value, register_mode)};
  static constexpr int input_count = arraysize(inputs);

  EmitWithContinuation(opcode, output_count, outputs, input_count, inputs,
                       temp_count, temps, cont);
}

void InstructionSelector::VisitWord64Xor(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& bitwise_xor = this->Get(node).template Cast<WordBinopOp>();
  int64_t mask;
  if (this->MatchIntegralWord64Constant(bitwise_xor.right(), &mask) &&
      mask == -1) {
    Emit(kPPC_Not, g.DefineAsRegister(node), g.UseRegister(bitwise_xor.left()));
  } else {
    VisitBinop(this, node, kPPC_Xor, kInt16Imm_Unsigned);
  }
}

void InstructionSelector::VisitWord32Shl(OpIndex node) {
  PPCOperandGenerator g(this);
  const ShiftOp& shl = this->Get(node).template Cast<ShiftOp>();
  const Operation& lhs = this->Get(shl.left());
  int64_t value;
  if (lhs.Is<Opmask::kWord32BitwiseAnd>() &&
      this->MatchSignedIntegralConstant(shl.right(), &value) &&
      base::IsInRange(value, 0, 31)) {
    int sh = value;
    int mb;
    int me;
    const WordBinopOp& bitwise_and = lhs.Cast<WordBinopOp>();
    int64_t right_value;
    if (MatchSignedIntegralConstant(bitwise_and.right(), &right_value) &&
        IsContiguousMask32(right_value << sh, &mb, &me)) {
      // Adjust the mask such that it doesn't include any rotated bits.
      if (me < sh) me = sh;
      if (mb >= me) {
        Emit(kPPC_RotLeftAndMask32, g.DefineAsRegister(node),
             g.UseRegister(bitwise_and.left()), g.TempImmediate(sh),
             g.TempImmediate(mb), g.TempImmediate(me));
        return;
      }
    }
  }
  VisitRRO(this, kPPC_ShiftLeft32, node, kShift32Imm);
}

void InstructionSelector::VisitWord64Shl(OpIndex node) {
  PPCOperandGenerator g(this);
  const ShiftOp& shl = this->Get(node).template Cast<ShiftOp>();
  const Operation& lhs = this->Get(shl.left());
  int64_t value;
  if (lhs.Is<Opmask::kWord64BitwiseAnd>() &&
      this->MatchSignedIntegralConstant(shl.right(), &value) &&
      base::IsInRange(value, 0, 63)) {
    int sh = value;
    int mb;
    int me;
    const WordBinopOp& bitwise_and = lhs.Cast<WordBinopOp>();
    int64_t right_value;
    if (MatchSignedIntegralConstant(bitwise_and.right(), &right_value) &&
        IsContiguousMask64(right_value << sh, &mb, &me)) {
      // Adjust the mask such that it doesn't include any rotated bits.
      if (me < sh) me = sh;
      if (mb >= me) {
        bool match = false;
        ArchOpcode opcode;
        int mask;
        if (me == 0) {
          match = true;
          opcode = kPPC_RotLeftAndClearLeft64;
          mask = mb;
        } else if (mb == 63) {
          match = true;
          opcode = kPPC_RotLeftAndClearRight64;
          mask = me;
        } else if (sh && me <= sh) {
          match = true;
          opcode = kPPC_RotLeftAndClear64;
          mask = mb;
        }
        if (match) {
          Emit(opcode, g.DefineAsRegister(node),
               g.UseRegister(bitwise_and.left()), g.TempImmediate(sh),
               g.TempImmediate(mask));
          return;
        }
      }
    }
  }
  VisitRRO(this, kPPC_ShiftLeft64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Shr(OpIndex node) {
  PPCOperandGenerator g(this);
  const ShiftOp& shr = this->Get(node).template Cast<ShiftOp>();
  const Operation& lhs = this->Get(shr.left());
  int64_t value;
  if (lhs.Is<Opmask::kWord32BitwiseAnd>() &&
      MatchSignedIntegralConstant(shr.right(), &value) &&
      base::IsInRange(value, 0, 31)) {
    int sh = value;
    int mb;
    int me;
    const WordBinopOp& bitwise_and = lhs.Cast<WordBinopOp>();
    uint64_t right_value;
    if (MatchUnsignedIntegralConstant(bitwise_and.right(), &right_value) &&
        IsContiguousMask32(static_cast<uint32_t>(right_value >> sh), &mb,
                           &me)) {
      // Adjust the mask such that it doesn't include any rotated bits.
      if (mb > 31 - sh) mb = 31 - sh;
      sh = (32 - sh) & 0x1F;
      if (mb >= me) {
        Emit(kPPC_RotLeftAndMask32, g.DefineAsRegister(node),
             g.UseRegister(bitwise_and.left()), g.TempImmediate(sh),
             g.TempImmediate(mb), g.TempImmediate(me));
        return;
      }
    }
  }
  VisitRRO(this, kPPC_ShiftRight32, node, kShift32Imm);
}

void InstructionSelector::VisitWord64Shr(OpIndex node) {
  PPCOperandGenerator g(this);
  const ShiftOp& shr = this->Get(node).template Cast<ShiftOp>();
  const Operation& lhs = this->Get(shr.left());
  int64_t value;
  if (lhs.Is<Opmask::kWord64BitwiseAnd>() &&
      MatchSignedIntegralConstant(shr.right(), &value) &&
      base::IsInRange(value, 0, 63)) {
    int sh = value;
    int mb;
    int me;
    const WordBinopOp& bitwise_and = lhs.Cast<WordBinopOp>();
    uint64_t right_value;
    if (MatchUnsignedIntegralConstant(bitwise_and.right(), &right_value) &&
        IsContiguousMask64(static_cast<uint64_t>(right_value >> sh), &mb,
                           &me)) {
      // Adjust the mask such that it doesn't include any rotated bits.
      if (mb > 63 - sh) mb = 63 - sh;
      sh = (64 - sh) & 0x3F;
      if (mb >= me) {
        bool match = false;
        ArchOpcode opcode;
        int mask;
        if (me == 0) {
          match = true;
          opcode = kPPC_RotLeftAndClearLeft64;
          mask = mb;
        } else if (mb == 63) {
          match = true;
          opcode = kPPC_RotLeftAndClearRight64;
          mask = me;
        }
        if (match) {
          Emit(opcode, g.DefineAsRegister(node),
               g.UseRegister(bitwise_and.left()), g.TempImmediate(sh),
               g.TempImmediate(mask));
          return;
        }
      }
    }
  }
  VisitRRO(this, kPPC_ShiftRight64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Sar(OpIndex node) {
  PPCOperandGenerator g(this);
  const ShiftOp& sar = this->Get(node).template Cast<ShiftOp>();
  const Operation& lhs = this->Get(sar.left());
  if (CanCover(node, sar.left()) && lhs.Is<Opmask::kWord32ShiftLeft>()) {
    const ShiftOp& shl = lhs.Cast<ShiftOp>();
    uint64_t sar_value;
    uint64_t shl_value;
    if (MatchUnsignedIntegralConstant(sar.right(), &sar_value) &&
        MatchUnsignedIntegralConstant(shl.right(), &shl_value)) {
      uint32_t sar_by = sar_value;
      uint32_t shl_by = shl_value;
      if ((sar_by == shl_by) && (sar_by == 16)) {
        Emit(kPPC_ExtendSignWord16, g.DefineAsRegister(node),
             g.UseRegister(shl.left()));
        return;
      } else if ((sar_by == shl_by) && (sar_by == 24)) {
        Emit(kPPC_ExtendSignWord8, g.DefineAsRegister(node),
             g.UseRegister(shl.left()));
        return;
      }
    }
  }
  VisitRRO(this, kPPC_ShiftRightAlg32, node, kShift32Imm);
}

void InstructionSelector::VisitWord64Sar(OpIndex node) {
  PPCOperandGenerator g(this);
  DCHECK(this->Get(node).template Cast<ShiftOp>().IsRightShift());
  const ShiftOp& shift = this->Get(node).template Cast<ShiftOp>();
  const Operation& lhs = this->Get(shift.left());
  int64_t constant_rhs;

  if (lhs.Is<LoadOp>() &&
      this->MatchIntegralWord64Constant(shift.right(), &constant_rhs) &&
      constant_rhs == 32 && this->CanCover(node, shift.left())) {
    // Just load and sign-extend the interesting 4 bytes instead. This
    // happens, for example, when we're loading and untagging SMIs.
    const LoadOp& load = lhs.Cast<LoadOp>();
    int64_t offset = 0;
    if (load.index().has_value()) {
      int64_t index_constant;
      if (this->MatchIntegralWord64Constant(load.index().value(),
                                            &index_constant)) {
        DCHECK_EQ(load.element_size_log2, 0);
        offset = index_constant;
      }
    } else {
      offset = load.offset;
    }
    offset = SmiWordOffset(offset);
    if (g.CanBeImmediate(offset, kInt16Imm_4ByteAligned)) {
      Emit(kPPC_LoadWordS32 | AddressingModeField::encode(kMode_MRI),
           g.DefineAsRegister(node), g.UseRegister(load.base()),
           g.TempImmediate(offset), g.UseImmediate(0));
      return;
    }
  }

  VisitRRO(this, kPPC_ShiftRightAlg64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Rol(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitWord64Rol(OpIndex node) { UNREACHABLE(); }

// TODO(mbrandy): Absorb logical-and into rlwinm?
void InstructionSelector::VisitWord32Ror(OpIndex node) {
  VisitRRO(this, kPPC_RotRight32, node, kShift32Imm);
}

// TODO(mbrandy): Absorb logical-and into rldic?
void InstructionSelector::VisitWord64Ror(OpIndex node) {
  VisitRRO(this, kPPC_RotRight64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Clz(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_Cntlz32, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord64Clz(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_Cntlz64, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord32Popcnt(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_Popcnt32, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord64Popcnt(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_Popcnt64, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord32Ctz(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitWord64Ctz(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitWord32ReverseBits(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitWord64ReverseBits(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitWord64ReverseBytes(OpIndex node) {
  PPCOperandGenerator g(this);
  InstructionOperand temp[] = {g.TempRegister()};
  OpIndex input = this->Get(node).input(0);
  const Operation& input_op = this->Get(input);
  if (CanCover(node, input) && input_op.Is<LoadOp>()) {
    auto load = load_view(input);
    LoadRepresentation load_rep = load.loaded_rep();
    if (load_rep.representation() == MachineRepresentation::kWord64) {
      OpIndex base = load.base();
      OpIndex offset = load.index();
      bool is_atomic = load.is_atomic();
      Emit(kPPC_LoadByteRev64 | AddressingModeField::encode(kMode_MRR),
           g.DefineAsRegister(node), g.UseRegister(base), g.UseRegister(offset),
           g.UseImmediate(is_atomic));
      return;
    }
  }
  const Operation& op = this->Get(node);
  Emit(kPPC_ByteRev64, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)), 1, temp);
}

void InstructionSelector::VisitWord32ReverseBytes(OpIndex node) {
  PPCOperandGenerator g(this);
  OpIndex input = this->Get(node).input(0);
  const Operation& input_op = this->Get(input);
  if (CanCover(node, input) && input_op.Is<LoadOp>()) {
    auto load = load_view(input);
    LoadRepresentation load_rep = load.loaded_rep();
    if (load_rep.representation() == MachineRepresentation::kWord32) {
      OpIndex base = load.base();
      OpIndex offset = load.index();
      bool is_atomic = load.is_atomic();
      Emit(kPPC_LoadByteRev32 | AddressingModeField::encode(kMode_MRR),
           g.DefineAsRegister(node), g.UseRegister(base), g.UseRegister(offset),
           g.UseImmediate(is_atomic));
      return;
    }
  }
  const Operation& op = this->Get(node);
  Emit(kPPC_ByteRev32, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)));
}

void InstructionSelector::VisitSimd128ReverseBytes(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_LoadReverseSimd128RR, g.DefineAsRegister(node),
       g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitInt32Add(OpIndex node) {
  VisitBinop(this, node, kPPC_Add32, kInt16Imm);
}

void InstructionSelector::VisitInt64Add(OpIndex node) {
  VisitBinop(this, node, kPPC_Add64, kInt16Imm);
}

void InstructionSelector::VisitInt32Sub(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& sub = this->Get(node).template Cast<WordBinopOp>();
  if (this->MatchIntegralZero(sub.left())) {
    Emit(kPPC_Neg, g.DefineAsRegister(node), g.UseRegister(sub.right()));
  } else {
    VisitBinop(this, node, kPPC_Sub, kInt16Imm_Negate);
  }
}

void InstructionSelector::VisitInt64Sub(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& sub = this->Get(node).template Cast<WordBinopOp>();
  if (this->MatchIntegralZero(sub.left())) {
    Emit(kPPC_Neg, g.DefineAsRegister(node), g.UseRegister(sub.right()));
  } else {
    VisitBinop(this, node, kPPC_Sub, kInt16Imm_Negate);
  }
}

namespace {

void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  InstructionOperand left, InstructionOperand right,
                  FlagsContinuation* cont);
void EmitInt32MulWithOverflow(InstructionSelector* selector, OpIndex node,
                              FlagsContinuation* cont) {
  PPCOperandGenerator g(selector);
  const OverflowCheckedBinopOp& op =
      selector->Get(node).Cast<OverflowCheckedBinopOp>();
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  InstructionOperand result_operand = g.DefineAsRegister(node);
  InstructionOperand high32_operand = g.TempRegister();
  InstructionOperand temp_operand = g.TempRegister();
  {
    InstructionOperand outputs[] = {result_operand, high32_operand};
    InstructionOperand inputs[] = {g.UseRegister(lhs), g.UseRegister(rhs)};
    selector->Emit(kPPC_Mul32WithHigh32, 2, outputs, 2, inputs);
  }
  {
    InstructionOperand shift_31 = g.UseImmediate(31);
    InstructionOperand outputs[] = {temp_operand};
    InstructionOperand inputs[] = {result_operand, shift_31};
    selector->Emit(kPPC_ShiftRightAlg32, 1, outputs, 2, inputs);
  }

  VisitCompare(selector, kPPC_Cmp32, high32_operand, temp_operand, cont);
}

void EmitInt64MulWithOverflow(InstructionSelector* selector, OpIndex node,
                              FlagsContinuation* cont) {
  PPCOperandGenerator g(selector);
  const OverflowCheckedBinopOp& op =
      selector->Cast<OverflowCheckedBinopOp>(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  InstructionOperand result = g.DefineAsRegister(node);
  InstructionOperand left = g.UseRegister(lhs);
  InstructionOperand high = g.TempRegister();
  InstructionOperand result_sign = g.TempRegister();
  InstructionOperand right = g.UseRegister(rhs);
  selector->Emit(kPPC_Mul64, result, left, right);
  selector->Emit(kPPC_MulHighS64, high, left, right);
  selector->Emit(kPPC_ShiftRightAlg64, result_sign, result,
                 g.TempImmediate(63));
  // Test whether {high} is a sign-extension of {result}.
  selector->EmitWithContinuation(kPPC_Cmp64, high, result_sign, cont);
}

}  // namespace

void InstructionSelector::VisitInt32Mul(OpIndex node) {
  VisitRRR(this, kPPC_Mul32, node);
}

void InstructionSelector::VisitInt64Mul(OpIndex node) {
  VisitRRR(this, kPPC_Mul64, node);
}

void InstructionSelector::VisitInt32MulHigh(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kPPC_MulHigh32, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitUint32MulHigh(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kPPC_MulHighU32, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitInt64MulHigh(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kPPC_MulHighS64, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitUint64MulHigh(OpIndex node) {
  PPCOperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kPPC_MulHighU64, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitInt32Div(OpIndex node) {
  VisitRRR(this, kPPC_Div32, node);
}

void InstructionSelector::VisitInt64Div(OpIndex node) {
  VisitRRR(this, kPPC_Div64, node);
}

void InstructionSelector::VisitUint32Div(OpIndex node) {
  VisitRRR(this, kPPC_DivU32, node);
}

void InstructionSelector::VisitUint64Div(OpIndex node) {
  VisitRRR(this, kPPC_DivU64, node);
}

void InstructionSelector::VisitInt32Mod(OpIndex node) {
  VisitRRR(this, kPPC_Mod32, node);
}

void InstructionSelector::VisitInt64Mod(OpIndex node) {
  VisitRRR(this, kPPC_Mod64, node);
}

void InstructionSelector::VisitUint32Mod(OpIndex node) {
  VisitRRR(this, kPPC_ModU32, node);
}

void InstructionSelector::VisitUint64Mod(OpIndex node) {
  VisitRRR(this, kPPC_ModU64, node);
}

void InstructionSelector::VisitChangeFloat32ToFloat64(OpIndex node) {
  VisitRR(this, kPPC_Float32ToDouble, node);
}

void InstructionSelector::VisitRoundInt32ToFloat32(OpIndex node) {
  VisitRR(this, kPPC_Int32ToFloat32, node);
}

void InstructionSelector::VisitRoundUint32ToFloat32(OpIndex node) {
  VisitRR(this, kPPC_Uint32ToFloat32, node);
}

void InstructionSelector::VisitChangeInt32ToFloat64(OpIndex node) {
  VisitRR(this, kPPC_Int32ToDouble, node);
}

void InstructionSelector::VisitChangeUint32ToFloat64(OpIndex node) {
  VisitRR(this, kPPC_Uint32ToDouble, node);
}

void InstructionSelector::VisitChangeFloat64ToInt32(OpIndex node) {
  VisitRR(this, kPPC_DoubleToInt32, node);
}

void InstructionSelector::VisitChangeFloat64ToUint32(OpIndex node) {
  VisitRR(this, kPPC_DoubleToUint32, node);
}

void InstructionSelector::VisitTruncateFloat64ToUint32(OpIndex node) {
  VisitRR(this, kPPC_DoubleToUint32, node);
}

void InstructionSelector::VisitSignExtendWord8ToInt32(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_ExtendSignWord8, node);
}

void InstructionSelector::VisitSignExtendWord16ToInt32(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_ExtendSignWord16, node);
}

void InstructionSelector::VisitTryTruncateFloat32ToInt64(OpIndex node) {
  VisitTryTruncateDouble(this, kPPC_DoubleToInt64, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToInt64(OpIndex node) {
  VisitTryTruncateDouble(this, kPPC_DoubleToInt64, node);
}

void InstructionSelector::VisitTruncateFloat64ToInt64(OpIndex node) {
  VisitRR(this, kPPC_DoubleToInt64, node);
}

void InstructionSelector::VisitTryTruncateFloat32ToUint64(OpIndex node) {
  VisitTryTruncateDouble(this, kPPC_DoubleToUint64, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToUint64(OpIndex node) {
  VisitTryTruncateDouble(this, kPPC_DoubleToUint64, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToInt32(OpIndex node) {
  VisitTryTruncateDouble(this, kPPC_DoubleToInt32, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToUint32(OpIndex node) {
  VisitTryTruncateDouble(this, kPPC_DoubleToUint32, node);
}

void InstructionSelector::VisitBitcastWord32ToWord64(OpIndex node) {
  DCHECK(SmiValuesAre31Bits());
  DCHECK(COMPRESS_POINTERS_BOOL);
  EmitIdentity(node);
}

void InstructionSelector::VisitChangeInt32ToInt64(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_ExtendSignWord32, node);
}

void InstructionSelector::VisitSignExtendWord8ToInt64(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_ExtendSignWord8, node);
}

void InstructionSelector::VisitSignExtendWord16ToInt64(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_ExtendSignWord16, node);
}

void InstructionSelector::VisitSignExtendWord32ToInt64(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_ExtendSignWord32, node);
}

bool InstructionSelector::ZeroExtendsWord32ToWord64NoPhis(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitChangeUint32ToUint64(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_Uint32ToUint64, node);
}

void InstructionSelector::VisitTruncateFloat64ToFloat16RawBits(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitChangeFloat16RawBitsToFloat64(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitChangeFloat64ToUint64(OpIndex node) {
  VisitRR(this, kPPC_DoubleToUint64, node);
}

void InstructionSelector::VisitChangeFloat64ToInt64(OpIndex node) {
  VisitRR(this, kPPC_DoubleToInt64, node);
}

void InstructionSelector::VisitTruncateFloat64ToFloat32(OpIndex node) {
  VisitRR(this, kPPC_DoubleToFloat32, node);
}

void InstructionSelector::VisitTruncateFloat64ToWord32(OpIndex node) {
  VisitRR(this, kArchTruncateDoubleToI, node);
}

void InstructionSelector::VisitRoundFloat64ToInt32(OpIndex node) {
  VisitRR(this, kPPC_DoubleToInt32, node);
}

void InstructionSelector::VisitTruncateFloat32ToInt32(OpIndex node) {
  PPCOperandGenerator g(this);
  const ChangeOp& op = Cast<ChangeOp>(node);
  InstructionCode opcode = kPPC_Float32ToInt32;
  if (op.Is<Opmask::kTruncateFloat32ToInt32OverflowToMin>()) {
    opcode |= MiscField::encode(true);
  }
  Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input()));
}

void InstructionSelector::VisitTruncateFloat32ToUint32(OpIndex node) {
  PPCOperandGenerator g(this);
  const ChangeOp& op = Cast<ChangeOp>(node);
  InstructionCode opcode = kPPC_Float32ToUint32;
  if (op.Is<Opmask::kTruncateFloat32ToUint32OverflowToMin>()) {
    opcode |= MiscField::encode(true);
  }

  Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input()));
}

void InstructionSelector::VisitTruncateInt64ToInt32(OpIndex node) {
  // TODO(mbrandy): inspect input to see if nop is appropriate.
  VisitRR(this, kPPC_Int64ToInt32, node);
}

void InstructionSelector::VisitRoundInt64ToFloat32(OpIndex node) {
  VisitRR(this, kPPC_Int64ToFloat32, node);
}

void InstructionSelector::VisitRoundInt64ToFloat64(OpIndex node) {
  VisitRR(this, kPPC_Int64ToDouble, node);
}

void InstructionSelector::VisitChangeInt64ToFloat64(OpIndex node) {
  VisitRR(this, kPPC_Int64ToDouble, node);
}

void InstructionSelector::VisitRoundUint64ToFloat32(OpIndex node) {
  VisitRR(this, kPPC_Uint64ToFloat32, node);
}

void InstructionSelector::VisitRoundUint64ToFloat64(OpIndex node) {
  VisitRR(this, kPPC_Uint64ToDouble, node);
}

void InstructionSelector::VisitBitcastFloat32ToInt32(OpIndex node) {
  VisitRR(this, kPPC_BitcastFloat32ToInt32, node);
}

void InstructionSelector::VisitBitcastFloat64ToInt64(OpIndex node) {
  VisitRR(this, kPPC_BitcastDoubleToInt64, node);
}

void InstructionSelector::VisitBitcastInt32ToFloat32(OpIndex node) {
  VisitRR(this, kPPC_BitcastInt32ToFloat32, node);
}

void InstructionSelector::VisitBitcastInt64ToFloat64(OpIndex node) {
  VisitRR(this, kPPC_BitcastInt64ToDouble, node);
}

void InstructionSelector::VisitFloat32Add(OpIndex node) {
  VisitRRR(this, kPPC_AddDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Add(OpIndex node) {
  // TODO(mbrandy): detect multiply-add
  VisitRRR(this, kPPC_AddDouble, node);
}

void InstructionSelector::VisitFloat32Sub(OpIndex node) {
  VisitRRR(this, kPPC_SubDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Sub(OpIndex node) {
  // TODO(mbrandy): detect multiply-subtract
  VisitRRR(this, kPPC_SubDouble, node);
}

void InstructionSelector::VisitFloat32Mul(OpIndex node) {
  VisitRRR(this, kPPC_MulDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Mul(OpIndex node) {
  // TODO(mbrandy): detect negate
  VisitRRR(this, kPPC_MulDouble, node);
}

void InstructionSelector::VisitFloat32Div(OpIndex node) {
  VisitRRR(this, kPPC_DivDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Div(OpIndex node) {
  VisitRRR(this, kPPC_DivDouble, node);
}

void InstructionSelector::VisitFloat64Mod(OpIndex node) {
  PPCOperandGenerator g(this);
  const FloatBinopOp& op = Cast<FloatBinopOp>(node);
  Emit(kPPC_ModDouble, g.DefineAsFixed(node, d1), g.UseFixed(op.input(0), d1),
       g.UseFixed(op.input(1), d2))
      ->MarkAsCall();
}

void InstructionSelector::VisitFloat32Max(OpIndex node) {
  VisitRRR(this, kPPC_MaxDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Max(OpIndex node) {
  VisitRRR(this, kPPC_MaxDouble, node);
}

void InstructionSelector::VisitFloat64SilenceNaN(OpIndex node) {
  VisitRR(this, kPPC_Float64SilenceNaN, node);
}

void InstructionSelector::VisitFloat32Min(OpIndex node) {
  VisitRRR(this, kPPC_MinDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Min(OpIndex node) {
  VisitRRR(this, kPPC_MinDouble, node);
}

void InstructionSelector::VisitFloat32Abs(OpIndex node) {
  VisitRR(this, kPPC_AbsDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Abs(OpIndex node) {
  VisitRR(this, kPPC_AbsDouble, node);
}

void InstructionSelector::VisitFloat32Sqrt(OpIndex node) {
  VisitRR(this, kPPC_SqrtDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Ieee754Unop(OpIndex node,
                                                  InstructionCode opcode) {
  PPCOperandGenerator g(this);
  const FloatUnaryOp& op = Cast<FloatUnaryOp>(node);
  Emit(opcode, g.DefineAsFixed(node, d1), g.UseFixed(op.input(), d1))
      ->MarkAsCall();
}

void InstructionSelector::VisitFloat64Ieee754Binop(OpIndex node,
                                                   InstructionCode opcode) {
  PPCOperandGenerator g(this);
  const FloatBinopOp& op = Cast<FloatBinopOp>(node);
  Emit(opcode, g.DefineAsFixed(node, d1), g.UseFixed(op.input(0), d1),
       g.UseFixed(op.input(1), d2))
      ->MarkAsCall();
}

void InstructionSelector::VisitFloat64Sqrt(OpIndex node) {
  VisitRR(this, kPPC_SqrtDouble, node);
}

void InstructionSelector::VisitFloat32RoundDown(OpIndex node) {
  VisitRR(this, kPPC_FloorDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64RoundDown(OpIndex node) {
  VisitRR(this, kPPC_FloorDouble, node);
}

void InstructionSelector::VisitFloat32RoundUp(OpIndex node) {
  VisitRR(this, kPPC_CeilDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64RoundUp(OpIndex node) {
  VisitRR(this, kPPC_CeilDouble, node);
}

void InstructionSelector::VisitFloat32RoundTruncate(OpIndex node) {
  VisitRR(this, kPPC_TruncateDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64RoundTruncate(OpIndex node) {
  VisitRR(this, kPPC_TruncateDouble, node);
}

void InstructionSelector::VisitFloat64RoundTiesAway(OpIndex node) {
  VisitRR(this, kPPC_RoundDouble, node);
}

void InstructionSelector::VisitFloat32Neg(OpIndex node) {
  VisitRR(this, kPPC_NegDouble, node);
}

void InstructionSelector::VisitFloat64Neg(OpIndex node) {
  VisitRR(this, kPPC_NegDouble, node);
}

void InstructionSelector::VisitInt32AddWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kPPC_AddWithOverflow32, kInt16Imm, &cont);
  }
    FlagsContinuation cont;
    VisitBinop(this, node, kPPC_AddWithOverflow32, kInt16Imm, &cont);
}

void InstructionSelector::VisitInt32SubWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kPPC_SubWithOverflow32, kInt16Imm_Negate,
                      &cont);
  }
    FlagsContinuation cont;
    VisitBinop(this, node, kPPC_SubWithOverflow32, kInt16Imm_Negate, &cont);
}

void InstructionSelector::VisitInt64AddWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kPPC_Add64, kInt16Imm, &cont);
  }
    FlagsContinuation cont;
    VisitBinop(this, node, kPPC_Add64, kInt16Imm, &cont);
}

void InstructionSelector::VisitInt64SubWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kPPC_Sub, kInt16Imm_Negate, &cont);
  }
    FlagsContinuation cont;
    VisitBinop(this, node, kPPC_Sub, kInt16Imm_Negate, &cont);
}

void InstructionSelector::VisitInt64MulWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kNotEqual, ovf.value());
    return EmitInt64MulWithOverflow(this, node, &cont);
  }
    FlagsContinuation cont;
    EmitInt64MulWithOverflow(this, node, &cont);
}

static bool CompareLogical(FlagsContinuation* cont) {
  switch (cont->condition()) {
    case kUnsignedLessThan:
    case kUnsignedGreaterThanOrEqual:
    case kUnsignedLessThanOrEqual:
    case kUnsignedGreaterThan:
      return true;
    default:
      return false;
  }
  UNREACHABLE();
}

namespace {

// Shared routine for multiple compare operations.
void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  InstructionOperand left, InstructionOperand right,
                  FlagsContinuation* cont) {
  selector->EmitWithContinuation(opcode, left, right, cont);
}

// Shared routine for multiple word compare operations.
void VisitWordCompare(InstructionSelector* selector, OpIndex node,
                      InstructionCode opcode, FlagsContinuation* cont,
                      bool commutative, ImmediateMode immediate_mode) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);

  // Match immediates on left or right side of comparison.
  if (g.CanBeImmediate(rhs, immediate_mode)) {
    VisitCompare(selector, opcode, g.UseRegister(lhs), g.UseImmediate(rhs),
                 cont);
  } else if (g.CanBeImmediate(lhs, immediate_mode)) {
    if (!commutative) cont->Commute();
    VisitCompare(selector, opcode, g.UseRegister(rhs), g.UseImmediate(lhs),
                 cont);
  } else {
    VisitCompare(selector, opcode, g.UseRegister(lhs), g.UseRegister(rhs),
                 cont);
  }
}

void VisitWord32Compare(InstructionSelector* selector, OpIndex node,
                        FlagsContinuation* cont) {
  ImmediateMode mode = (CompareLogical(cont) ? kInt16Imm_Unsigned : kInt16Imm);
  VisitWordCompare(selector, node, kPPC_Cmp32, cont, false, mode);
}

void VisitWord64Compare(InstructionSelector* selector, OpIndex node,
                        FlagsContinuation* cont) {
  ImmediateMode mode = (CompareLogical(cont) ? kInt16Imm_Unsigned : kInt16Imm);
  VisitWordCompare(selector, node, kPPC_Cmp64, cont, false, mode);
}

// Shared routine for multiple float32 compare operations.
void VisitFloat32Compare(InstructionSelector* selector, OpIndex node,
                         FlagsContinuation* cont) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  VisitCompare(selector, kPPC_CmpDouble, g.UseRegister(lhs), g.UseRegister(rhs),
               cont);
}

// Shared routine for multiple float64 compare operations.
void VisitFloat64Compare(InstructionSelector* selector, OpIndex node,
                         FlagsContinuation* cont) {
  PPCOperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  VisitCompare(selector, kPPC_CmpDouble, g.UseRegister(lhs), g.UseRegister(rhs),
               cont);
}

}  // namespace

void InstructionSelector::VisitWordCompareZero(OpIndex user, OpIndex value,
                                               FlagsContinuation* cont) {
  // Try to combine with comparisons against 0 by simply inverting the branch.
  ConsumeEqualZero(&user, &value, cont);

  if (CanCover(user, value)) {
    const Operation& value_op = Get(value);
    if (const ComparisonOp* comparison = value_op.TryCast<ComparisonOp>()) {
      switch (comparison->rep.MapTaggedToWord().value()) {
        case RegisterRepresentation::Word32():
          cont->OverwriteAndNegateIfEqual(
              GetComparisonFlagCondition(*comparison));
          return VisitWord32Compare(this, value, cont);
        case RegisterRepresentation::Word64():
          cont->OverwriteAndNegateIfEqual(
              GetComparisonFlagCondition(*comparison));
          return VisitWord64Compare(this, value, cont);
        case RegisterRepresentation::Float32():
          switch (comparison->kind) {
            case ComparisonOp::Kind::kEqual:
              cont->OverwriteAndNegateIfEqual(kEqual);
              return VisitFloat32Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThan:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
              return VisitFloat32Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThanOrEqual:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
              return VisitFloat32Compare(this, value, cont);
            default:
              UNREACHABLE();
          }
        case RegisterRepresentation::Float64():
          switch (comparison->kind) {
            case ComparisonOp::Kind::kEqual:
              cont->OverwriteAndNegateIfEqual(kEqual);
              return VisitFloat64Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThan:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
              return VisitFloat64Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThanOrEqual:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
              return VisitFloat64Compare(this, value, cont);
            default:
              UNREACHABLE();
          }
        default:
          break;
      }
    } else if (const ProjectionOp* projection =
                   value_op.TryCast<ProjectionOp>()) {
      // Check if this is the overflow output projection of an
      // <Operation>WithOverflow node.
      if (projection->index == 1u) {
        // We cannot combine the <Operation>WithOverflow with this branch
        // unless the 0th projection (the use of the actual value of the
        // <Operation> is either nullptr, which means there's no use of the
        // actual value, or was already defined, which means it is scheduled
        // *AFTER* this branch).
        OpIndex node = projection->input();
        if (const OverflowCheckedBinopOp* binop =
                TryCast<OverflowCheckedBinopOp>(node);
            binop && CanDoBranchIfOverflowFusion(node)) {
          const bool is64 = binop->rep == WordRepresentation::Word64();
          switch (binop->kind) {
            case OverflowCheckedBinopOp::Kind::kSignedAdd:
              cont->OverwriteAndNegateIfEqual(kOverflow);
              return VisitBinop(this, node,
                                is64 ? kPPC_Add64 : kPPC_AddWithOverflow32,
                                kInt16Imm, cont);
            case OverflowCheckedBinopOp::Kind::kSignedSub:
              cont->OverwriteAndNegateIfEqual(kOverflow);
              return VisitBinop(this, node,
                                is64 ? kPPC_Sub : kPPC_SubWithOverflow32,
                                kInt16Imm_Negate, cont);
            case OverflowCheckedBinopOp::Kind::kSignedMul:
              if (is64) {
                cont->OverwriteAndNegateIfEqual(kNotEqual);
                return EmitInt64MulWithOverflow(this, node, cont);
              } else {
                cont->OverwriteAndNegateIfEqual(kNotEqual);
                return EmitInt32MulWithOverflow(this, node, cont);
              }
          }
        }
      }
    } else if (value_op.Is<Opmask::kWord32Sub>()) {
      return VisitWord32Compare(this, value, cont);
    } else if (value_op.Is<Opmask::kWord32BitwiseAnd>()) {
      return VisitWordCompare(this, value, kPPC_Tst32, cont, true,
                              kInt16Imm_Unsigned);
    } else if (value_op.Is<Opmask::kWord64Sub>()) {
      return VisitWord64Compare(this, value, cont);
    } else if (value_op.Is<Opmask::kWord64BitwiseAnd>()) {
      return VisitWordCompare(this, value, kPPC_Tst64, cont, true,
                              kInt16Imm_Unsigned);
    } else if (value_op.Is<StackPointerGreaterThanOp>()) {
      cont->OverwriteAndNegateIfEqual(kStackPointerGreaterThanCondition);
      return VisitStackPointerGreaterThan(value, cont);
    }
  }

  // Branch could not be combined with a compare, emit compare against 0.
  PPCOperandGenerator g(this);
  VisitCompare(this, kPPC_Cmp32, g.UseRegister(value), g.TempImmediate(0),
               cont);
}

void InstructionSelector::VisitSwitch(OpIndex node, const SwitchInfo& sw) {
  PPCOperandGenerator g(this);
  const SwitchOp& op = Cast<SwitchOp>(node);
  InstructionOperand value_operand = g.UseRegister(op.input());

  // Emit either ArchTableSwitch or ArchBinarySearchSwitch.
  if (enable_switch_jump_table_) {
    static const size_t kMaxTableSwitchValueRange = 2 << 16;
    size_t table_space_cost = 4 + sw.value_range();
    size_t table_time_cost = 3;
    size_t lookup_space_cost = 3 + 2 * sw.case_count();
    size_t lookup_time_cost = sw.case_count();
    if (sw.case_count() > 0 &&
        table_space_cost + 3 * table_time_cost <=
            lookup_space_cost + 3 * lookup_time_cost &&
        sw.min_value() > std::numeric_limits<int32_t>::min() &&
        sw.value_range() <= kMaxTableSwitchValueRange) {
      InstructionOperand index_operand = value_operand;
      if (sw.min_value()) {
      index_operand = g.TempRegister();
      Emit(kPPC_Sub, index_operand, value_operand,
           g.TempImmediate(sw.min_value()));
      }
      // Zero extend, because we use it as 64-bit index into the jump table.
      InstructionOperand index_operand_zero_ext = g.TempRegister();
      Emit(kPPC_Uint32ToUint64, index_operand_zero_ext, index_operand);
      index_operand = index_operand_zero_ext;
      // Generate a table lookup.
      return EmitTableSwitch(sw, index_operand);
    }
  }

  // Generate a tree of conditional jumps.
  return EmitBinarySearchSwitch(sw, value_operand);
}

void InstructionSelector::VisitWord32Equal(OpIndex const node) {
  const Operation& equal = Get(node);
  DCHECK(equal.Is<ComparisonOp>());
  OpIndex left = equal.input(0);
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  if (isolate() && (V8_STATIC_ROOTS_BOOL ||
                    (COMPRESS_POINTERS_BOOL && !isolate()->bootstrapper()))) {
    PPCOperandGenerator g(this);
    const RootsTable& roots_table = isolate()->roots_table();
    RootIndex root_index;
    Handle<HeapObject> right;
    // HeapConstants and CompressedHeapConstants can be treated the same when
    // using them as an input to a 32-bit comparison. Check whether either is
    // present.
    if (MatchHeapConstant(node, &right) && !right.is_null() &&
        roots_table.IsRootHandle(right, &root_index)) {
      if (RootsTable::IsReadOnly(root_index)) {
        Tagged_t ptr =
            MacroAssemblerBase::ReadOnlyRootPtr(root_index, isolate());
        if (g.CanBeImmediate(ptr, kInt16Imm)) {
          return VisitCompare(this, kPPC_Cmp32, g.UseRegister(left),
                              g.TempImmediate(ptr), &cont);
        }
      }
    }
  }
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitInt32LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitInt32LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitUint32LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitUint32LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitWord64Equal(OpIndex const node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitInt64LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitInt64LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitUint64LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitUint64LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitInt32MulWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kNotEqual, ovf.value());
    return EmitInt32MulWithOverflow(this, node, &cont);
  }
  FlagsContinuation cont;
  EmitInt32MulWithOverflow(this, node, &cont);
}

void InstructionSelector::VisitFloat32Equal(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat32LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat32LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64Equal(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::EmitMoveParamToFPR(OpIndex node, int index) {}

void InstructionSelector::EmitMoveFPRToParam(InstructionOperand* op,
                                             LinkageLocation location) {}

void InstructionSelector::EmitPrepareArguments(
    ZoneVector<PushParameter>* arguments, const CallDescriptor* call_descriptor,
    OpIndex node) {
  PPCOperandGenerator g(this);

  // Prepare for C function call.
  if (call_descriptor->IsCFunctionCall()) {
    Emit(kArchPrepareCallCFunction | MiscField::encode(static_cast<int>(
                                         call_descriptor->ParameterCount())),
         0, nullptr, 0, nullptr);

    // Poke any stack arguments.
    int slot = kStackFrameExtraParamSlot;
    for (PushParameter input : (*arguments)) {
      if (!input.node.valid()) continue;
      Emit(kPPC_StoreToStackSlot, g.NoOutput(), g.UseRegister(input.node),
           g.TempImmediate(slot));
      ++slot;
    }
  } else {
    // Push any stack arguments.
    int stack_decrement = 0;
    for (PushParameter input : base::Reversed(*arguments)) {
      stack_decrement += kSystemPointerSize;
      // Skip any alignment holes in pushed nodes.
      if (!input.node.valid()) continue;
      InstructionOperand decrement = g.UseImmediate(stack_decrement);
      stack_decrement = 0;
      Emit(kPPC_Push, g.NoOutput(), decrement, g.UseRegister(input.node));
    }
  }
}

bool InstructionSelector::IsTailCallAddressImmediate() { return false; }

void InstructionSelector::VisitFloat64ExtractLowWord32(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_DoubleExtractLowWord32, g.DefineAsRegister(node),
       g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitFloat64ExtractHighWord32(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_DoubleExtractHighWord32, g.DefineAsRegister(node),
       g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitBitcastWord32PairToFloat64(OpIndex node) {
  PPCOperandGenerator g(this);
  const auto& bitcast = this->Cast<BitcastWord32PairToFloat64Op>(node);
  OpIndex hi = bitcast.high_word32();
  OpIndex lo = bitcast.low_word32();

  InstructionOperand temps[] = {g.TempRegister()};
  Emit(kPPC_DoubleFromWord32Pair, g.DefineAsRegister(node), g.UseRegister(hi),
       g.UseRegister(lo), arraysize(temps), temps);
}

void InstructionSelector::VisitFloat64InsertLowWord32(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitFloat64InsertHighWord32(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitMemoryBarrier(OpIndex node) {
  PPCOperandGenerator g(this);
  Emit(kPPC_Sync, g.NoOutput());
}

void InstructionSelector::VisitWord32AtomicLoad(OpIndex node) {
  auto load = load_view(node);
  ImmediateMode mode;
  InstructionCode opcode = SelectLoadOpcode(load.loaded_rep(), &mode);
  VisitLoadCommon(this, node, mode, opcode);
}

void InstructionSelector::VisitWord64AtomicLoad(OpIndex node) {
  auto load = load_view(node);
  ImmediateMode mode;
  InstructionCode opcode = SelectLoadOpcode(load.loaded_rep(), &mode);
  VisitLoadCommon(this, node, mode, opcode);
}

void InstructionSelector::VisitWord32AtomicStore(OpIndex node) {
  auto store = store_view(node);
  AtomicStoreParameters store_params(store.stored_rep().representation(),
                                     store.stored_rep().write_barrier_kind(),
                                     store.memory_order().value(),
                                     store.access_kind());
  VisitStoreCommon(this, node, store_params.store_representation(),
                   store_params.order());
}

void InstructionSelector::VisitWord64AtomicStore(OpIndex node) {
  auto store = store_view(node);
  AtomicStoreParameters store_params(store.stored_rep().representation(),
                                     store.stored_rep().write_barrier_kind(),
                                     store.memory_order().value(),
                                     store.access_kind());
  VisitStoreCommon(this, node, store_params.store_representation(),
                   store_params.order());
}

void VisitAtomicExchange(InstructionSelector* selector, OpIndex node,
                         ArchOpcode opcode) {
  PPCOperandGenerator g(selector);
  const AtomicRMWOp& atomic_op = selector->Cast<AtomicRMWOp>(node);
  OpIndex base = atomic_op.base();
  OpIndex index = atomic_op.index();
  OpIndex value = atomic_op.value();

  AddressingMode addressing_mode = kMode_MRR;
  InstructionOperand inputs[3];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);
  InstructionOperand outputs[1];
  outputs[0] = g.UseUniqueRegister(node);
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode);
  selector->Emit(code, 1, outputs, input_count, inputs);
}

void InstructionSelector::VisitWord32AtomicExchange(OpIndex node) {
  ArchOpcode opcode;
    const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
    if (atomic_op.memory_rep == MemoryRepresentation::Int8()) {
      opcode = kAtomicExchangeInt8;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
      opcode = kPPC_AtomicExchangeUint8;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Int16()) {
      opcode = kAtomicExchangeInt16;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
      opcode = kPPC_AtomicExchangeUint16;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Int32() ||
               atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
      opcode = kPPC_AtomicExchangeWord32;
    } else {
      UNREACHABLE();
    }
  VisitAtomicExchange(this, node, opcode);
}

void InstructionSelector::VisitWord64AtomicExchange(OpIndex node) {
  ArchOpcode opcode;
    const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
    if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
      opcode = kPPC_AtomicExchangeUint8;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
      opcode = kPPC_AtomicExchangeUint16;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
      opcode = kPPC_AtomicExchangeWord32;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint64()) {
      opcode = kPPC_AtomicExchangeWord64;
    } else {
      UNREACHABLE();
    }
  VisitAtomicExchange(this, node, opcode);
}

void VisitAtomicCompareExchange(InstructionSelector* selector, OpIndex node,
                                ArchOpcode opcode) {
  PPCOperandGenerator g(selector);
  const AtomicRMWOp& atomic_op = selector->Cast<AtomicRMWOp>(node);
  OpIndex base = atomic_op.base();
  OpIndex index = atomic_op.index();
  OpIndex old_value = atomic_op.expected().value();
  OpIndex new_value = atomic_op.value();

  AddressingMode addressing_mode = kMode_MRR;
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode);

  InstructionOperand inputs[4];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(old_value);
  inputs[input_count++] = g.UseUniqueRegister(new_value);

  InstructionOperand outputs[1];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  selector->Emit(code, output_count, outputs, input_count, inputs);
}

void InstructionSelector::VisitTaggedAtomicExchange(OpIndex node) {
  PPCOperandGenerator g(this);
  const AtomicRMWOp& atomic_op = Cast<AtomicRMWOp>(node);
  OpIndex base = atomic_op.base();
  OpIndex index = atomic_op.index();
  OpIndex value = atomic_op.value();

  AddressingMode addressing_mode = kMode_MRR;
  InstructionOperand inputs[3];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);
  InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
  size_t const temp_count = arraysize(temps);
  InstructionOperand outputs[1];
  outputs[0] = g.UseUniqueRegister(node);
  InstructionCode code = kAtomicExchangeWithWriteBarrier |
                         AddressingModeField::encode(addressing_mode);
  Emit(code, 1, outputs, input_count, inputs, temp_count, temps);
}

void InstructionSelector::VisitWord32AtomicCompareExchange(OpIndex node) {
  ArchOpcode opcode;
    const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
    if (atomic_op.memory_rep == MemoryRepresentation::Int8()) {
      opcode = kAtomicCompareExchangeInt8;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
      opcode = kPPC_AtomicCompareExchangeUint8;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Int16()) {
      opcode = kAtomicCompareExchangeInt16;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
      opcode = kPPC_AtomicCompareExchangeUint16;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Int32() ||
               atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
      opcode = kPPC_AtomicCompareExchangeWord32;
    } else {
      UNREACHABLE();
    }
  VisitAtomicCompareExchange(this, node, opcode);
}

void InstructionSelector::VisitWord64AtomicCompareExchange(OpIndex node) {
  ArchOpcode opcode;
    const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
    if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
      opcode = kPPC_AtomicCompareExchangeUint8;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
      opcode = kPPC_AtomicCompareExchangeUint16;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
      opcode = kPPC_AtomicCompareExchangeWord32;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint64()) {
      opcode = kPPC_AtomicCompareExchangeWord64;
    } else {
      UNREACHABLE();
    }
  VisitAtomicCompareExchange(this, node, opcode);
}

void VisitAtomicBinaryOperation(InstructionSelector* selector, OpIndex node,
                                ArchOpcode int8_op, ArchOpcode uint8_op,
                                ArchOpcode int16_op, ArchOpcode uint16_op,
                                ArchOpcode int32_op, ArchOpcode uint32_op,
                                ArchOpcode int64_op, ArchOpcode uint64_op) {
  PPCOperandGenerator g(selector);
  ArchOpcode opcode;
    const AtomicRMWOp& atomic_op =
        selector->Get(node).template Cast<AtomicRMWOp>();
    OpIndex base = atomic_op.base();
    OpIndex index = atomic_op.index();
    OpIndex value = atomic_op.value();
    if (atomic_op.memory_rep == MemoryRepresentation::Int8()) {
      opcode = int8_op;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
      opcode = uint8_op;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Int16()) {
      opcode = int16_op;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
      opcode = uint16_op;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Int32()) {
      opcode = int32_op;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
      opcode = uint32_op;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Int64()) {
      opcode = int64_op;
    } else if (atomic_op.memory_rep == MemoryRepresentation::Uint64()) {
      opcode = uint64_op;
    } else {
      UNREACHABLE();
    }

  AddressingMode addressing_mode = kMode_MRR;
  InstructionCode code = opcode | AddressingModeField::encode(addressing_mode);
  InstructionOperand inputs[3];

  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);

  InstructionOperand outputs[1];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  selector->Emit(code, output_count, outputs, input_count, inputs);
}

void InstructionSelector::VisitTaggedAtomicCompareExchange(OpIndex node) {
  VisitAtomicCompareExchange(this, node,
                             kAtomicCompareExchangeWithWriteBarrier);
}

void InstructionSelector::VisitWord32AtomicBinaryOperation(
    OpIndex node, ArchOpcode int8_op, ArchOpcode uint8_op, ArchOpcode int16_op,
    ArchOpcode uint16_op, ArchOpcode word32_op) {
  // Unused
  UNREACHABLE();
}

void InstructionSelector::VisitWord64AtomicBinaryOperation(
    OpIndex node, ArchOpcode uint8_op, ArchOpcode uint16_op,
    ArchOpcode uint32_op, ArchOpcode uint64_op) {
  // Unused
  UNREACHABLE();
}

#define VISIT_ATOMIC_BINOP(op)                                     \
  void InstructionSelector::VisitWord32Atomic##op(OpIndex node) {  \
    VisitAtomicBinaryOperation(                                    \
        this, node, kPPC_Atomic##op##Int8, kPPC_Atomic##op##Uint8, \
        kPPC_Atomic##op##Int16, kPPC_Atomic##op##Uint16,           \
        kPPC_Atomic##op##Int32, kPPC_Atomic##op##Uint32,           \
        kPPC_Atomic##op##Int64, kPPC_Atomic##op##Uint64);          \
  }                                                                \
  void InstructionSelector::VisitWord64Atomic##op(OpIndex node) {  \
    VisitAtomicBinaryOperation(                                    \
        this, node, kPPC_Atomic##op##Int8, kPPC_Atomic##op##Uint8, \
        kPPC_Atomic##op##Int16, kPPC_Atomic##op##Uint16,           \
        kPPC_Atomic##op##Int32, kPPC_Atomic##op##Uint32,           \
        kPPC_Atomic##op##Int64, kPPC_Atomic##op##Uint64);          \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

void InstructionSelector::VisitInt32AbsWithOverflow(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitInt64AbsWithOverflow(OpIndex node) {
  UNREACHABLE();
}

#define SIMD_TYPES(V) \
  V(F64x2)            \
  V(F32x4)            \
  V(I64x2)            \
  V(I32x4)            \
  V(I16x8)            \
  V(I8x16)

#define SIMD_BINOP_LIST(V) \
  V(F64x2Add)              \
  V(F64x2Sub)              \
  V(F64x2Mul)              \
  V(F64x2Eq)               \
  V(F64x2Ne)               \
  V(F64x2Le)               \
  V(F64x2Lt)               \
  V(F64x2Div)              \
  V(F64x2Min)              \
  V(F64x2Max)              \
  V(F64x2Pmin)             \
  V(F64x2Pmax)             \
  V(F32x4Add)              \
  V(F32x4Sub)              \
  V(F32x4Mul)              \
  V(F32x4Eq)               \
  V(F32x4Ne)               \
  V(F32x4Lt)               \
  V(F32x4Le)               \
  V(F32x4Div)              \
  V(F32x4Min)              \
  V(F32x4Max)              \
  V(F32x4Pmin)             \
  V(F32x4Pmax)             \
  V(I64x2Add)              \
  V(I64x2Sub)              \
  V(I64x2Mul)              \
  V(I64x2Eq)               \
  V(I64x2Ne)               \
  V(I64x2ExtMulLowI32x4S)  \
  V(I64x2ExtMulHighI32x4S) \
  V(I64x2ExtMulLowI32x4U)  \
  V(I64x2ExtMulHighI32x4U) \
  V(I64x2GtS)              \
  V(I64x2GeS)              \
  V(I64x2Shl)              \
  V(I64x2ShrS)             \
  V(I64x2ShrU)             \
  V(I32x4Add)              \
  V(I32x4Sub)              \
  V(I32x4Mul)              \
  V(I32x4MinS)             \
  V(I32x4MinU)             \
  V(I32x4MaxS)             \
  V(I32x4MaxU)             \
  V(I32x4Eq)               \
  V(I32x4Ne)               \
  V(I32x4GtS)              \
  V(I32x4GeS)              \
  V(I32x4GtU)              \
  V(I32x4GeU)              \
  V(I32x4DotI16x8S)        \
  V(I32x4ExtMulLowI16x8S)  \
  V(I32x4ExtMulHighI16x8S) \
  V(I32x4ExtMulLowI16x8U)  \
  V(I32x4ExtMulHighI16x8U) \
  V(I32x4Shl)              \
  V(I32x4ShrS)             \
  V(I32x4ShrU)             \
  V(I16x8Add)              \
  V(I16x8Sub)              \
  V(I16x8Mul)              \
  V(I16x8MinS)             \
  V(I16x8MinU)             \
  V(I16x8MaxS)             \
  V(I16x8MaxU)             \
  V(I16x8Eq)               \
  V(I16x8Ne)               \
  V(I16x8GtS)              \
  V(I16x8GeS)              \
  V(I16x8GtU)              \
  V(I16x8GeU)              \
  V(I16x8SConvertI32x4)    \
  V(I16x8UConvertI32x4)    \
  V(I16x8AddSatS)          \
  V(I16x8SubSatS)          \
  V(I16x8AddSatU)          \
  V(I16x8SubSatU)          \
  V(I16x8RoundingAverageU) \
  V(I16x8Q15MulRSatS)      \
  V(I16x8ExtMulLowI8x16S)  \
  V(I16x8ExtMulHighI8x16S) \
  V(I16x8ExtMulLowI8x16U)  \
  V(I16x8ExtMulHighI8x16U) \
  V(I16x8Shl)              \
  V(I16x8ShrS)             \
  V(I16x8ShrU)             \
  V(I8x16Add)              \
  V(I8x16Sub)              \
  V(I8x16MinS)             \
  V(I8x16MinU)             \
  V(I8x16MaxS)             \
  V(I8x16MaxU)             \
  V(I8x16Eq)               \
  V(I8x16Ne)               \
  V(I8x16GtS)              \
  V(I8x16GeS)              \
  V(I8x16GtU)              \
  V(I8x16GeU)              \
  V(I8x16SConvertI16x8)    \
  V(I8x16UConvertI16x8)    \
  V(I8x16AddSatS)          \
  V(I8x16SubSatS)          \
  V(I8x16AddSatU)          \
  V(I8x16SubSatU)          \
  V(I8x16RoundingAverageU) \
  V(I8x16Swizzle)          \
  V(I8x16Shl)              \
  V(I8x16ShrS)             \
  V(I8x16ShrU)             \
  V(S128And)               \
  V(S128Or)                \
  V(S128Xor)               \
  V(S128AndNot)

#define SIMD_UNOP_LIST(V)      \
  V(F64x2Abs)                  \
  V(F64x2Neg)                  \
  V(F64x2Sqrt)                 \
  V(F64x2Ceil)                 \
  V(F64x2Floor)                \
  V(F64x2Trunc)                \
  V(F64x2ConvertLowI32x4S)     \
  V(F64x2ConvertLowI32x4U)     \
  V(F64x2PromoteLowF32x4)      \
  V(F32x4Abs)                  \
  V(F32x4Neg)                  \
  V(F32x4Sqrt)                 \
  V(F32x4SConvertI32x4)        \
  V(F32x4UConvertI32x4)        \
  V(F32x4Ceil)                 \
  V(F32x4Floor)                \
  V(F32x4Trunc)                \
  V(F32x4DemoteF64x2Zero)      \
  V(I64x2Abs)                  \
  V(I64x2Neg)                  \
  V(I64x2SConvertI32x4Low)     \
  V(I64x2SConvertI32x4High)    \
  V(I64x2UConvertI32x4Low)     \
  V(I64x2UConvertI32x4High)    \
  V(I64x2AllTrue)              \
  V(I64x2BitMask)              \
  V(I32x4Neg)                  \
  V(I32x4Abs)                  \
  V(I32x4SConvertF32x4)        \
  V(I32x4UConvertF32x4)        \
  V(I32x4SConvertI16x8Low)     \
  V(I32x4SConvertI16x8High)    \
  V(I32x4UConvertI16x8Low)     \
  V(I32x4UConvertI16x8High)    \
  V(I32x4ExtAddPairwiseI16x8S) \
  V(I32x4ExtAddPairwiseI16x8U) \
  V(I32x4TruncSatF64x2SZero)   \
  V(I32x4TruncSatF64x2UZero)   \
  V(I32x4AllTrue)              \
  V(I32x4BitMask)              \
  V(I16x8Neg)                  \
  V(I16x8Abs)                  \
  V(I16x8AllTrue)              \
  V(I16x8BitMask)              \
  V(I8x16Neg)                  \
  V(I8x16Abs)                  \
  V(I8x16Popcnt)               \
  V(I8x16AllTrue)              \
  V(I8x16BitMask)              \
  V(I16x8SConvertI8x16Low)     \
  V(I16x8SConvertI8x16High)    \
  V(I16x8UConvertI8x16Low)     \
  V(I16x8UConvertI8x16High)    \
  V(I16x8ExtAddPairwiseI8x16S) \
  V(I16x8ExtAddPairwiseI8x16U) \
  V(S128Not)                   \
  V(V128AnyTrue)

#define SIMD_VISIT_SPLAT(Type, T, LaneSize)                     \
  void InstructionSelector::Visit##Type##Splat(OpIndex node) {  \
    PPCOperandGenerator g(this);                                \
    const Operation& op = this->Get(node);                      \
    Emit(kPPC_##T##Splat | LaneSizeField::encode(LaneSize),     \
         g.DefineAsRegister(node), g.UseRegister(op.input(0))); \
  }
SIMD_VISIT_SPLAT(F64x2, F, 64)
SIMD_VISIT_SPLAT(F32x4, F, 32)
SIMD_VISIT_SPLAT(I64x2, I, 64)
SIMD_VISIT_SPLAT(I32x4, I, 32)
SIMD_VISIT_SPLAT(I16x8, I, 16)
SIMD_VISIT_SPLAT(I8x16, I, 8)
#undef SIMD_VISIT_SPLAT

#define SIMD_VISIT_EXTRACT_LANE(Type, T, Sign, LaneSize)                   \
  void InstructionSelector::Visit##Type##ExtractLane##Sign(OpIndex node) { \
    PPCOperandGenerator g(this);                                           \
    int32_t lane;                                                          \
    const Operation& op = this->Get(node);                                 \
    lane = op.template Cast<Simd128ExtractLaneOp>().lane;                  \
    Emit(kPPC_##T##ExtractLane##Sign | LaneSizeField::encode(LaneSize),    \
         g.DefineAsRegister(node), g.UseRegister(op.input(0)),             \
         g.UseImmediate(lane));                                            \
  }
SIMD_VISIT_EXTRACT_LANE(F64x2, F, , 64)
SIMD_VISIT_EXTRACT_LANE(F32x4, F, , 32)
SIMD_VISIT_EXTRACT_LANE(I64x2, I, , 64)
SIMD_VISIT_EXTRACT_LANE(I32x4, I, , 32)
SIMD_VISIT_EXTRACT_LANE(I16x8, I, U, 16)
SIMD_VISIT_EXTRACT_LANE(I16x8, I, S, 16)
SIMD_VISIT_EXTRACT_LANE(I8x16, I, U, 8)
SIMD_VISIT_EXTRACT_LANE(I8x16, I, S, 8)
#undef SIMD_VISIT_EXTRACT_LANE

#define SIMD_VISIT_REPLACE_LANE(Type, T, LaneSize)                   \
  void InstructionSelector::Visit##Type##ReplaceLane(OpIndex node) { \
    PPCOperandGenerator g(this);                                     \
    int32_t lane;                                                    \
    const Operation& op = this->Get(node);                           \
    lane = op.template Cast<Simd128ReplaceLaneOp>().lane;            \
    Emit(kPPC_##T##ReplaceLane | LaneSizeField::encode(LaneSize),    \
         g.DefineSameAsFirst(node), g.UseRegister(op.input(0)),      \
         g.UseImmediate(lane), g.UseRegister(op.input(1)));          \
  }
SIMD_VISIT_REPLACE_LANE(F64x2, F, 64)
SIMD_VISIT_REPLACE_LANE(F32x4, F, 32)
SIMD_VISIT_REPLACE_LANE(I64x2, I, 64)
SIMD_VISIT_REPLACE_LANE(I32x4, I, 32)
SIMD_VISIT_REPLACE_LANE(I16x8, I, 16)
SIMD_VISIT_REPLACE_LANE(I8x16, I, 8)
#undef SIMD_VISIT_REPLACE_LANE

#define SIMD_VISIT_BINOP(Opcode)                                              \
  void InstructionSelector::Visit##Opcode(OpIndex node) {                     \
    PPCOperandGenerator g(this);                                              \
    const Operation& op = this->Get(node);                                    \
    InstructionOperand temps[] = {g.TempRegister()};                          \
    Emit(kPPC_##Opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)), \
         g.UseRegister(op.input(1)), arraysize(temps), temps);                \
  }
SIMD_BINOP_LIST(SIMD_VISIT_BINOP)
#undef SIMD_VISIT_BINOP
#undef SIMD_BINOP_LIST

#define SIMD_VISIT_UNOP(Opcode)                                                \
  void InstructionSelector::Visit##Opcode(OpIndex node) {                      \
    PPCOperandGenerator g(this);                                               \
    const Operation& op = this->Get(node);                                     \
    Emit(kPPC_##Opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0))); \
  }
SIMD_UNOP_LIST(SIMD_VISIT_UNOP)
#undef SIMD_VISIT_UNOP
#undef SIMD_UNOP_LIST

#define SIMD_VISIT_QFMOP(Opcode)                                               \
  void InstructionSelector::Visit##Opcode(OpIndex node) {                      \
    PPCOperandGenerator g(this);                                               \
    const Operation& op = this->Get(node);                                     \
    Emit(kPPC_##Opcode, g.DefineSameAsFirst(node), g.UseRegister(op.input(0)), \
         g.UseRegister(op.input(1)), g.UseRegister(op.input(2)));              \
  }
SIMD_VISIT_QFMOP(F64x2Qfma)
SIMD_VISIT_QFMOP(F64x2Qfms)
SIMD_VISIT_QFMOP(F32x4Qfma)
SIMD_VISIT_QFMOP(F32x4Qfms)
#undef SIMD_VISIT_QFMOP

#define SIMD_RELAXED_OP_LIST(V)                           \
  V(F64x2RelaxedMin, F64x2Pmin)                           \
  V(F64x2RelaxedMax, F64x2Pmax)                           \
  V(F32x4RelaxedMin, F32x4Pmin)                           \
  V(F32x4RelaxedMax, F32x4Pmax)                           \
  V(I32x4RelaxedTruncF32x4S, I32x4SConvertF32x4)          \
  V(I32x4RelaxedTruncF32x4U, I32x4UConvertF32x4)          \
  V(I32x4RelaxedTruncF64x2SZero, I32x4TruncSatF64x2SZero) \
  V(I32x4RelaxedTruncF64x2UZero, I32x4TruncSatF64x2UZero) \
  V(I16x8RelaxedQ15MulRS, I16x8Q15MulRSatS)               \
  V(I8x16RelaxedLaneSelect, S128Select)                   \
  V(I16x8RelaxedLaneSelect, S128Select)                   \
  V(I32x4RelaxedLaneSelect, S128Select)                   \
  V(I64x2RelaxedLaneSelect, S128Select)

#define SIMD_VISIT_RELAXED_OP(name, op) \
  void InstructionSelector::Visit##name(OpIndex node) { Visit##op(node); }
SIMD_RELAXED_OP_LIST(SIMD_VISIT_RELAXED_OP)
#undef SIMD_VISIT_RELAXED_OP
#undef SIMD_RELAXED_OP_LIST

#define F16_OP_LIST(V)    \
  V(F16x8Splat)           \
  V(F16x8ExtractLane)     \
  V(F16x8ReplaceLane)     \
  V(F16x8Abs)             \
  V(F16x8Neg)             \
  V(F16x8Sqrt)            \
  V(F16x8Floor)           \
  V(F16x8Ceil)            \
  V(F16x8Trunc)           \
  V(F16x8NearestInt)      \
  V(F16x8Add)             \
  V(F16x8Sub)             \
  V(F16x8Mul)             \
  V(F16x8Div)             \
  V(F16x8Min)             \
  V(F16x8Max)             \
  V(F16x8Pmin)            \
  V(F16x8Pmax)            \
  V(F16x8Eq)              \
  V(F16x8Ne)              \
  V(F16x8Lt)              \
  V(F16x8Le)              \
  V(F16x8SConvertI16x8)   \
  V(F16x8UConvertI16x8)   \
  V(I16x8SConvertF16x8)   \
  V(I16x8UConvertF16x8)   \
  V(F32x4PromoteLowF16x8) \
  V(F16x8DemoteF32x4Zero) \
  V(F16x8DemoteF64x2Zero) \
  V(F16x8Qfma)            \
  V(F16x8Qfms)

#define VISIT_F16_OP(name) \
  void InstructionSelector::Visit##name(OpIndex node) { UNIMPLEMENTED(); }
F16_OP_LIST(VISIT_F16_OP)
#undef VISIT_F16_OP
#undef F16_OP_LIST
#undef SIMD_TYPES

#if V8_ENABLE_WEBASSEMBLY
void InstructionSelector::VisitI8x16Shuffle(OpIndex node) {
  uint8_t shuffle[kSimd128Size];
  bool is_swizzle;
  // TODO(nicohartmann@): Properly use view here once Turboshaft support is
  // implemented.
  auto view = this->simd_shuffle_view(node);
  CanonicalizeShuffle(view, shuffle, &is_swizzle);
  PPCOperandGenerator g(this);
  OpIndex input0 = view.input(0);
  OpIndex input1 = view.input(1);
  // Remap the shuffle indices to match IBM lane numbering.
  int max_index = 15;
  int total_lane_count = 2 * kSimd128Size;
  uint8_t shuffle_remapped[kSimd128Size];
  for (int i = 0; i < kSimd128Size; i++) {
    uint8_t current_index = shuffle[i];
    shuffle_remapped[i] = (current_index <= max_index
                               ? max_index - current_index
                               : total_lane_count - current_index + max_index);
  }
  Emit(kPPC_I8x16Shuffle, g.DefineAsRegister(node), g.UseRegister(input0),
       g.UseRegister(input1),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle_remapped)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle_remapped + 4)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle_remapped + 8)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle_remapped + 12)));
}

void InstructionSelector::VisitSetStackPointer(OpIndex node) {
  OperandGenerator g(this);
  const SetStackPointerOp& op = Cast<SetStackPointerOp>(node);
  // TODO(miladfarca): Optimize by using UseAny.
  auto input = g.UseRegister(op.input(0));
  Emit(kArchSetStackPointer, 0, nullptr, 1, &input);
}

#else
void InstructionSelector::VisitI8x16Shuffle(OpIndex node) { UNREACHABLE(); }
#endif  // V8_ENABLE_WEBASSEMBLY

void InstructionSelector::VisitS128Zero(OpIndex node) {
  PPCOperandGenerator g(this);
  Emit(kPPC_S128Zero, g.DefineAsRegister(node));
}

void InstructionSelector::VisitS128Select(OpIndex node) {
  PPCOperandGenerator g(this);
  const Simd128TernaryOp& op = Cast<Simd128TernaryOp>(node);
  Emit(kPPC_S128Select, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)), g.UseRegister(op.input(2)));
}

// This is a replica of SimdShuffle::Pack4Lanes. However, above function will
// not be available on builds with webassembly disabled, hence we need to have
// it declared locally as it is used on other visitors such as S128Const.
static int32_t Pack4Lanes(const uint8_t* shuffle) {
  int32_t result = 0;
  for (int i = 3; i >= 0; --i) {
    result <<= 8;
    result |= shuffle[i];
  }
  return result;
}

void InstructionSelector::VisitS128Const(OpIndex node) {
  PPCOperandGenerator g(this);
  uint32_t val[kSimd128Size / sizeof(uint32_t)];
  const Simd128ConstantOp& constant =
      this->Get(node).template Cast<Simd128ConstantOp>();
  memcpy(val, constant.value, kSimd128Size);
  // If all bytes are zeros, avoid emitting code for generic constants.
  bool all_zeros = !(val[0] || val[1] || val[2] || val[3]);
  bool all_ones = val[0] == UINT32_MAX && val[1] == UINT32_MAX &&
                  val[2] == UINT32_MAX && val[3] == UINT32_MAX;
  InstructionOperand dst = g.DefineAsRegister(node);
  if (all_zeros) {
    Emit(kPPC_S128Zero, dst);
  } else if (all_ones) {
    Emit(kPPC_S128AllOnes, dst);
  } else {
    // We have to use Pack4Lanes to reverse the bytes (lanes) on BE,
    // Which in this case is ineffective on LE.
    Emit(kPPC_S128Const, g.DefineAsRegister(node),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]))),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]) + 4)),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]) + 8)),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]) + 12)));
  }
}

void InstructionSelector::VisitI16x8DotI8x16I7x16S(OpIndex node) {
  PPCOperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kPPC_I16x8DotI8x16S, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)), g.UseUniqueRegister(op.input(1)));
}

void InstructionSelector::VisitI32x4DotI8x16I7x16AddS(OpIndex node) {
  PPCOperandGenerator g(this);
  const Simd128TernaryOp& op = Cast<Simd128TernaryOp>(node);
  Emit(kPPC_I32x4DotI8x16AddS, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)), g.UseUniqueRegister(op.input(1)),
       g.UseUniqueRegister(op.input(2)));
}

void InstructionSelector::EmitPrepareResults(
    ZoneVector<PushParameter>* results, const CallDescriptor* call_descriptor,
    OpIndex node) {
  PPCOperandGenerator g(this);

  for (PushParameter output : *results) {
    if (!output.location.IsCallerFrameSlot()) continue;
    // Skip any alignment holes in nodes.
    if (output.node.valid()) {
      DCHECK(!call_descriptor->IsCFunctionCall());
      if (output.location.GetType() == MachineType::Float32()) {
        MarkAsFloat32(output.node);
      } else if (output.location.GetType() == MachineType::Float64()) {
        MarkAsFloat64(output.node);
      } else if (output.location.GetType() == MachineType::Simd128()) {
        MarkAsSimd128(output.node);
      }
      int offset = call_descriptor->GetOffsetToReturns();
      int reverse_slot = -output.location.GetLocation() - offset;
      Emit(kPPC_Peek, g.DefineAsRegister(output.node),
           g.UseImmediate(reverse_slot));
    }
  }
}

void InstructionSelector::VisitLoadLane(OpIndex node) {
  PPCOperandGenerator g(this);
  InstructionCode opcode = kArchNop;
    const Simd128LaneMemoryOp& load =
        this->Get(node).template Cast<Simd128LaneMemoryOp>();
    switch (load.lane_kind) {
      case Simd128LaneMemoryOp::LaneKind::k8:
        opcode = kPPC_S128Load8Lane;
        break;
      case Simd128LaneMemoryOp::LaneKind::k16:
        opcode = kPPC_S128Load16Lane;
        break;
      case Simd128LaneMemoryOp::LaneKind::k32:
        opcode = kPPC_S128Load32Lane;
        break;
      case Simd128LaneMemoryOp::LaneKind::k64:
        opcode = kPPC_S128Load64Lane;
        break;
    }
    Emit(opcode | AddressingModeField::encode(kMode_MRR),
         g.DefineSameAsFirst(node), g.UseRegister(load.value()),
         g.UseRegister(load.base()), g.UseRegister(load.index()),
         g.UseImmediate(load.lane));
}

void InstructionSelector::VisitLoadTransform(OpIndex node) {
  PPCOperandGenerator g(this);
  ArchOpcode opcode;
    const Simd128LoadTransformOp& op =
        this->Get(node).template Cast<Simd128LoadTransformOp>();
    OpIndex base = op.base();
    OpIndex index = op.index();

    switch (op.transform_kind) {
      case Simd128LoadTransformOp::TransformKind::k8Splat:
        opcode = kPPC_S128Load8Splat;
        break;
      case Simd128LoadTransformOp::TransformKind::k16Splat:
        opcode = kPPC_S128Load16Splat;
        break;
      case Simd128LoadTransformOp::TransformKind::k32Splat:
        opcode = kPPC_S128Load32Splat;
        break;
      case Simd128LoadTransformOp::TransformKind::k64Splat:
        opcode = kPPC_S128Load64Splat;
        break;
      case Simd128LoadTransformOp::TransformKind::k8x8S:
        opcode = kPPC_S128Load8x8S;
        break;
      case Simd128LoadTransformOp::TransformKind::k8x8U:
        opcode = kPPC_S128Load8x8U;
        break;
      case Simd128LoadTransformOp::TransformKind::k16x4S:
        opcode = kPPC_S128Load16x4S;
        break;
      case Simd128LoadTransformOp::TransformKind::k16x4U:
        opcode = kPPC_S128Load16x4U;
        break;
      case Simd128LoadTransformOp::TransformKind::k32x2S:
        opcode = kPPC_S128Load32x2S;
        break;
      case Simd128LoadTransformOp::TransformKind::k32x2U:
        opcode = kPPC_S128Load32x2U;
        break;
      case Simd128LoadTransformOp::TransformKind::k32Zero:
        opcode = kPPC_S128Load32Zero;
        break;
      case Simd128LoadTransformOp::TransformKind::k64Zero:
        opcode = kPPC_S128Load64Zero;
        break;
      default:
        UNIMPLEMENTED();
    }
    Emit(opcode | AddressingModeField::encode(kMode_MRR),
         g.DefineAsRegister(node), g.UseRegister(base), g.UseRegister(index));
}

void InstructionSelector::VisitStoreLane(OpIndex node) {
  PPCOperandGenerator g(this);
  InstructionCode opcode = kArchNop;
  InstructionOperand inputs[4];
  const Simd128LaneMemoryOp& store =
      this->Get(node).template Cast<Simd128LaneMemoryOp>();
  switch (store.lane_kind) {
    case Simd128LaneMemoryOp::LaneKind::k8:
      opcode = kPPC_S128Store8Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k16:
      opcode = kPPC_S128Store16Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k32:
      opcode = kPPC_S128Store32Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k64:
      opcode = kPPC_S128Store64Lane;
      break;
  }
  inputs[0] = g.UseRegister(store.value());
  inputs[1] = g.UseRegister(store.base());
  inputs[2] = g.UseRegister(store.index());
  inputs[3] = g.UseImmediate(store.lane);
  Emit(opcode | AddressingModeField::encode(kMode_MRR), 0, nullptr, 4, inputs);
}

void InstructionSelector::AddOutputToSelectContinuation(OperandGenerator* g,
                                                        int first_input_index,
                                                        OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitFloat32RoundTiesEven(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitFloat64RoundTiesEven(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitF64x2NearestInt(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitF32x4NearestInt(OpIndex node) { UNREACHABLE(); }

MachineOperatorBuilder::Flags
InstructionSelector::SupportedMachineOperatorFlags() {
  return MachineOperatorBuilder::kFloat32RoundDown |
         MachineOperatorBuilder::kFloat64RoundDown |
         MachineOperatorBuilder::kFloat32RoundUp |
         MachineOperatorBuilder::kFloat64RoundUp |
         MachineOperatorBuilder::kFloat32RoundTruncate |
         MachineOperatorBuilder::kFloat64RoundTruncate |
         MachineOperatorBuilder::kFloat64RoundTiesAway |
         MachineOperatorBuilder::kWord32Popcnt |
         MachineOperatorBuilder::kWord64Popcnt;
  // We omit kWord32ShiftIsSafe as s[rl]w use 0x3F as a mask rather than 0x1F.
}

MachineOperatorBuilder::AlignmentRequirements
InstructionSelector::AlignmentRequirements() {
  return MachineOperatorBuilder::AlignmentRequirements::
      FullUnalignedAccessSupport();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
