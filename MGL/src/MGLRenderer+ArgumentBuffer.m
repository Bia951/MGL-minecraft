/*
 * MGLRenderer+ArgumentBuffer.m
 *
 * UBOs and SSBOs are encoded into SPIRV-Cross argument buffers.  Texture and
 * sampler descriptor sets stay discrete so the existing, heavily-tested
 * texture binding path remains authoritative.
 */
#import "MGLRenderer_Private.h"
#import "mgl_metal_bridge.h"

static inline uint64_t mglABHashMix(uint64_t h, uint64_t value)
{
    h ^= value;
    h *= 1099511628211ull;
    return h;
}

static inline GLuint mglABElementCount(int resourceType, const SpirvResource *resource)
{
    return mglStageBufferResourceElementCount(resourceType, resource);
}


static NSUInteger mglABSizeConstantCapacity(Program *program, int stage)
{
    NSUInteger capacity = 31u;
    SpirvResourceList *resources =
        &program->spirv_resources_list[stage][SPVC_RESOURCE_TYPE_STORAGE_BUFFER];
    for (GLuint i = 0; i < resources->count; i++) {
        SpirvResource *resource = &resources->list[i];
        if (!resource->uses_argument_buffer) continue;
        NSUInteger end = (NSUInteger)resource->argument_id +
            MAX(1u, mglABElementCount(SPVC_RESOURCE_TYPE_STORAGE_BUFFER, resource));
        capacity = MAX(capacity, end);
    }
    return MIN(MAX(capacity, 31u), 4096u);
}

@implementation MGLRenderer (ArgumentBuffer)

- (id<MTLBuffer>)argumentBufferFallbackWithLength:(NSUInteger)length
{
    const NSUInteger minimum = 64u * 1024u;
    NSUInteger required = MAX(length, minimum);
    if (!_argumentBufferFallbackStorage || _argumentBufferFallbackStorage.length < required) {
        /* Existing encoded argument buffers retain only a GPU address, not
         * the Objective-C resource. Keep superseded fallback buffers alive
         * until renderer teardown; cached argument buffers are re-encoded on
         * their next use because the fallback pointer changes. */
        if (_argumentBufferFallbackStorage) {
            if (!_argumentBufferRetiredFallbackStorage) {
                _argumentBufferRetiredFallbackStorage = [NSMutableArray array];
            }
            [_argumentBufferRetiredFallbackStorage addObject:_argumentBufferFallbackStorage];
        }
        NSUInteger grown = minimum;
        while (grown < required && grown <= NSUIntegerMax / 2u) {
            grown *= 2u;
        }
        _argumentBufferFallbackStorage = [_device newBufferWithLength:grown
                                                               options:MTLResourceStorageModeShared];
        if (_argumentBufferFallbackStorage.contents) {
            memset(_argumentBufferFallbackStorage.contents, 0, grown);
        }
        _argumentBufferFallbackStorage.label = @"MGL argument-buffer fallback";
    }
    return _argumentBufferFallbackStorage;
}

- (id<MTLFunction>)argumentBufferFunctionForProgram:(Program *)program stage:(int)stage
{
    if (!program || stage < 0 || stage >= _MAX_SHADER_TYPES) {
        return nil;
    }
    Shader *shader = program->shader_slots[stage];
    return shader ? (__bridge id<MTLFunction>)shader->mtl_data.function : nil;
}

- (bool)bindArgumentBuffersForProgram:(Program *)program
                                stage:(int)stage
                              context:(GLMContext)bindingContext
                        renderEncoder:(id<MTLRenderCommandEncoder>)renderEncoder
                       computeEncoder:(id<MTLComputeCommandEncoder>)computeEncoder
{
    if (!program || stage < 0 || stage >= _MAX_SHADER_TYPES) {
        return true;
    }

    Spirv *spirv = &program->spirv[stage];
    if (!spirv->uses_argument_buffers || spirv->argument_buffer_set_mask == 0u) {
        return true;
    }

    id<MTLFunction> function = [self argumentBufferFunctionForProgram:program stage:stage];
    if (!function) {
        NSLog(@"MGL ARGUMENT BUFFER ERROR: no Metal function program=%u stage=%d", program->name, stage);
        return false;
    }

    GLMContext resourceContext = bindingContext ? bindingContext : ctx;
    GLMState *state = MGL_STATE(resourceContext);
    for (GLuint set = 0; set < MGL_MAX_ARGUMENT_BUFFER_SETS; set++) {
        if ((spirv->argument_buffer_set_mask & (1u << set)) == 0u) {
            continue;
        }

        id<MTLArgumentEncoder> argumentEncoder =
            (__bridge id<MTLArgumentEncoder>)spirv->mtl_argument_encoders[set];
        if (!argumentEncoder) {
            @try {
                argumentEncoder = [function newArgumentEncoderWithBufferIndex:set];
            } @catch (NSException *exception) {
                NSLog(@"MGL ARGUMENT BUFFER ERROR: encoder creation exception program=%u stage=%d set=%u: %@",
                      program->name, stage, set, exception);
                return false;
            }
            if (!argumentEncoder) {
                NSLog(@"MGL ARGUMENT BUFFER ERROR: newArgumentEncoderWithBufferIndex failed program=%u stage=%d set=%u",
                      program->name, stage, set);
                return false;
            }
            spirv->mtl_argument_encoders[set] = (void *)CFBridgingRetain(argumentEncoder);
        }

        uint64_t signature = 1469598103934665603ull;
        signature = mglABHashMix(signature, set);
        NSUInteger maximumRequiredSize = 0u;
        uint32_t sizeConstantsStorage[4096];
        uint32_t *sizeConstants = NULL;
        NSUInteger sizeConstantCapacity = 0u;
        if (set == 1u && spirv->needs_buffer_size_buffer) {
            sizeConstantCapacity = mglABSizeConstantCapacity(program, stage);
            memset(sizeConstantsStorage, 0, sizeConstantCapacity * sizeof(uint32_t));
            sizeConstants = sizeConstantsStorage;
            [self appendArgumentBufferSizeConstantsForProgram:program
                                                         stage:stage
                                                       context:resourceContext
                                                          data:sizeConstants
                                                      capacity:sizeConstantCapacity];
            for (NSUInteger si = 0; si < sizeConstantCapacity; si++) {
                signature = mglABHashMix(signature, sizeConstants[si]);
            }
        }

        const int resourceTypes[] = {
            SPVC_RESOURCE_TYPE_UNIFORM_BUFFER,
            SPVC_RESOURCE_TYPE_STORAGE_BUFFER
        };

        for (NSUInteger ti = 0; ti < sizeof(resourceTypes) / sizeof(resourceTypes[0]); ti++) {
            const int resourceType = resourceTypes[ti];
            SpirvResourceList *resources = &program->spirv_resources_list[stage][resourceType];
            const GLuint bufferIndex = resourceType == SPVC_RESOURCE_TYPE_UNIFORM_BUFFER
                ? _UNIFORM_BUFFER : _SHADER_STORAGE_BUFFER;

            for (GLuint ri = 0; ri < resources->count; ri++) {
                SpirvResource *resource = &resources->list[ri];
                if (!resource->uses_argument_buffer || resource->argument_buffer_set != set) {
                    continue;
                }

                const GLuint elementCount = MAX(1u, mglABElementCount(resourceType, resource));
                for (GLuint element = 0; element < elementCount; element++) {
                    const GLuint argumentID = resource->argument_id + element;
                    const GLuint clientBinding =
                        mglClientBufferBindingForResourceElement(resourceType, resource, element);
                    BufferBaseTarget *base = clientBinding < MAX_BINDABLE_BUFFERS
                        ? &state->buffer_base[bufferIndex].buffers[clientBinding] : NULL;
                    Buffer *bufferObject = base ? base->buf : NULL;
                    id<MTLBuffer> metalBuffer = nil;
                    NSUInteger offset = 0u;
                    NSUInteger visibleSize = resource->required_size;

                    if (bufferObject) {
                        if (![self processBuffer:bufferObject]) {
                            return false;
                        }
                        metalBuffer = (__bridge id<MTLBuffer>)bufferObject->data.mtl_data;
                        if (base->offset > 0) {
                            offset = (NSUInteger)base->offset;
                        }
                        if (base->size > 0) {
                            visibleSize = (NSUInteger)base->size;
                        } else if (bufferObject->size > (GLsizeiptr)offset) {
                            visibleSize = (NSUInteger)(bufferObject->size - (GLsizeiptr)offset);
                        }
                        if (!metalBuffer || offset >= metalBuffer.length) {
                            metalBuffer = nil;
                        }
                    }

                    maximumRequiredSize = MAX(maximumRequiredSize, MAX(visibleSize, resource->required_size));
                    if (!metalBuffer) {
                        metalBuffer = [self argumentBufferFallbackWithLength:maximumRequiredSize];
                        offset = 0u;
                    }
                    if (!metalBuffer) {
                        return false;
                    }

                    signature = mglABHashMix(signature, argumentID);
                    signature = mglABHashMix(signature, (uint64_t)(uintptr_t)(__bridge void *)metalBuffer);
                    signature = mglABHashMix(signature, offset);
                    signature = mglABHashMix(signature, visibleSize);
                }
            }
        }

        id<MTLBuffer> argumentBuffer = (__bridge id<MTLBuffer>)spirv->mtl_argument_buffers[set];
        if (!argumentBuffer ||
            argumentBuffer.length < argumentEncoder.encodedLength ||
            spirv->argument_buffer_signatures[set] != signature) {
            id<MTLBuffer> replacement = [_device newBufferWithLength:argumentEncoder.encodedLength
                                                               options:MTLResourceStorageModeShared];
            if (!replacement) {
                NSLog(@"MGL ARGUMENT BUFFER ERROR: storage allocation failed program=%u stage=%d set=%u length=%lu",
                      program->name, stage, set, (unsigned long)argumentEncoder.encodedLength);
                return false;
            }
            replacement.label = [NSString stringWithFormat:@"MGL p%u stage%d argument-set%u",
                                 program->name, stage, set];
            [argumentEncoder setArgumentBuffer:replacement offset:0];

            id<MTLBuffer> auxiliaryBuffer = nil;
            if (sizeConstants) {
                auxiliaryBuffer = [_device newBufferWithBytes:sizeConstants
                                                       length:sizeConstantCapacity * sizeof(uint32_t)
                                                      options:MTLResourceStorageModeShared];
                if (!auxiliaryBuffer) {
                    return false;
                }
                auxiliaryBuffer.label = [NSString stringWithFormat:@"MGL p%u stage%d set%u size constants",
                                         program->name, stage, set];
                [argumentEncoder setBuffer:auxiliaryBuffer
                                    offset:0
                                   atIndex:MGL_BUFFER_SIZE_BUFFER_INDEX];
            }

            for (NSUInteger ti = 0; ti < sizeof(resourceTypes) / sizeof(resourceTypes[0]); ti++) {
                const int resourceType = resourceTypes[ti];
                SpirvResourceList *resources = &program->spirv_resources_list[stage][resourceType];
                const GLuint bufferIndex = resourceType == SPVC_RESOURCE_TYPE_UNIFORM_BUFFER
                    ? _UNIFORM_BUFFER : _SHADER_STORAGE_BUFFER;

                for (GLuint ri = 0; ri < resources->count; ri++) {
                    SpirvResource *resource = &resources->list[ri];
                    if (!resource->uses_argument_buffer || resource->argument_buffer_set != set) {
                        continue;
                    }
                    const GLuint elementCount = MAX(1u, mglABElementCount(resourceType, resource));
                    for (GLuint element = 0; element < elementCount; element++) {
                        const GLuint argumentID = resource->argument_id + element;
                        const GLuint clientBinding =
                            mglClientBufferBindingForResourceElement(resourceType, resource, element);
                        BufferBaseTarget *base = clientBinding < MAX_BINDABLE_BUFFERS
                            ? &state->buffer_base[bufferIndex].buffers[clientBinding] : NULL;
                        Buffer *bufferObject = base ? base->buf : NULL;
                        id<MTLBuffer> metalBuffer = nil;
                        NSUInteger offset = 0u;
                        NSUInteger visibleSize = resource->required_size;

                        if (bufferObject) {
                            if (![self processBuffer:bufferObject]) {
                                return false;
                            }
                            metalBuffer = (__bridge id<MTLBuffer>)bufferObject->data.mtl_data;
                            if (base->offset > 0) offset = (NSUInteger)base->offset;
                            if (base->size > 0) {
                                visibleSize = (NSUInteger)base->size;
                            } else if (bufferObject->size > (GLsizeiptr)offset) {
                                visibleSize = (NSUInteger)(bufferObject->size - (GLsizeiptr)offset);
                            }
                            if (!metalBuffer || offset >= metalBuffer.length) {
                                metalBuffer = nil;
                            }
                        }
                        if (!metalBuffer) {
                            metalBuffer = [self argumentBufferFallbackWithLength:MAX(visibleSize, resource->required_size)];
                            offset = 0u;
                        }
                        if (!metalBuffer) return false;

                        [argumentEncoder setBuffer:metalBuffer offset:offset atIndex:argumentID];
                    }
                }
            }

            mglSafeReleaseMetalObj((void **)&spirv->mtl_argument_buffers[set]);
            spirv->mtl_argument_buffers[set] = (void *)CFBridgingRetain(replacement);
            mglSafeReleaseMetalObj((void **)&spirv->mtl_argument_aux_buffers[set]);
            if (auxiliaryBuffer) {
                spirv->mtl_argument_aux_buffers[set] = (void *)CFBridgingRetain(auxiliaryBuffer);
            }
            spirv->argument_buffer_signatures[set] = signature;
            spirv->argument_buffer_lengths[set] = argumentEncoder.encodedLength;
            argumentBuffer = replacement;
        }

        /* Resources referenced indirectly through an argument buffer are not
         * made resident merely by binding the argument-buffer storage. */
        for (NSUInteger ti = 0; ti < sizeof(resourceTypes) / sizeof(resourceTypes[0]); ti++) {
            const int resourceType = resourceTypes[ti];
            SpirvResourceList *resources = &program->spirv_resources_list[stage][resourceType];
            const GLuint bufferIndex = resourceType == SPVC_RESOURCE_TYPE_UNIFORM_BUFFER
                ? _UNIFORM_BUFFER : _SHADER_STORAGE_BUFFER;
            const MTLResourceUsage usage = resourceType == SPVC_RESOURCE_TYPE_STORAGE_BUFFER
                ? (MTLResourceUsageRead | MTLResourceUsageWrite) : MTLResourceUsageRead;
            for (GLuint ri = 0; ri < resources->count; ri++) {
                SpirvResource *resource = &resources->list[ri];
                if (!resource->uses_argument_buffer || resource->argument_buffer_set != set) continue;
                const GLuint elementCount = MAX(1u, mglABElementCount(resourceType, resource));
                for (GLuint element = 0; element < elementCount; element++) {
                    const GLuint clientBinding =
                        mglClientBufferBindingForResourceElement(resourceType, resource, element);
                    BufferBaseTarget *base = clientBinding < MAX_BINDABLE_BUFFERS
                        ? &state->buffer_base[bufferIndex].buffers[clientBinding] : NULL;
                    id<MTLBuffer> metalBuffer = base && base->buf
                        ? (__bridge id<MTLBuffer>)base->buf->data.mtl_data
                        : _argumentBufferFallbackStorage;
                    if (!metalBuffer) continue;
                    if (renderEncoder) [renderEncoder useResource:metalBuffer usage:usage];
                    if (computeEncoder) [computeEncoder useResource:metalBuffer usage:usage];
                }
            }
        }

        id<MTLBuffer> auxiliaryBuffer =
            (__bridge id<MTLBuffer>)spirv->mtl_argument_aux_buffers[set];
        if (auxiliaryBuffer) {
            if (renderEncoder) [renderEncoder useResource:auxiliaryBuffer usage:MTLResourceUsageRead];
            if (computeEncoder) [computeEncoder useResource:auxiliaryBuffer usage:MTLResourceUsageRead];
        }
        if (renderEncoder) {
            if (stage == _VERTEX_SHADER) {
                [renderEncoder setVertexBuffer:argumentBuffer offset:0 atIndex:set];
                [self recordLastBoundVertexBuffer:argumentBuffer offset:0 atIndex:set];
            } else if (stage == _FRAGMENT_SHADER) {
                [renderEncoder setFragmentBuffer:argumentBuffer offset:0 atIndex:set];
                [self recordLastBoundFragmentBuffer:argumentBuffer offset:0 atIndex:set];
            }
        }
        if (computeEncoder) {
            [computeEncoder setBuffer:argumentBuffer offset:0 atIndex:set];
        }
    }

    return true;
}

