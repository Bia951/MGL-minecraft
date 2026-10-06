#import "MGLRenderer_Private.h"
#import "mgl_resolved_texture_bindings.h"
#import "MGLRenderer+ArgumentBuffer_Private.h"

@implementation MGLRenderer (ResolvedTextures)

- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    [_currentRenderEncoder useResource:resource usage:usage];
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
        // Upload mapped buffers before texture copies. Native depth's earlier
        // selection is a preflight: its shader/PSO/views must already be usable
        // before the normal resolver is permitted to bypass a depth copy.
        BOOL buffersReady = [self mapBuffersToMTL] &&
            [self updateDirtyBaseBufferList:&ctx->state.vertex_buffer_map_list] &&
            [self updateDirtyBaseBufferList:&ctx->state.fragment_buffer_map_list];
        if (buffersReady && [self bindActiveTexturesToMTL]) {
            _resolvedTextureBindingsPreparing = YES;
            g_mglRecordingBufferBindings = 1;
            // Encoder-ending copies require a bounded restart. Partial slot
            // collections are discarded, never published or reused next draw.
            for (NSUInteger attempt = 0; attempt < 3; attempt++) {
                _resolvedTextureBindings = [MGLResolvedTextureBindings new];
                if (![self restoreRenderEncoderAfterTextureUploadForDraw:"resolve-textures-before-final-pipeline"])
                    break;
                _currentDrawUsesRTSampledCopy = NO;
                // Resolve every buffer/conversion/inline snapshot once too.
                // Force collection without poisoning the real encoder's dedup
                // state; restore that state after collection below.
                _lastBoundValid = NO;
                BOOL vertexReady = [self bindVertexBuffersToCurrentRenderEncoder];
                _lastBoundValid = NO;
                BOOL fragmentReady = vertexReady && [self bindFragmentBuffersToCurrentRenderEncoder];
                BOOL argumentsReady = fragmentReady &&
                    [self bindArgumentBuffersForProgram:vertex stage:_VERTEX_SHADER context:ctx
                                         renderEncoder:_currentRenderEncoder computeEncoder:nil] &&
                    [self bindArgumentBuffersForProgram:fragment stage:_FRAGMENT_SHADER context:ctx
                                         renderEncoder:_currentRenderEncoder computeEncoder:nil];
                BOOL sizesReady = argumentsReady && [self bindBufferSizeConstantsForRenderEncoder];
                if (sizesReady && [self bindTexturesToCurrentRenderEncoder] && _currentRenderEncoder &&
                    _currentCommandBuffer && [_resolvedTextureBindings seal]) {
                    _resolvedTextureCommandBuffer = _currentCommandBuffer;
                    _resolvedTextureEncoder = _currentRenderEncoder;
                    const Program *programs[2] = {vertex, fragment};
                    for (NSUInteger i = 0; i < 2; i++) {
                        _resolvedTextureProgramInstances[i] = programs[i]->msl_texture_cache_instance_id;
                        _resolvedTextureLinkGenerations[i] = programs[i]->msl_texture_cache_generation;
                    }
                    complete = YES;
                    break;
                }
                // A benign color-copy/encoder interruption need not discard
                // a usable native shader/view combination. A rejected backing
                // or changed program/unit does: retry all bindings with base.
                if (_nativeDepthReady && ![self nativeDepthBindingsRemainUsable]) [self resetNativeDepthDraw];
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
