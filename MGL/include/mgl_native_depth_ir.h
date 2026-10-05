#ifndef MGL_NATIVE_DEPTH_IR_H
#define MGL_NATIVE_DEPTH_IR_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Private, transactional fragment variant. Resource IDs index the masks;
 * public GL reflection and the source module remain untouched. Unsupported
 * resource uses reject the entire requested combination. Caller frees output.
 * Single non-array float sampler2D only, sample/LOD/Grad/fetch and size queries.
 * Both input and output are checked by SPIRV-Tools before publication. */
bool mglNativeDepthIR(const uint32_t *words, size_t count,
                      const uint32_t *resources, size_t resource_count,
                      uint64_t depth_mask, uint64_t flip_mask,
                      uint32_t **output, size_t *output_count);
#endif
