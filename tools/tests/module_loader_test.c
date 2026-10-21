#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BASE 0x08810000u
#define TEST_REGION_SIZE 0x00010000u
#define FIXTURE_SIZE 0x00000700u
#define SEG0_OFFSET 0x00000200u
#define SEG1_OFFSET 0x00000400u
#define RELOC_OFFSET 0x00000500u
#define SHSTR_OFFSET 0x00000580u
#define SECTION_OFFSET 0x00000600u

#define TEST_IMPORT_NID 0x87654321u
#define TEST_EXPORT_NID 0x12345678u
#define TEST_EXIT_STATUS 0x000055aau

uint8_t *g_mem;
int g_sr_gpu_vram_read_watch;
int g_sr_gpu_vram_write_watch;

void sr_gpu_vram_read(uint32_t address, uint32_t bytes) {
    (void)address;
    (void)bytes;
}

void sr_gpu_vram_dirty(uint32_t address, uint32_t bytes) {
    (void)address;
    (void)bytes;
}

void sr_gpu_vram_prepare_write(uint32_t address, uint32_t bytes) {
    (void)address;
    (void)bytes;
}

void sr_oor(uint32_t address, uint32_t value, int store) {
    fprintf(
        stderr, "unexpected out-of-range %s at 0x%08x = 0x%08x\n",
        store ? "store" : "read", address, value);
    abort();
}

struct SrBlockAlloc {
    uint32_t base;
    uint32_t size;
    uint32_t allocated_size;
    int allocated;
};

static SrBlockAlloc test_allocator;
static uint32_t next_uid;
static uint32_t last_unregistered_uid;
static uint32_t scheduler_thread;
static uint32_t scheduler_entry;
static int scheduler_priority;
static uint32_t scheduler_stack_size;
static uint32_t scheduler_attr;
static uint32_t scheduler_gp;
static uint32_t scheduler_arg_size;
static uint32_t scheduler_arg_address;
static int scheduler_deleted;
static int scheduler_blocked;

uint32_t sr_ba_alloc(
    SrBlockAlloc *allocator, uint32_t *size, int from_top, const char *tag) {
    (void)from_top;
    (void)tag;
    uint32_t rounded = (*size + 0xffu) & ~0xffu;
    if (allocator->allocated || !rounded || rounded > allocator->size)
        return SR_BA_FAIL;
    allocator->allocated = 1;
    allocator->allocated_size = rounded;
    *size = rounded;
    return allocator->base;
}

int sr_ba_free(SrBlockAlloc *allocator, uint32_t position) {
    if (!allocator->allocated || position != allocator->base)
        return 0;
    allocator->allocated = 0;
    allocator->allocated_size = 0;
    return 1;
}

SrBlockAlloc *sr_kmem_user(void) {
    return &test_allocator;
}

uint32_t sr_alloc_uid_type(uint32_t type) {
    if (type != SR_TMID_MODULE) {
        fprintf(stderr, "unexpected UID type %u\n", type);
        abort();
    }
    return next_uid++;
}

void sr_unregister_uid(uint32_t uid) {
    last_unregistered_uid = uid;
}

uint32_t sched_create_thread(
    const char *name, uint32_t entry, int priority, uint32_t stack_size,
    uint32_t attr) {
    if (strcmp(name, "testmod") != 0) {
        fprintf(stderr, "unexpected module thread name %s\n", name);
        abort();
    }
    scheduler_thread = next_uid++;
    scheduler_entry = entry;
    scheduler_priority = priority;
    scheduler_stack_size = stack_size;
    scheduler_attr = attr;
    return scheduler_thread;
}

void sched_set_thread_gp(uint32_t uid, uint32_t gp) {
    if (uid != scheduler_thread) abort();
    scheduler_gp = gp;
}

uint32_t sched_start_thread(
    uint32_t uid, uint32_t arg_size, uint32_t arg_address) {
    if (uid != scheduler_thread) abort();
    scheduler_arg_size = arg_size;
    scheduler_arg_address = arg_address;
    return 0;
}

int sched_is_dormant(uint32_t uid) {
    if (uid != scheduler_thread) abort();
    return 1;
}

