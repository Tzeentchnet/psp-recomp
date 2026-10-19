/* Vulkan GE backend descriptor (see README.md). */
#ifndef GE_GPU_H
#define GE_GPU_H

#include "../ge_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SDL3/Vulkan presenter must be initialized before backend->init(). */
const GeBackend *gegpu_backend(void);

#ifdef __cplusplus
}
#endif

#endif
