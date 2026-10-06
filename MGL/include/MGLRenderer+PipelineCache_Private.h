#ifndef MGLRenderer_PipelineCache_Private_h
#define MGLRenderer_PipelineCache_Private_h

#import "MGLRenderer_Private.h"

@interface MGLEarlyPipelineCacheEntry : NSObject
@property(nonatomic, strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic, strong) NSData *descriptorKey;
@property(nonatomic) MTLPixelFormat color0Format;
@property(nonatomic) MTLPixelFormat depthFormat;
@property(nonatomic) MTLPixelFormat stencilFormat;
@end

@interface MGLRenderer (PipelineCache)
- (NSData *)earlyPipelineInputKeyForVertexProgram:(Program *)vs
                                fragmentProgram:(Program *)fs
                                            vao:(VertexArray *)vao;
@end

#endif