void sched_block_on(uint32_t object) {
    (void)object;
    scheduler_blocked++;
}

int32_t sched_thread_exit_status(uint32_t uid) {
    if (uid != scheduler_thread) abort();
    return TEST_EXIT_STATUS;
}

int sched_delete_thread(uint32_t uid) {
    if (uid != scheduler_thread) abort();
    scheduler_deleted++;
    return 0;
}

#include "../../src/rt/module.c"

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf( \
                stderr, "module_loader_test:%d: %s\n", \
                __LINE__, #condition); \
            failures++; \
            return; \
        } \
    } while (0)

static void put16(uint8_t *data, uint32_t offset, uint16_t value) {
    memcpy(data + offset, &value, sizeof(value));
}

static void put32(uint8_t *data, uint32_t offset, uint32_t value) {
    memcpy(data + offset, &value, sizeof(value));
}

static uint32_t relocation_info(
    uint32_t type, uint32_t offset_segment, uint32_t address_segment) {
    return type | (offset_segment << 8) | (address_segment << 16);
}

static void add_relocation(
    uint8_t *data, uint32_t *count, uint32_t offset, uint32_t type) {
    put32(data, RELOC_OFFSET + *count * 8, offset);
    put32(
        data, RELOC_OFFSET + *count * 8 + 4,
        relocation_info(type, 0, 0));
    (*count)++;
}

