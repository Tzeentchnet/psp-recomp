extern "C" {
#include "recomp.h"
}

#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {
int g_sr_gpu_vram_read_watch;
int g_sr_gpu_vram_write_watch;
int sr_sched_on;
int32_t sr_timeslice;
void sr_gpu_vram_read(uint32_t, uint32_t) {}
void sr_gpu_vram_dirty(uint32_t, uint32_t) {}
void sr_gpu_vram_prepare_write(uint32_t, uint32_t) {}
void sr_yield(CpuState *) {}
}

static constexpr uint32_t TEST_HLE_NID = 0x11111111u;
static constexpr uint32_t TEST_GUEST_NID = 0x22222222u;
static uint32_t guest_export_address;
static int hle_calls;
static int guest_export_calls;
static int native_callback_calls;

extern "C" void test_native_callback(CpuState *state) {
    native_callback_calls++;
    state->r[2] = 0x40u;
}

extern "C" int sr_module_syscall_binding(
    uint32_t code, const char **library, uint32_t *nid) {
    if (!nid) return 0;
    if (code == 1) {
        if (library) *library = "TestHLE";
        *nid = TEST_HLE_NID;
        return 1;
    }
    if (code == 2) {
        if (library) *library = "testlib";
        *nid = TEST_GUEST_NID;
        return 1;
    }
    return 0;
}

extern "C" int sr_module_syscall_nid(uint32_t code, uint32_t *nid) {
    return sr_module_syscall_binding(code, nullptr, nid);
}

extern "C" void sr_syscall_lib(
    CpuState *state, const char *library, uint32_t nid) {
    if (nid == TEST_HLE_NID) {
        if (!library || std::strcmp(library, "TestHLE") != 0)
            std::abort();
        hle_calls++;
        state->r[2] = 0x20u;
        return;
    }
    if (nid == TEST_GUEST_NID) {
        if (!library || std::strcmp(library, "testlib") != 0)
            std::abort();
        guest_export_calls++;
        dispatch(state, guest_export_address);
        return;
    }
    std::fprintf(stderr, "unexpected test NID 0x%08x\n", nid);
    std::abort();
}

