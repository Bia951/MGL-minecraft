#import "mgl_uniform_snapshot.h"
#include <stdlib.h>
#include <string.h>

extern Buffer *mglRendererGetValidatedBuffer(GLMContext, Buffer *, const char *, NSUInteger);

typedef struct {
    GLuint location;
    uintptr_t identity;
    vm_address_t address;
    GLsizeiptr size;
    BOOL fallback;
} MGLUniformSnapshotSource;

static Buffer *resolveSource(GLMContext ctx, BufferBaseTarget *buffers,
                             BufferBaseTarget *fallback, GLuint location, BOOL *usedFallback)
{
    *usedFallback = NO;
    Buffer *buffer = mglRendererGetValidatedBuffer(ctx, buffers[location].buf,
                                                  "uniform-snapshot", location);
    if (!buffer && fallback) {
        buffer = mglRendererGetValidatedBuffer(ctx, fallback[location].buf,
                                               "uniform-snapshot-fallback", location);
        *usedFallback = YES;
    }
    return buffer;
}

static BOOL eligible(Buffer *buffer)
{
    if (!buffer) return YES; // An absent source contributes immutable zeroes.
    return buffer->plain_uniform_snapshot_private && !buffer->mapped &&
        buffer->size >= 0 && buffer->size <= 256u * 1024u &&
        (!buffer->data.buffer_data || buffer->data.buffer_size >= (size_t)buffer->size);
}

@implementation MGLPackedUniformSnapshot {
    MGLUniformSnapshotSource *_sources;
    NSUInteger _sourceCount;
    NSArray<NSData *> *_sourceBytes;
}

- (instancetype)initWithBytes:(NSData *)bytes resource:(const SpirvResource *)resource
                     element:(GLuint)element baseLocation:(GLint)base locationStep:(GLuint)step
                     buffers:(BufferBaseTarget *)buffers fallbackBuffers:(BufferBaseTarget *)fallback
                     context:(GLMContext)context
{
    self = [super init];
    if (!self) return nil;
    if (!bytes || bytes.length > 256u * 1024u || !buffers || !context || !resource ||
        !resource->ubo_members || resource->ubo_member_count > 4096u || !step) return nil;
    /* Count distinct locations first so small snapshots don't reserve the
     * full GL location space on every capture. */
    BOOL seen[MAX_BINDABLE_BUFFERS] = {0};
    NSUInteger sourceCapacity = 0;
    const uint64_t start = (uint64_t)element * step;
    const uint64_t end = start + step;
    for (GLuint m = 0; m < resource->ubo_member_count; m++) {
        const SpirvUBOMember *member = &resource->ubo_members[m];
        if (member->location_offset < 0 || (uint64_t)member->location_offset < start ||
            (uint64_t)member->location_offset >= end) continue;
        const int64_t first = (int64_t)base + member->location_offset;
        const int64_t count = member->size > 1 ? member->size : 1;
        if (count > MAX_BINDABLE_BUFFERS) return nil;
        for (int64_t ai = 0; ai < count; ai++) {
            int64_t location = first + ai;
            if (location < 0 || location >= MAX_BINDABLE_BUFFERS || seen[location]) continue;
            seen[location] = YES;
            sourceCapacity++;
        }
    }
    _sources = sourceCapacity ? calloc(sourceCapacity, sizeof(*_sources)) : NULL;
    if (sourceCapacity && !_sources) return nil;
    memset(seen, 0, sizeof(seen));
    NSMutableArray<NSData *> *sourceBytes = [NSMutableArray new];
    _retainedBytes = bytes.length;
    for (GLuint m = 0; m < resource->ubo_member_count; m++) {
        const SpirvUBOMember *member = &resource->ubo_members[m];
        if (member->location_offset < 0 || (uint64_t)member->location_offset < start ||
            (uint64_t)member->location_offset >= end) continue;
        const int64_t first = (int64_t)base + member->location_offset;
        const int64_t count = member->size > 1 ? member->size : 1;
        if (count > MAX_BINDABLE_BUFFERS) return nil;
        for (int64_t ai = 0; ai < count; ai++) {
            int64_t location = first + ai;
            if (location < 0 || location >= MAX_BINDABLE_BUFFERS || seen[location]) continue;
            seen[location] = YES;
            BOOL usedFallback;
            Buffer *buffer = resolveSource(context, buffers, fallback, (GLuint)location, &usedFallback);
            if (!eligible(buffer)) return nil;
            MGLUniformSnapshotSource *source = &_sources[_sourceCount++];
            source->location = (GLuint)location;
            source->identity = (uintptr_t)buffer;
            source->fallback = usedFallback;
            source->address = buffer ? buffer->data.buffer_data : 0;
            source->size = buffer ? buffer->size : 0;
            /* The private flag is only set for MGL-owned uniform allocations
             * and immutable clones. Their backing memory is known readable;
             * avoid a duplicate VM probe after source resolution. */
            NSData *data = source->address && source->size
                ? [NSData dataWithBytes:(const void *)(uintptr_t)source->address length:(NSUInteger)source->size]
                : [NSData data];
            _retainedBytes += data.length;
            if (_retainedBytes > 1024u * 1024u) return nil;
            [sourceBytes addObject:data];
        }
    }
    _sourceBytes = [sourceBytes copy];
    _bytes = [bytes copy]; // Mutable callers cannot overwrite old versions.
    return self;
}

- (BOOL)matchesBuffers:(BufferBaseTarget *)buffers fallbackBuffers:(BufferBaseTarget *)fallback
              context:(GLMContext)context
{
    if (!buffers || !context) return NO;
    for (NSUInteger i = 0; i < _sourceCount; i++) {
        const MGLUniformSnapshotSource *source = &_sources[i];
        BOOL usedFallback;
        Buffer *buffer = resolveSource(context, buffers, fallback, source->location, &usedFallback);
        if (!eligible(buffer) || (uintptr_t)buffer != source->identity || usedFallback != source->fallback ||
            (buffer ? buffer->data.buffer_data : 0) != source->address ||
            (buffer ? buffer->size : 0) != source->size) return NO;
        NSData *data = _sourceBytes[i];
        if (data.length && memcmp(data.bytes, (const void *)(uintptr_t)source->address, data.length)) return NO;
    }
    return YES;
}

- (void)dealloc { free(_sources); }
@end
