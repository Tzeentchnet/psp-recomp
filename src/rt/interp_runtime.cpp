extern "C" {
#include "recomp.h"
}

#include "../ref/interp.h"

#include <cstring>

namespace {

bool valid_span(uint32_t address, std::size_t size) {
    uint32_t physical = SR_PHYS(address);
    if (physical >= SR_SCRATCHPAD_BASE &&
        physical < SR_SCRATCHPAD_BASE + SR_SCRATCHPAD_SIZE) {
        return size <= SR_SCRATCHPAD_SIZE &&
               physical - SR_SCRATCHPAD_BASE <=
                   SR_SCRATCHPAD_SIZE - size;
    }
    if (physical < 0x04000000u || physical >= 0x0c000000u)
        return false;
    return size <= 0x08000000u &&
           physical - 0x04000000u <= 0x08000000u - size;
}

bool read_runtime(void *, uint32_t address, void *output, std::size_t size) {
    if (!valid_span(address, size))
        return false;
    switch (size) {
        case 1: {
            uint8_t value = MEM_R8(address);
            std::memcpy(output, &value, sizeof(value));
            return true;
        }
        case 2: {
            uint16_t value = MEM_R16(address);
            std::memcpy(output, &value, sizeof(value));
            return true;
        }
        case 4: {
            uint32_t value = MEM_R32(address);
            std::memcpy(output, &value, sizeof(value));
            return true;
        }
        default:
            return false;
    }
}

bool write_runtime(
    void *, uint32_t address, const void *input, std::size_t size) {
    if (!valid_span(address, size))
        return false;
    switch (size) {
        case 1: {
            uint8_t value;
            std::memcpy(&value, input, sizeof(value));
            MEM_W8(address, value);
            return true;
        }
        case 2: {
            uint16_t value;
            std::memcpy(&value, input, sizeof(value));
            MEM_W16(address, value);
            return true;
        }
        case 4: {
            uint32_t value;
            std::memcpy(&value, input, sizeof(value));
            MEM_W32(address, value);
            return true;
        }
        default:
            return false;
    }
}

bool has_native_target(void *context, uint32_t address) {
    const uint32_t initial = *(const uint32_t *)context;
    return address != initial && sr_lookup(address) != nullptr;
}

bool execute_vfpu_fallback(
    void *, ref::CpuState *state, uint32_t instruction) {
    CpuState runtime = {};
    std::memcpy(runtime.vi, state->vi, sizeof(runtime.vi));
    std::memcpy(
        runtime.vfpuCtrl, state->vfpuCtrl, sizeof(runtime.vfpuCtrl));
    if (sr_vfpu_interp(&runtime, instruction) == SR_VFPU_OTHER)
        return false;
    std::memcpy(state->vi, runtime.vi, sizeof(state->vi));
    std::memcpy(
        state->vfpuCtrl, runtime.vfpuCtrl, sizeof(state->vfpuCtrl));
    return true;
}

void import_state(ref::CpuState *destination, const CpuState *source, uint32_t pc) {
    std::memcpy(destination->r, source->r, sizeof(destination->r));
    destination->hi = source->hi;
    destination->lo = source->lo;
    destination->pc = pc;
    std::memcpy(destination->fi, source->fi, sizeof(destination->fi));
    destination->fcr31 = source->fcr31;
    destination->fpcond = source->fpcond;
    std::memcpy(destination->vi, source->vi, sizeof(destination->vi));
    std::memcpy(
        destination->vfpuCtrl, source->vfpuCtrl,
        sizeof(destination->vfpuCtrl));
    destination->next_pc = 0;
    destination->in_delay_slot = false;
}

void export_state(CpuState *destination, const ref::CpuState *source) {
    std::memcpy(destination->r, source->r, sizeof(destination->r));
    destination->hi = source->hi;
    destination->lo = source->lo;
    destination->pc = source->pc;
    std::memcpy(destination->fi, source->fi, sizeof(destination->fi));
    destination->fcr31 = source->fcr31;
    destination->fpcond = source->fpcond;
    std::memcpy(destination->vi, source->vi, sizeof(destination->vi));
    std::memcpy(
        destination->vfpuCtrl, source->vfpuCtrl,
        sizeof(destination->vfpuCtrl));
}

void yield_runtime(
    void *context, ref::CpuState *state, uint32_t steps) {
    CpuState *runtime = static_cast<CpuState *>(context);
    export_state(runtime, state);
    if (sr_sched_on) {
        sr_timeslice -= (int32_t)steps;
        if (sr_timeslice <= 0)
            sr_yield(runtime);
    }
    import_state(state, runtime, runtime->pc);
}

SrInterpStopReason convert_reason(ref::StopReason reason) {
    switch (reason) {
        case ref::StopReason::kTargetReached:
            return SR_INTERP_RETURNED;
        case ref::StopReason::kSyscall:
            return SR_INTERP_SYSCALL;
        case ref::StopReason::kUnimplemented:
            return SR_INTERP_UNIMPLEMENTED;
        case ref::StopReason::kMemoryFault:
            return SR_INTERP_MEMORY_FAULT;
        case ref::StopReason::kStepLimit:
            return SR_INTERP_STEP_LIMIT;
        case ref::StopReason::kBreak:
            return SR_INTERP_BREAK;
        case ref::StopReason::kNativeTarget:
            return SR_INTERP_NATIVE_TARGET;
        case ref::StopReason::kRunning:
            break;
    }
    return SR_INTERP_UNIMPLEMENTED;
}

}  // namespace

extern "C" int sr_interpret_fallback(
    CpuState *state, uint32_t target, uint32_t return_pc,
    uint64_t max_steps, int allow_native_handoff, SrInterpStop *stop) {
    ref::CpuState interpreted;
    import_state(&interpreted, state, target);
    ref::Memory memory(nullptr, read_runtime, write_runtime);
    uint32_t initial_target = target;
    ref::StepResult result = ref::Run(
        &interpreted, &memory, max_steps, nullptr, true, return_pc,
        allow_native_handoff ? has_native_target : nullptr,
        allow_native_handoff ? &initial_target : nullptr,
        execute_vfpu_fallback, nullptr,
        yield_runtime, state);
    export_state(state, &interpreted);
    if (stop) {
        stop->reason = convert_reason(result.reason);
        stop->pc = result.pc;
        stop->op = result.op;
        stop->memory_address = memory.fault_addr();
        stop->steps = result.steps;
    }
    return result.reason == ref::StopReason::kTargetReached;
}
