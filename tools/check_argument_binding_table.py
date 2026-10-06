#!/usr/bin/env python3
"""Offline exact argument-descriptor and indirect-residency checks (no device)."""
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#import "mgl_argument_binding_table.h"
#import "mgl_resolved_texture_bindings.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while(0)
@interface Encoder : NSObject {
@public NSMutableDictionary *values; NSUInteger calls;
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i;
@end
@implementation Encoder
- (instancetype)init { self = [super init]; if(self) values = [NSMutableDictionary new]; return self; }
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { values[@(i)] = @[b, @(o)]; calls++; }
@end
@interface Resident : NSObject <MGLResolvedResourceUseSink> {
@public id resource; MTLResourceUsage usage; NSUInteger calls;
}
@end
@implementation Resident
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { resource = r; usage = u; calls++; }
@end
static MGLArgumentBindingTable *make(id b, NSUInteger offset, NSUInteger size, MTLResourceUsage usage, NSData *sizes) {
    MGLArgumentBindingTable *t = [MGLArgumentBindingTable new];
    if (![t addBuffer:b argument:7 offset:offset visibleSize:size usage:usage] || ![t sealWithSizeConstants:sizes]) return nil;
    return t;
}
int main(void) { @autoreleasepool {
    NSObject *buffer = [NSObject new], *different = [NSObject new];
    NSMutableData *sizes = [NSMutableData dataWithLength:124];
    MGLArgumentBindingTable *a = make(buffer, 16, 64, MTLResourceUsageRead, sizes);
    MGLArgumentBindingTable *same = make(buffer, 16, 64, MTLResourceUsageRead, sizes);
    CHECK(a && same && [a hasSameBindingsAs:same]);
    CHECK(![a hasSameBindingsAs:make(different,16,64,MTLResourceUsageRead,sizes)]);
    CHECK(![a hasSameBindingsAs:make(buffer,20,64,MTLResourceUsageRead,sizes)]);
    CHECK(![a hasSameBindingsAs:make(buffer,16,68,MTLResourceUsageRead,sizes)]);
    CHECK(![a hasSameBindingsAs:make(buffer,16,64,MTLResourceUsageWrite,sizes)]);
    ((uint8_t *)sizes.mutableBytes)[0] = 1;
    CHECK(((const uint8_t *)a.sizeConstants.bytes)[0] == 0);
    CHECK(![a hasSameBindingsAs:make(buffer,16,64,MTLResourceUsageRead,sizes)]);
    Encoder *encoder = [Encoder new]; CHECK([a encodeTo:(id<MTLArgumentEncoder>)encoder]);
    CHECK(encoder->calls == 1 && encoder->values[@7][0] == buffer && [encoder->values[@7][1] unsignedIntegerValue] == 16);
    MGLArgumentBindingTable *duplicate = [MGLArgumentBindingTable new];
    CHECK([duplicate addBuffer:(id<MTLBuffer>)buffer argument:0 offset:0 visibleSize:64 usage:MTLResourceUsageRead]);
    CHECK(![duplicate addBuffer:(id<MTLBuffer>)different argument:0 offset:0 visibleSize:64 usage:MTLResourceUsageRead]);
    CHECK(![duplicate sealWithSizeConstants:nil] && ![duplicate encodeTo:(id<MTLArgumentEncoder>)encoder]);
    MGLArgumentBindingTable *bad = [MGLArgumentBindingTable new];
    CHECK(![bad sealWithSizeConstants:[NSMutableData dataWithLength:16385]] && ![bad encodeTo:(id<MTLArgumentEncoder>)encoder]);
    CHECK(![a addBuffer:(id<MTLBuffer>)different argument:1 offset:0 visibleSize:64 usage:MTLResourceUsageRead]);
    CHECK(![a encodeTo:(id<MTLArgumentEncoder>)encoder]);
    MGLResolvedTextureBindings *draw = [MGLResolvedTextureBindings new];
    [draw recordResource:(id<MTLResource>)buffer usage:MTLResourceUsageRead];
    [draw recordResource:(id<MTLResource>)buffer usage:MTLResourceUsageWrite];
    Resident *resident = [Resident new]; CHECK(![draw replayResourcesToSink:resident]);
    CHECK([draw seal] && [draw replayResourcesToSink:resident]);
    CHECK(resident->calls == 1 && resident->resource == buffer && resident->usage == (MTLResourceUsageRead | MTLResourceUsageWrite));
    MGLArgumentBindingTable *life = [MGLArgumentBindingTable new];
    NSObject *temporary = [NSObject new]; __weak NSObject *weak = temporary;
    CHECK([life addBuffer:(id<MTLBuffer>)temporary argument:3 offset:0 visibleSize:4 usage:MTLResourceUsageRead]);
    CHECK([life sealWithSizeConstants:nil]); temporary = nil; CHECK(weak != nil);
    life = nil; CHECK(weak == nil);
    printf("PASS exact descriptor keys, mutation isolation, argument encoding, rejection, residency merging, strong lifetimes\n");
    return 0;
} }
'''

def run(*args):
    p = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"exit {p.returncode}: {args}\n{p.stdout}\n{p.stderr}")
    return p.stdout

with tempfile.TemporaryDirectory(prefix="mgl-argument-table-") as tmp:
    tmp = pathlib.Path(tmp)
    source = tmp / "argument_adapter.m"
    exe = tmp / "argument_adapter"
    source.write_text(HARNESS)
    run("clang", "-fobjc-arc", source, "-o", exe, "-I" + str(ROOT / "MGL/include"),
        "-L" + str(ROOT / "build"), "-lmgl", "-framework", "Foundation", "-framework", "Metal",
        "-Wl,-rpath," + str(ROOT / "build"))
    print(run(exe), end="")
