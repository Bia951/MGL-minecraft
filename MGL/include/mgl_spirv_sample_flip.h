#ifndef MGL_SPIRV_SAMPLE_FLIP_H
#define MGL_SPIRV_SAMPLE_FLIP_H

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MGL_SPIRV_SAMPLE_FLIP_MAX_RESOURCES 64u

typedef struct MGLSpirvSampleFlipResource {
    uint32_t resource_id;
    uint32_t spec_id;
} MGLSpirvSampleFlipResource;

typedef struct MGLSpirvSampleFlipStats {
    uint32_t candidate_resources;
    uint32_t accepted_resources;
    uint32_t rejected_resources;
    uint32_t rewritten_sample_ops;
    uint32_t rewritten_fetch_ops;
} MGLSpirvSampleFlipStats;

/* Inline opaque sampler functions, then conditionally transform supported
 * 2D non-array, non-multisampled sampled-image coordinates. On success,
 * *out_words is malloc-owned and resource metadata maps original resource
 * variable IDs to SPIR-V specialization IDs. On failure, outputs are cleared
 * and no transformed metadata is published. */
bool mglTransformSPIRVSampleFlip(const uint32_t *words,
                                size_t word_count,
                                uint32_t **out_words,
                                size_t *out_word_count,
                                MGLSpirvSampleFlipResource *out_resources,
                                size_t resource_capacity,
                                size_t *out_resource_count,
                                MGLSpirvSampleFlipStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif
