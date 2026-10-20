extern "C" {
#include "recomp.h"
}

#include "../ref/interp.h"

#include <cstring>

namespace {

bool valid_span(uint32_t address, std::size_t size) {
    uint32_t physical = SR_PHYS(address);
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
        case ref::StopReason::kRunning:
            break;
    }
    return SR_INTERP_UNIMPLEMENTED;
}

}  // namespace

extern "C" int sr_interpret_fallback(
    CpuState *state, uint32_t target, uint32_t return_pc,
    uint64_t max_steps, SrInterpStop *stop) {
    ref::CpuState interpreted;
    import_state(&interpreted, state, target);
    ref::Memory memory(nullptr, read_runtime, write_runtime);
    ref::StepResult result = ref::Run(
        &interpreted, &memory, max_steps, nullptr, true, return_pc);
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
