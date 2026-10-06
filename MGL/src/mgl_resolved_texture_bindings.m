#import "mgl_resolved_texture_bindings.h"

@implementation MGLResolvedTextureBindings {
    id<MTLTexture> _textures[2][TEXTURE_UNITS];
    id<MTLSamplerState> _samplers[2][TEXTURE_UNITS];
    BOOL _textureWritten[2][TEXTURE_UNITS];
    BOOL _samplerWritten[2][TEXTURE_UNITS];
}
- (instancetype)init
{
    self = [super init];
    if (self) _valid = YES;
    return self;
}
- (void)recordTexture:(id<MTLTexture>)texture vertex:(BOOL)vertex slot:(NSUInteger)slot
{
    if (_sealed || slot >= TEXTURE_UNITS) { _valid = NO; return; }
    NSUInteger stage = vertex ? 0u : 1u;
    _textures[stage][slot] = texture;
    _textureWritten[stage][slot] = YES;
}
- (void)recordSampler:(id<MTLSamplerState>)sampler vertex:(BOOL)vertex slot:(NSUInteger)slot
{
    if (_sealed || slot >= TEXTURE_UNITS) { _valid = NO; return; }
    NSUInteger stage = vertex ? 0u : 1u;
    _samplers[stage][slot] = sampler;
    _samplerWritten[stage][slot] = YES;
}
- (BOOL)seal { _sealed = YES; return _valid; }
- (BOOL)replayToSink:(id<MGLResolvedTextureBindingSink>)sink
{
    if (!_sealed || !_valid || !sink) return NO;
    for (NSUInteger i = 0; i < TEXTURE_UNITS; i++) {
        if (_textureWritten[0][i]) [sink setVertexTextureIfNeeded:_textures[0][i] atIndex:i];
        if (_samplerWritten[0][i]) [sink setVertexSamplerStateIfNeeded:_samplers[0][i] atIndex:i];
        if (_textureWritten[1][i]) [sink setFragmentTextureIfNeeded:_textures[1][i] atIndex:i];
        if (_samplerWritten[1][i]) [sink setFragmentSamplerStateIfNeeded:_samplers[1][i] atIndex:i];
    }
    return YES;
}
@end
