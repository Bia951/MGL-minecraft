#import "MGLRenderer_Private.h"
#import "MGLRenderer+RenderPass_Private.h"
#import "MGLRenderer+PipelineCache_Private.h"

#include <string.h>

enum {
    kEarlyAttrSize, kEarlyAttrType, kEarlyAttrNormalized, kEarlyAttrInteger,
    kEarlyAttrLong, kEarlyAttrRelativeOffset, kEarlyAttrBindingOffset,
    kEarlyAttrStride, kEarlyAttrDivisor, kEarlyAttrBindingIndex,
    kEarlyAttrEnabled, kEarlyAttrCurrent, kEarlyAttrStreamGroup,
    kEarlyAttrResolvedOffset, kEarlyAttrResolvedStride, kEarlyAttrResolvedDivisor,
    kEarlyAttrWordCount
};

typedef struct {
    uint64_t version[2];
    uint64_t programs[16];
    uint64_t state[48];
    uint64_t colorAttachments[MAX_COLOR_ATTACHMENTS][4];
    uint64_t depthStencil[8];
    uint64_t blend[MAX_COLOR_ATTACHMENTS][12];
    uint64_t drawBuffers[MAX_COLOR_ATTACHMENTS * 4 + 8];
    uint64_t attributes[MAX_ATTRIBS][kEarlyAttrWordCount];
} MGLEarlyPipelineInputKey;

#define KEY_SET(array, index, value) ((array)[(index)] = (uint64_t)(uintptr_t)(value))

@implementation MGLRenderer (PipelineCache)

