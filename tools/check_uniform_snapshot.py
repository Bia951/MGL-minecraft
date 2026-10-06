#!/usr/bin/env python3
"""Offline immutable CPU snapshot/dependency tests, without GL APIs/devices."""
import os
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#import "mgl_uniform_snapshot.h"
extern Buffer *findBuffer(GLMContext, GLuint);
extern Buffer *getBuffer(GLMContext, GLenum, GLuint);
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
static Buffer *source(GLMContext ctx, GLuint name, size_t length) {
    Buffer *b = calloc(1, sizeof(*b));
    b->name = name; b->target = GL_UNIFORM_BUFFER;
    b->size = length; b->data.buffer_size = length;
    b->data.buffer_data = (vm_address_t)calloc(1, length);
    b->plain_uniform_snapshot_private = GL_TRUE;
    insertHashElement(&ctx->state.buffer_table, name, b);
    return b;
}
int main(int argc, char **argv) { @autoreleasepool {
    CHECK(argc == 2 && mglPackedUniformReuseEnabled() == atoi(argv[1]));
    GLMContext ctx = calloc(1, sizeof(*ctx)); initHashTable(&ctx->state.buffer_table, 64);
    BufferBaseTarget own[MAX_BINDABLE_BUFFERS] = {0}, fallback[MAX_BINDABLE_BUFFERS] = {0};
    SpirvUBOMember members[4] = {0};
    members[0].location_offset = 0; members[0].size = 1;
    members[1].location_offset = 1; members[1].size = 1;
    members[2].location_offset = 2; members[2].size = 2;
    members[3].location_offset = 4; members[3].size = 1;
    SpirvResource r = {0}; r.ubo_members = members; r.ubo_member_count = 4;
    own[8].buf = source(ctx, 1, 12); own[9].buf = source(ctx, 2, 36);
    own[10].buf = source(ctx, 3, 4); fallback[11].buf = source(ctx, 4, 4);
    own[12].buf = source(ctx, 5, 4);
    NSMutableData *packed = [NSMutableData dataWithLength:80];
    ((uint8_t *)packed.mutableBytes)[0] = 7;
    MGLPackedUniformSnapshot *snapshot = [[MGLPackedUniformSnapshot alloc]
        initWithBytes:packed resource:&r element:0 baseLocation:8 locationStep:4
        buffers:own fallbackBuffers:fallback context:ctx];
    CHECK(snapshot && [snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]);
    ((uint8_t *)packed.mutableBytes)[0] = 99;
    CHECK(((const uint8_t *)snapshot.bytes.bytes)[0] == 7); // Never alias mutable packed output.
    ((uint8_t *)(uintptr_t)own[8].buf->data.buffer_data)[0] = 1;
    CHECK(![snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]);
    CHECK(((const uint8_t *)snapshot.bytes.bytes)[0] == 7); // Old version stays immutable.
    ((uint8_t *)(uintptr_t)own[8].buf->data.buffer_data)[0] = 0;
    CHECK([snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]);
    ((uint8_t *)(uintptr_t)fallback[11].buf->data.buffer_data)[0] = 2;
    CHECK(![snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]); // Other program/global update.
    ((uint8_t *)(uintptr_t)fallback[11].buf->data.buffer_data)[0] = 0;
    own[11].buf = source(ctx, 6, 4);
    CHECK(![snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]); // Preferred source replaces fallback.
    own[11].buf = NULL;
    CHECK([snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]);
    Buffer *old = own[8].buf; own[8].buf = source(ctx, 7, 12);
    CHECK(![snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]); // Equal bytes, different object.
    own[8].buf = old;
    old->mapped = GL_TRUE;
    CHECK(![snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]); old->mapped = GL_FALSE;
    MGLPackedUniformSnapshot *second = [[MGLPackedUniformSnapshot alloc]
        initWithBytes:packed resource:&r element:1 baseLocation:8 locationStep:4
        buffers:own fallbackBuffers:fallback context:ctx];
    CHECK(second && [second matchesBuffers:own fallbackBuffers:fallback context:ctx]);
    ((uint8_t *)(uintptr_t)old->data.buffer_data)[0] = 3;
    CHECK([second matchesBuffers:own fallbackBuffers:fallback context:ctx]); // Independent array element.
    ((uint8_t *)(uintptr_t)old->data.buffer_data)[0] = 0;
    CHECK(findBuffer(ctx, old->name) == old && !old->plain_uniform_snapshot_private);
    CHECK(![snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]);
    CHECK(![[MGLPackedUniformSnapshot alloc] initWithBytes:packed resource:&r element:0
        baseLocation:8 locationStep:4 buffers:own fallbackBuffers:fallback context:ctx]); // Public/GPU alias excluded.
    CHECK(getBuffer(ctx, GL_UNIFORM_BUFFER, own[12].buf->name) == own[12].buf);
    CHECK(![second matchesBuffers:own fallbackBuffers:fallback context:ctx]); // Public binding revocation too.
    own[8].buf = NULL; deleteHashElement(&ctx->state.buffer_table, old->name);
    free((void *)(uintptr_t)old->data.buffer_data); free(old);
    CHECK(![snapshot matchesBuffers:own fallbackBuffers:fallback context:ctx]); // No dereference/retention of old GL pointer.
    printf("PASS immutable bytes, exact dependency guards, arrays, fallback updates, alias/mapping exclusion, deletion\n");
    return 0;
} }
'''

def run(*args, env=None):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, env=env)
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {args}\n{result.stdout}\n{result.stderr}")
    return result.stdout

with tempfile.TemporaryDirectory(prefix="mgl-uniform-snapshot-") as tmp:
    tmp = pathlib.Path(tmp)
    source = tmp / "snapshot_adapter.m"
    exe = tmp / "snapshot_adapter"
    source.write_text(HARNESS)
    run("clang", "-fobjc-arc", "-DMGL_GL_CORE", source, "-o", exe, "-I" + str(ROOT / "MGL/include"),
        "-I" + str(ROOT / "MGL/src"), "-I" + str(ROOT / "MGL/include/GL"), "-I" + str(ROOT / "external/SPIRV-Cross"),
        "-I" + str(ROOT / "external/glslang/glslang/Include"),
        "-L" + str(ROOT / "build"), "-lmgl", "-framework", "Foundation",
        "-Wl,-rpath," + str(ROOT / "build"))
    for value, expected in ((None, 0), ("1", 1), ("off", 0)):
        env = dict(os.environ)
        env.pop("MGL_PACKED_UNIFORM_REUSE", None)
        if value is not None:
            env["MGL_PACKED_UNIFORM_REUSE"] = value
        print(run(exe, expected, env=env), end="")
    print("PASS default-off and independent switch parsing")
