#import "mgl_argument_binding_table.h"

@implementation MGLArgumentBindingTable {
    NSMutableArray<NSArray *> *_building;
    NSMutableSet<NSNumber *> *_arguments;
    NSArray<NSArray *> *_exactKey;
    BOOL _valid;
    BOOL _sealed;
}
- (instancetype)init
{
    self = [super init];
    if (self) { _valid = YES; _building = [NSMutableArray new]; _arguments = [NSMutableSet new]; }
    return self;
}
- (BOOL)addBuffer:(id<MTLBuffer>)buffer argument:(NSUInteger)argument
          offset:(NSUInteger)offset visibleSize:(NSUInteger)size usage:(MTLResourceUsage)usage
{
    NSNumber *index = @(argument);
    if (_sealed || !buffer || _building.count >= 4096u || [_arguments containsObject:index]) {
        _valid = NO; return NO;
    }
    [_arguments addObject:index];
    [_building addObject:@[index, buffer, @(offset), @(size), @(usage)]];
    return YES;
}
- (BOOL)sealWithSizeConstants:(NSData *)sizeConstants
{
    if (_sealed) return NO;
    _sealed = YES;
    if (!_valid || sizeConstants.length > 4096u * sizeof(uint32_t)) { _valid = NO; return NO; }
    _entries = [_building copy];
    _sizeConstants = [sizeConstants copy];
    NSMutableArray *key = [NSMutableArray new];
    for (NSArray *entry in _entries)
        [key addObject:@[entry[0], @((uintptr_t)(__bridge void *)entry[1]), entry[2], entry[3], entry[4]]];
    _exactKey = [key copy];
    _building = nil; _arguments = nil;
    return YES;
}
- (BOOL)hasSameBindingsAs:(MGLArgumentBindingTable *)other
{
    return _sealed && _valid && other && other->_sealed && other->_valid &&
        [_exactKey isEqual:other->_exactKey] &&
        (_sizeConstants == other->_sizeConstants || [_sizeConstants isEqual:other->_sizeConstants]);
}
- (BOOL)encodeTo:(id<MTLArgumentEncoder>)encoder
{
    if (!_sealed || !_valid || !encoder) return NO;
    for (NSArray *entry in _entries)
        [encoder setBuffer:entry[1] offset:[entry[2] unsignedIntegerValue] atIndex:[entry[0] unsignedIntegerValue]];
    return YES;
}
@end
