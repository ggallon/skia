/*
 * Copyright 2022 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/private/SkMalloc.h"
#include "src/core/SkArenaAlloc.h"
#include "src/core/SkOpts.h"
#include "src/sksl/codegen/SkSLRasterPipelineBuilder.h"
#include "src/sksl/tracing/SkRPDebugTrace.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace SkSL {
namespace RP {

using SkRP = SkRasterPipeline;

#define ALL_SINGLE_SLOT_UNARY_OP_CASES \
         BuilderOp::bitwise_not

#define ALL_SINGLE_SLOT_BINARY_OP_CASES \
         BuilderOp::bitwise_and:        \
    case BuilderOp::bitwise_or:         \
    case BuilderOp::bitwise_xor

#define ALL_MULTI_SLOT_BINARY_OP_CASES \
         BuilderOp::add_n_floats:      \
    case BuilderOp::add_n_ints:        \
    case BuilderOp::cmple_n_floats:    \
    case BuilderOp::cmple_n_ints:      \
    case BuilderOp::cmplt_n_floats:    \
    case BuilderOp::cmplt_n_ints:      \
    case BuilderOp::cmpeq_n_floats:    \
    case BuilderOp::cmpeq_n_ints:      \
    case BuilderOp::cmpne_n_floats:    \
    case BuilderOp::cmpne_n_ints

void Builder::unary_op(BuilderOp op, int32_t slots) {
    switch (op) {
        case ALL_SINGLE_SLOT_UNARY_OP_CASES:
            SkASSERT(slots == 1);
            fInstructions.push_back({op, {}, slots});
            break;

        default:
            SkDEBUGFAIL("not a unary op");
            break;
    }
}

void Builder::binary_op(BuilderOp op, int32_t slots) {
    switch (op) {
        case ALL_SINGLE_SLOT_BINARY_OP_CASES:
            SkASSERT(slots == 1);
            [[fallthrough]];

        case ALL_MULTI_SLOT_BINARY_OP_CASES:
            fInstructions.push_back({op, {}, slots});
            break;

        default:
            SkDEBUGFAIL("not a binary op");
            break;
    }
}

std::unique_ptr<Program> Builder::finish(int numValueSlots, SkRPDebugTrace* debugTrace) {
    return std::make_unique<Program>(std::move(fInstructions), numValueSlots, debugTrace);
}

void Program::optimize() {
    // TODO(johnstiles): perform any last-minute cleanup of the instruction stream here
}

int Program::numTempStackSlots() {
    int largest = 0;
    int current = 0;
    for (const Instruction& inst : fInstructions) {
        switch (inst.fOp) {
            case BuilderOp::push_literal_f:
            case BuilderOp::push_condition_mask:
                ++current;
                largest = std::max(current, largest);
                break;

            case BuilderOp::push_slots:
                current += inst.fImmA;
                largest = std::max(current, largest);
                break;

            case BuilderOp::duplicate:
                current += inst.fImmA;
                largest = std::max(current, largest);
                break;

            case BuilderOp::pop_condition_mask:
            case ALL_SINGLE_SLOT_BINARY_OP_CASES:
                current -= 1;
                break;

            case ALL_MULTI_SLOT_BINARY_OP_CASES:
            case BuilderOp::discard_stack:
                current -= inst.fImmA;
                break;

            default:
                // This op doesn't affect the stack.
                break;
        }
        SkASSERTF(current >= 0, "unbalanced temp stack push/pop");
    }

    SkASSERTF(current == 0, "unbalanced temp stack push/pop");
    return largest;
}

Program::Program(SkTArray<Instruction> instrs, int numValueSlots, SkRPDebugTrace* debugTrace)
        : fInstructions(std::move(instrs))
        , fNumValueSlots(numValueSlots)
        , fDebugTrace(debugTrace) {
    this->optimize();
    fNumTempStackSlots = this->numTempStackSlots();
}

void Program::dump(SkWStream* s) {
    if (fDebugTrace) {
        fDebugTrace->dump(s);
    }
}

template <typename T>
[[maybe_unused]] static void* context_bit_pun(T val) {
    static_assert(sizeof(T) <= sizeof(void*));
    void* contextBits = nullptr;
    memcpy(&contextBits, &val, sizeof(val));
    return contextBits;
}

void Program::appendStages(SkRasterPipeline* pipeline, SkArenaAlloc* alloc) {
    // skslc and sksl-minify do not actually include SkRasterPipeline.
#if !defined(SKSL_STANDALONE)
    // Allocate a contiguous slab of slot data.
    const int N = SkOpts::raster_pipeline_highp_stride;
    const int totalSlots = fNumValueSlots + fNumTempStackSlots;
    const int vectorWidth = N * sizeof(float);
    const int allocSize = vectorWidth * totalSlots;
    float* slotPtr = static_cast<float*>(alloc->makeBytesAlignedTo(allocSize, vectorWidth));
    sk_bzero(slotPtr, allocSize);

    // Store the temp stack immediately after the values.
    float* slotPtrEnd = slotPtr + (N * fNumValueSlots);
    float* tempStackBase = slotPtrEnd;
    float* tempStackPtr = tempStackBase;
    [[maybe_unused]] float* tempStackEnd = tempStackBase + (N * fNumTempStackSlots);

    for (const Instruction& inst : fInstructions) {
        auto SlotA = [&]() { return &slotPtr[N * inst.fSlotA]; };
        auto SlotB = [&]() { return &slotPtr[N * inst.fSlotB]; };

        switch (inst.fOp) {
            case BuilderOp::init_lane_masks:
                pipeline->append(SkRP::init_lane_masks);
                break;

            case BuilderOp::store_src_rg:
                pipeline->append(SkRP::store_src_rg, SlotA());
                break;

            case BuilderOp::store_src:
                pipeline->append(SkRP::store_src, SlotA());
                break;

            case BuilderOp::store_dst:
                pipeline->append(SkRP::store_dst, SlotA());
                break;

            case BuilderOp::load_src:
                pipeline->append(SkRP::load_src, SlotA());
                break;

            case BuilderOp::load_dst:
                pipeline->append(SkRP::load_dst, SlotA());
                break;

            case BuilderOp::immediate_f: {
                pipeline->append(SkRP::immediate_f, context_bit_pun(inst.fImmA));
                break;
            }
            case BuilderOp::load_unmasked:
                pipeline->append(SkRP::load_unmasked, SlotA());
                break;

            case BuilderOp::store_unmasked:
                pipeline->append(SkRP::store_unmasked, SlotA());
                break;

            case BuilderOp::store_masked:
                pipeline->append(SkRP::store_masked, SlotA());
                break;

            case ALL_SINGLE_SLOT_UNARY_OP_CASES: {
                float* dst = tempStackPtr - N;  // overwrite the dest value
                pipeline->append((SkRP::Stage)inst.fOp, dst);
                break;
            }
            case ALL_SINGLE_SLOT_BINARY_OP_CASES: {
                tempStackPtr -= N;              // pop the source value
                float* dst = tempStackPtr - N;  // overwrite the dest value
                pipeline->append_adjacent_single_slot_op((SkRP::Stage)inst.fOp, dst, tempStackPtr);
                break;
            }
            case ALL_MULTI_SLOT_BINARY_OP_CASES: {
                tempStackPtr -= N * inst.fImmA;              // pop the source value
                float* dst = tempStackPtr - N * inst.fImmA;  // overwrite the dest value
                pipeline->append_adjacent_multi_slot_op(alloc, (SkRP::Stage)inst.fOp,
                                                        dst, tempStackPtr, inst.fImmA);
                break;
            }
            case BuilderOp::copy_slot_masked:
                pipeline->append_copy_slots_masked(alloc, SlotA(), SlotB(), inst.fImmA);
                break;

            case BuilderOp::copy_slot_unmasked:
                pipeline->append_copy_slots_unmasked(alloc, SlotA(), SlotB(), inst.fImmA);
                break;

            case BuilderOp::zero_slot_unmasked:
                pipeline->append_zero_slots_unmasked(SlotA(), inst.fImmA);
                break;

            case BuilderOp::push_slots:
                pipeline->append_copy_slots_unmasked(alloc, tempStackPtr, SlotA(), inst.fImmA);
                tempStackPtr += N * inst.fImmA;
                break;

            case BuilderOp::push_condition_mask:
                pipeline->append(SkRP::combine_condition_mask, tempStackPtr - N);
                tempStackPtr += N;
                break;

            case BuilderOp::pop_condition_mask:
                tempStackPtr -= N;
                pipeline->append(SkRP::load_condition_mask, tempStackPtr);
                break;

            case BuilderOp::push_literal_f:
                if (inst.fImmA == 0) {
                    pipeline->append_zero_slots_unmasked(tempStackPtr, /*numSlots=*/1);
                } else {
                    pipeline->append(SkRP::immediate_f, context_bit_pun(inst.fImmA));
                    pipeline->append(SkRP::store_unmasked, tempStackPtr);
                }
                tempStackPtr += N;
                break;

            case BuilderOp::copy_stack_to_slots: {
                float* src = tempStackPtr - N * inst.fImmA;
                pipeline->append_copy_slots_masked(alloc, SlotA(), src, inst.fImmA);
                break;
            }
            case BuilderOp::copy_stack_to_slots_unmasked: {
                float* src = tempStackPtr - N * inst.fImmA;
                pipeline->append_copy_slots_unmasked(alloc, SlotA(), src, inst.fImmA);
                break;
            }
            case BuilderOp::duplicate:
                pipeline->append(SkRP::load_unmasked, tempStackPtr - N);
                for (int index = 0; index < inst.fImmA; ++index) {
                    pipeline->append(SkRP::store_unmasked, tempStackPtr);
                    tempStackPtr += N;
                }
                break;

            case BuilderOp::discard_stack:
                tempStackPtr -= N * inst.fImmA;
                break;

            case BuilderOp::update_return_mask:
                pipeline->append(SkRP::update_return_mask);
                break;

            default:
                SkDEBUGFAILF("Raster Pipeline: unsupported instruction %d", (int)inst.fOp);
                break;
        }
        SkASSERT(tempStackPtr >= tempStackBase);
        SkASSERT(tempStackPtr <= tempStackEnd);
    }
#endif
}

}  // namespace RP
}  // namespace SkSL
