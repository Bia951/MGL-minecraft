#ifndef MGL_TEXEL_BUFFER_H
#define MGL_TEXEL_BUFFER_H

#include "spirv_cross_c.h"

/* Process-wide opt-in for native Metal texture-buffer MSL. The environment
 * value is sampled once so shader compilation and renderer setup agree. */
#ifdef __cplusplus
extern "C" {
#endif
int mglNativeTexelBufferEnabled(void);
#ifdef __cplusplus
}
#endif

/* Native Metal texture-buffer MSL is currently supported only for sampled
 * images. Storage images need a writeback path when staging is required. */
static inline spvc_result mglSPVCFindStorageTexelBufferImage(spvc_compiler compiler,
                                                              spvc_bool *found)
{
    if (!compiler || !found) {
        return SPVC_ERROR_INVALID_ARGUMENT;
    }

    *found = SPVC_FALSE;
    spvc_resources resources = NULL;
    spvc_result result = spvc_compiler_create_shader_resources(compiler, &resources);
    if (result != SPVC_SUCCESS || !resources) {
        return result != SPVC_SUCCESS ? result : SPVC_ERROR_INVALID_ARGUMENT;
    }

    const spvc_reflected_resource *images = NULL;
    size_t image_count = 0;
    result = spvc_resources_get_resource_list_for_type(resources,
                                                        SPVC_RESOURCE_TYPE_STORAGE_IMAGE,
                                                        &images,
                                                        &image_count);
    if (result != SPVC_SUCCESS) {
        return result;
    }

    for (size_t i = 0; i < image_count; i++) {
        spvc_type image_type = spvc_compiler_get_type_handle(compiler, images[i].type_id);
        if (!image_type) {
            return SPVC_ERROR_INVALID_ARGUMENT;
        }
        if (spvc_type_get_image_dimension(image_type) == SpvDimBuffer) {
            *found = SPVC_TRUE;
            break;
        }
    }
    return SPVC_SUCCESS;
}

#endif /* MGL_TEXEL_BUFFER_H */
