/* Runtime loader for relocatable user PRX modules.
 *
 * Plaintext PRXs are allocated in the PSP user partition, relocated in guest
 * memory, and have their import stubs connected to the native HLE dispatcher.
 * Named exports remain guest code and are executed through the interpreter
 * fallback until a game chooses to statically compile an overlay.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR_MAX_MODULES 128
#define SR_MAX_GUEST_EXPORTS 4096
#define SR_MAX_MODULE_SYSCALLS 4096
#define SHT_PRX_RELOC 0x700000a0u

#define SCE_KERNEL_ERROR_UNKNOWN_MODULE       0x8002012eu
#define SCE_KERNEL_ERROR_ERROR                0x80020001u
#define SCE_KERNEL_ERROR_NO_MEMORY            0x80020190u
#define SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE 0x80020148u

#define NID_MODULE_START                  0xd632acdbu
#define NID_MODULE_STOP                   0xcee8593cu
#define NID_MODULE_INFO                   0xf01d73a7u
#define NID_MODULE_START_THREAD_PARAMETER 0x0f7c276cu

typedef struct SrModule {
    int used;
    int started;
    int fake;
    int hle_replaced;
    uint32_t uid;
    uint32_t base;
    uint32_t size;
    uint32_t entry;
    uint32_t module_start;
    uint32_t module_stop;
    uint32_t gp;
    uint32_t attr;
    uint32_t start_priority;
    uint32_t start_stack_size;
    uint32_t start_attr;
    char name[32];
    char path[256];
} SrModule;

typedef struct SrGuestExport {
    int active;
    uint32_t module_uid;
    uint32_t nid;
    uint32_t address;
    char library[48];
} SrGuestExport;

typedef struct SrModuleSyscall {
    uint32_t nid;
    char library[48];
} SrModuleSyscall;

static SrModule s_modules[SR_MAX_MODULES];
static SrGuestExport s_exports[SR_MAX_GUEST_EXPORTS];
static uint32_t s_export_count;
static SrModuleSyscall s_syscalls[SR_MAX_MODULE_SYSCALLS];
static uint32_t s_syscall_count;

static uint16_t file_r16(const uint8_t *data, uint32_t offset) {
    uint16_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static uint32_t file_r32(const uint8_t *data, uint32_t offset) {
    uint32_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static int file_range(uint32_t size, uint32_t offset, uint32_t length) {
    return offset <= size && length <= size - offset;
}

static int guest_range(uint32_t address, uint32_t size) {
    if (!size) return sr_inrange(address);
    return sr_inrange(address) &&
           address <= UINT32_MAX - (size - 1) &&
           sr_inrange(address + size - 1);
}

static void guest_string(uint32_t address, char *output, size_t output_size) {
    size_t i = 0;
    if (!output_size) return;
    if (!address || !sr_inrange(address)) {
        output[0] = 0;
        return;
    }
    while (i + 1 < output_size && sr_inrange(address + (uint32_t)i)) {
        uint8_t value = MEM_R8(address + (uint32_t)i);
        if (!value) break;
        output[i++] = (char)value;
    }
    output[i] = 0;
}

static const char *path_basename(const char *path) {
    const char *name = path ? path : "module";
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

static SrModule *module_slot(void) {
    for (int i = 0; i < SR_MAX_MODULES; i++)
        if (!s_modules[i].used) return &s_modules[i];
    return NULL;
}

static SrModule *module_by_uid(uint32_t uid) {
    for (int i = 0; i < SR_MAX_MODULES; i++)
        if (s_modules[i].used && s_modules[i].uid == uid)
            return &s_modules[i];
    return NULL;
}

static int module_active(uint32_t uid) {
    return module_by_uid(uid) != NULL;
}

static int is_hle_replaced_module(const char *name) {
    return name && name[0] == 's' && name[1] == 'c' && name[2] == 'e';
}

static uint32_t bind_module_syscall(uint32_t nid, const char *library) {
    const char *name = library ? library : "";
    for (uint32_t i = 0; i < s_syscall_count; i++) {
        if (s_syscalls[i].nid == nid &&
            strcmp(s_syscalls[i].library, name) == 0)
            return i + 1;
    }
    if (s_syscall_count >= SR_MAX_MODULE_SYSCALLS) {
        fprintf(stderr, "module loader: syscall binding table exhausted\n");
        return 0;
    }
    SrModuleSyscall *binding = &s_syscalls[s_syscall_count++];
    binding->nid = nid;
    snprintf(binding->library, sizeof(binding->library), "%s", name);
    return s_syscall_count;
}

int sr_module_syscall_nid(uint32_t code, uint32_t *nid) {
    if (!code || code > s_syscall_count || !nid) return 0;
    *nid = s_syscalls[code - 1].nid;
    return 1;
}

int sr_module_patch_import(
    uint32_t stub, uint32_t nid, const char *library) {
    if (!guest_range(stub, 8)) {
        fprintf(
            stderr,
            "module loader: import stub 0x%08x is outside guest memory\n",
            stub);
        return 0;
    }
    uint32_t code = bind_module_syscall(nid, library);
    if (!code || code > 0xfffffu) return 0;
    MEM_W32(stub, 0x03e00008u);
    MEM_W32(stub + 4, (code << 6) | 0x0000000cu);
    return 1;
}

static int register_guest_export(
    uint32_t module_uid, const char *library, uint32_t nid, uint32_t address) {
    if (s_export_count >= SR_MAX_GUEST_EXPORTS) {
        fprintf(stderr, "module loader: guest export table exhausted\n");
        return 0;
    }
    SrGuestExport *entry = &s_exports[s_export_count++];
    entry->active = 1;
    entry->module_uid = module_uid;
    entry->nid = nid;
    entry->address = address;
    snprintf(
        entry->library, sizeof(entry->library), "%s",
        library ? library : "");
    if (getenv("SR_MODULE_LOG"))
        fprintf(
            stderr, "module export %s/0x%08x -> 0x%08x\n",
            entry->library, nid, address);
    return 1;
}

int sr_module_find_export(
    uint32_t nid, uint32_t *address, const char **library) {
    for (uint32_t i = s_export_count; i != 0; i--) {
        SrGuestExport *entry = &s_exports[i - 1];
        if (!entry->active || entry->nid != nid ||
            !module_active(entry->module_uid))
            continue;
        if (address) *address = entry->address;
        if (library) *library = entry->library;
        return 1;
    }
    return 0;
}

static int apply_relocations(
    const uint8_t *data, uint32_t data_size,
    uint32_t section_offset, uint32_t section_size,
    const uint32_t *segment_bases, uint32_t segment_count) {
    if (!file_range(data_size, section_offset, section_size) ||
        (section_size & 7u) != 0)
        return 0;
    uint32_t count = section_size / 8;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t relocation = section_offset + i * 8;
        uint32_t offset = file_r32(data, relocation);
        uint32_t info = file_r32(data, relocation + 4);
        uint32_t type = info & 0xfu;
        uint32_t offset_segment = (info >> 8) & 0xffu;
        uint32_t address_segment = (info >> 16) & 0xffu;
        if (offset_segment >= segment_count) return 0;
        uint32_t address = segment_bases[offset_segment] + offset;
        uint32_t relocate_to =
            address_segment < segment_count
                ? segment_bases[address_segment]
                : 0;
        if (!guest_range(address, 4)) return 0;
        uint32_t op = MEM_R32(address);
        switch (type) {
            case 0:
            case 7:
                break;
            case 1:
            case 6:
                op = (op & 0xffff0000u) |
                     (((op & 0xffffu) + relocate_to) & 0xffffu);
                break;
            case 2:
                op += relocate_to;
                break;
            case 4:
                op = (op & 0xfc000000u) |
                     (((op & 0x03ffffffu) + (relocate_to >> 2)) &
                      0x03ffffffu);
                break;
            case 5: {
                int found_low = 0;
                for (uint32_t j = i + 1; j < count; j++) {
                    uint32_t next = section_offset + j * 8;
                    uint32_t next_offset = file_r32(data, next);
                    uint32_t next_info = file_r32(data, next + 4);
                    uint32_t next_type = next_info & 0xfu;
                    uint32_t next_segment = (next_info >> 8) & 0xffu;
                    if (next_type == 5) continue;
                    if (next_segment >= segment_count) return 0;
                    uint32_t low_address =
                        segment_bases[next_segment] + next_offset;
                    if (!guest_range(low_address, 4)) return 0;
                    uint32_t low_op = MEM_R32(low_address);
                    int32_t low = (int16_t)(low_op & 0xffffu);
                    uint32_t value =
                        ((op & 0xffffu) << 16) + (uint32_t)low +
                        relocate_to;
                    uint32_t high =
                        ((value >> 16) + ((value & 0x8000u) != 0)) &
                        0xffffu;
                    op = (op & 0xffff0000u) | high;
                    found_low = 1;
                    break;
                }
                if (!found_low) return 0;
                break;
            }
            default:
                fprintf(
                    stderr,
                    "module loader: unsupported relocation type %u\n",
                    type);
                return 0;
        }
        MEM_W32(address, op);
    }
    return 1;
}

static int section_named(
    const uint8_t *data, uint32_t data_size,
    uint32_t section_headers, uint16_t section_size,
    uint16_t section_count, uint16_t string_index,
    const char *wanted, uint32_t *address, uint32_t *size) {
    if (string_index >= section_count) return 0;
    uint32_t strings_header =
        section_headers + (uint32_t)string_index * section_size;
    if (!file_range(data_size, strings_header, 40)) return 0;
    uint32_t strings_offset = file_r32(data, strings_header + 16);
    uint32_t strings_size = file_r32(data, strings_header + 20);
    if (!file_range(data_size, strings_offset, strings_size)) return 0;
    for (uint16_t i = 0; i < section_count; i++) {
        uint32_t header = section_headers + (uint32_t)i * section_size;
        if (!file_range(data_size, header, 40)) return 0;
        uint32_t name_offset = file_r32(data, header);
        if (name_offset >= strings_size) continue;
        const char *name = (const char *)(data + strings_offset + name_offset);
        size_t available = strings_size - name_offset;
        if (!memchr(name, 0, available)) continue;
        if (strcmp(name, wanted) != 0) continue;
        if (address) *address = file_r32(data, header + 12);
        if (size) *size = file_r32(data, header + 20);
        return 1;
    }
    return 0;
}

static int parse_imports(SrModule *module, uint32_t start, uint32_t end) {
    uint32_t position = start;
    while (position < end) {
        if (!guest_range(position, 20)) return 0;
        uint32_t name_address = MEM_R32(position);
        uint32_t words = MEM_R8(position + 8);
        uint32_t variable_count = MEM_R8(position + 9);
        uint32_t function_count = MEM_R16(position + 10);
        uint32_t nid_table = MEM_R32(position + 12);
        uint32_t first_stub = MEM_R32(position + 16);
        if (words < 5 || words > (end - position) / 4) return 0;
        char library[48];
        guest_string(name_address, library, sizeof(library));
        if (function_count > 1024 ||
            !guest_range(nid_table, function_count * 4) ||
            !guest_range(first_stub, function_count * 8))
            return 0;
        for (uint32_t i = 0; i < function_count; i++) {
            uint32_t nid = MEM_R32(nid_table + i * 4);
            uint32_t stub = first_stub + i * 8;
            if (!sr_module_patch_import(stub, nid, library)) return 0;
        }
        if (getenv("SR_MODULE_LOG"))
            fprintf(
                stderr, "module %s imports %u funcs, %u vars from %s\n",
                module->name, function_count, variable_count, library);
        position += words * 4;
    }
    return position == end;
}

static int parse_exports(SrModule *module, uint32_t start, uint32_t end) {
    uint32_t position = start;
    while (position < end) {
        if (!guest_range(position, 16)) return 0;
        uint32_t name_address = MEM_R32(position);
        uint32_t words = MEM_R8(position + 8);
        uint32_t variable_count = MEM_R8(position + 9);
        uint32_t function_count = MEM_R16(position + 10);
        uint32_t table = MEM_R32(position + 12);
        if (words < 4 || words > (end - position) / 4) return 0;
        uint32_t count = function_count + variable_count;
        if (count > 2048 || !guest_range(table, count * 8)) return 0;
        char library[48];
        guest_string(name_address, library, sizeof(library));
        for (uint32_t i = 0; i < count; i++) {
            uint32_t nid = MEM_R32(table + i * 4);
            uint32_t address = MEM_R32(table + (count + i) * 4);
            if (i < function_count) {
                if (!name_address) {
                    if (nid == NID_MODULE_START)
                        module->module_start = address;
                    else if (nid == NID_MODULE_STOP)
                        module->module_stop = address;
                } else if (address && address != UINT32_MAX) {
                    if (!register_guest_export(
                            module->uid, library, nid, address))
                        return 0;
                }
            } else if (!name_address) {
                if (nid == NID_MODULE_INFO) {
                    continue;
                }
                if (nid == NID_MODULE_START_THREAD_PARAMETER &&
                    guest_range(address, 16) && MEM_R32(address) != 0) {
                    module->start_priority = MEM_R32(address + 4);
                    module->start_stack_size = MEM_R32(address + 8);
                    module->start_attr = MEM_R32(address + 12);
                }
            }
        }
        position += words * 4;
    }
    return position == end;
}

static uint32_t create_fake_module(const char *path) {
    SrModule *module = module_slot();
    if (!module) return SCE_KERNEL_ERROR_NO_MEMORY;
    memset(module, 0, sizeof(*module));
    module->used = 1;
    module->fake = 1;
    module->hle_replaced = 1;
    module->uid = sr_alloc_uid_type(SR_TMID_MODULE);
    snprintf(module->path, sizeof(module->path), "%s", path ? path : "");
    snprintf(module->name, sizeof(module->name), "%s", path_basename(path));
    if (getenv("SR_MODULE_LOG"))
        fprintf(
            stderr, "module loader: HLE-only module %s uid=0x%x\n",
            module->name, module->uid);
    return module->uid;
}

uint32_t sr_module_load_image(
    const uint8_t *data, uint32_t data_size, const char *path) {
    if (!data || data_size < 52)
        return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
    if (memcmp(data, "~PSP", 4) == 0)
        return create_fake_module(path);
    if (memcmp(data, "\x7f" "ELF", 4) != 0 ||
        data[4] != 1 || data[5] != 1 ||
        file_r16(data, 16) != 0xffa0u)
        return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;

    uint32_t program_headers = file_r32(data, 28);
    uint32_t section_headers = file_r32(data, 32);
    uint16_t program_size = file_r16(data, 42);
    uint16_t program_count = file_r16(data, 44);
    uint16_t section_size = file_r16(data, 46);
    uint16_t section_count = file_r16(data, 48);
    uint16_t string_index = file_r16(data, 50);
    if (program_size < 32 || section_size < 40 ||
        !file_range(
            data_size, program_headers,
            (uint32_t)program_size * program_count) ||
        !file_range(
            data_size, section_headers,
            (uint32_t)section_size * section_count))
        return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;

    uint32_t image_size = 0;
    uint32_t load_count = 0;
    for (uint16_t i = 0; i < program_count; i++) {
        uint32_t header = program_headers + (uint32_t)i * program_size;
        if (file_r32(data, header) != 1) continue;
        uint32_t file_offset = file_r32(data, header + 4);
        uint32_t virtual_address = file_r32(data, header + 8);
        uint32_t file_size = file_r32(data, header + 16);
        uint32_t memory_size = file_r32(data, header + 20);
        if (file_size > memory_size ||
            !file_range(data_size, file_offset, file_size) ||
            virtual_address > UINT32_MAX - memory_size)
            return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
        uint32_t end = virtual_address + memory_size;
        if (end > image_size) image_size = end;
        load_count++;
    }
    if (!load_count || load_count > 16 || !image_size)
        return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;

    uint32_t allocation_size = image_size;
    char tag[48];
    snprintf(tag, sizeof(tag), "ELF/%s", path_basename(path));
    uint32_t base =
        sr_ba_alloc(sr_kmem_user(), &allocation_size, 0, tag);
    if (base == SR_BA_FAIL) return SCE_KERNEL_ERROR_NO_MEMORY;
    if (getenv("SR_MODULE_LOG"))
        fprintf(
            stderr,
            "module loader: allocated %s at %08x-%08x\n",
            path ? path : "(unknown)", base, base + allocation_size);
    memset(SR_HOST(base), 0, allocation_size);

    uint32_t segment_bases[16];
    uint32_t segment_index = 0;
    for (uint16_t i = 0; i < program_count; i++) {
        uint32_t header = program_headers + (uint32_t)i * program_size;
        if (file_r32(data, header) != 1) continue;
        uint32_t file_offset = file_r32(data, header + 4);
        uint32_t virtual_address = file_r32(data, header + 8);
        uint32_t file_size = file_r32(data, header + 16);
        segment_bases[segment_index++] = base + virtual_address;
        memcpy(
            SR_HOST(base + virtual_address),
            data + file_offset,
            file_size);
    }

    for (uint16_t i = 0; i < section_count; i++) {
        uint32_t header = section_headers + (uint32_t)i * section_size;
        if (file_r32(data, header + 4) != SHT_PRX_RELOC) continue;
        if (!apply_relocations(
                data, data_size,
                file_r32(data, header + 16),
                file_r32(data, header + 20),
                segment_bases, segment_index)) {
            fprintf(
                stderr,
                "module loader: relocation section %u failed for %s\n",
                i, path ? path : "(unknown)");
            sr_ba_free(sr_kmem_user(), base);
            return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
        }
    }

    uint32_t module_info_offset, module_info_size;
    if (!section_named(
            data, data_size, section_headers, section_size,
            section_count, string_index, ".rodata.sceModuleInfo",
            &module_info_offset, &module_info_size) ||
        module_info_size < 52 ||
        !guest_range(base + module_info_offset, 52)) {
        fprintf(
            stderr,
            "module loader: missing or invalid module info in %s\n",
            path ? path : "(unknown)");
        sr_ba_free(sr_kmem_user(), base);
        return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
    }

    SrModule *module = module_slot();
    if (!module) {
        sr_ba_free(sr_kmem_user(), base);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }
    memset(module, 0, sizeof(*module));
    module->used = 1;
    module->uid = sr_alloc_uid_type(SR_TMID_MODULE);
    module->base = base;
    module->size = allocation_size;
    module->entry = base + file_r32(data, 24);
    uint32_t info = base + module_info_offset;
    module->attr = MEM_R16(info);
    for (int i = 0; i < 28 && MEM_R8(info + 4 + (uint32_t)i); i++)
        module->name[i] = (char)MEM_R8(info + 4 + (uint32_t)i);
    module->gp = MEM_R32(info + 32);
    uint32_t export_start = MEM_R32(info + 36);
    uint32_t export_end = MEM_R32(info + 40);
    uint32_t import_start = MEM_R32(info + 44);
    uint32_t import_end = MEM_R32(info + 48);
    module->start_priority = 0x20;
    module->start_stack_size = 0x40000;
    module->start_attr = module->attr;
    module->hle_replaced = is_hle_replaced_module(module->name);
    snprintf(module->path, sizeof(module->path), "%s", path ? path : "");

    if (!parse_imports(module, import_start, import_end) ||
        !parse_exports(module, export_start, export_end)) {
        fprintf(
            stderr,
            "module loader: invalid import/export tables in %s "
            "(imports %08x-%08x, exports %08x-%08x)\n",
            module->name, import_start, import_end,
            export_start, export_end);
        for (uint32_t i = 0; i < s_export_count; i++)
            if (s_exports[i].module_uid == module->uid)
                s_exports[i].active = 0;
        sr_unregister_uid(module->uid);
        sr_ba_free(sr_kmem_user(), base);
        memset(module, 0, sizeof(*module));
        return SCE_KERNEL_ERROR_UNSUPPORTED_PRX_TYPE;
    }
    if (module->module_start)
        module->entry = module->module_start;
    if (getenv("SR_MODULE_LOG") || !module->hle_replaced)
        fprintf(
            stderr,
            "module loader: %s uid=0x%x base=%08x size=%x "
            "entry=%08x gp=%08x%s\n",
            module->name, module->uid, module->base, module->size,
            module->entry, module->gp,
            module->hle_replaced ? " (HLE replaced)" : "");
    return module->uid;
}

uint32_t sr_module_start(
    uint32_t uid, uint32_t arg_size, uint32_t arg_address,
    uint32_t status_address, uint32_t option_address) {
    SrModule *module = module_by_uid(uid);
    if (!module) return SCE_KERNEL_ERROR_UNKNOWN_MODULE;
    if (module->started) return SCE_KERNEL_ERROR_ERROR;
    if (module->fake || module->hle_replaced ||
        !module->entry || module->entry == UINT32_MAX) {
        if (status_address && guest_range(status_address, 4))
            MEM_W32(status_address, 0);
        module->started = 1;
        return uid;
    }

    uint32_t priority =
        module->start_priority ? module->start_priority : 0x20;
    uint32_t stack_size =
        module->start_stack_size ? module->start_stack_size : 0x40000;
    uint32_t attr = module->start_attr;
    if (option_address && guest_range(option_address, 20) &&
        MEM_R32(option_address) != 0) {
        uint32_t option_stack = MEM_R32(option_address + 8);
        uint32_t option_priority = MEM_R32(option_address + 12);
        uint32_t option_attr = MEM_R32(option_address + 16);
        if (option_stack) stack_size = option_stack;
        if (option_priority) priority = option_priority;
        if (option_attr) attr = option_attr;
    }

    uint32_t thread = sched_create_thread(
        module->name, module->entry, (int)priority, stack_size, attr);
    if ((int32_t)thread < 0) return thread;
    sched_set_thread_gp(thread, module->gp);
    uint32_t result =
        sched_start_thread(thread, arg_size, arg_address);
    if ((int32_t)result < 0) {
        sched_delete_thread(thread);
        return result;
    }
    while (!sched_is_dormant(thread))
        sched_block_on(thread);
    int32_t status = sched_thread_exit_status(thread);
    if (status_address && guest_range(status_address, 4))
        MEM_W32(status_address, (uint32_t)status);
    sched_delete_thread(thread);
    module->started = 1;
    return uid;
}

uint32_t sr_module_unload(uint32_t uid) {
    SrModule *module = module_by_uid(uid);
    if (!module) return SCE_KERNEL_ERROR_UNKNOWN_MODULE;
    for (uint32_t i = 0; i < s_export_count; i++)
        if (s_exports[i].module_uid == uid)
            s_exports[i].active = 0;
    if (module->base && !sr_ba_free(sr_kmem_user(), module->base)) {
        fprintf(
            stderr,
            "module loader: cannot release %s at 0x%08x\n",
            module->name, module->base);
        return SCE_KERNEL_ERROR_ERROR;
    }
    sr_unregister_uid(uid);
    memset(module, 0, sizeof(*module));
    return uid;
}
