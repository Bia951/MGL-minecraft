/*
 * mgl_spirv_resource.m
 * MGL
 *
 * Implementation of the SPIR-V Resource Helper Subsystem.
 * See mgl_spirv_resource.h for the API contract.
 */

#import "mgl_spirv_resource.h"
#include "mgl_msl_compat.h"
#include "mgl_sampler_compat.h"

#import <Foundation/Foundation.h>
#import "spirv_cross_c.h"

#include <string.h>

GLuint mglClientBufferBindingForResource(int resourceType, const SpirvResource *res)
{
    if (!res) {
        return 0u;
    }

    /*
     * Plain uniforms are represented internally as one tiny GL buffer per
     * uniform location. SPIRV-Cross usually reports descriptor binding 0 for
     * all of them, while the generated MSL assigns distinct [[buffer(n)]]
     * slots. Use the GL uniform location to find the client-side buffer, then
     * map that location to the reflected Metal slot later.
     */
    if (resourceType == SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT) {
        if (res->uniform_location >= 0 && res->uniform_location < MAX_BINDABLE_BUFFERS) {
            return (GLuint)res->uniform_location;
        }
        if (res->location < MAX_BINDABLE_BUFFERS) {
            return res->location;
        }
        if (res->gl_binding < MAX_BINDABLE_BUFFERS) {
            return res->gl_binding;
        }
    }

    return res->gl_binding;
}

GLuint mglMetalResourceSlot(const SpirvResource *res)
{
    return res ? res->binding : 0u;
}

GLuint mglStageBufferResourceElementCount(int resourceType, const SpirvResource *res)
{
    if (resourceType == SPVC_RESOURCE_TYPE_UNIFORM_BUFFER &&
        res &&
        res->ubo_array_size > 1u) {
        return res->ubo_array_size;
    }
    if (resourceType == SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT &&
        res &&
        res->ubo_members &&
        res->gl_array_size > 1) {
        return (GLuint)res->gl_array_size;
    }
    if (resourceType == SPVC_RESOURCE_TYPE_STORAGE_BUFFER &&
        res &&
        res->gl_array_size > 1) {
        return (GLuint)res->gl_array_size;
    }

    return 1u;
}

GLuint mglClientBufferBindingForResourceElement(int resourceType,
                                                const SpirvResource *res,
                                                GLuint element)
{
    GLuint baseBinding = mglClientBufferBindingForResource(resourceType, res);

    if (resourceType == SPVC_RESOURCE_TYPE_UNIFORM_BUFFER &&
        res &&
        res->ubo_array_bindings &&
        element < res->ubo_array_size) {
        return res->ubo_array_bindings[element];
    }

    return baseBinding + element;
}

GLuint mglMetalResourceSlotForElement(const SpirvResource *res, GLuint element)
{
    return mglMetalResourceSlot(res) + element;
}

bool mglPlainUniformAllowsGlobalFallback(const SpirvResource *res)
{
    (void)res;
    /* Default-block uniform values belong to one linked program. An unset
     * uniform starts at zero; a context-wide numeric slot is another value. */
    return false;
}

const char *mglSpirvResourceTypeName(int type)
{
    switch (type) {
        case SPVC_RESOURCE_TYPE_UNIFORM_BUFFER: return "uniform_buffer";
        case SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT: return "uniform_constant";
        case SPVC_RESOURCE_TYPE_STORAGE_BUFFER: return "storage_buffer";
        case SPVC_RESOURCE_TYPE_STAGE_INPUT: return "stage_input";
        case SPVC_RESOURCE_TYPE_STAGE_OUTPUT: return "stage_output";
        case SPVC_RESOURCE_TYPE_SAMPLED_IMAGE: return "sampled_image";
        case SPVC_RESOURCE_TYPE_SEPARATE_IMAGE: return "separate_image";
        case SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS: return "separate_sampler";
        case SPVC_RESOURCE_TYPE_PUSH_CONSTANT: return "push_constant";
        default: return "resource";
    }
}

/* Select client buffer sources from the same resource metadata used to bind
 * the draw. Masks deduplicate shared locations across shader stages. */
#define MGL_BINDING_MASK_WORDS ((MAX_BINDABLE_BUFFERS + 63u) / 64u)

static void mglSelectBufferSlot(uint64_t *mask, GLint slot)
{
    if (slot >= 0 && slot < MAX_BINDABLE_BUFFERS) {
        mask[(GLuint)slot / 64u] |= UINT64_C(1) << ((GLuint)slot % 64u);
    }
}