extern "C" void sr_syscall(CpuState *state, uint32_t nid) {
    sr_syscall_lib(state, nullptr, nid);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf( \
                stderr, "interp_fallback_test:%d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static uint32_t jump_instruction(uint32_t address) {
    return 0x08000000u | ((address >> 2) & 0x03ffffffu);
}

static uint32_t call_instruction(uint32_t address) {
    return 0x0c000000u | ((address >> 2) & 0x03ffffffu);
}

static uint32_t syscall_instruction(uint32_t code) {
    return (code << 6) | 0x0000000cu;
}

int main() {
    sr_mem_init();

    CHECK(sr_inrange(SR_SCRATCHPAD_BASE));
    CHECK(sr_inrange(SR_SCRATCHPAD_BASE + SR_SCRATCHPAD_SIZE - 1u));
    CHECK(!sr_inrange(SR_SCRATCHPAD_BASE - 1u));
    CHECK(!sr_inrange(SR_SCRATCHPAD_BASE + SR_SCRATCHPAD_SIZE));
    MEM_W32(SR_SCRATCHPAD_BASE, 0x12345678u);
    CHECK(MEM_R32(SR_SCRATCHPAD_BASE) == 0x12345678u);
    CHECK(MEM_R32(0x80010000u) == 0x12345678u);

    const uint32_t function = 0x08810000u;
    const uint32_t output = 0x08811000u;
    const uint32_t return_pc = 0x08800080u;
    MEM_W32(function + 0, 0x2402002au);
    MEM_W32(function + 4, 0xac820000u);
    MEM_W32(function + 8, 0x03e00008u);
    MEM_W32(function + 12, 0x00000000u);

    CpuState state{};
    state.r[4] = output;
    state.r[31] = return_pc;
    state.vi[17] = 0x7fc12345u;
    state.vfpuCtrl[3] = 0x12345678u;
    SrInterpStop stop{};
    CHECK(sr_interpret_fallback(
        &state, function, return_pc, 16, 1, &stop));
    CHECK(stop.reason == SR_INTERP_RETURNED);
    CHECK(stop.steps == 4);
    CHECK(state.pc == return_pc);
    CHECK(state.r[2] == 42);
    CHECK(MEM_R32(output) == 42);
    CHECK(state.vi[17] == 0x7fc12345u);
    CHECK(state.vfpuCtrl[3] == 0x12345678u);

    const uint32_t scratch_function = 0x08810020u;
    MEM_W32(scratch_function + 0, 0xac850000u);
    MEM_W32(scratch_function + 4, 0x8c820000u);
    MEM_W32(scratch_function + 8, 0x03e00008u);
    MEM_W32(scratch_function + 12, 0);
    std::memset(&state, 0, sizeof(state));
    state.r[4] = SR_SCRATCHPAD_BASE + SR_SCRATCHPAD_SIZE - 4u;
    state.r[5] = 0x89abcdefu;
    state.r[31] = return_pc;
    CHECK(sr_interpret_fallback(
        &state, scratch_function, return_pc, 16, 1, &stop));
    CHECK(state.r[2] == 0x89abcdefu);
    CHECK(
        MEM_R32(SR_SCRATCHPAD_BASE + SR_SCRATCHPAD_SIZE - 4u) ==
        0x89abcdefu);

    const uint32_t vfpu_function = 0x08810040u;
    const uint32_t vfpu_source = 0x08811200u;
    const uint32_t vfpu_copy = 0x08811300u;
    const uint32_t vadd_s =
        (0x18u << 26) | (2u << 16) | (1u << 8);
    const uint32_t lvq_v0_a0 = (0x36u << 26) | (4u << 21);
    const uint32_t svq_v0_a1 = (0x3eu << 26) | (5u << 21);
    MEM_W32(vfpu_function + 0, lvq_v0_a0);
    MEM_W32(vfpu_function + 4, svq_v0_a1);
    MEM_W32(vfpu_function + 8, vadd_s);
    MEM_W32(vfpu_function + 12, 0x03e00008u);
    MEM_W32(vfpu_function + 16, 0);
    for (uint32_t i = 0; i < 4; i++)
        MEM_W32(vfpu_source + i * 4, 0x11111111u * (i + 1));

    std::memset(&state, 0, sizeof(state));
    state.r[4] = vfpu_source;
    state.r[5] = vfpu_copy;
    state.r[31] = return_pc;
    state.vfpuCtrl[0] = 0xe4u;
    state.vfpuCtrl[1] = 0xe4u;
    state.v[4] = 1.5f;
    state.v[8] = 2.25f;
    CHECK(sr_interpret_fallback(
        &state, vfpu_function, return_pc, 16, 1, &stop));
    for (uint32_t i = 0; i < 4; i++)
        CHECK(MEM_R32(vfpu_copy + i * 4) == 0x11111111u * (i + 1));
    CHECK(state.v[0] == 3.75f);

    const char *miss_log = "interp_fallback_test.misses";
    const char *profile_log = "interp_fallback_test.profile";
    std::remove(miss_log);
    std::remove(profile_log);
#if defined(_WIN32)
    _putenv_s("SR_INTERP_MISS_LOG", miss_log);
    _putenv_s("SR_INTERP_PROFILE", profile_log);
#else
    setenv("SR_INTERP_MISS_LOG", miss_log, 1);
    setenv("SR_INTERP_PROFILE", profile_log, 1);
#endif
    sr_set_dispatch_table(nullptr, 0);
    std::memset(&state, 0, sizeof(state));
    state.r[4] = output;
    state.r[31] = return_pc;
    dispatch(&state, function);
    CHECK(state.r[2] == 42);
    std::memset(&state, 0, sizeof(state));
    state.r[4] = output;
    state.r[31] = return_pc;
    dispatch(&state, function);
    FILE *log = std::fopen(miss_log, "r");
    CHECK(log != nullptr);
    char line[128];
    int addresses = 0;
    while (std::fgets(line, sizeof(line), log))
        if (line[0] != '#') addresses++;
    CHECK(std::fclose(log) == 0);
    CHECK(addresses == 1);

    const uint32_t faulting = 0x08810100u;
    MEM_W32(faulting, 0x8c820000u);
    std::memset(&state, 0, sizeof(state));
    CHECK(!sr_interpret_fallback(
        &state, faulting, return_pc, 4, 1, &stop));
    CHECK(stop.reason == SR_INTERP_MEMORY_FAULT);
    CHECK(stop.pc == faulting);
    CHECK(stop.memory_address == 0);

    const uint32_t looping = 0x08810200u;
    MEM_W32(looping, jump_instruction(looping));
    MEM_W32(looping + 4, 0);
    std::memset(&state, 0, sizeof(state));
    CHECK(!sr_interpret_fallback(
        &state, looping, return_pc, 8, 1, &stop));
    CHECK(stop.reason == SR_INTERP_STEP_LIMIT);
    CHECK(stop.steps == 8);

    const uint32_t vnop_function = 0x08810240u;
    MEM_W32(vnop_function + 0, 0xffff040du);
    MEM_W32(vnop_function + 4, 0x2402005au);
    MEM_W32(vnop_function + 8, 0x03e00008u);
    MEM_W32(vnop_function + 12, 0);
    std::memset(&state, 0, sizeof(state));
    state.r[31] = return_pc;
    CHECK(sr_interpret_fallback(
        &state, vnop_function, return_pc, 8, 1, &stop));
    CHECK(state.r[2] == 0x5au);

    const uint32_t vi2f_function = 0x08810260u;
    MEM_W32(vi2f_function + 0, 0xd2806565u);
    MEM_W32(vi2f_function + 4, 0x03e00008u);
    MEM_W32(vi2f_function + 8, 0);
    std::memset(&state, 0, sizeof(state));
    state.vi[23] = 42u;
    state.vfpuCtrl[0] = 0xe4u;
    state.vfpuCtrl[1] = 0xe4u;
    state.r[31] = return_pc;
    CHECK(sr_interpret_fallback(
        &state, vi2f_function, return_pc, 8, 1, &stop));
    CHECK(state.v[23] == 42.0f);

    const uint32_t vf2iz_function = 0x08810280u;
    MEM_W32(vf2iz_function + 0, 0xd2378484u);
    MEM_W32(vf2iz_function + 4, 0x03e00008u);
    MEM_W32(vf2iz_function + 8, 0);
    std::memset(&state, 0, sizeof(state));
    state.v[16] = 1.5f;
    state.v[17] = -1.5f;
    state.vfpuCtrl[0] = 0xe4u;
    state.vfpuCtrl[1] = 0xe4u;
    state.r[31] = return_pc;
    CHECK(sr_interpret_fallback(
        &state, vf2iz_function, return_pc, 8, 1, &stop));
    CHECK(state.vi[16] == 12582912u);
    CHECK(state.vi[17] == (uint32_t)-12582912);

    const uint32_t vi2uc_function = 0x088102a0u;
    MEM_W32(vi2uc_function + 0, 0xd03c8484u);
    MEM_W32(vi2uc_function + 4, 0x03e00008u);
    MEM_W32(vi2uc_function + 8, 0);
    std::memset(&state, 0, sizeof(state));
    state.vi[16] = 0;
    state.vi[17] = 0x00800000u;
    state.vi[18] = 0x7f800000u;
    state.vi[19] = 0xffffffffu;
    state.vfpuCtrl[0] = 0xe4u;
    state.vfpuCtrl[1] = 0xe4u;
    state.r[31] = return_pc;
    CHECK(sr_interpret_fallback(
        &state, vi2uc_function, return_pc, 8, 1, &stop));
    CHECK(state.vi[16] == 0x00ff0100u);

    const uint32_t bvt_function = 0x088102c0u;
    MEM_W32(bvt_function + 0, 0x49010002u);
    MEM_W32(bvt_function + 4, 0x24020001u);
    MEM_W32(bvt_function + 8, 0x24020002u);
    MEM_W32(bvt_function + 12, 0x03e00008u);
    MEM_W32(bvt_function + 16, 0);
    std::memset(&state, 0, sizeof(state));
    state.vfpuCtrl[3] = 1;
    state.r[31] = return_pc;
    CHECK(sr_interpret_fallback(
        &state, bvt_function, return_pc, 8, 1, &stop));
    CHECK(state.r[2] == 1);
    std::memset(&state, 0, sizeof(state));
    state.r[31] = return_pc;
    CHECK(sr_interpret_fallback(
        &state, bvt_function, return_pc, 8, 1, &stop));
    CHECK(state.r[2] == 2);

    const uint32_t unsupported = 0x08810300u;
    MEM_W32(unsupported, 0x4c00003fu);
    std::memset(&state, 0, sizeof(state));
    CHECK(!sr_interpret_fallback(
        &state, unsupported, return_pc, 4, 1, &stop));
    CHECK(stop.reason == SR_INTERP_UNIMPLEMENTED);
    CHECK(stop.pc == unsupported);
    CHECK(stop.op == 0x4c00003fu);

    const uint32_t import_stub = 0x08810500u;
    MEM_W32(import_stub + 0, 0x03e00008u);
    MEM_W32(import_stub + 4, syscall_instruction(1));

    const uint32_t syscall_function = 0x08810400u;
    const uint32_t syscall_output = 0x08811100u;
    MEM_W32(syscall_function + 0, 0x27bdfff0u);
    MEM_W32(syscall_function + 4, 0xafbf0000u);
    MEM_W32(syscall_function + 8, call_instruction(import_stub));
    MEM_W32(syscall_function + 12, 0);
    MEM_W32(syscall_function + 16, 0xac820000u);
    MEM_W32(syscall_function + 20, 0x8fbf0000u);
    MEM_W32(syscall_function + 24, 0x03e00008u);
    MEM_W32(syscall_function + 28, 0x27bd0010u);

    hle_calls = 0;
    std::memset(&state, 0, sizeof(state));
    state.r[4] = syscall_output;
    state.r[29] = 0x0881f000u;
    state.r[31] = 0x08800090u;
    dispatch(&state, syscall_function);
    CHECK(hle_calls == 1);
    CHECK(state.pc == 0x08800090u);
    CHECK(state.r[29] == 0x0881f000u);
    CHECK(state.r[2] == 0x20u);
    CHECK(MEM_R32(syscall_output) == 0x20u);

    const uint32_t outer_function = 0x08810600u;
    const uint32_t inner_function = 0x08810700u;
    MEM_W32(outer_function + 0, 0x27bdfff0u);
    MEM_W32(outer_function + 4, 0xafbf0000u);
    MEM_W32(outer_function + 8, call_instruction(inner_function));
    MEM_W32(outer_function + 12, 0);
    MEM_W32(outer_function + 16, 0x24420001u);
    MEM_W32(outer_function + 20, 0x8fbf0000u);
    MEM_W32(outer_function + 24, 0x03e00008u);
    MEM_W32(outer_function + 28, 0x27bd0010u);
    MEM_W32(inner_function + 0, 0x27bdfff0u);
    MEM_W32(inner_function + 4, 0xafbf0000u);
    MEM_W32(inner_function + 8, call_instruction(import_stub));
    MEM_W32(inner_function + 12, 0);
    MEM_W32(inner_function + 16, 0x24420002u);
    MEM_W32(inner_function + 20, 0x8fbf0000u);
    MEM_W32(inner_function + 24, 0x03e00008u);
    MEM_W32(inner_function + 28, 0x27bd0010u);

    hle_calls = 0;
    std::memset(&state, 0, sizeof(state));
    state.r[29] = 0x0881f000u;
    state.r[31] = 0x088000a0u;
    dispatch(&state, outer_function);
    CHECK(hle_calls == 1);
    CHECK(state.pc == 0x088000a0u);
    CHECK(state.r[29] == 0x0881f000u);
    CHECK(state.r[2] == 0x23u);

    const uint32_t guest_stub = 0x08810900u;
    const uint32_t guest_caller = 0x08810800u;
    guest_export_address = 0x08810a00u;
    MEM_W32(guest_stub + 0, 0x03e00008u);
    MEM_W32(guest_stub + 4, syscall_instruction(2));
    MEM_W32(guest_caller + 0, 0x27bdfff0u);
    MEM_W32(guest_caller + 4, 0xafbf0000u);
    MEM_W32(guest_caller + 8, call_instruction(guest_stub));
    MEM_W32(guest_caller + 12, 0);
    MEM_W32(guest_caller + 16, 0x24420001u);
    MEM_W32(guest_caller + 20, 0x8fbf0000u);
    MEM_W32(guest_caller + 24, 0x03e00008u);
    MEM_W32(guest_caller + 28, 0x27bd0010u);
    MEM_W32(guest_export_address + 0, 0x24020077u);
    MEM_W32(guest_export_address + 4, 0x03e00008u);
    MEM_W32(guest_export_address + 8, 0);

    guest_export_calls = 0;
    std::memset(&state, 0, sizeof(state));
    state.r[29] = 0x0881f000u;
    state.r[31] = 0x088000b0u;
    dispatch(&state, guest_caller);
    CHECK(guest_export_calls == 1);
    CHECK(state.pc == 0x088000b0u);
    CHECK(state.r[29] == 0x0881f000u);
    CHECK(state.r[2] == 0x78u);

    const uint32_t callback_caller = 0x08810b00u;
    const uint32_t callback = 0x08810c00u;
    MEM_W32(callback_caller + 0, 0x27bdfff0u);
    MEM_W32(callback_caller + 4, 0xafbf0000u);
    MEM_W32(callback_caller + 8, 0x00a0f809u);
    MEM_W32(callback_caller + 12, 0);
    MEM_W32(callback_caller + 16, 0x24420001u);
    MEM_W32(callback_caller + 20, 0x8fbf0000u);
    MEM_W32(callback_caller + 24, 0x03e00008u);
    MEM_W32(callback_caller + 28, 0x27bd0010u);
    MEM_W32(callback + 0, 0x27bdfff0u);
    MEM_W32(callback + 4, 0xafbf0000u);
    MEM_W32(callback + 8, call_instruction(import_stub));
    MEM_W32(callback + 12, 0);
    MEM_W32(callback + 16, 0x24420003u);
    MEM_W32(callback + 20, 0x8fbf0000u);
    MEM_W32(callback + 24, 0x03e00008u);
    MEM_W32(callback + 28, 0x27bd0010u);

    hle_calls = 0;
    std::memset(&state, 0, sizeof(state));
    state.r[5] = callback;
    state.r[29] = 0x0881f000u;
    state.r[31] = 0x088000c0u;
    dispatch(&state, callback_caller);
    CHECK(hle_calls == 1);
    CHECK(state.pc == 0x088000c0u);
    CHECK(state.r[29] == 0x0881f000u);
    CHECK(state.r[2] == 0x24u);

    const uint32_t native_target = 0x08900000u;
    const uint32_t native_caller = 0x08810f00u;
    const SrDispatchEntry native_entries[] = {
        {native_target, test_native_callback},
    };
    sr_set_dispatch_table(native_entries, 1);
    MEM_W32(native_caller + 0, 0x27bdfff0u);
    MEM_W32(native_caller + 4, 0xafbf0000u);
    MEM_W32(native_caller + 8, call_instruction(native_target));
    MEM_W32(native_caller + 12, 0);
    MEM_W32(native_caller + 16, 0x24420001u);
    MEM_W32(native_caller + 20, 0x8fbf0000u);
    MEM_W32(native_caller + 24, 0x03e00008u);
    MEM_W32(native_caller + 28, 0x27bd0010u);

    native_callback_calls = 0;
    std::memset(&state, 0, sizeof(state));
    state.r[29] = 0x0881f000u;
    state.r[31] = 0x088000d0u;
    dispatch(&state, native_caller);
    CHECK(native_callback_calls == 1);
    CHECK(state.pc == 0x088000d0u);
    CHECK(state.r[29] == 0x0881f000u);
    CHECK(state.r[2] == 0x41u);

    MEM_W32(native_target + 0, 0x24020055u);
    MEM_W32(native_target + 4, 0x03e00008u);
    MEM_W32(native_target + 8, 0);
    native_callback_calls = 0;
    std::memset(&state, 0, sizeof(state));
    state.r[31] = 0x088000d4u;
    sr_interpret_direct(&state, native_target);
    CHECK(native_callback_calls == 0);
    CHECK(state.pc == 0x088000d4u);
    CHECK(state.r[2] == 0x55u);

    native_callback_calls = 0;
    std::memset(&state, 0, sizeof(state));
    state.r[29] = 0x0881f000u;
    state.r[31] = 0x088000d8u;
    CHECK(sr_interpret_fallback(
        &state, native_caller, state.r[31], 32, 0, &stop));
    CHECK(native_callback_calls == 0);
    CHECK(state.pc == 0x088000d8u);
    CHECK(state.r[29] == 0x0881f000u);
    CHECK(state.r[2] == 0x56u);

    sr_set_dispatch_table(nullptr, 0);

    const uint32_t unknown_stub = 0x08810e00u;
    const uint32_t unknown_caller = 0x08810d00u;
    MEM_W32(unknown_stub + 0, 0x03e00008u);
    MEM_W32(unknown_stub + 4, syscall_instruction(63));
    MEM_W32(unknown_caller + 0, call_instruction(unknown_stub));
    MEM_W32(unknown_caller + 4, 0);
    std::memset(&state, 0, sizeof(state));
    state.r[31] = 0x088000d0u;
    CHECK(!sr_interpret_fallback(
        &state, unknown_caller, state.r[31], 8, 1, &stop));
    CHECK(stop.reason == SR_INTERP_SYSCALL);
    CHECK(((stop.op >> 6) & 0xfffffu) == 63u);
    uint32_t unknown_nid = 0;
    CHECK(!sr_module_syscall_nid(63, &unknown_nid));

    CHECK(sr_interp_profile_write());
    log = std::fopen(profile_log, "r");
    CHECK(log != nullptr);
    bool found_profile = false;
    while (std::fgets(line, sizeof(line), log)) {
        char module[32];
        unsigned target;
        unsigned long long calls;
        unsigned long long instructions;
        if (std::sscanf(
                line, "%31s\t0x%x\t%llu\t%llu",
                module, &target, &calls, &instructions) == 4 &&
            std::strcmp(module, "unknown") == 0 &&
            target == function) {
            CHECK(calls == 2);
            CHECK(instructions == 8);
            found_profile = true;
        }
    }
    CHECK(std::fclose(log) == 0);
    CHECK(found_profile);
    CHECK(std::remove(profile_log) == 0);

    std::puts("interpreter fallback tests passed");
    return 0;
}
