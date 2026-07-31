/*
 * MGLRenderer+ArgumentBuffer_Private.h
 * Runtime binding for SPIRV-Cross Metal argument buffers.
 */
#ifndef MGLRenderer_ArgumentBuffer_Private_h
#define MGLRenderer_ArgumentBuffer_Private_h

#import "MGLRenderer_Private.h"

@interface MGLRenderer (ArgumentBuffer)
- (bool)bindArgumentBuffersForProgram:(Program *)program
                                stage:(int)stage
                              context:(GLMContext)bindingContext
                        renderEncoder:(id<MTLRenderCommandEncoder>)renderEncoder
                       computeEncoder:(id<MTLComputeCommandEncoder>)computeEncoder;
- (void)appendArgumentBufferSizeConstantsForProgram:(Program *)program
                                               stage:(int)stage
                                             context:(GLMContext)bindingContext
                                                data:(uint32_t *)data
                                            capacity:(NSUInteger)capacity;
@end

#endif
