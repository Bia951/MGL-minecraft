// Opt-in private shader/resource transaction. No application-specific names.
#import "MGLRenderer_Private.h"
#import "mgl_spirv_compile.h"

@implementation MGLRenderer (NativeDepth)

- (void)resetNativeDepthDraw
{
    if (_nativeDepthSelectedPipeline && _pipelineState == _nativeDepthSelectedPipeline)
        _pipelineState = _nativeDepthBasePipeline;
    _nativeDepthSelectedPipeline = nil;
    _nativeDepthBasePipeline = nil;
    _nativeDepthReady = NO;
    _nativeDepthMask = _nativeDepthFlipMask = 0;
    for (NSUInteger i = 0; i < 64; i++) {
        _nativeDepthGLTextures[i] = NULL;
        _nativeDepthBackings[i] = NULL;
        _nativeDepthTextures[i] = nil;
        _nativeDepthSamplers[i] = nil;
    }
}

- (id<MTLFunction>)nativeDepthFunctionForProgram:(Program *)program
                                          depth:(uint64_t)depth flip:(uint64_t)flip
{
    Spirv *stage = &program->spirv[_FRAGMENT_SHADER];
    if (!_nativeDepthShaderCache) _nativeDepthShaderCache = [NSMutableDictionary new];
    NSMutableDictionary<NSArray *, id> *cache = _nativeDepthShaderCache;
    // No Program/Texture pointers are retained. Deletion and relink cannot
    // reuse an entry belonging to a different lifetime/generation.
    NSArray *key = @[@(program->msl_texture_cache_instance_id),
                     @(program->msl_texture_cache_generation), @(depth), @(flip)];
    id existing = cache[key];
    if (existing == [NSNull null]) return nil;
    if (existing) return ((NSArray *)existing)[1];
    // Strict renderer-wide bound, including failures. Drop dependent PSOs
    // when evicting so they do not keep an unbounded set of shader variants.
    if (cache.count >= 128u) {
        [cache removeObjectForKey:cache.allKeys.firstObject];
        [_nativeDepthPipelineCache removeAllObjects];
    }
    char *source = mglNativeDepthMSL(ctx, program, depth, flip);
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    if (source) {
        library = [self compileShader:source];
        if (library) function = [self newFunctionFromLibrary:library
                  entryName:[NSString stringWithUTF8String:stage->entry_point]
                     source:source label:@"MGL native depth fragment"];
        free(source);
    }
    cache[key] = function ? @[library, function] : (id)[NSNull null];
    return function;
}

