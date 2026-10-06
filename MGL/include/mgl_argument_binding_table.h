#ifndef MGL_ARGUMENT_BINDING_TABLE_H
#define MGL_ARGUMENT_BINDING_TABLE_H
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* Exact immutable argument descriptors. GPU buffer contents are live, not
 * copied. Pointer identities cannot be recycled while the table owns buffers. */
@interface MGLArgumentBindingTable : NSObject
@property(nonatomic, readonly) NSArray<NSArray *> *entries;
@property(nonatomic, readonly) NSData *sizeConstants;
- (BOOL)addBuffer:(id<MTLBuffer>)buffer argument:(NSUInteger)argument
          offset:(NSUInteger)offset visibleSize:(NSUInteger)size usage:(MTLResourceUsage)usage;
- (BOOL)sealWithSizeConstants:(NSData *)sizeConstants;
- (BOOL)hasSameBindingsAs:(MGLArgumentBindingTable *)other;
- (BOOL)encodeTo:(id<MTLArgumentEncoder>)encoder;
@end
#endif