static void build_fixture(uint8_t data[FIXTURE_SIZE]) {
    memset(data, 0, FIXTURE_SIZE);
    data[0] = 0x7f;
    data[1] = 'E';
    data[2] = 'L';
    data[3] = 'F';
    data[4] = 1;
    data[5] = 1;
    put16(data, 16, 0xffa0u);
    put32(data, 24, 0x10u);
    put32(data, 28, 0x40u);
    put32(data, 32, SECTION_OFFSET);
    put16(data, 40, 52);
    put16(data, 42, 32);
    put16(data, 44, 2);
    put16(data, 46, 40);
    put16(data, 48, 4);
    put16(data, 50, 3);

    put32(data, 0x40, 1);
    put32(data, 0x44, SEG0_OFFSET);
    put32(data, 0x48, 0);
    put32(data, 0x50, 0x200);
    put32(data, 0x54, 0x280);
    put32(data, 0x58, 5);
    put32(data, 0x5c, 0x100);

    put32(data, 0x60, 1);
    put32(data, 0x64, SEG1_OFFSET);
    put32(data, 0x68, 0x300);
    put32(data, 0x70, 0x20);
    put32(data, 0x74, 0x80);
    put32(data, 0x78, 6);
    put32(data, 0x7c, 0x100);

    uint8_t *segment = data + SEG0_OFFSET;
    put32(segment, 0x10, 0x03e00008u);
    put32(segment, 0x14, 0);
    put32(segment, 0x20, 0x03e00008u);
    put32(segment, 0x24, 0);
    put32(segment, 0x30, 0x03e00008u);
    put32(segment, 0x34, 0);

    uint8_t *info = segment + 0x80;
    put16(info, 0, 0);
    info[2] = 1;
    info[3] = 1;
    memcpy(info + 4, "testmod", 8);
    put32(info, 32, 0x1e0);
    put32(info, 36, 0x100);
    put32(info, 40, 0x120);
    put32(info, 44, 0x120);
    put32(info, 48, 0x134);

    memcpy(segment + 0xc0, "testlib", 8);
    memcpy(segment + 0xd0, "dependency", 11);

    uint8_t *syslib = segment + 0x100;
    syslib[8] = 4;
    syslib[9] = 1;
    put16(syslib, 10, 1);
    put32(syslib, 12, 0x180);

    uint8_t *named = segment + 0x110;
    put32(named, 0, 0xc0);
    named[8] = 4;
    named[9] = 0;
    put16(named, 10, 1);
    put32(named, 12, 0x190);

    uint8_t *import_entry = segment + 0x120;
    put32(import_entry, 0, 0xd0);
    import_entry[8] = 5;
    import_entry[9] = 0;
    put16(import_entry, 10, 1);
    put32(import_entry, 12, 0x198);
    put32(import_entry, 16, 0x1a0);

    put32(segment, 0x180, NID_MODULE_START);
    put32(segment, 0x184, NID_MODULE_START_THREAD_PARAMETER);
    put32(segment, 0x188, 0x20);
    put32(segment, 0x18c, 0x1b0);
    put32(segment, 0x190, TEST_EXPORT_NID);
    put32(segment, 0x194, 0x30);
    put32(segment, 0x198, TEST_IMPORT_NID);
    put32(segment, 0x1a0, 0x03e00008u);
    put32(segment, 0x1a4, 0);

    put32(segment, 0x1b0, 3);
    put32(segment, 0x1b4, 0x22);
    put32(segment, 0x1b8, 0x3000);
    put32(segment, 0x1bc, 0x80000000u);

    for (uint32_t i = 0; i < 0x20; i++)
        data[SEG1_OFFSET + i] = (uint8_t)(0xa0u + i);

    uint32_t relocations = 0;
    add_relocation(data, &relocations, 0x80 + 32, 2);
    add_relocation(data, &relocations, 0x80 + 36, 2);
    add_relocation(data, &relocations, 0x80 + 40, 2);
    add_relocation(data, &relocations, 0x80 + 44, 2);
    add_relocation(data, &relocations, 0x80 + 48, 2);
    add_relocation(data, &relocations, 0x100 + 12, 2);
    add_relocation(data, &relocations, 0x110 + 0, 2);
    add_relocation(data, &relocations, 0x110 + 12, 2);
    add_relocation(data, &relocations, 0x120 + 0, 2);
    add_relocation(data, &relocations, 0x120 + 12, 2);
    add_relocation(data, &relocations, 0x120 + 16, 2);
    add_relocation(data, &relocations, 0x188, 2);
    add_relocation(data, &relocations, 0x18c, 2);
    add_relocation(data, &relocations, 0x194, 2);

    uint8_t *names = data + SHSTR_OFFSET;
    uint32_t module_name = 1;
    strcpy((char *)names + module_name, ".rodata.sceModuleInfo");
    uint32_t relocation_name =
        module_name + (uint32_t)strlen((char *)names + module_name) + 1;
    strcpy((char *)names + relocation_name, ".rel.test");
    uint32_t string_name =
        relocation_name +
        (uint32_t)strlen((char *)names + relocation_name) + 1;
    strcpy((char *)names + string_name, ".shstrtab");
    uint32_t names_size =
        string_name + (uint32_t)strlen((char *)names + string_name) + 1;

    uint8_t *module_section = data + SECTION_OFFSET + 40;
    put32(module_section, 0, module_name);
    put32(module_section, 4, 1);
    put32(module_section, 8, 2);
    put32(module_section, 12, 0x80);
    put32(module_section, 16, SEG0_OFFSET + 0x80);
    put32(module_section, 20, 52);
    put32(module_section, 32, 4);

    uint8_t *relocation_section = data + SECTION_OFFSET + 80;
    put32(relocation_section, 0, relocation_name);
    put32(relocation_section, 4, SHT_PRX_RELOC);
    put32(relocation_section, 16, RELOC_OFFSET);
    put32(relocation_section, 20, relocations * 8);
    put32(relocation_section, 32, 4);
    put32(relocation_section, 36, 8);

    uint8_t *string_section = data + SECTION_OFFSET + 120;
    put32(string_section, 0, string_name);
    put32(string_section, 4, 3);
    put32(string_section, 16, SHSTR_OFFSET);
    put32(string_section, 20, names_size);
    put32(string_section, 32, 1);
}

static void reset_runtime(void) {
    memset(s_modules, 0, sizeof(s_modules));
    memset(s_exports, 0, sizeof(s_exports));
    memset(s_syscalls, 0, sizeof(s_syscalls));
    s_export_count = 0;
    s_syscall_count = 0;
    test_allocator.base = TEST_BASE;
    test_allocator.size = TEST_REGION_SIZE;
    test_allocator.allocated_size = 0;
    test_allocator.allocated = 0;
    next_uid = 0x110;
    last_unregistered_uid = 0;
    scheduler_thread = 0;
    scheduler_entry = 0;
    scheduler_priority = 0;
    scheduler_stack_size = 0;
    scheduler_attr = 0;
    scheduler_gp = 0;
    scheduler_arg_size = 0;
    scheduler_arg_address = 0;
    scheduler_deleted = 0;
    scheduler_blocked = 0;
    memset(SR_HOST(TEST_BASE), 0xcc, TEST_REGION_SIZE);
}

