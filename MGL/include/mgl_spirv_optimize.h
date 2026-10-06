#ifndef MGL_SPIRV_OPTIMIZE_H
#define MGL_SPIRV_OPTIMIZE_H

#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Optimize a malloc-owned SPIR-V word array for performance under the
 * OpenGL 4.5 environment. On success, replaces *words with a malloc-owned
 * optimized array and updates *word_count. On failure, leaves both inputs
 * unchanged. */
bool mglOptimizeSPIRVForPerformance(unsigned **words, size_t *word_count);

#ifdef __cplusplus
}
#endif

#endif
