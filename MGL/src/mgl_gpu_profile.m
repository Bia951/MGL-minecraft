/* Opt-in asynchronous encoder timestamp diagnostics. No waits on the render thread. */
#import "mgl_gpu_profile.h"
#import "mgl_frame_activity.h"
#import <objc/runtime.h>
#include <stdlib.h>

@interface MGLGPUProfile : NSObject
@property id<MTLCounterSampleBuffer> samples;
@property NSMutableArray<NSDictionary *> *passes;
@property NSUInteger next;
@property uint64_t frame;
@property MTLTimestamp cpuStart;
@property MTLTimestamp gpuStart;
@end
@implementation MGLGPUProfile
@end

static int profileMode(void)
{
    static int enabled;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ const char *v = getenv("MGL_GPU_PROFILE"); enabled = v ? atoi(v) : 0; });
    return enabled;
}

bool mglGPUProfileEnabled(void) { return profileMode() != 0; }

static MGLGPUProfile *profileForBuffer(id<MTLCommandBuffer> cb)
{
    static char key;
    if (!cb || !mglGPUProfileEnabled()) return nil;
    MGLGPUProfile *p = objc_getAssociatedObject(cb, &key);
    if (p) return p;
    uint64_t frame = MGL_FRAME_LOAD(g_mglSwapCallCount);
    if (frame % 30 != 0) return nil;
    id<MTLDevice> device = cb.device;
    if (profileMode() == 1 && ![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        static dispatch_once_t warning;
        dispatch_once(&warning, ^{ NSLog(@"MGL GPU PROFILE unsupported stage boundary device=%@", device.name); });
        return nil;
    }
    id<MTLCounterSampleBuffer> samples = nil;
    if (profileMode() == 1) {
        id<MTLCounterSet> set = nil;
        for (id<MTLCounterSet> candidate in device.counterSets)
            if ([candidate.name isEqualToString:MTLCommonCounterSetTimestamp]) { set = candidate; break; }
        if (!set) return nil;
        MTLCounterSampleBufferDescriptor *desc = [MTLCounterSampleBufferDescriptor new];
        desc.counterSet = set;
        desc.storageMode = MTLStorageModeShared;
        desc.sampleCount = 2048;
        NSError *error = nil;
        samples = [device newCounterSampleBufferWithDescriptor:desc error:&error];
        if (!samples) { NSLog(@"MGL GPU PROFILE allocation failed: %@", error); return nil; }
    }
    p = [MGLGPUProfile new];
    p.samples = samples;
    p.passes = [NSMutableArray new];
    p.frame = frame;
    if (samples) {
        MTLTimestamp cpu, gpu;
        [device sampleTimestamps:&cpu gpuTimestamp:&gpu];
        p.cpuStart = cpu; p.gpuStart = gpu;
    }
    objc_setAssociatedObject(cb, &key, p, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status != MTLCommandBufferStatusCompleted) return;
        if (!p.samples) {
            NSLog(@"MGL GPU CB BASE frame=%llu cb=%p gpu_ms=%.6f start=%.9f end=%.9f", (unsigned long long)p.frame, completed, (completed.GPUEndTime-completed.GPUStartTime)*1000.0, completed.GPUStartTime, completed.GPUEndTime);
            return;
        }
        if (!p.next) return;
        MTLTimestamp cpuEnd, gpuEnd;
        [device sampleTimestamps:&cpuEnd gpuTimestamp:&gpuEnd];
        if (gpuEnd <= p.gpuStart || cpuEnd <= p.cpuStart) return;
        // sampleTimestamps returns CPU nanoseconds, not mach_absolute_time ticks.
        double nsPerTick = (double)(cpuEnd - p.cpuStart) / (double)(gpuEnd - p.gpuStart);
        NSData *data = [p.samples resolveCounterRange:NSMakeRange(0, p.next)];
        if (data.length < p.next * sizeof(MTLCounterResultTimestamp)) return;
        const MTLCounterResultTimestamp *timestamps = data.bytes;
        NSLog(@"MGL GPU CB frame=%llu cb=%p gpu_ms=%.6f ns_per_tick=%.6f passes=%lu start=%.9f end=%.9f", (unsigned long long)p.frame, completed, (completed.GPUEndTime - completed.GPUStartTime)*1000.0, nsPerTick, (unsigned long)p.passes.count, completed.GPUStartTime, completed.GPUEndTime);
        for (NSDictionary *pass in p.passes) {
            NSUInteger i = [pass[@"index"] unsignedIntegerValue];
            NSUInteger count = [pass[@"count"] unsignedIntegerValue];
            uint64_t lo = UINT64_MAX, hi = 0;
            bool valid = true;
            for (NSUInteger j=0; j<count; ++j) {
                uint64_t t = timestamps[i+j].timestamp;
                if (!t || t == MTLCounterErrorValue) { valid = false; break; }
                lo = MIN(lo,t); hi = MAX(hi,t);
            }
            if (!valid) { NSLog(@"MGL GPU PASS frame=%llu invalid %@", (unsigned long long)p.frame, pass[@"label"]); continue; }
            double vertex = count == 4 ? (double)(timestamps[i+1].timestamp - timestamps[i].timestamp)*nsPerTick/1e6 : 0;
            double fragment = count == 4 ? (double)(timestamps[i+3].timestamp - timestamps[i+2].timestamp)*nsPerTick/1e6 : 0;
            // Render stages can overlap. The envelope and stage intervals are reported separately.
            NSLog(@"MGL GPU PASS frame=%llu cb=%p kind=%@ ms=%.6f vertex_ms=%.6f fragment_ms=%.6f begin=%llu end=%llu vs_begin=%llu vs_end=%llu fs_begin=%llu fs_end=%llu %@", (unsigned long long)p.frame, completed, pass[@"kind"], (double)(hi-lo)*nsPerTick/1e6, vertex, fragment, (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)(count==4 ? timestamps[i].timestamp : lo), (unsigned long long)(count==4 ? timestamps[i+1].timestamp : hi), (unsigned long long)(count==4 ? timestamps[i+2].timestamp : 0), (unsigned long long)(count==4 ? timestamps[i+3].timestamp : 0), pass[@"label"]);
        }
    }];
    return p;
}

