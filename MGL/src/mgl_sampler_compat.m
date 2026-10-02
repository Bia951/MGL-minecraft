/*
 * mgl_sampler_compat.m
 * MGL
 *
 * Implementation of the Sampler Compatibility Subsystem.
 *
 * See mgl_sampler_compat.h for the architectural rationale.  This module
 * owns the pure spec-compliance helpers for translating OpenGL sampler /
 * resource semantics to Metal binding:
 *   - Program SPIR-V resource queries (by name, image dim, Metal binding).
 *   - Sampler-like resource classification heuristics.
 *   - Binding-trace gating for debugging.
 *
 * The helpers here are pure: they do not touch the renderer ivar, the
 * command buffer, or the render encoder.  They operate only on the
 * Program / SpirvResource structures passed in as arguments.
 *
 * External dependencies:
 *   - Program / SpirvResource / SpirvResourceList types (glm_context.h).
 *   - SPVC_RESOURCE_TYPE_* constants (spirv_cross_c.h, pulled through
 *     MGLRenderer.m's include chain).
 *   - _MAX_SHADER_TYPES / _MAX_SPIRV_RES / _VERTEX_SHADER (glm_context.h).
 *   - TEXTURE_UNITS (glm_limits.h).
 */

#import "mgl_sampler_compat.h"
#include "mgl_uniform_reflection.h"
#include "mgl_msl_compat.h"
#include "mgl_trace_strategy.h"
#import <Foundation/Foundation.h>
#import "spirv_cross_c.h"
#include <string.h>

/* === Program SPIR-V resource queries === */