- (NSData *)earlyPipelineInputKeyForVertexProgram:(Program *)vs
                                  fragmentProgram:(Program *)fs
                                              vao:(VertexArray *)vao
{
    if (!ctx || !_renderPassDescriptor || !vs || !vao ||
        (vs->dirty_bits & DIRTY_PROGRAM) || (fs && (fs->dirty_bits & DIRTY_PROGRAM)) ||
        vs != mglResolveProgramForStageFromState(ctx, _VERTEX_SHADER) ||
        fs != mglResolveProgramForStageFromState(ctx, _FRAGMENT_SHADER) ||
        vao != mglRendererGetValidatedVAO(ctx, __FUNCTION__)) {
        return nil;
    }

    const GLboolean discard = ctx->state.caps.rasterizer_discard;
    const uintptr_t vsFunction = (uintptr_t)vs->spirv[_VERTEX_SHADER].mtl_function;
    const uintptr_t fsFunction = fs ? (uintptr_t)fs->spirv[_FRAGMENT_SHADER].mtl_function : 0u;
    if (!vsFunction || (!fsFunction && !discard)) {
        return nil;
    }

    MGLEarlyPipelineInputKey key = {0};
    KEY_SET(key.version, 0, 1u);
    KEY_SET(key.programs, 0, vs->msl_texture_cache_instance_id);
    KEY_SET(key.programs, 1, vs->msl_texture_cache_generation);
    KEY_SET(key.programs, 2, vs->spirv[_VERTEX_SHADER].mtl_function);
    KEY_SET(key.programs, 3, vs->spirv[_VERTEX_SHADER].mtl_zero_to_one_function);
    KEY_SET(key.programs, 4, vs->spirv[_VERTEX_SHADER].mtl_upper_left_function);
    KEY_SET(key.programs, 5, vs->spirv[_VERTEX_SHADER].mtl_upper_left_zero_to_one_function);
    KEY_SET(key.programs, 6, vs->spirv_resources_list[_VERTEX_SHADER][_STAGE_OUTPUT_RES].count);
    KEY_SET(key.programs, 7, vs->mslCacheValid);
    KEY_SET(key.programs, 8, vs->vertexAttribUsageMask);
    KEY_SET(key.programs, 9, fs ? fs->msl_texture_cache_instance_id : 0u);
    KEY_SET(key.programs, 10, fs ? fs->msl_texture_cache_generation : 0u);
    KEY_SET(key.programs, 11, fsFunction);
    KEY_SET(key.programs, 13, mglCurrentRenderProgramKey(ctx));

    KEY_SET(key.state, 0, ctx->state.var.clip_origin);
    KEY_SET(key.state, 1, ctx->state.var.clip_depth_mode);
    KEY_SET(key.state, 2, discard);
    KEY_SET(key.state, 3, ctx->state.caps.sample_alpha_to_coverage);
    KEY_SET(key.state, 4, ctx->state.caps.sample_alpha_to_one);
    KEY_SET(key.state, 5, mglEnvFlagEnabled("MGL_ENABLE_ICB_PIPELINES"));
    KEY_SET(key.state, 33, mglEnvFlagEnabled("MGL_SPARSE_VERTEX_SIGNATURE"));
    KEY_SET(key.state, 6, ctx->state.max_color_attachments);
    KEY_SET(key.state, 7, ctx->state.framebuffer != NULL);
    KEY_SET(key.state, 8, ctx->pixel_format.format);
    KEY_SET(key.state, 9, ctx->pixel_format.type);
    KEY_SET(key.state, 10, ctx->pixel_format.mtl_pixel_format);
    KEY_SET(key.state, 11, ctx->depth_format.format);
    KEY_SET(key.state, 12, ctx->depth_format.type);
    KEY_SET(key.state, 13, ctx->depth_format.mtl_pixel_format);
    KEY_SET(key.state, 14, ctx->stencil_format.format);
    KEY_SET(key.state, 15, ctx->stencil_format.type);
    KEY_SET(key.state, 16, ctx->stencil_format.mtl_pixel_format);
    KEY_SET(key.state, 17, ctx->default_framebuffer_linear_mtl_pixel_format);
    KEY_SET(key.state, 18, ctx->default_framebuffer_srgb_mtl_pixel_format);
    KEY_SET(key.state, 19, _mslCacheEnabled);
    KEY_SET(key.state, 20, vao->enabled_attribs);

    const NSUInteger colorLimit = MIN((NSUInteger)ctx->state.max_color_attachments,
                                      (NSUInteger)MAX_COLOR_ATTACHMENTS);
    Framebuffer *fbo = ctx->state.framebuffer;
    KEY_SET(key.state, 21, fbo ? fbo->color_attachment_bitfield : 0u);
    KEY_SET(key.state, 22, fbo ? fbo->dirty_bits : 0u);
    KEY_SET(key.state, 23, ctx->state.draw_buffer);
    KEY_SET(key.state, 24, ctx->state.draw_buffer_count);
    KEY_SET(key.state, 25, ctx->state.default_draw_buffer);
    KEY_SET(key.state, 26, ctx->state.default_draw_buffer_count);
    if (fbo) {
        KEY_SET(key.state, 27, fbo->draw_buffer);
        KEY_SET(key.state, 28, fbo->draw_buffer_count);
        KEY_SET(key.state, 29, fbo->read_buffer);
        KEY_SET(key.state, 30, fbo->default_samples);
    }

    id<MTLTexture> drawableTexture = _drawable.texture;
    KEY_SET(key.state, 31, drawableTexture ? drawableTexture.pixelFormat : MTLPixelFormatInvalid);
    KEY_SET(key.state, 32, drawableTexture ? drawableTexture.sampleCount : 0u);

    for (NSUInteger i = 0; i < MAX_COLOR_ATTACHMENTS; i++) {
        id<MTLTexture> color = _renderPassDescriptor.colorAttachments[i].texture;
        KEY_SET(key.colorAttachments[i], 0, color ? color.pixelFormat : MTLPixelFormatInvalid);
        KEY_SET(key.colorAttachments[i], 1, color ? color.sampleCount : 0u);
        KEY_SET(key.drawBuffers, i, mglMetalDrawBufferAt(ctx, (GLuint)i));

        KEY_SET(key.blend[i], 0, ctx->state.caps.blendi[i]);
        KEY_SET(key.blend[i], 1, ctx->state.caps.use_color_mask[i]);
        for (NSUInteger c = 0; c < 4u; c++) {
            KEY_SET(key.blend[i], 2u + c, ctx->state.var.color_writemask[i][c]);
        }
        KEY_SET(key.blend[i], 6, ctx->state.var.blend_src_rgb[i]);
        KEY_SET(key.blend[i], 7, ctx->state.var.blend_dst_rgb[i]);
        KEY_SET(key.blend[i], 8, ctx->state.var.blend_src_alpha[i]);
        KEY_SET(key.blend[i], 9, ctx->state.var.blend_dst_alpha[i]);
        KEY_SET(key.blend[i], 10, ctx->state.var.blend_equation_rgb[i]);
        KEY_SET(key.blend[i], 11, ctx->state.var.blend_equation_alpha[i]);
    }
    KEY_SET(key.drawBuffers, MAX_COLOR_ATTACHMENTS, mglMetalDrawBufferCount(ctx));
    for (NSUInteger i = 0; i < MAX_COLOR_ATTACHMENTS; i++) {
        KEY_SET(key.drawBuffers, MAX_COLOR_ATTACHMENTS + 1u + i,
                ctx->state.draw_buffers[i]);
        KEY_SET(key.drawBuffers, MAX_COLOR_ATTACHMENTS * 2u + 1u + i,
                ctx->state.default_draw_buffers[i]);
        if (fbo) {
            KEY_SET(key.drawBuffers, MAX_COLOR_ATTACHMENTS * 3u + 1u + i,
                    fbo->draw_buffers[i]);
        }
    }

    id<MTLTexture> depth = _renderPassDescriptor.depthAttachment.texture;
    id<MTLTexture> stencil = _renderPassDescriptor.stencilAttachment.texture;
    KEY_SET(key.depthStencil, 0, depth ? depth.pixelFormat : MTLPixelFormatInvalid);
    KEY_SET(key.depthStencil, 1, depth ? depth.sampleCount : 0u);
    KEY_SET(key.depthStencil, 2, stencil ? stencil.pixelFormat : MTLPixelFormatInvalid);
    KEY_SET(key.depthStencil, 3, stencil ? stencil.sampleCount : 0u);

    if (fbo) {
        for (NSUInteger i = 0; i < colorLimit; i++) {
            FBOAttachment *attachment = &fbo->color_attachments[i];
            if (attachment->texture) {
                Texture *texture = [self framebufferAttachmentTexture:attachment];
                if (!texture || !texture->mtl_data || texture->dirty_bits || attachment->dirty_bits) {
                    return nil;
                }
                KEY_SET(key.colorAttachments[i], 2, mtlPixelFormatForGLTex(texture));
                id<MTLTexture> metalTexture = (__bridge id<MTLTexture>)texture->mtl_data;
                KEY_SET(key.colorAttachments[i], 3, metalTexture.sampleCount);
            }
            if ((fbo->color_attachment_bitfield >> (i + 1u)) == 0u) break;
        }
        FBOAttachment *attachments[2] = {&fbo->depth, &fbo->stencil};
        for (NSUInteger i = 0; i < 2u; i++) {
            FBOAttachment *attachment = attachments[i];
            if (!attachment->texture) continue;
            Texture *texture = [self framebufferAttachmentTexture:attachment];
            if (!texture || !texture->mtl_data || texture->dirty_bits || attachment->dirty_bits) {
                return nil;
            }
            KEY_SET(key.depthStencil, 4u + i * 2u, mtlPixelFormatForGLTex(texture));
            id<MTLTexture> metalTexture = (__bridge id<MTLTexture>)texture->mtl_data;
            KEY_SET(key.depthStencil, 5u + i * 2u, metalTexture.sampleCount);
        }
    }

    const char *msl = vs->spirv[_VERTEX_SHADER].msl_str;
    uint32_t descriptorAttributeMask = 0u;
    for (GLuint i = 0; i < MAX_ATTRIBS; i++) {
        uint64_t *attribute = key.attributes[i];
        const VertexAttrib *attrib = &vao->attrib[i];
        KEY_SET(attribute, kEarlyAttrSize, attrib->size);
        KEY_SET(attribute, kEarlyAttrType, attrib->type);
        KEY_SET(attribute, kEarlyAttrNormalized, attrib->normalized);
        KEY_SET(attribute, kEarlyAttrInteger, attrib->integer);
        KEY_SET(attribute, kEarlyAttrLong, attrib->long_attribute);
        KEY_SET(attribute, kEarlyAttrRelativeOffset, attrib->relativeoffset);
        KEY_SET(attribute, kEarlyAttrBindingOffset, attrib->binding_offset);
        KEY_SET(attribute, kEarlyAttrStride, attrib->stride);
        KEY_SET(attribute, kEarlyAttrDivisor, attrib->divisor);
        KEY_SET(attribute, kEarlyAttrBindingIndex, attrib->buffer_bindingindex);
        const BOOL enabled = (vao->enabled_attribs & (1u << i)) != 0u;
        KEY_SET(attribute, kEarlyAttrEnabled, enabled);
        KEY_SET(attribute, kEarlyAttrCurrent, !enabled);

        if (!mglRendererProgramUsesVertexAttrib(vs, i)) continue;
        BOOL mslUsesAttribute = YES;
        if (msl && _mslCacheEnabled && vs->mslCacheValid) {
            mslUsesAttribute = (vs->vertexAttribUsageMask & (1u << i)) != 0u;
        } else if (msl) {
            char pattern[32];
            snprintf(pattern, sizeof(pattern), "[[attribute(%u)]]", i);
            mslUsesAttribute = strstr(msl, pattern) != NULL;
        }
        if (mslUsesAttribute) descriptorAttributeMask |= 1u << i;
    }
    KEY_SET(key.programs, 12, descriptorAttributeMask);

    for (GLuint i = 0; i < MAX_ATTRIBS; i++) {
        if ((descriptorAttributeMask & (1u << i)) == 0u) continue;
        MGLResolvedVertexAttribBinding resolved = {0};
        const BOOL enabled = (vao->enabled_attribs & (1u << i)) != 0u;
        if (enabled && !mglRendererResolveVertexAttribBinding(ctx, vao, i, __FUNCTION__, &resolved)) {
            return nil;
        }
        int slot = mglRendererResolveVertexAttributeBufferIndex(ctx, vao, i, __FUNCTION__);
        if (slot < 0) return nil;
        uint64_t *attribute = key.attributes[i];
        if (enabled) {
            KEY_SET(attribute, kEarlyAttrResolvedOffset, resolved.binding_offset);
            KEY_SET(attribute, kEarlyAttrResolvedStride, resolved.stride);
            KEY_SET(attribute, kEarlyAttrResolvedDivisor, resolved.divisor);
            /* The assigned slot is exactly the stream-equivalence group used
             * by ResolveVertexAttributeBufferIndex/SameVertexStream. Avoid
             * putting transient Buffer identities in this reusable key. */
        }
        KEY_SET(attribute, kEarlyAttrStreamGroup, slot);
    }

    return [NSData dataWithBytes:&key length:sizeof(key)];
}

@end

#undef KEY_SET