- (void)appendArgumentBufferSizeConstantsForProgram:(Program *)program
                                               stage:(int)stage
                                             context:(GLMContext)bindingContext
                                                data:(uint32_t *)data
                                            capacity:(NSUInteger)capacity
{
    if (!program || !data || stage < 0 || stage >= _MAX_SHADER_TYPES) return;
    SpirvResourceList *resources =
        &program->spirv_resources_list[stage][SPVC_RESOURCE_TYPE_STORAGE_BUFFER];
    GLMContext resourceContext = bindingContext ? bindingContext : ctx;
    GLMState *state = MGL_STATE(resourceContext);
    for (GLuint ri = 0; ri < resources->count; ri++) {
        SpirvResource *resource = &resources->list[ri];
        if (!resource->uses_argument_buffer) continue;
        const GLuint elementCount = MAX(1u, mglABElementCount(SPVC_RESOURCE_TYPE_STORAGE_BUFFER, resource));
        for (GLuint element = 0; element < elementCount; element++) {
            NSUInteger idInSet = (NSUInteger)resource->argument_id + element;
            if (idInSet >= capacity) continue;
            GLuint clientBinding = mglClientBufferBindingForResourceElement(
                SPVC_RESOURCE_TYPE_STORAGE_BUFFER, resource, element);
            if (clientBinding >= MAX_BINDABLE_BUFFERS) continue;
            BufferBaseTarget *base = &state->buffer_base[_SHADER_STORAGE_BUFFER].buffers[clientBinding];
            if (!base->buf) continue;
            GLsizeiptr visible = base->size > 0 ? base->size : (base->buf->size - base->offset);
            if (visible < 0) visible = 0;
            data[idInSet] = (uint32_t)MIN((uint64_t)visible, (uint64_t)UINT32_MAX);
        }
    }
}

@end
