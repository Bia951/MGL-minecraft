/*
 * mgl_rt_sync.m
 * MGL
 *
 * Implementation of the Render Target Synchronization gate helpers.
 *
 * See mgl_rt_sync.h for the architectural rationale.  This module owns the
 * pure decision/gate logic for RT synchronization:
 *   - Which textures need a GL-sampled copy?
 *   - Is a texture currently an active render-pass attachment?
 *   - Does a framebuffer match the "GL sampled copy" pattern?
 *
 * The actual Y-flipped copy generation (updateGLSampledRenderTargetCopyForTexture)
 * and encoder lifecycle management stay in MGLRenderer.m because they require
 * access to MTLRenderCommandEncoder, the command buffer ivar, and a number of
 * renderer-internal helpers.  Splitting the gate logic out here makes the
 * decision rules testable and lets future pipeline-cache code query RT-sync
 * state without dragging in the full renderer.
 */

#import "mgl_rt_sync.h"

/* findTexture is implemented in textures.c and resolves a GL texture name to
 * a Texture *.  Declared here so this module does not need to include the
 * full MGLRenderer private header. */
extern Texture *findTexture(GLMContext ctx, GLuint texture);

bool mglTextureIsAttachmentOfFramebuffer(Framebuffer *fbo, Texture *tex)
{
    if (!fbo || !tex || !tex->mtl_data) {
        return false;
    }
    GLuint texName = tex->name;
    if (texName == 0u) {
        return false;
    }
    for (GLuint i = 0u; i < MAX_COLOR_ATTACHMENTS; i++) {
        if (((fbo->color_attachment_bitfield >> i) & 1u) == 0u) {
            continue;
        }
        if (fbo->color_attachments[i].texture == texName) {
            return true;
        }
    }
    if (fbo->depth.texture == texName) {
        return true;
    }
    if (fbo->stencil.texture == texName) {
        return true;
    }
    return false;
}

bool mglFramebufferLooksLikeGLSampledCopyRenderTarget(GLMContext glctx,
                                                      Framebuffer *fbo,
                                                      Texture **outColor,
                                                      Texture **outDepth)
{
    if (outColor) {
        *outColor = NULL;
    }
    if (outDepth) {
        *outDepth = NULL;
    }
    if (!glctx || !fbo || ((fbo->color_attachment_bitfield & 1u) == 0u)) {
        return false;
    }

    FBOAttachment *color0 = &fbo->color_attachments[0];
    Texture *color = NULL;
    if (color0->textarget == GL_RENDERBUFFER) {
        color = color0->buf.rbo ? color0->buf.rbo->tex : NULL;
    } else {
        color = color0->buf.tex;
        if (!color && color0->texture != 0u) {
            color = findTexture(glctx, color0->texture);
        }
    }
    if (!mglTextureCanUseGLSampledRenderTargetCopy(color)) {
        return false;
    }

    Texture *depth = NULL;
    if (fbo->depth.texture) {
        if (fbo->depth.textarget == GL_RENDERBUFFER) {
            depth = fbo->depth.buf.rbo ? fbo->depth.buf.rbo->tex : NULL;
        } else {
            depth = fbo->depth.buf.tex;
            if (!depth && fbo->depth.texture != 0u) {
                depth = findTexture(glctx, fbo->depth.texture);
            }
        }
    }

    if (outColor) {
        *outColor = color;
    }
    if (outDepth) {
        *outDepth = depth;
    }
    return true;
}

NSUInteger mglGLSampledCopyLevelCount(Texture *tex, id<MTLTexture> source)
{
    NSUInteger copyLevelCount = 1u;
    if (source.mipmapLevelCount > 1u) {
        GLuint highestGLLevel = tex->num_levels > 0u ? tex->num_levels - 1u : 0u;
        if (tex->mipmap_levels > 0u && highestGLLevel >= tex->mipmap_levels) {
            highestGLLevel = tex->mipmap_levels - 1u;
        }

        GLuint maxParamLevel = tex->params.max_level;
        if (maxParamLevel != 1000u && maxParamLevel < highestGLLevel) {
            highestGLLevel = maxParamLevel;
        }

        NSUInteger highestSourceLevel = source.mipmapLevelCount - 1u;
        if (tex->params.base_level > highestGLLevel &&
            (NSUInteger)tex->params.base_level <= highestSourceLevel) {
            highestGLLevel = tex->params.base_level;
        }
        if ((NSUInteger)highestGLLevel > highestSourceLevel) {
            highestGLLevel = (GLuint)highestSourceLevel;
        }

        copyLevelCount = (NSUInteger)highestGLLevel + 1u;
    }

    return copyLevelCount;
}

bool mglGLSampledCopyIsFresh(Texture *tex, id<MTLTexture> source)
{
    NSUInteger levels = mglGLSampledCopyLevelCount(tex, source);
    uint32_t windowMask = levels >= 32u ? UINT32_MAX : ((1u << levels) - 1u);
    return tex->mtl_gl_sampled_data &&
        tex->mtl_gl_sampled_width == source.width &&
        tex->mtl_gl_sampled_height == source.height &&
        tex->mtl_gl_sampled_format == source.pixelFormat &&
        tex->mtl_gl_sampled_levels == levels &&
        tex->mtl_gl_sampled_write_version == tex->mtl_render_target_write_version &&
        !(tex->mtl_gl_sampled_dirty_mip_mask & windowMask);
}
