#!/usr/bin/env python3
"""Offline binding-record replay checks; no GL context, Metal device or draw."""
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#import "mgl_resolved_texture_bindings.h"
#include "mgl_frame_activity.h"
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while(0)
@interface Sink : NSObject <MGLResolvedTextureBindingSink, MGLResolvedBufferBindingSink> {
@public id textures[2][TEXTURE_UNITS]; id samplers[2][TEXTURE_UNITS]; NSUInteger calls;
    id buffers[2][MAX_MAPPED_BUFFERS]; NSData *bytes[2][MAX_MAPPED_BUFFERS]; NSUInteger offsets[2][MAX_MAPPED_BUFFERS];
}
@end
@implementation Sink
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { buffers[0][i] = b; offsets[0][i] = o; calls++; }
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { buffers[1][i] = b; offsets[1][i] = o; calls++; }
- (void)setVertexBytes:(const void *)b length:(NSUInteger)n atIndex:(NSUInteger)i { bytes[0][i] = [NSData dataWithBytes:b length:n]; calls++; }
- (void)setFragmentBytes:(const void *)b length:(NSUInteger)n atIndex:(NSUInteger)i { bytes[1][i] = [NSData dataWithBytes:b length:n]; calls++; }
- (void)setVertexTextureIfNeeded:(id<MTLTexture>)value atIndex:(NSUInteger)i { textures[0][i] = value; calls++; }
- (void)setFragmentTextureIfNeeded:(id<MTLTexture>)value atIndex:(NSUInteger)i { textures[1][i] = value; calls++; }
- (void)setVertexSamplerStateIfNeeded:(id<MTLSamplerState>)value atIndex:(NSUInteger)i { samplers[0][i] = value; calls++; }
- (void)setFragmentSamplerStateIfNeeded:(id<MTLSamplerState>)value atIndex:(NSUInteger)i { samplers[1][i] = value; calls++; }
@end
int main(void) { @autoreleasepool {
    Sink *sink = [Sink new];
    MGLResolvedTextureBindings *plan = [MGLResolvedTextureBindings new];
    NSObject *old = [NSObject new], *texture = [NSObject new], *warm = [NSObject new], *sampler = [NSObject new];
    [plan recordTexture:(id<MTLTexture>)old vertex:NO slot:3];
    [plan recordTexture:(id<MTLTexture>)texture vertex:NO slot:3];
    [plan recordSampler:(id<MTLSamplerState>)warm vertex:NO slot:3];
    [plan recordSampler:(id<MTLSamplerState>)sampler vertex:NO slot:3];
    [plan recordTexture:(id<MTLTexture>)texture vertex:YES slot:3];
    [plan recordSampler:(id<MTLSamplerState>)warm vertex:YES slot:0];
    [plan recordTexture:nil vertex:NO slot:7];
    CHECK(![plan replayToSink:sink] && sink->calls == 0); // Partial collections cannot be replayed.
    CHECK([plan seal] && [plan replayToSink:sink]);
    CHECK(sink->calls == 5 && sink->textures[1][3] == texture && sink->textures[0][3] == texture);
    CHECK(sink->samplers[1][3] == sampler && sink->samplers[0][0] == warm && !sink->textures[1][7]);
    CHECK(!sink->textures[0][7] && !sink->samplers[1][0]); // Untouched slots are not written.
    [plan recordSampler:nil vertex:NO slot:3];
    CHECK(!plan.valid && ![plan replayToSink:sink] && sink->calls == 5); // Sealed snapshots are immutable.
    MGLResolvedTextureBindings *invalid = [MGLResolvedTextureBindings new];
    [invalid recordTexture:nil vertex:YES slot:TEXTURE_UNITS];
    CHECK(![invalid seal] && ![invalid replayToSink:sink] && sink->calls == 5);
    MGLResolvedTextureBindings *empty = [MGLResolvedTextureBindings new];
    CHECK([empty seal] && [empty replayToSink:sink] && sink->calls == 5);
    MGLResolvedTextureBindings *lifetime = [MGLResolvedTextureBindings new];
    NSObject *temporary = [NSObject new]; __weak NSObject *weak = temporary;
    [lifetime recordTexture:(id<MTLTexture>)temporary vertex:NO slot:1]; temporary = nil;
    CHECK(weak != nil); // Temporary views remain alive even before replay.
    CHECK([lifetime seal]);
    Sink *owner = [Sink new]; CHECK([lifetime replayToSink:owner]); lifetime = nil;
    CHECK(weak != nil); // The actual sink takes ownership at binding.
    owner = nil; CHECK(weak == nil);
    MGLResolvedTextureBindings *bp = [MGLResolvedTextureBindings new];
    uint32_t value = 123;
    [bp setVertexBuffer:(id<MTLBuffer>)texture offset:16 atIndex:4];
    [bp setVertexBytes:&value length:sizeof(value) atIndex:4];
    [bp setFragmentBytes:&value length:sizeof(value) atIndex:4];
    [bp setFragmentBuffer:(id<MTLBuffer>)texture offset:32 atIndex:4];
    [bp setFragmentBuffer:nil offset:0 atIndex:5];
    value = 999;
    CHECK(![bp replayBuffersToSink:sink]);
    CHECK([bp seal] && [bp replayBuffersToSink:sink]);
    CHECK(sink->calls == 8 && !sink->buffers[0][4] && sink->buffers[1][4] == texture && sink->offsets[1][4] == 32);
    CHECK(*(const uint32_t *)sink->bytes[0][4].bytes == 123 && !sink->bytes[1][4] && !sink->buffers[1][5]);
    MGLResolvedTextureBindings *tooBig = [MGLResolvedTextureBindings new];
    [tooBig setVertexBytes:&value length:4097 atIndex:0]; CHECK(![tooBig seal]);
    MGLResolvedTextureBindings *badSlot = [MGLResolvedTextureBindings new];
    [badSlot setFragmentBuffer:nil offset:0 atIndex:31]; CHECK(![badSlot seal]);
    setenv("MGL_PERF_SUMMARY", "1", 1); CHECK(mglPerfSummaryEnabled());
    uint64_t before = MGL_FRAME_LOAD(g_mglSetVertexBufferCallsSinceSwap);
    g_mglRecordingBufferBindings = 1; MGL_PERF_INC(g_mglSetVertexBufferCallsSinceSwap);
    CHECK(MGL_FRAME_LOAD(g_mglSetVertexBufferCallsSinceSwap) == before);
    g_mglRecordingBufferBindings = 0; MGL_PERF_INC(g_mglSetVertexBufferCallsSinceSwap);
    CHECK(MGL_FRAME_LOAD(g_mglSetVertexBufferCallsSinceSwap) == before + 1);
    printf("PASS last-write replay, nil/untouched slots, stage isolation, sealing, bounds, strong lifetimes\n");
    return 0;
} }
'''

def run(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {args}\n{result.stdout}\n{result.stderr}")
    return result.stdout

with tempfile.TemporaryDirectory(prefix="mgl-resolved-textures-") as tmp:
    tmp = pathlib.Path(tmp)
    source = tmp / "replay_adapter.m"
    exe = tmp / "replay_adapter"
    source.write_text(HARNESS)
    run("clang", "-fobjc-arc", source, "-o", exe, "-I" + str(ROOT / "MGL/include"),
        "-I" + str(ROOT / "MGL/include/GL"), "-L" + str(ROOT / "build"), "-lmgl", "-framework", "Foundation",
        "-framework", "Metal", "-Wl,-rpath," + str(ROOT / "build"))
    print(run(exe), end="")
