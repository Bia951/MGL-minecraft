#!/usr/bin/env python3
"""CPU-only native sampler cache test; factory is mocked, no device/context."""
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#import "MGLRenderer.h"
@interface MGLRenderer (SamplerTest)
- (id<MTLSamplerState>)nativeDepthSamplerForParameters:(const TextureParameter *)p;
@end
@interface FakeRenderer : MGLRenderer { @public NSUInteger calls; GLuint lastTarget; }
@end
@implementation FakeRenderer
- (id<MTLSamplerState>)createMTLSamplerForTexParam:(TextureParameter *)p target:(GLuint)t {
    calls++; lastTarget = t;
    return (id<MTLSamplerState>)[NSObject new];
}
@end
// Zero-initialized receiver avoids the real renderer initializer. Keep it for
// process lifetime to avoid unrelated renderer/device teardown code as well.
static FakeRenderer *renderer;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d\n",__LINE__); return 1; } } while(0)
int main(void) { @autoreleasepool {
    renderer = [FakeRenderer alloc];
    TextureParameter p = {0}; p.min_filter = GL_NEAREST; p.mag_filter = GL_LINEAR;
    p.wrap_s = p.wrap_t = p.wrap_r = GL_CLAMP_TO_EDGE; p.max_lod = 1000; p.max_anisotropy = 1;
    p.compare_mode = GL_NONE; p.compare_func = GL_LEQUAL;
    TextureParameter original = p;
    id first = [renderer nativeDepthSamplerForParameters:&p];
    CHECK(first && renderer->calls == 1 && renderer->lastTarget == GL_TEXTURE_2D);
    CHECK([renderer nativeDepthSamplerForParameters:&p] == first);
    CHECK(memcmp(&p, &original, sizeof(p)) == 0);
    // A target-dependent legacy cache pointer is not part of this cache key.
    p.mtl_data = (void *)0x1234;
    CHECK([renderer nativeDepthSamplerForParameters:&p] == first && renderer->calls == 1);
    TextureParameter changed = p; changed.min_lod = 1;
    CHECK([renderer nativeDepthSamplerForParameters:&changed] != first);
    changed = p; changed.wrap_t = GL_REPEAT;
    CHECK([renderer nativeDepthSamplerForParameters:&changed] != first);
    changed = p; changed.border_color[2] = .5f;
    CHECK([renderer nativeDepthSamplerForParameters:&changed] != first);
    changed = p; changed.max_anisotropy = 2;
    CHECK([renderer nativeDepthSamplerForParameters:&changed] != first);
    changed = p; changed.lod_bias = -0.0f;
    CHECK([renderer nativeDepthSamplerForParameters:&changed] != first);
    changed = p; changed.compare_func = GL_GREATER;
    CHECK([renderer nativeDepthSamplerForParameters:&changed] != first);
    // More than 128 distinct states evicts the first entry; active references
    // remain alive, but cannot return an obsolete cached identity.
    for (int i = 0; i < 140; i++) { changed = p; changed.max_lod = (GLfloat)i; CHECK([renderer nativeDepthSamplerForParameters:&changed]); }
    CHECK([renderer nativeDepthSamplerForParameters:&p] != first);
    CHECK(renderer->lastTarget == GL_TEXTURE_2D);
    puts("PASS normalized target, exact sampler keys, independent legacy cache, bounded eviction");
    return 0;
} }
'''
with tempfile.TemporaryDirectory(prefix="mgl-native-sampler-") as tmp:
    tmp = pathlib.Path(tmp)
    source = tmp / "sampler_adapter.m"
    exe = tmp / "sampler_adapter"
    source.write_text(HARNESS)
    subprocess.run(["clang", "-fobjc-arc", "-DMGL_GL_CORE", str(source), "-o", str(exe),
                    "-I" + str(ROOT / "MGL/include"), "-I" + str(ROOT / "MGL/include/GL"),
                    "-I" + str(ROOT / "external/glslang/glslang/Include"),
                    "-I" + str(ROOT / "external/SPIRV-Cross"), "-I" + str(ROOT / "external/SPIRV-Tools/include"),
                    "-L" + str(ROOT / "build"), "-lmgl",
                    "-framework", "Foundation", "-framework", "Metal", "-framework", "QuartzCore",
                    "-framework", "Cocoa", "-Wl,-rpath," + str(ROOT / "build")], check=True)
    subprocess.run([str(exe)], check=True)
