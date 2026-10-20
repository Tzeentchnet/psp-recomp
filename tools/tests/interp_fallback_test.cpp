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

    std::puts("interpreter fallback tests passed");
    return 0;
}
