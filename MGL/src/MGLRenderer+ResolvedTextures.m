#import "MGLRenderer_Private.h"
#import "mgl_resolved_texture_bindings.h"
#import "MGLRenderer+ArgumentBuffer_Private.h"

@implementation MGLRenderer (ResolvedTextures)

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    id<MTLRenderCommandEncoder> encoder = _currentRenderEncoder;
    if (!encoder || !resource) return;
    if (_resolvedResourceEncoder != encoder) {
        _resolvedResourceEncoder = encoder;
        _resolvedResourceUsageByEncoder = [[NSMapTable alloc]
            initWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality
                  valueOptions:NSPointerFunctionsStrongMemory
                      capacity:64u];
    }
    NSNumber *oldUsage = [_resolvedResourceUsageByEncoder objectForKey:resource];
    MTLResourceUsage combinedUsage = usage | (MTLResourceUsage)oldUsage.unsignedIntegerValue;
    if (oldUsage && combinedUsage == (MTLResourceUsage)oldUsage.unsignedIntegerValue) return;
    [_resolvedResourceUsageByEncoder setObject:@(combinedUsage) forKey:resource];
    [encoder useResource:resource usage:combinedUsage];
}

- (id<MGLResolvedBufferBindingSink>)bufferBindingSink
{
    return _resolvedTextureBindingsPreparing ? _resolvedTextureBindings
        : (id<MGLResolvedBufferBindingSink>)_currentRenderEncoder;
}

- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (!_currentRenderEncoder || index >= kMGLMaxBufferSlots) return;
    if (!_lastBoundValid || _lastBoundVertexBuffers[index].buffer != buffer ||
        _lastBoundVertexBuffers[index].offset != offset) {
        [_currentRenderEncoder setVertexBuffer:buffer offset:offset atIndex:index];
        [self recordLastBoundVertexBuffer:buffer offset:offset atIndex:index];
        MGL_PERF_INC(g_mglSetVertexBufferCallsSinceSwap);
    } else MGL_PERF_INC(g_mglSetVertexBufferSkipsSinceSwap);
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    if (!_currentRenderEncoder || index >= kMGLMaxBufferSlots) return;
    if (!_lastBoundValid || _lastBoundFragmentBuffers[index].buffer != buffer ||
        _lastBoundFragmentBuffers[index].offset != offset) {
        [_currentRenderEncoder setFragmentBuffer:buffer offset:offset atIndex:index];
        [self recordLastBoundFragmentBuffer:buffer offset:offset atIndex:index];
        MGL_PERF_INC(g_mglSetFragmentBufferCallsSinceSwap);
    } else MGL_PERF_INC(g_mglSetFragmentBufferSkipsSinceSwap);
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (!_currentRenderEncoder || index >= kMGLMaxBufferSlots) return;
    [_currentRenderEncoder setVertexBytes:bytes length:length atIndex:index];
    [self invalidateLastBoundVertexBufferAtIndex:index];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
    if (!_currentRenderEncoder || index >= kMGLMaxBufferSlots) return;
    [_currentRenderEncoder setFragmentBytes:bytes length:length atIndex:index];
    [self invalidateLastBoundFragmentBufferAtIndex:index];
}

- (void)discardResolvedTextureBindings
{
    _resolvedTextureBindings = nil;
    _resolvedTextureCommandBuffer = nil;
    _resolvedTextureEncoder = nil;
}

- (bool)resolvedTextureBindingsMatchCurrentDraw
{
    if (!_resolvedTextureBindings.sealed || !_resolvedTextureBindings.valid || !ctx ||
        _parallelEncodeActive || _currentCommandBuffer != _resolvedTextureCommandBuffer ||
        _currentRenderEncoder != _resolvedTextureEncoder) return false;
    const int stages[2] = {_VERTEX_SHADER, _FRAGMENT_SHADER};
    for (NSUInteger i = 0; i < 2; i++) {
        Program *program = mglResolveProgramForStageFromState(ctx, stages[i]);
        if (!program || program->msl_texture_cache_instance_id != _resolvedTextureProgramInstances[i] ||
            program->msl_texture_cache_generation != _resolvedTextureLinkGenerations[i]) return false;
    }
    return true;
}

- (bool)prepareResolvedTextureBindingsForDraw
{
    return [self prepareResolvedTextureBindingsForDrawWithMappedCommandBuffer:nil];
}

