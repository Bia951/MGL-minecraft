#!/usr/bin/env python3
"""Offline compiler/ABI checks. No GL API/context/device/window/game is used.
The temporary C adapter only constructs compiler input records and invokes the
same reflection/compatibility passes as link, then the private variant compiler.
"""
import pathlib
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "glm_context.h"
#include "mgl_spirv_compile.h"
#include <stdlib.h>
#include <string.h>
static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
    void *p = calloc(1, *n + 1); if (p) fread(p, 1, *n, f);
    fclose(f); return p;
}
int main(int argc, char **argv) {
    if (argc != 6) return 1;
    GLMContext ctx = calloc(1, sizeof(*ctx));
    Program *p = calloc(1, sizeof(*p));
    Shader *s = calloc(1, sizeof(*s));
    if (!ctx || !p || !s) return 2;
    initHashTable(&ctx->state.buffer_table, 64);
    ctx->state.program = p;
    p->name = 1; s->name = 1; s->glm_type = _FRAGMENT_SHADER;
    p->shader_slots[_FRAGMENT_SHADER] = s;
    size_t bytes, source_size;
    s->src = read_file(argv[1], &source_size); s->src_len = source_size;
    p->spirv[_FRAGMENT_SHADER].ir = read_file(argv[2], &bytes);
    p->spirv[_FRAGMENT_SHADER].size = bytes / 4;
    p->spirv[_FRAGMENT_SHADER].msl_str = parseSPIRVShaderToMetal(ctx, p, _FRAGMENT_SHADER);
    if (!p->spirv[_FRAGMENT_SHADER].msl_str) return 3;
    applyMSLUniformBufferPacking(p, _FRAGMENT_SHADER);
    Program *before = malloc(sizeof(*p)); memcpy(before, p, sizeof(*p));
    Shader shader_before = *s;
    SpirvResource *copies[_MAX_SPIRV_RES] = {0};
    for (int type = 0; type < _MAX_SPIRV_RES; type++) {
        SpirvResourceList *list = &p->spirv_resources_list[_FRAGMENT_SHADER][type];
        if (list->count) { copies[type] = malloc(list->count * sizeof(SpirvResource));
            memcpy(copies[type], list->list, list->count * sizeof(SpirvResource)); }
    }
    // Reflection order is toolchain-defined; find the actual 'a' bit.
    SpirvResourceList *images = &p->spirv_resources_list[_FRAGMENT_SHADER][SPVC_RESOURCE_TYPE_SAMPLED_IMAGE];
    uint64_t mask = 0;
    for (GLuint i = 0; i < images->count && i < 64; i++)
        if (images->list[i].name && !strcmp(images->list[i].name, "a")) mask |= UINT64_C(1) << i;
    if (!mask) return 4;
    char *variant = mglNativeDepthMSL(ctx, p, mask, atoi(argv[5]) ? mask : 0);
    if (memcmp(p, before, sizeof(*p)) || memcmp(s, &shader_before, sizeof(*s))) return 5;
    for (int type = 0; type < _MAX_SPIRV_RES; type++) {
        SpirvResourceList *list = &p->spirv_resources_list[_FRAGMENT_SHADER][type];
        if (list->count && memcmp(copies[type], list->list, list->count * sizeof(SpirvResource))) return 6;
    }
    if (!variant) return 7;
    FILE *f = fopen(argv[3], "w"); if (!f) return 8; fputs(variant, f); fclose(f);
    f = fopen(argv[4], "w"); if (!f) return 9; fputs(p->spirv[_FRAGMENT_SHADER].msl_str, f); fclose(f);
    // The short-lived adapter process owns all remaining compiler inputs.
    free(variant);
    return 0;
}
'''
HEADER = '''#version 450
layout(binding=0) uniform sampler2D a;
layout(binding=1) uniform sampler2D b;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
'''
CASES = {
    "sample": "void main(){color=texture(a,uv)+texture(b,uv);}",
    "lod": "void main(){color=textureLod(a,uv,1)+texture(b,uv);}",
    "grad": "void main(){color=textureGrad(a,uv,dFdx(uv),dFdy(uv))+texture(b,uv);}",
    "fetch": "void main(){color=texelFetch(a,ivec2(uv),1)+texture(b,uv);}",
    "ubo": "layout(std140,binding=3) uniform Params{mat4 m; vec3 v; float f;};\nvoid main(){color=(texture(a,uv)+texture(b,uv))*m[0]+vec4(v,f);}",
    "plain": "layout(location=3) uniform float scale;\nvoid main(){color=(texture(a,uv)+texture(b,uv))*scale;}",
    "initializer": "layout(location=3) uniform float scale=2.0;\nvoid main(){color=(texture(a,uv)+texture(b,uv))*scale;}",
}
REJECTED = {
    "gather": "void main(){color=textureGather(a,uv)+texture(b,uv);}",
    "offset": "void main(){color=textureOffset(a,uv,ivec2(1))+texture(b,uv);}",
    "projection": "void main(){color=textureProj(a,vec3(uv,1))+texture(b,uv);}",
    "helper": "vec4 sample_it(sampler2D s,vec2 p){return texture(s,p);}\nvoid main(){color=sample_it(a,uv)+texture(b,uv);}",
}

def run(*args):
    proc = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if proc.returncode:
        raise RuntimeError(f"{args}: exit {proc.returncode}\n{proc.stdout}\n{proc.stderr}")
    return proc

with tempfile.TemporaryDirectory(prefix="mgl-depth-msl-") as tmp:
    tmp = pathlib.Path(tmp)
    c = tmp / "compiler_adapter.c"
    exe = tmp / "compiler_adapter"
    c.write_text(HARNESS)
    run("clang", "-DMGL_GL_CORE", c, "-o", exe, "-I" + str(ROOT / "MGL/include"),
        "-I" + str(ROOT / "MGL/include/GL"),
        "-I" + str(ROOT / "external/glslang/glslang/Include"),
        "-I" + str(ROOT / "external/SPIRV-Cross"),
        "-I" + str(ROOT / "external/SPIRV-Tools/include"),
        "-L" + str(ROOT / "build"), "-lmgl", "-Wl,-rpath," + str(ROOT / "build"))
    for name, body in CASES.items():
        source = tmp / (name + ".frag")
        spirv = source.with_suffix(".spv")
        source.write_text(HEADER + body)
        run(ROOT / "external/glslang/build/StandAlone/glslang", "-G", "-o", spirv, source)
        for flip in (0, 1):
            variant = tmp / f"{name}-{flip}.metal"
            base = tmp / f"{name}-base.metal"
            run(exe, source, spirv, variant, base, flip)
            text = variant.read_text()
            assert "depth2d<float>" in text and "texture2d<float>" in text
            for metal in (variant, base):
                run("xcrun", "-sdk", "macosx", "metal", "-std=metal3.1", "-c", metal,
                    "-o", metal.with_suffix(".air"))
        print("PASS", name)
    for name, body in REJECTED.items():
        source = tmp / (name + ".frag")
        spirv = source.with_suffix(".spv")
        source.write_text(HEADER + body)
        run(ROOT / "external/glslang/build/StandAlone/glslang", "-G", "-o", spirv, source)
        for flip in (0, 1):
            proc = subprocess.run([str(exe), str(source), str(spirv), str(tmp / "unused.metal"),
                                   str(tmp / "unused-base.metal"), str(flip)], capture_output=True, text=True)
            assert proc.returncode == 7, (name, proc.returncode, proc.stdout, proc.stderr)
        print("PASS fallback", name)
print("PASS private MSL ABI checks, reflection immutability, and Metal compilation")