static int apply_single_relocation(
    uint32_t type, uint32_t initial, uint32_t expected) {
    uint8_t relocation[8] = {0};
    uint32_t segments[] = {0x08812340u};
    MEM_W32(segments[0], initial);
    put32(relocation, 0, 0);
    put32(relocation, 4, relocation_info(type, 0, 0));
    if (!apply_relocations(
            relocation, sizeof(relocation), 0, sizeof(relocation),
            segments, 1))
        return 0;
    return MEM_R32(segments[0]) == expected;
}

static void test_relocations(void) {
    reset_runtime();
    CHECK(apply_single_relocation(0, 0x12345678u, 0x12345678u));
    CHECK(apply_single_relocation(7, 0x12345678u, 0x12345678u));
    CHECK(apply_single_relocation(1, 0x24080010u, 0x24082350u));
    CHECK(apply_single_relocation(6, 0x24080010u, 0x24082350u));
    CHECK(apply_single_relocation(2, 0x00000010u, 0x08812350u));
    CHECK(apply_single_relocation(
        4, 0x08000004u,
        0x08000000u |
            ((4u + (0x08812340u >> 2)) & 0x03ffffffu)));

    uint8_t hi_lo[16] = {0};
    uint32_t segments[] = {0x08812340u};
    MEM_W32(segments[0], 0x3c080001u);
    MEM_W32(segments[0] + 4, 0x25088000u);
    put32(hi_lo, 0, 0);
    put32(hi_lo, 4, relocation_info(5, 0, 0));
    put32(hi_lo, 8, 4);
    put32(hi_lo, 12, relocation_info(6, 0, 0));
    CHECK(apply_relocations(
        hi_lo, sizeof(hi_lo), 0, sizeof(hi_lo), segments, 1));
    CHECK(MEM_R32(segments[0]) == 0x3c080882u);
    CHECK(MEM_R32(segments[0] + 4) == 0x2508a340u);

    uint8_t invalid[8] = {0};
    put32(invalid, 0, 0);
    put32(invalid, 4, relocation_info(15, 0, 0));
    CHECK(!apply_relocations(
        invalid, sizeof(invalid), 0, sizeof(invalid), segments, 1));
    put32(invalid, 4, relocation_info(2, 1, 0));
    CHECK(!apply_relocations(
        invalid, sizeof(invalid), 0, sizeof(invalid), segments, 1));
}