- (bool)prepareResolvedTextureBindingsForDrawWithMappedCommandBuffer:(id<MTLCommandBuffer>)mappedCommandBuffer
{
    [self discardResolvedTextureBindings];
    if (_resolvedArgumentBufferCommandBuffer != _currentCommandBuffer) {
        [_resolvedArgumentBufferCache removeAllObjects];
        _resolvedArgumentBufferCommandBuffer = _currentCommandBuffer;
    }
    if (!_resolvedTexturePlanEnabled || _parallelEncodeActive || !ctx || !_currentRenderEncoder) return true;
    Program *vertex = mglResolveProgramForStageFromState(ctx, _VERTEX_SHADER);
    Program *fragment = mglResolveProgramForStageFromState(ctx, _FRAGMENT_SHADER);
    if (!vertex || !fragment || !vertex->msl_texture_cache_instance_id || !fragment->msl_texture_cache_instance_id) return true;

    BOOL complete = NO;
    id<MTLRenderCommandEncoder> initialEncoder = _currentRenderEncoder;
    BOOL initialLastBoundValid = _lastBoundValid;
    int savedRecordingCounters = g_mglRecordingBufferBindings;
    MGLLastBoundBuffer savedVertex[kMGLMaxBufferSlots] = {0};
    MGLLastBoundBuffer savedFragment[kMGLMaxBufferSlots] = {0};
    for (NSUInteger i = 0; i < kMGLMaxBufferSlots; i++) {
        savedVertex[i] = _lastBoundVertexBuffers[i];
        savedFragment[i] = _lastBoundFragmentBuffers[i];
    }
    @try {
        /* Reuse the map produced by dirty-state sync only while its command
         * buffer remains current. Texture upload and sampled-copy preflight can
         * rotate it, so compare identity rather than relying on dirty bits. */
        BOOL buffersReady = YES;
        if (!mappedCommandBuffer || mappedCommandBuffer != _currentCommandBuffer) {
            buffersReady = [self mapBuffersToMTL];
            if (buffersReady) mappedCommandBuffer = _currentCommandBuffer;
        }
        if (buffersReady) {
            buffersReady = [self updateDirtyBaseBufferList:&ctx->state.vertex_buffer_map_list] &&
                [self updateDirtyBaseBufferList:&ctx->state.fragment_buffer_map_list];
        }
        if (buffersReady) {
            _resolvedTextureBindings = [MGLResolvedTextureBindings new];
            _resolvedTextureBindingsPreparing = YES;
            g_mglRecordingBufferBindings = 1;

            /* Resolve ordinary uploads and render-target copies before any
             * bindings are captured. This prevents retries from rescanning all
             * resources, and makes the prepared replay include the final state. */
            BOOL texturesUploaded = [self bindActiveTexturesToMTL];
            BOOL copiesPrepared = texturesUploaded && [self prepareSampledCopiesForDraw];
            Program *sampleFlipProgram = mglResolveProgramForStageFromState(ctx, _FRAGMENT_SHADER);
            if (copiesPrepared && mglEnvFlagEnabled("MGL_RT_SAMPLE_FLIP") && sampleFlipProgram &&
                sampleFlipProgram->spirv[_FRAGMENT_SHADER].sample_flip_resource_count) {
                uint64_t mask = [self fragmentSampleFlipMaskForProgram:sampleFlipProgram];
                if (_pipelineSampleFlipMask != mask ||
                    _pipelineSampleFlipProgramInstance != sampleFlipProgram->msl_texture_cache_instance_id ||
                    _pipelineSampleFlipProgramGeneration != sampleFlipProgram->msl_texture_cache_generation) {
                    BOOL hadNativeDepth = _nativeDepthReady;
                    if (hadNativeDepth) [self resetNativeDepthDraw];
                    copiesPrepared = [self syncPipelineStateWithDeferredBufferMap:NO mappedCommandBuffer:NULL];
                    if (copiesPrepared && hadNativeDepth)
                        copiesPrepared = [self selectNativeDepthPipelineForDraw];
                    if (copiesPrepared && _currentRenderEncoder && _pipelineState) {
                        [_currentRenderEncoder setRenderPipelineState:_pipelineState];
                        _lastPipelineState = _pipelineState;
                    }
                }
            }
            if (copiesPrepared && mappedCommandBuffer != _currentCommandBuffer) {
                copiesPrepared = [self mapBuffersToMTL] &&
                    [self updateDirtyBaseBufferList:&ctx->state.vertex_buffer_map_list] &&
                    [self updateDirtyBaseBufferList:&ctx->state.fragment_buffer_map_list];
            }
            if (copiesPrepared &&
                [self restoreRenderEncoderAfterTextureUploadForDraw:"resolve-textures-before-bind-collection"]) {
                _currentDrawUsesRTSampledCopy = NO;
                _lastBoundValid = NO;
                id<MTLCommandBuffer> collectionCommandBuffer = _currentCommandBuffer;
                BOOL vertexReady = [self bindVertexBuffersToCurrentRenderEncoder];
                _lastBoundValid = NO;
                BOOL fragmentReady = vertexReady && [self bindFragmentBuffersToCurrentRenderEncoder];
                BOOL argumentsReady = fragmentReady &&
                    [self bindArgumentBuffersForProgram:vertex stage:_VERTEX_SHADER context:ctx
                                         renderEncoder:_currentRenderEncoder computeEncoder:nil] &&
                    [self bindArgumentBuffersForProgram:fragment stage:_FRAGMENT_SHADER context:ctx
                                         renderEncoder:_currentRenderEncoder computeEncoder:nil];
                BOOL sizesReady = argumentsReady && [self bindBufferSizeConstantsForRenderEncoder];

                /* A sampled-copy update can still interrupt texture binding.
                 * Retry only that texture pass: the captured buffers and
                 * argument tables are unchanged and remain valid for replay. */
                BOOL texturesReady = NO;
                for (NSUInteger attempt = 0; sizesReady && attempt < 3u; attempt++) {
                    texturesReady = [self bindTexturesToCurrentRenderEncoder];
                    if (texturesReady) break;
                    /* Arena ranges and descriptor cache leases cannot migrate
                     * to a different CB; let the legacy path remap there. */
                    if (_currentCommandBuffer != collectionCommandBuffer) break;
                    if (_nativeDepthReady && ![self nativeDepthBindingsRemainUsable]) {
                        [self resetNativeDepthDraw];
                    }
                    if (attempt + 1u >= 3u ||
                        ![self restoreRenderEncoderAfterTextureUploadForDraw:"resolve-textures-retry"]) break;
                }
                if (texturesReady && _currentRenderEncoder &&
                    _currentCommandBuffer == collectionCommandBuffer &&
                    [_resolvedTextureBindings seal]) {
                    _resolvedTextureCommandBuffer = _currentCommandBuffer;
                    _resolvedTextureEncoder = _currentRenderEncoder;
                    const Program *programs[2] = {vertex, fragment};
                    for (NSUInteger i = 0; i < 2; i++) {
                        _resolvedTextureProgramInstances[i] = programs[i]->msl_texture_cache_instance_id;
                        _resolvedTextureLinkGenerations[i] = programs[i]->msl_texture_cache_generation;
                    }
                    complete = YES;
                }
            }
        }
    } @catch (NSException *exception) {
        complete = NO;
    } @finally {
        _resolvedTextureBindingsPreparing = NO;
        g_mglRecordingBufferBindings = savedRecordingCounters;
        if (_currentRenderEncoder == initialEncoder) {
            for (NSUInteger i = 0; i < kMGLMaxBufferSlots; i++) {
                _lastBoundVertexBuffers[i] = savedVertex[i];
                _lastBoundFragmentBuffers[i] = savedFragment[i];
            }
            _lastBoundValid = initialLastBoundValid;
        } else [self invalidateLastBoundState];
    }
    if (!complete) {
        [self discardResolvedTextureBindings];
        _currentDrawUsesRTSampledCopy = NO;
        if (_nativeDepthReady) [self resetNativeDepthDraw];
        // Restore only the existing encoder/base pipeline; the caller's normal
        // validation and resource sync then perform full legacy fallback.
        if (!_currentRenderEncoder &&
            ![self restoreRenderEncoderAfterTextureUploadForDraw:"resolve-textures-legacy-fallback"]) return false;
    }
    return true;
}

- (bool)replayResolvedTextureBindingsForDraw
{
    if (![self resolvedTextureBindingsMatchCurrentDraw]) return false;
    BOOL result = [_resolvedTextureBindings replayResourcesToSink:(id<MGLResolvedResourceUseSink>)self] &&
        [_resolvedTextureBindings replayBuffersToSink:(id<MGLResolvedBufferBindingSink>)self] &&
        [_resolvedTextureBindings replayToSink:(id<MGLResolvedTextureBindingSink>)self];
    if (result) _lastBoundValid = YES;
    [self discardResolvedTextureBindings]; // One draw/encoder/CB only.
    return result;
}
@end
