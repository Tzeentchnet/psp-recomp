extern "C" {
#include "recomp.h"
}

#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {
int g_sr_gpu_vram_read_watch;
int g_sr_gpu_vram_write_watch;
void sr_gpu_vram_read(uint32_t, uint32_t) {}
void sr_gpu_vram_dirty(uint32_t, uint32_t) {}
void sr_gpu_vram_prepare_write(uint32_t, uint32_t) {}
}

static constexpr uint32_t TEST_HLE_NID = 0x11111111u;
static constexpr uint32_t TEST_GUEST_NID = 0x22222222u;
static uint32_t guest_export_address;
static int hle_calls;
static int guest_export_calls;

extern "C" int sr_module_syscall_nid(uint32_t code, uint32_t *nid) {
    if (!nid) return 0;
    if (code == 1) {
        *nid = TEST_HLE_NID;
        return 1;
    }
    if (code == 2) {
        *nid = TEST_GUEST_NID;
        return 1;
    }
    return 0;
}

extern "C" void sr_syscall(CpuState *state, uint32_t nid) {
    if (nid == TEST_HLE_NID) {
        hle_calls++;
        state->r[2] = 0x20u;
        return;
    }
    if (nid == TEST_GUEST_NID) {
        guest_export_calls++;
        dispatch(state, guest_export_address);
        return;
    }
    std::fprintf(stderr, "unexpected test NID 0x%08x\n", nid);
    std::abort();
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
        &state, function, return_pc, 16, &stop));
    CHECK(stop.reason == SR_INTERP_RETURNED);
    CHECK(stop.steps == 4);
    CHECK(state.pc == return_pc);
    CHECK(state.r[2] == 42);
    CHECK(MEM_R32(output) == 42);
    CHECK(state.vi[17] == 0x7fc12345u);
    CHECK(state.vfpuCtrl[3] == 0x12345678u);

    const char *miss_log = "interp_fallback_test.misses";
    std::remove(miss_log);
#if defined(_WIN32)
    _putenv_s("SR_INTERP_MISS_LOG", miss_log);
#else
    setenv("SR_INTERP_MISS_LOG", miss_log, 1);
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
    CHECK(!sr_interpret_fallback(&state, faulting, return_pc, 4, &stop));
    CHECK(stop.reason == SR_INTERP_MEMORY_FAULT);
    CHECK(stop.pc == faulting);
    CHECK(stop.memory_address == 0);

    const uint32_t looping = 0x08810200u;
    MEM_W32(looping, jump_instruction(looping));
    MEM_W32(looping + 4, 0);
    std::memset(&state, 0, sizeof(state));
    CHECK(!sr_interpret_fallback(&state, looping, return_pc, 8, &stop));
    CHECK(stop.reason == SR_INTERP_STEP_LIMIT);
    CHECK(stop.steps == 8);

    const uint32_t unsupported = 0x08810300u;
    MEM_W32(unsupported, 0xffffffffu);
    std::memset(&state, 0, sizeof(state));
    CHECK(!sr_interpret_fallback(&state, unsupported, return_pc, 4, &stop));
    CHECK(stop.reason == SR_INTERP_UNIMPLEMENTED);
    CHECK(stop.pc == unsupported);
    CHECK(stop.op == 0xffffffffu);

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

    const uint32_t unknown_stub = 0x08810e00u;
    const uint32_t unknown_caller = 0x08810d00u;
    MEM_W32(unknown_stub + 0, 0x03e00008u);
    MEM_W32(unknown_stub + 4, syscall_instruction(63));
    MEM_W32(unknown_caller + 0, call_instruction(unknown_stub));
    MEM_W32(unknown_caller + 4, 0);
    std::memset(&state, 0, sizeof(state));
    state.r[31] = 0x088000d0u;
    CHECK(!sr_interpret_fallback(
        &state, unknown_caller, state.r[31], 8, &stop));
    CHECK(stop.reason == SR_INTERP_SYSCALL);
    CHECK(((stop.op >> 6) & 0xfffffu) == 63u);
    uint32_t unknown_nid = 0;
    CHECK(!sr_module_syscall_nid(63, &unknown_nid));

    std::puts("interpreter fallback tests passed");
    return 0;
}
