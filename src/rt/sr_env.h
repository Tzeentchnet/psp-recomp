/* Cached getenv for string-literal names. The environment is fixed after startup, but several
 * runtime paths (scheduler loop, HLE calls, GE ops) query it per iteration; a CRT getenv scans
 * the whole environment block each time. Include after <stdlib.h>. Only literals may be passed
 * through the macro; use (getenv)(name) for computed names. */
#ifndef SR_ENV_H
#define SR_ENV_H

#include <stdint.h>
#include <stdlib.h>

#define SR_ENV_SLOTS 256u

static inline const char *sr_getenv_cached(const char *name) {
    static const char *names[SR_ENV_SLOTS];
    static const char *values[SR_ENV_SLOTS];
    uint32_t slot = (uint32_t)(((uintptr_t)name >> 3) * 2654435761u) & (SR_ENV_SLOTS - 1u);
    for (uint32_t probe = 0; probe < SR_ENV_SLOTS; probe++) {
        uint32_t i = (slot + probe) & (SR_ENV_SLOTS - 1u);
        if (names[i] == name) return values[i];
        if (!names[i]) {
            names[i] = name;
            values[i] = (getenv)(name);
            return values[i];
        }
    }
    return (getenv)(name);
}

#define getenv(name) sr_getenv_cached(name)

#endif
