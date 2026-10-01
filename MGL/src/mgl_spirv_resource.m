/*
 * mgl_spirv_resource.m
 * MGL
 *
 * Implementation of the SPIR-V Resource Helper Subsystem.
 * See mgl_spirv_resource.h for the API contract.
 */

#import "mgl_spirv_resource.h"

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