static void test_load_start_unload(void) {
    uint8_t fixture[FIXTURE_SIZE];
    build_fixture(fixture);
    reset_runtime();

    uint32_t uid =
        sr_module_load_image(fixture, sizeof(fixture), "test/testmod.prx");
    CHECK(uid == 0x110u);
    CHECK(test_allocator.allocated);
    CHECK(test_allocator.allocated_size == 0x400u);
    CHECK(MEM_R32(TEST_BASE + 0x10) == 0x03e00008u);
    CHECK(MEM_R8(TEST_BASE + 0x300) == 0xa0u);
    CHECK(MEM_R8(TEST_BASE + 0x31f) == 0xbfu);
    CHECK(MEM_R8(TEST_BASE + 0x220) == 0);
    CHECK(MEM_R8(TEST_BASE + 0x320) == 0);

    uint32_t export_address = 0;
    const char *library = NULL;
    CHECK(sr_module_find_export(
        TEST_EXPORT_NID, &export_address, &library));
    CHECK(export_address == TEST_BASE + 0x30);
    CHECK(strcmp(library, "testlib") == 0);

    CHECK(MEM_R32(TEST_BASE + 0x1a0) == 0x03e00008u);
    CHECK(MEM_R32(TEST_BASE + 0x1a4) == 0x0000004cu);
    uint32_t import_nid = 0;
    CHECK(sr_module_syscall_nid(1, &import_nid));
    CHECK(import_nid == TEST_IMPORT_NID);

    uint32_t status = TEST_BASE + 0x270;
    CHECK(sr_module_start(uid, 7, TEST_BASE + 0x200, status, 0) == uid);
    CHECK(scheduler_entry == TEST_BASE + 0x20);
    CHECK(scheduler_priority == 0x22);
    CHECK(scheduler_stack_size == 0x3000);
    CHECK(scheduler_attr == 0x80000000u);
    CHECK(scheduler_gp == TEST_BASE + 0x1e0);
    CHECK(scheduler_arg_size == 7);
    CHECK(scheduler_arg_address == TEST_BASE + 0x200);
    CHECK(!scheduler_blocked);
    CHECK(scheduler_deleted == 1);
    CHECK(MEM_R32(status) == TEST_EXIT_STATUS);
    CHECK(sr_module_start(uid, 0, 0, 0, 0) == SCE_KERNEL_ERROR_ERROR);

    CHECK(sr_module_unload(uid) == uid);
    CHECK(!test_allocator.allocated);
    CHECK(last_unregistered_uid == uid);
    CHECK(!sr_module_find_export(
        TEST_EXPORT_NID, &export_address, &library));

    memset(SR_HOST(TEST_BASE), 0xcc, TEST_REGION_SIZE);
    uint32_t reloaded =
        sr_module_load_image(fixture, sizeof(fixture), "test/testmod.prx");
    CHECK(reloaded == 0x112u);
    CHECK(s_modules[0].base == TEST_BASE);
    CHECK(sr_module_unload(reloaded) == reloaded);
}

static void test_malformed_cleanup(void) {
    uint8_t fixture[FIXTURE_SIZE];
    build_fixture(fixture);
    reset_runtime();

    uint8_t bad_magic[52] = {0};
    CHECK(
        sr_module_load_image(
            bad_magic, sizeof(bad_magic), "bad.prx") ==
        SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE);
    CHECK(!test_allocator.allocated);

    uint8_t bad_segment[FIXTURE_SIZE];
    memcpy(bad_segment, fixture, sizeof(bad_segment));
    put32(bad_segment, 0x50, 0x281);
    put32(bad_segment, 0x54, 0x280);
    CHECK(
        sr_module_load_image(
            bad_segment, sizeof(bad_segment), "bad-segment.prx") ==
        SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE);
    CHECK(!test_allocator.allocated);

    uint8_t bad_import[FIXTURE_SIZE];
    memcpy(bad_import, fixture, sizeof(bad_import));
    bad_import[SEG0_OFFSET + 0x120 + 8] = 0;
    uint32_t result = sr_module_load_image(
        bad_import, sizeof(bad_import), "bad-import.prx");
    CHECK(result == SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE);
    CHECK(!test_allocator.allocated);
    CHECK(last_unregistered_uid == 0x110u);

    uint32_t uid =
        sr_module_load_image(fixture, sizeof(fixture), "test/testmod.prx");
    CHECK(uid == 0x111u);
    CHECK(s_modules[0].base == TEST_BASE);
    CHECK(sr_module_unload(uid) == uid);
}

static void test_hle_only_module(void) {
    uint8_t encrypted[52] = {0};
    memcpy(encrypted, "~PSP", 4);
    reset_runtime();
    uint32_t uid =
        sr_module_load_image(encrypted, sizeof(encrypted), "sceCodec.prx");
    CHECK(uid == 0x110u);
    CHECK(!test_allocator.allocated);
    uint32_t status = TEST_BASE + 0x20;
    MEM_W32(status, 0xffffffffu);
    CHECK(sr_module_start(uid, 0, 0, status, 0) == uid);
    CHECK(MEM_R32(status) == 0);
    CHECK(sr_module_unload(uid) == uid);
}

int main(void) {
    uint8_t *memory = (uint8_t *)calloc(0x02000000u, 1);
    if (!memory) return 2;
    g_mem = memory;

    test_relocations();
    test_load_start_unload();
    test_malformed_cleanup();
    test_hle_only_module();

    free(memory);
    if (failures) {
        fprintf(stderr, "module loader tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("module loader tests passed");
    return 0;
}
