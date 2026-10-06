#ifndef MGL_GPU_PROFILE_H
#define MGL_GPU_PROFILE_H
#import <Metal/Metal.h>
#include <stdbool.h>
bool mglGPUProfileEnabled(void);
id<MTLRenderCommandEncoder> mglProfileRender(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *pass, const char *site, unsigned line, unsigned program, unsigned fbo);
id<MTLParallelRenderCommandEncoder> mglProfileParallelRender(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *pass, const char *site, unsigned line, unsigned program, unsigned fbo);
id<MTLComputeCommandEncoder> mglProfileCompute(id<MTLCommandBuffer> cb, const char *site, unsigned line);
id<MTLBlitCommandEncoder> mglProfileBlit(id<MTLCommandBuffer> cb, const char *site, unsigned line);
#endif