static NSUInteger addPass(MGLGPUProfile *p, NSUInteger count, NSString *kind, NSString *label)
{
    if (!p || p.next + count > p.samples.sampleCount) return NSNotFound;
    NSUInteger i = p.next;
    p.next += count;
    [p.passes addObject:@{@"index":@(i), @"count":@(count), @"kind":kind, @"label":label}];
    return i;
}

static MTLRenderPassDescriptor *profileRenderDescriptor(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *pass, const char *site, unsigned line, unsigned program, unsigned fbo)
{
    MGLGPUProfile *p = profileForBuffer(cb);
    if (!p.samples) return pass;
    id<MTLTexture> t = pass.colorAttachments[0].texture ?: pass.depthAttachment.texture;
    NSUInteger i = addPass(p,4,@"render",[NSString stringWithFormat:@"%s:%u context_program=%u context_fbo=%u size=%lux%lu",site,line,program,fbo,(unsigned long)t.width,(unsigned long)t.height]);
    // Descriptors may be reused. Sample only a copy to avoid stale attachments.
    MTLRenderPassDescriptor *sampled = i != NSNotFound ? [pass copy] : pass;
    if (i != NSNotFound) {
        MTLRenderPassSampleBufferAttachmentDescriptor *a = sampled.sampleBufferAttachments[0];
        a.sampleBuffer = p.samples;
        a.startOfVertexSampleIndex=i; a.endOfVertexSampleIndex=i+1;
        a.startOfFragmentSampleIndex=i+2; a.endOfFragmentSampleIndex=i+3;
    }
    return sampled;
}

id<MTLRenderCommandEncoder> mglProfileRender(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *pass, const char *site, unsigned line, unsigned program, unsigned fbo)
{
    if (mglGPUProfileEnabled()) pass = profileRenderDescriptor(cb,pass,site,line,program,fbo);
    return [cb renderCommandEncoderWithDescriptor:pass];
}

id<MTLParallelRenderCommandEncoder> mglProfileParallelRender(id<MTLCommandBuffer> cb, MTLRenderPassDescriptor *pass, const char *site, unsigned line, unsigned program, unsigned fbo)
{
    if (mglGPUProfileEnabled()) pass = profileRenderDescriptor(cb,pass,site,line,program,fbo);
    return [cb parallelRenderCommandEncoderWithDescriptor:pass];
}

id<MTLComputeCommandEncoder> mglProfileCompute(id<MTLCommandBuffer> cb, const char *site, unsigned line)
{
    if (!mglGPUProfileEnabled()) return [cb computeCommandEncoder];
    MGLGPUProfile *p = profileForBuffer(cb);
    if (!p.samples) return [cb computeCommandEncoder];
    NSUInteger i = addPass(p,2,@"compute",[NSString stringWithFormat:@"%s:%u",site,line]);
    if (i == NSNotFound) return [cb computeCommandEncoder];
    MTLComputePassDescriptor *pass = [MTLComputePassDescriptor new];
    pass.sampleBufferAttachments[0].sampleBuffer=p.samples;
    pass.sampleBufferAttachments[0].startOfEncoderSampleIndex=i;
    pass.sampleBufferAttachments[0].endOfEncoderSampleIndex=i+1;
    return [cb computeCommandEncoderWithDescriptor:pass];
}

id<MTLBlitCommandEncoder> mglProfileBlit(id<MTLCommandBuffer> cb, const char *site, unsigned line)
{
    if (!mglGPUProfileEnabled()) return [cb blitCommandEncoder];
    MGLGPUProfile *p = profileForBuffer(cb);
    if (!p.samples) return [cb blitCommandEncoder];
    NSUInteger i = addPass(p,2,@"blit",[NSString stringWithFormat:@"%s:%u",site,line]);
    if (i == NSNotFound) return [cb blitCommandEncoder];
    MTLBlitPassDescriptor *pass = [MTLBlitPassDescriptor new];
    pass.sampleBufferAttachments[0].sampleBuffer=p.samples;
    pass.sampleBufferAttachments[0].startOfEncoderSampleIndex=i;
    pass.sampleBufferAttachments[0].endOfEncoderSampleIndex=i+1;
    return [cb blitCommandEncoderWithDescriptor:pass];
}