- (bool)selectNativeDepthPipelineForDraw
{
    if (!_nativeDepthSamplingEnabled || _parallelEncodeActive || !ctx || !_pipelineState ||
        ctx->state.caps.rasterizer_discard) return true;
    Program *program = mglResolveProgramForStageFromState(ctx, _FRAGMENT_SHADER);
    Program *vertex = mglResolveProgramForStageFromState(ctx, _VERTEX_SHADER);
    if (!program || !vertex || program->spirv[_FRAGMENT_SHADER].uses_argument_buffers) return true;
    SpirvResourceList *images = &program->spirv_resources_list[_FRAGMENT_SHADER][SPVC_RESOURCE_TYPE_SAMPLED_IMAGE];
    if (!images->list || images->count > 64u) return true;

    uint64_t depth = 0, flip = 0;
    for (GLuint i = 0; i < images->count; i++) {
        SpirvResource *res = &images->list[i];
        if (res->gl_type != GL_SAMPLER_2D || res->is_array || res->image_arrayed ||
            res->image_multisampled || res->image_dim != SpvDim2D || res->binding >= TEXTURE_UNITS ||
            res->gl_binding >= TEXTURE_UNITS ||
            mglShouldSkipStageTextureResource(program, _FRAGMENT_SHADER, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, res)) continue;
        GLuint unit = [self textureUnitForSampledResource:res metalBinding:res->binding stage:_FRAGMENT_SHADER];
        if (unit >= TEXTURE_UNITS) continue;
        Texture *tex = [self textureForSampledResource:res metalBinding:res->binding
                                               stage:_FRAGMENT_SHADER expectedType:MTLTextureType2D];
        if (!tex || tex->target != GL_TEXTURE_2D || tex->params.depth_stencil_mode != GL_DEPTH_COMPONENT ||
            tex->params.swizzle_r != GL_RED || tex->params.swizzle_g != GL_GREEN ||
            tex->params.swizzle_b != GL_BLUE || tex->params.swizzle_a != GL_ALPHA) continue;
        // Only consider depth candidates. Do not upload every color binding here.
        if (!mglRendererGLInternalFormatLooksDepthOrStencil(tex->internalformat)) continue;
        if (mglTextureIsAttachmentOfFramebuffer(ctx->state.framebuffer, tex)) continue;
        TextureParameter *params = ctx->state.texture_samplers[unit]
            ? &ctx->state.texture_samplers[unit]->params : &tex->params;
        if (params->compare_mode != GL_NONE) continue;
        if (![self bindMTLTexture:tex]) return false;
        id<MTLTexture> backing = tex->mtl_data ? (__bridge id<MTLTexture>)tex->mtl_data : nil;
        if (!backing || backing.pixelFormat != MTLPixelFormatDepth32Float ||
            backing.textureType != MTLTextureType2D || backing.sampleCount != 1u ||
            !(backing.usage & MTLTextureUsageShaderRead) || [self currentRenderPassUsesTexture:backing]) continue;
        NSUInteger base = tex->params.base_level;
        NSUInteger max = MIN((NSUInteger)tex->params.max_level, backing.mipmapLevelCount - 1u);
        if (base >= backing.mipmapLevelCount || max < base) continue;
        id<MTLTexture> view = backing;
        if (base || max + 1u != backing.mipmapLevelCount) {
            view = [backing newTextureViewWithPixelFormat:backing.pixelFormat
                    textureType:MTLTextureType2D levels:NSMakeRange(base, max - base + 1u)
                    slices:NSMakeRange(0, 1)];
            if (!view) continue; // Never bind a wrong mip range on view failure.
        }
        Sampler *glSampler = ctx->state.texture_samplers[unit];
        id<MTLSamplerState> sampler = nil;
        if (glSampler) {
            if (glSampler->dirty_bits && glSampler->mtl_data)
                mglSafeReleaseMetalObj((void **)&glSampler->mtl_data);
            if (!glSampler->mtl_data) {
                sampler = [self createMTLSamplerForTexParam:params target:tex->target];
                if (sampler) {
                    glSampler->mtl_data = (void *)CFBridgingRetain(sampler);
                    glSampler->dirty_bits = 0;
                }
            } else sampler = (__bridge id<MTLSamplerState>)glSampler->mtl_data;
        } else if (tex->params.mtl_data) sampler = (__bridge id<MTLSamplerState>)tex->params.mtl_data;
        if (!sampler) continue;
        _nativeDepthGLTextures[i] = tex;
        _nativeDepthBackings[i] = tex->mtl_data;
        _nativeDepthTextureUnits[i] = unit;
        _nativeDepthTextures[i] = view;
        _nativeDepthSamplers[i] = sampler;
        depth |= UINT64_C(1) << i;
        // Reuse the existing orientation authority; depth can request this
        // flip independently of any color-copy/coordinate experiment.
        if (mglDecideYFlipForSampledRT(tex, vertex) == MGL_YFLIP_USE_SAMPLED_COPY)
            flip |= UINT64_C(1) << i;
    }
    // Texture preparation may have ended the encoder. Restore with the base
    // pipeline before descriptor generation, including invalidated buffers.
    if (!_currentRenderEncoder && ![self restoreRenderEncoderAfterTextureUploadForDraw:"native-depth-prepare"])
        return false;
    if (!depth) return true;
    id<MTLFunction> function = [self nativeDepthFunctionForProgram:program depth:depth flip:flip];
    if (!function) return true;
    MTLRenderPipelineDescriptor *descriptor = [self generatePipelineDescriptor];
    MTLVertexDescriptor *vertexDescriptor = [self generateVertexDescriptor];
    if (!descriptor || !vertexDescriptor) return true;
    descriptor.vertexDescriptor = vertexDescriptor;
    [self bindBlendStateToPipelineStateDescriptor:descriptor];
    descriptor.fragmentFunction = function;
    NSArray *key = @[@(vertex->msl_texture_cache_instance_id), @(vertex->msl_texture_cache_generation),
                     @(program->msl_texture_cache_instance_id), @(program->msl_texture_cache_generation),
                     @(depth), @(flip), @(ctx->state.var.clip_origin), @(ctx->state.var.clip_depth_mode),
                     @(mglPipelineDescriptorSignature(descriptor)), @(mglVertexDescriptorSignature(vertexDescriptor))];
    if (!_nativeDepthPipelineCache) _nativeDepthPipelineCache = [NSMutableDictionary new];
    id cached = _nativeDepthPipelineCache[key];
    if (cached == [NSNull null]) return true;
    id<MTLRenderPipelineState> pipeline = cached;
    if (!pipeline) {
        if (_nativeDepthPipelineCache.count >= 128u) [_nativeDepthPipelineCache removeAllObjects];
        NSError *error = nil;
        @try { pipeline = [_device newRenderPipelineStateWithDescriptor:descriptor error:&error]; }
        @catch (NSException *exception) { pipeline = nil; }
        _nativeDepthPipelineCache[key] = pipeline ?: (id)[NSNull null];
    }
    if (!pipeline) return true; // Do not use the normal PSO's emergency fallback chain.
    _nativeDepthBasePipeline = _pipelineState;
    _nativeDepthSelectedPipeline = pipeline;
    _pipelineState = pipeline;
    _nativeDepthMask = depth;
    _nativeDepthFlipMask = flip;
    _nativeDepthProgramInstance = program->msl_texture_cache_instance_id;
    _nativeDepthLinkGeneration = program->msl_texture_cache_generation;
    _nativeDepthReady = YES;
    return true;
}

- (bool)nativeDepthBindingAtResourceIndex:(GLuint)index program:(Program *)program texture:(Texture *)texture
{
    return _nativeDepthReady && index < 64u && (_nativeDepthMask & (UINT64_C(1) << index)) &&
        program && program->msl_texture_cache_instance_id == _nativeDepthProgramInstance &&
        program->msl_texture_cache_generation == _nativeDepthLinkGeneration &&
        texture && texture == _nativeDepthGLTextures[index] &&
        texture->mtl_data == _nativeDepthBackings[index] && _nativeDepthTextures[index] &&
        _nativeDepthSamplers[index] && _pipelineState == _nativeDepthSelectedPipeline;
}
@end
