#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>

#define SR_MAX_DISPATCH_TABLES 32

static const SrDispatchTable *s_dispatch_tables[SR_MAX_DISPATCH_TABLES];
static uint32_t s_dispatch_table_count;
static SrDispatchTable s_legacy_table;
static RecompFn *s_disabled_functions;
static size_t s_disabled_function_count;
static size_t s_disabled_function_capacity;

static int function_is_disabled(RecompFn function) {
    for (size_t i = 0; i < s_disabled_function_count; i++)
        if (s_disabled_functions[i] == function) return 1;
    return 0;
}

void sr_disable_native_function(RecompFn function) {
    if (!function || function_is_disabled(function)) return;
    if (s_disabled_function_count == s_disabled_function_capacity) {
        size_t capacity =
            s_disabled_function_capacity
                ? s_disabled_function_capacity * 2
                : 32;
        RecompFn *grown = (RecompFn *)realloc(
            s_disabled_functions, capacity * sizeof(*grown));
        if (!grown) {
            fprintf(
                stderr,
                "dispatch table: cannot grow disabled function list\n");
            abort();
        }
        s_disabled_functions = grown;
        s_disabled_function_capacity = capacity;
    }
    s_disabled_functions[s_disabled_function_count++] = function;
}

static int validate_table(const SrDispatchTable *table) {
    if (!table || !table->name || !table->name[0] ||
        (table->count != 0 && table->entries == NULL)) {
        fprintf(stderr, "dispatch table: invalid descriptor\n");
        return 0;
    }
    for (uint32_t i = 0; i < table->count; i++) {
        if (table->entries[i].function == NULL) {
            fprintf(
                stderr,
                "dispatch table %s: null function at entry %u "
                "(0x%08x)\n",
                table->name, i, table->entries[i].address);
            return 0;
        }
        if ((table->entries[i].address & 3u) != 0) {
            fprintf(
                stderr,
                "dispatch table %s: unaligned address at entry %u "
                "(0x%08x)\n",
                table->name, i, table->entries[i].address);
            return 0;
        }
        if (i != 0 &&
            table->entries[i - 1].address >=
                table->entries[i].address) {
            fprintf(
                stderr,
                "dispatch table %s: entries %u and %u are not ordered\n",
                table->name, i - 1, i);
            return 0;
        }
    }
    return 1;
}

static RecompFn table_lookup(
    const SrDispatchTable *table, uint32_t address) {
    uint32_t low = 0;
    uint32_t high = table->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t candidate = table->entries[middle].address;
        if (candidate < address)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < table->count &&
        table->entries[low].address == address)
        return table->entries[low].function;
    return NULL;
}

int sr_activate_dispatch_table(const SrDispatchTable *table) {
    if (!validate_table(table)) return 0;
    for (uint32_t i = 0; i < s_dispatch_table_count; i++) {
        if (s_dispatch_tables[i] == table) return 1;
        for (uint32_t j = 0; j < table->count; j++) {
            if (table_lookup(
                    s_dispatch_tables[i],
                    table->entries[j].address)) {
                fprintf(
                    stderr,
                    "dispatch table %s collides with %s at 0x%08x\n",
                    table->name, s_dispatch_tables[i]->name,
                    table->entries[j].address);
                return 0;
            }
        }
    }
    if (s_dispatch_table_count == SR_MAX_DISPATCH_TABLES) {
        fprintf(
            stderr, "dispatch table: active table limit %u reached\n",
            SR_MAX_DISPATCH_TABLES);
        return 0;
    }
    s_dispatch_tables[s_dispatch_table_count++] = table;
    return 1;
}

int sr_deactivate_dispatch_table(const SrDispatchTable *table) {
    for (uint32_t i = 0; i < s_dispatch_table_count; i++) {
        if (s_dispatch_tables[i] != table) continue;
        for (uint32_t j = i + 1; j < s_dispatch_table_count; j++)
            s_dispatch_tables[j - 1] = s_dispatch_tables[j];
        s_dispatch_tables[--s_dispatch_table_count] = NULL;
        return 1;
    }
    return 0;
}

void sr_set_dispatch_table(const SrDispatchEntry *entries, uint32_t count) {
    s_dispatch_table_count = 0;
    if (!count) {
        if (entries) {
            fprintf(
                stderr,
                "sr_set_dispatch_table: nonnull entries with zero count\n");
            abort();
        }
        return;
    }
    s_legacy_table.name = "primary";
    s_legacy_table.entries = entries;
    s_legacy_table.count = count;
    if (!sr_activate_dispatch_table(&s_legacy_table))
        abort();
}

RecompFn sr_lookup(uint32_t address) {
    for (uint32_t i = 0; i < s_dispatch_table_count; i++) {
        RecompFn function =
            table_lookup(s_dispatch_tables[i], address);
        if (function && !function_is_disabled(function))
            return function;
    }
    return NULL;
}

const char *sr_interpreter_module(uint32_t address) {
    for (uint32_t i = 0; i < s_dispatch_table_count; i++) {
        RecompFn function =
            table_lookup(s_dispatch_tables[i], address);
        if (function && function_is_disabled(function))
            return s_dispatch_tables[i]->name;
    }
    return NULL;
}
