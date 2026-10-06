#!/usr/bin/env python3
"""Offline binding-record replay checks; no GL context, Metal device or draw."""
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#import "mgl_resolved_texture_bindings.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while(0)
@interface Sink : NSObject <MGLResolvedTextureBindingSink> {
@public id textures[2][TEXTURE_UNITS]; id samplers[2][TEXTURE_UNITS]; NSUInteger calls;
}
@end
@implementation Sink
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
        "-L" + str(ROOT / "build"), "-lmgl", "-framework", "Foundation",
        "-framework", "Metal", "-Wl,-rpath," + str(ROOT / "build"))
    print(run(exe), end="")
