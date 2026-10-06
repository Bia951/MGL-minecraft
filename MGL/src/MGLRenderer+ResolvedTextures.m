#import "MGLRenderer_Private.h"
#import "mgl_resolved_texture_bindings.h"

@implementation MGLRenderer (ResolvedTextures)

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
    if (!_resolvedTexturePlanEnabled || _parallelEncodeActive || !ctx || !_currentRenderEncoder) return true;
    Program *vertex = mglResolveProgramForStageFromState(ctx, _VERTEX_SHADER);
    Program *fragment = mglResolveProgramForStageFromState(ctx, _FRAGMENT_SHADER);
    if (!vertex || !fragment || !vertex->msl_texture_cache_instance_id || !fragment->msl_texture_cache_instance_id ||
        vertex->spirv[_VERTEX_SHADER].uses_argument_buffers ||
        fragment->spirv[_FRAGMENT_SHADER].uses_argument_buffers) return true;

    BOOL complete = NO;
    @try {
        // Upload mapped buffers before texture copies. Native depth's earlier
        // selection is a preflight: its shader/PSO/views must already be usable
        // before the normal resolver is permitted to bypass a depth copy.
        BOOL buffersReady = [self mapBuffersToMTL] &&
            [self updateDirtyBaseBufferList:&ctx->state.vertex_buffer_map_list] &&
            [self updateDirtyBaseBufferList:&ctx->state.fragment_buffer_map_list];
        if (buffersReady && [self bindActiveTexturesToMTL]) {
            _resolvedTextureBindingsPreparing = YES;
            // Encoder-ending copies require a bounded restart. Partial slot
            // collections are discarded, never published or reused next draw.
            for (NSUInteger attempt = 0; attempt < 3; attempt++) {
                _resolvedTextureBindings = [MGLResolvedTextureBindings new];
                if (![self restoreRenderEncoderAfterTextureUploadForDraw:"resolve-textures-before-final-pipeline"])
                    break;
                _currentDrawUsesRTSampledCopy = NO;
                if ([self bindTexturesToCurrentRenderEncoder] && _currentRenderEncoder &&
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
                // A private backing check or copy interrupted preparation.
                // The retry must resolve every binding against the base shader.
                if (_nativeDepthReady) [self resetNativeDepthDraw];
            }
        }
    } @catch (NSException *exception) {
        complete = NO;
    } @finally {
        _resolvedTextureBindingsPreparing = NO;
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
    BOOL result = [_resolvedTextureBindings replayToSink:(id<MGLResolvedTextureBindingSink>)self];
    [self discardResolvedTextureBindings]; // One draw/encoder/CB only.
    return result;
}
@end
