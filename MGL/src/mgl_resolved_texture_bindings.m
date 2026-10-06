#import "mgl_resolved_texture_bindings.h"

@implementation MGLResolvedTextureBindings {
    id<MTLTexture> _textures[2][TEXTURE_UNITS];
    id<MTLSamplerState> _samplers[2][TEXTURE_UNITS];
    BOOL _textureWritten[2][TEXTURE_UNITS];
    BOOL _samplerWritten[2][TEXTURE_UNITS];
    id<MTLBuffer> _buffers[2][MAX_MAPPED_BUFFERS];
    NSData *_bytes[2][MAX_MAPPED_BUFFERS];
    NSUInteger _offsets[2][MAX_MAPPED_BUFFERS];
    uint8_t _bufferKind[2][MAX_MAPPED_BUFFERS]; // 0 untouched, 1 buffer, 2 bytes
    NSMutableDictionary<NSValue *, NSArray *> *_resources;
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
- (void)recordBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset vertex:(BOOL)vertex slot:(NSUInteger)slot
{
    if (_sealed || slot >= 31u) { _valid = NO; return; }
    NSUInteger stage = vertex ? 0 : 1;
    _buffers[stage][slot] = buffer;
    _bytes[stage][slot] = nil;
    _offsets[stage][slot] = offset;
    _bufferKind[stage][slot] = 1;
}
- (void)recordBytes:(const void *)bytes length:(NSUInteger)length vertex:(BOOL)vertex slot:(NSUInteger)slot
{
    if (_sealed || slot >= 31u || length > 4096u || (length && !bytes)) { _valid = NO; return; }
    NSUInteger stage = vertex ? 0 : 1;
    _bytes[stage][slot] = [NSData dataWithBytes:bytes length:length];
    _buffers[stage][slot] = nil;
    _bufferKind[stage][slot] = 2;
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{ [self recordBuffer:buffer offset:offset vertex:YES slot:index]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{ [self recordBuffer:buffer offset:offset vertex:NO slot:index]; }
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{ [self recordBytes:bytes length:length vertex:YES slot:index]; }
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{ [self recordBytes:bytes length:length vertex:NO slot:index]; }
- (void)recordResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    if (_sealed || !resource) { _valid = NO; return; }
    if (!_resources) _resources = [NSMutableDictionary new];
    NSValue *key = [NSValue valueWithPointer:(__bridge void *)resource];
    NSArray *old = _resources[key];
    if (!old && _resources.count >= 4096u) { _valid = NO; return; }
    _resources[key] = @[resource, @(usage | (old ? [old[1] unsignedIntegerValue] : 0u))];
}
- (BOOL)replayResourcesToSink:(id<MGLResolvedResourceUseSink>)sink
{
    if (!_sealed || !_valid || !sink) return NO;
    for (NSArray *record in _resources.allValues)
        [sink useResource:record[0] usage:[record[1] unsignedIntegerValue]];
    return YES;
}
- (BOOL)seal { _sealed = YES; return _valid; }
- (BOOL)replayBuffersToSink:(id<MGLResolvedBufferBindingSink>)sink
{
    if (!_sealed || !_valid || !sink) return NO;
    for (NSUInteger i = 0; i < MAX_MAPPED_BUFFERS; i++) {
        if (_bufferKind[0][i] == 1) [sink setVertexBuffer:_buffers[0][i] offset:_offsets[0][i] atIndex:i];
        else if (_bufferKind[0][i] == 2) [sink setVertexBytes:_bytes[0][i].bytes length:_bytes[0][i].length atIndex:i];
        if (_bufferKind[1][i] == 1) [sink setFragmentBuffer:_buffers[1][i] offset:_offsets[1][i] atIndex:i];
        else if (_bufferKind[1][i] == 2) [sink setFragmentBytes:_bytes[1][i].bytes length:_bytes[1][i].length atIndex:i];
    }
    return YES;
}
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