bool mglProgramHasImageDim(Program *program, GLuint imageDim)
{
    if (!program) {
        return false;
    }

    const int resourceTypes[] = {
        SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_IMAGE,
        SPVC_RESOURCE_TYPE_STORAGE_IMAGE
    };

    for (int stage = 0; stage < _MAX_SHADER_TYPES; stage++) {
        for (size_t t = 0; t < sizeof(resourceTypes) / sizeof(resourceTypes[0]); t++) {
            int type = resourceTypes[t];
            if (type < 0 || type >= _MAX_SPIRV_RES) {
                continue;
            }
            SpirvResourceList *resources = &program->spirv_resources_list[stage][type];
            for (GLuint i = 0; i < resources->count; i++) {
                if (resources->list[i].image_dim == imageDim) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool mglProgramHasResourceName(Program *program,
                               int stage,
                               int type,
                               const char *name)
{
    if (!program || stage < 0 || stage >= _MAX_SHADER_TYPES ||
        type < 0 || type >= _MAX_SPIRV_RES || !name) {
        return false;
    }

    SpirvResourceList *resources = &program->spirv_resources_list[stage][type];
    for (GLuint i = 0; resources->list && i < resources->count; i++) {
        if (resources->list[i].name && strcmp(resources->list[i].name, name) == 0) {
            return true;
        }
    }

    return false;
}

bool mglProgramHasAnyResourceName(Program *program, const char *name)
{
    if (!program || !name) {
        return false;
    }

    for (int stage = 0; stage < _MAX_SHADER_TYPES; stage++) {
        for (int type = 0; type < _MAX_SPIRV_RES; type++) {
            if (mglProgramHasResourceName(program, stage, type, name)) {
                return true;
            }
        }
    }

    return false;
}

bool mglProgramHasResourceNamed(Program *program,
                                int stage,
                                int type,
                                const char *name)
{
    if (!program || !name || stage < 0 || stage >= _MAX_SHADER_TYPES ||
        type < 0 || type >= _MAX_SPIRV_RES) {
        return false;
    }

    SpirvResourceList *resources = &program->spirv_resources_list[stage][type];
    for (GLuint i = 0; i < resources->count; i++) {
        SpirvResource *res = &resources->list[i];
        if (res->name && strcmp(res->name, name) == 0) {
            return true;
        }
    }

    return false;
}

/* === Binding-trace gating === */

bool mglProgramNeedsBindingTrace(Program *program)
{
    if (!program) {
        return false;
    }

    return mglTraceLogIsEnabled() &&
           (mglTraceLogResourcesVerbose() || mglTraceLogProgramListContains(program->name));
}

/* === Sampler-like resource classification === */

bool mglRendererResourceLooksSamplerLike(const SpirvResource *res, int type)
{
    return mglProgramResourceLooksSamplerLike(res, type);
}

SpirvResource *mglFindSamplerResourceForMetalBinding(Program *program,
                                                     int stage,
                                                     GLuint metalBinding)
{
    static const int samplerResourceTypes[] = {
        SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT,
        SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS,
        SPVC_RESOURCE_TYPE_STORAGE_IMAGE
    };

    if (!program || stage < 0 || stage >= _MAX_SHADER_TYPES || metalBinding >= TEXTURE_UNITS) {
        return NULL;
    }

    for (size_t rt = 0; rt < sizeof(samplerResourceTypes) / sizeof(samplerResourceTypes[0]); rt++) {
        int resType = samplerResourceTypes[rt];
        SpirvResourceList *resources = &program->spirv_resources_list[stage][resType];
        for (GLuint i = 0; resources->list && i < resources->count; i++) {
            SpirvResource *res = &resources->list[i];
            if (res->binding == metalBinding &&
                mglRendererResourceLooksSamplerLike(res, resType)) {
                return res;
            }
        }
    }

    return NULL;
}

/* Shared unit precedence for draw-time binding and hazard tracking. */
GLint mglResolveSamplerTextureUnit(Program *program,
                                  const SpirvResource *resource,
                                  GLuint metalBinding,
                                  int stage)
{
    if (!program) {
        return resource && resource->sampler_unit >= 0 &&
               resource->sampler_unit < (GLint)TEXTURE_UNITS
            ? resource->sampler_unit
            : (GLint)metalBinding;
    }

    /* Explicit glUniform1i state on the resource wins over binding-level
     * defaults. Non-explicit reflected sampler values are only a fallback,
     * after stage/global sampler-unit state, matching the draw-time resolver. */
    if (resource && resource->sampler_unit_explicit &&
        resource->sampler_unit >= 0 &&
        resource->sampler_unit < (GLint)TEXTURE_UNITS) {
        return resource->sampler_unit;
    }

    if (metalBinding >= TEXTURE_UNITS) {
        return (GLint)metalBinding;
    }

    bool stageValid = (stage >= 0 && stage < _MAX_SHADER_TYPES);
    bool stageExplicit = stageValid
        ? (program->sampler_units_explicit_by_stage[stage][metalBinding] == GL_TRUE)
        : false;
    bool globalExplicit = (program->sampler_units_explicit[metalBinding] == GL_TRUE);

    /* 2. Explicit stage array. */
    GLint unit = stageValid
        ? program->sampler_units_by_stage[stage][metalBinding]
        : program->sampler_units[metalBinding];
    if (stageExplicit && unit >= 0 && unit < (GLint)TEXTURE_UNITS) {
        return unit;
    }

    /* 3. Explicit global array. */
    unit = program->sampler_units[metalBinding];
    if (globalExplicit && unit >= 0 && unit < (GLint)TEXTURE_UNITS) {
        return unit;
    }

    /* 4. Non-explicit defaults (stage then global fallback). */
    GLint defaultUnit = stageValid
        ? program->sampler_units_by_stage[stage][metalBinding]
        : program->sampler_units[metalBinding];
    if (defaultUnit < 0 || defaultUnit >= (GLint)TEXTURE_UNITS) {
        defaultUnit = program->sampler_units[metalBinding];
    }

    /* 5. Per-resource non-explicit (set by reflection, not glUniform1i). */
    if (resource && !resource->sampler_unit_explicit &&
        resource->sampler_unit >= 0 &&
        resource->sampler_unit < (GLint)TEXTURE_UNITS) {
        return resource->sampler_unit;
    }

    if (defaultUnit >= 0 && defaultUnit < (GLint)TEXTURE_UNITS) {
        return defaultUnit;
    }

    /* 6. OpenGL default is unit 0. */
    return 0;
}

/* Resolves the GL texture unit that `res` samples after applying sampler-like
 * resource filtering for the hazard tracker. */
GLint mglSamplerResourceTextureUnit(Program *program,
                                   const SpirvResource *res,
                                   int stage,
                                   int resType)
{
    if (!program || !res || !mglRendererResourceLooksSamplerLike(res, resType)) {
        return -1;
    }
    return mglResolveSamplerTextureUnit(program, res, res->binding, stage);
}

bool mglProgramSamplesTextureUnit(Program *program, GLuint unit)
{
    if (!program) return false;

    static const int samplerResourceTypes[] = {
        SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT,
        SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS,
        SPVC_RESOURCE_TYPE_STORAGE_IMAGE
    };

    for (int stage = 0; stage < _MAX_SHADER_TYPES; stage++) {
        for (size_t rt = 0; rt < sizeof(samplerResourceTypes) / sizeof(samplerResourceTypes[0]); rt++) {
            int resType = samplerResourceTypes[rt];
            if (resType < 0 || resType >= _MAX_SPIRV_RES) continue;
            SpirvResourceList *resources = &program->spirv_resources_list[stage][resType];
            for (GLuint i = 0; resources->list && i < resources->count; i++) {
                GLint resolved = mglSamplerResourceTextureUnit(program,
                                                               &resources->list[i],
                                                               stage,
                                                               resType);
                if (resolved >= 0 && (GLuint)resolved == unit) {
                    return true;
                }
            }
        }
    }

    return false;
}

static uint16_t mglResourceTextureTargetMask(const SpirvResource *res, int type)
{
    const uint16_t allTargets = (1u << _MAX_TEXTURE_TYPES) - 1u;
    /* Storage images have a different GL binding namespace. Keep the old
     * conservative texture-unit treatment until image hazards are unified. */
    if (type == SPVC_RESOURCE_TYPE_STORAGE_IMAGE ||
        (type == SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT && !res->has_image_type)) {
        return allTargets;
    }
    int target;
    switch ((SpvDim)res->image_dim) {
        case SpvDim1D:
            target = res->image_arrayed ? _TEXTURE_1D_ARRAY : _TEXTURE_1D;
            break;
        case SpvDim2D:
            target = res->image_multisampled
                ? (res->image_arrayed ? _TEXTURE_2D_MULTISAMPLE_ARRAY : _TEXTURE_2D_MULTISAMPLE)
                : (res->image_arrayed ? _TEXTURE_2D_ARRAY : _TEXTURE_2D);
            break;
        case SpvDim3D: target = _TEXTURE_3D; break;
        case SpvDimCube:
            target = res->image_arrayed ? _TEXTURE_CUBE_MAP_ARRAY : _TEXTURE_CUBE_MAP;
            break;
        case SpvDimRect: target = _TEXTURE_RECTANGLE; break;
        case SpvDimBuffer: target = _TEXTURE_BUFFER_TARGET; break;
        default: return allTargets;
    }
    return (uint16_t)(1u << target);
}

static void mglBuildProgramTextureTargetMasks(Program *program, int stage,
                                             uint16_t masks[TEXTURE_UNITS])
{
    static const int types[] = {
        SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
        SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, SPVC_RESOURCE_TYPE_STORAGE_IMAGE
    };
    /* Separate sampler objects carry filtering state, not a texture target.
     * Their image resources supply the texture dependencies. */
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        int type = types[t];
        SpirvResourceList *list = &program->spirv_resources_list[stage][type];
        for (GLuint i = 0; list->list && i < list->count; i++) {
            SpirvResource *res = &list->list[i];
            if (!mglRendererResourceLooksSamplerLike(res, type)) continue;
            /* Match the draw-time binding decision: a reflected resource
             * removed from the executable MSL cannot read its GL binding. */
            if (mglShouldSkipStageTextureResource(program, stage, type, res)) continue;
            uint16_t targets = mglResourceTextureTargetMask(res, type);
            GLuint count = res->gl_array_size > 1 ? (GLuint)res->gl_array_size : 1u;
            for (GLuint element = 0; element < count && element < TEXTURE_UNITS; element++) {
                GLuint binding = res->binding + element;
                if (binding >= TEXTURE_UNITS) break;
                GLint unit = mglResolveSamplerTextureUnit(program, element ? NULL : res, binding, stage);
                if (unit >= 0 && unit < TEXTURE_UNITS) masks[unit] |= targets;
            }
        }
    }
}

void mglAccumulateProgramTextureTargetMasks(Program *program, int stage,
                                            uint16_t masks[TEXTURE_UNITS])
{
    if (!program || stage < 0 || stage >= _MAX_SHADER_TYPES) return;

    if (!program->sampler_texture_target_masks_valid[stage]) {
        uint16_t *cached = program->sampler_texture_target_masks[stage];
        memset(cached, 0, sizeof(program->sampler_texture_target_masks[stage]));
        mglBuildProgramTextureTargetMasks(program, stage, cached);
        program->sampler_texture_target_masks_valid[stage] = GL_TRUE;
    }

    for (GLuint unit = 0; unit < TEXTURE_UNITS; unit++) {
        masks[unit] |= program->sampler_texture_target_masks[stage][unit];
    }
}