void mglVisitDrawBufferBindings(GLMContext ctx, MGLDrawBufferBindingVisitor visit, void *data)
{
    static const int types[] = { SPVC_RESOURCE_TYPE_UNIFORM_BUFFER,
        SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT, SPVC_RESOURCE_TYPE_STORAGE_BUFFER,
        SPVC_RESOURCE_TYPE_ATOMIC_COUNTER };
    static const int targets[] = { _UNIFORM_BUFFER, _UNIFORM_CONSTANT,
        _SHADER_STORAGE_BUFFER, _ATOMIC_COUNTER_BUFFER, _TEXTURE_BUFFER };
    uint64_t baseMasks[5][MGL_BINDING_MASK_WORDS] = {{0}};
    uint64_t plainMasks[_MAX_SHADER_TYPES][MGL_BINDING_MASK_WORDS] = {{0}};
    Program *programs[_MAX_SHADER_TYPES] = {0};
    GLuint programCount = 0;
    ProgramPipeline *pipeline = ctx->state.program_pipeline;
    if (!ctx->state.program && !pipeline && ctx->state.var.program_pipeline_binding) {
        pipeline = searchHashTable(&ctx->state.program_pipeline_table,
                                   ctx->state.var.program_pipeline_binding);
    }
    for (int stage = 0; stage < _MAX_SHADER_TYPES; stage++) {
        if (stage == _COMPUTE_SHADER) continue;
        Program *program = ctx->state.program;
        if (!program && pipeline) program = pipeline->stage_programs[stage];
        if (!program) continue;
        GLuint pidx = 0;
        while (pidx < programCount && programs[pidx] != program) pidx++;
        if (pidx == programCount) programs[programCount++] = program;
        if (!program->draw_buffer_slot_masks_valid[stage]) {
            memset(program->draw_buffer_slot_masks[stage], 0,
                   sizeof(program->draw_buffer_slot_masks[stage]));
            for (GLuint t = 0; t < 4; t++) {
                SpirvResourceList *list = &program->spirv_resources_list[stage][types[t]];
                uint64_t *stageMask = program->draw_buffer_slot_masks[stage][t];
                for (GLuint r = 0; list->list && r < list->count; r++) {
                    SpirvResource *res = &list->list[r];
                    /* Argument-buffer resources are bound by their own encoder
                     * path, but still consume these GL client bindings. */
                    if (!res->uses_argument_buffer &&
                        mglShouldSkipStageBufferResource(program, stage, types[t], res)) continue;
                    if (t == 1 && mglRendererResourceLooksSamplerLike(res, types[t])) continue;
                    if (t == 1 && res->ubo_members && res->ubo_member_count && res->required_size) {
                        GLint base = res->uniform_location >= 0
                            ? res->uniform_location : (GLint)res->location;
                        for (GLuint m = 0; m < res->ubo_member_count; m++) {
                            SpirvUBOMember *member = &res->ubo_members[m];
                            GLint count = member->size > 1 ? member->size : 1;
                            for (GLint a = 0; a < count && a < MAX_BINDABLE_BUFFERS; a++) {
                                GLint slot = base + member->location_offset + a;
                                mglSelectBufferSlot(stageMask, slot);
                            }
                        }
                    } else {
                        GLuint count = mglStageBufferResourceElementCount(types[t], res);
                        for (GLuint a = 0; a < count && a < MAX_BINDABLE_BUFFERS; a++) {
                            GLuint slot = mglClientBufferBindingForResourceElement(types[t], res, a);
                            mglSelectBufferSlot(stageMask, (GLint)slot);
                        }
                    }
                }
            }
            program->draw_buffer_slot_masks_valid[stage] = GL_TRUE;
        }

        /* These masks depend on a program's reflected stage resources, not on
         * the currently bound Buffer objects. Union cached slot sets into the
         * per-draw target masks and the program's plain-uniform mask. */
        for (GLuint t = 0; t < 4; t++) {
            uint64_t *drawMask = t == 1 ? plainMasks[pidx] : baseMasks[t];
            const uint64_t *stageMask = program->draw_buffer_slot_masks[stage][t];
            for (GLuint w = 0; w < MGL_BINDING_MASK_WORDS; w++) {
                drawMask[w] |= stageMask[w];
            }
        }
    }
    /* Texel-buffer backing is resolved through textures rather than the four
     * shader-buffer resource classes. Preserve its existing dependencies. */
    for (GLuint w = 0; w < MGL_BINDING_MASK_WORDS; w++) {
        baseMasks[4][w] = UINT64_MAX;
        if (!programCount) for (GLuint t = 0; t < 4; t++) baseMasks[t][w] = UINT64_MAX;
    }
    for (GLuint t = 0; t < 5; t++) {
        for (GLuint w = 0; w < MGL_BINDING_MASK_WORDS; w++) {
            uint64_t bits = baseMasks[t][w];
            while (bits) {
                GLuint slot = w * 64u + (GLuint)__builtin_ctzll(bits);
                bits &= bits - 1u;
                if (slot < MAX_BINDABLE_BUFFERS) {
                    const BufferBaseTarget *binding =
                        &ctx->state.buffer_base[targets[t]].buffers[slot];
                    /* Texture-buffer backing has no shader-block dependency
                     * mask yet, so inspect all slots; empty slots cannot
                     * contribute either a live GL buffer name or resolved
                     * Buffer object and need no hash/hazard visitor work. */
                    if (t == 4 && binding->buffer == 0 && binding->buf == NULL) {
                        continue;
                    }
                    visit(ctx, binding,
                          ((uint64_t)(targets[t] + 1) * 131u) + slot, data);
                }
            }
        }
    }
    for (GLuint pidx = 0; pidx < programCount; pidx++) {
        for (GLuint w = 0; w < MGL_BINDING_MASK_WORDS; w++) {
            uint64_t bits = plainMasks[pidx][w];
            while (bits) {
                GLuint slot = w * 64u + (GLuint)__builtin_ctzll(bits);
                bits &= bits - 1u;
                if (slot < MAX_BINDABLE_BUFFERS) visit(ctx,
                    &programs[pidx]->plain_uniform_buffers[slot],
                    0x700u + slot + (uint64_t)programs[pidx]->name * 521u, data);
            }
        }
    }
}
