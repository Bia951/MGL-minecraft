#ifndef MGL_RESOLVED_TEXTURE_BINDINGS_H
#define MGL_RESOLVED_TEXTURE_BINDINGS_H
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "glm_limits.h"

@protocol MGLResolvedTextureBindingSink
- (void)setVertexTextureIfNeeded:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setFragmentTextureIfNeeded:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setVertexSamplerStateIfNeeded:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
- (void)setFragmentSamplerStateIfNeeded:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
@end

@protocol MGLResolvedResourceUseSink
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage;
@end

@protocol MGLResolvedBufferBindingSink
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
@end

/* One draw's final Metal bindings, not a GL pointer cache. Latest writes win;
 * explicit nil is distinct from an untouched slot. Strong resource ownership
 * survives temporary views, encoder interruption and autorelease pools. */
@interface MGLResolvedTextureBindings : NSObject <MGLResolvedBufferBindingSink>
@property(nonatomic, readonly) BOOL valid;
@property(nonatomic, readonly) BOOL sealed;
- (void)recordTexture:(id<MTLTexture>)texture vertex:(BOOL)vertex slot:(NSUInteger)slot;
- (void)recordSampler:(id<MTLSamplerState>)sampler vertex:(BOOL)vertex slot:(NSUInteger)slot;
- (BOOL)seal;
- (BOOL)replayToSink:(id<MGLResolvedTextureBindingSink>)sink;
- (BOOL)replayBuffersToSink:(id<MGLResolvedBufferBindingSink>)sink;
- (void)recordResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage;
- (BOOL)replayResourcesToSink:(id<MGLResolvedResourceUseSink>)sink;
@end
#endif
