#ifndef MGL_UNIFORM_SNAPSHOT_H
#define MGL_UNIFORM_SNAPSHOT_H
#import <Foundation/Foundation.h>
#include "glm_context.h"

/* Immutable CPU packed bytes plus an exact dependency witness. Only private,
 * unmapped CPU glUniform buffers qualify. No GL/Metal object or source pointer
 * is retained; each lookup resolves the live source and compares its exact
 * bytes and backing-presence state. */
@interface MGLPackedUniformSnapshot : NSObject
@property(nonatomic, readonly) NSData *bytes;
@property(nonatomic, readonly) NSUInteger retainedBytes;
- (instancetype)initWithBytes:(NSData *)bytes resource:(const SpirvResource *)resource
                     element:(GLuint)element baseLocation:(GLint)base locationStep:(GLuint)step
                     buffers:(BufferBaseTarget *)buffers fallbackBuffers:(BufferBaseTarget *)fallback
                     context:(GLMContext)context;
- (BOOL)matchesBuffers:(BufferBaseTarget *)buffers fallbackBuffers:(BufferBaseTarget *)fallback
              context:(GLMContext)context;
@end
#endif
