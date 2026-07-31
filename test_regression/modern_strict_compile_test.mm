//
//  modern_strict_compile_test.mm
//  MGL — Spec R12 / Task 12 pre-compilation validation
//
//  Compiles a set of GLSL 4.60 compute shaders through the modern strict
//  profile (mgl_toolchain_glsl_to_msl_profile) and verifies the resulting
//  MSL compiles with the Metal device compiler
//  (MTLDevice newLibraryWithSource:options:error:).
//
//  Also unit-tests the profile detector (mglDetectShaderProfile) so the
//  legacy / modern split rule stays correct.
//
//  Build:  make test-modern-strict
//  Run:    build/test_modern_strict
//
//  Exit code: 0 if all PASS, 1 if any FAIL.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

extern "C" {
#include "mgl_toolchain.h"
#include "mgl_shader_profile.h"
#include "glcorearb.h"
}

/* ------------------------------------------------------------------ */
/* Test shaders                                                       */
/* ------------------------------------------------------------------ */

/* Minimal GLSL 4.60 compute shader — no resources, just writes a constant
 * to a storage buffer to exercise the basic compile path. */
static const char *kComputeBasic460 =
    "#version 460 core\n"
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(std430, binding = 0) writeonly buffer OutBuf {\n"
    "    uint data[];\n"
    "} out_buf;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 8u +\n"
    "               gl_GlobalInvocationID.x;\n"
    "    out_buf.data[idx] = 0xCAFEBABEu;\n"
    "}\n";

/* GLSL 4.60 compute shader with SSBO runtime array + atomicAdd — exercises
 * the std430 / runtime-array / atomic semantics that Voxy relies on and
 * that the legacy string-level rewrites would corrupt. */
static const char *kComputeSSBOAtomic460 =
    "#version 460 core\n"
    "layout(local_size_x = 32) in;\n"
    "layout(std430, binding = 0) coherent buffer CounterBuf {\n"
    "    uint counts[];\n"
    "};\n"
    "layout(std430, binding = 1) readonly buffer InputBuf {\n"
    "    float values[];\n"
    "} in_buf;\n"
    "shared uint s_tally;\n"
    "void main() {\n"
    "    if (gl_LocalInvocationIndex == 0u) {\n"
    "        s_tally = 0u;\n"
    "    }\n"
    "    barrier();\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    float v = in_buf.values[idx];\n"
    "    if (v > 0.0) {\n"
    "        atomicAdd(s_tally, 1u);\n"
    "    }\n"
    "    barrier();\n"
    "    if (gl_LocalInvocationIndex == 0u) {\n"
    "        atomicAdd(counts[gl_WorkGroupID.x], s_tally);\n"
    "    }\n"
    "}\n";

/* GLSL 4.60 compute shader with image load/store — exercises the image
 * format / binding semantics that the modern strict profile must preserve. */
static const char *kComputeImage460 =
    "#version 460 core\n"
    "layout(local_size_x = 4) in;\n"
    "layout(binding = 0, r32f) uniform image2D img;\n"
    "void main() {\n"
    "    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);\n"
    "    float prev = imageLoad(img, coord).x;\n"
    "    imageStore(img, coord, vec4(prev + 1.0, 0.0, 0.0, 1.0));\n"
    "}\n";

/* GLSL 4.60 compute shader exercising GL_KHR_shader_subgroup ops — the
 * Voxy Hi-Z contract.  SPIRV-Cross lowers each op to a Metal simdgroup
 * intrinsic:
 *   subgroupBarrier          -> simdgroup_barrier(...)
 *   subgroupMax              -> simd_max(...)
 *   subgroupClusteredMax(_,4)-> quad_max(...)   (cluster size MUST be 4;
 *                              SPIRV-Cross throws on any other size because
 *                              Metal only exposes quad-level clustering)
 *   gl_SubgroupInvocationID  -> [[thread_index_in_simdgroup]]
 *   gl_SubgroupSize          -> [[thread_execution_width]]  (compute stage)
 * The modern strict profile (SPV_1_3 + auto-map) feeds these straight
 * through glslang -> SPIR-V -> SPIRV-Cross with no MSL string patch. */
static const char *kComputeSubgroup460 =
    "#version 460 core\n"
    "#extension GL_KHR_shader_subgroup_basic : enable\n"
    "#extension GL_KHR_shader_subgroup_arithmetic : enable\n"
    "#extension GL_KHR_shader_subgroup_clustered : enable\n"
    "layout(local_size_x = 32) in;\n"
    "layout(std430, binding = 0) writeonly buffer OutBuf {\n"
    "    uint data[];\n"
    "} out_buf;\n"
    "void main() {\n"
    "    uint v = gl_SubgroupInvocationID;\n"
    "    subgroupBarrier();\n"
    "    uint m = subgroupMax(v);\n"
    "    uint cm = subgroupClusteredMax(v, 4);\n"
    "    if (gl_SubgroupInvocationID == 0u) {\n"
    "        out_buf.data[gl_WorkGroupID.x] = m * 100u + cm + gl_SubgroupSize;\n"
    "    }\n"
    "}\n";

/* Legacy GLSL 3.30 fragment shader — must take the LEGACY profile, not
 * modern strict.  Used by the detector unit test. */
static const char *kLegacyFS330 =
    "#version 330 core\n"
    "in vec3 v_color;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = vec4(v_color, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

static void record_pass(const char *name) {
    fprintf(stderr, "  [PASS] %s\n", name);
    g_pass++;
}

static void record_fail(const char *name, const char *detail) {
    fprintf(stderr, "  [FAIL] %s: %s\n", name, detail ? detail : "(no detail)");
    g_fail++;
}

/* Compile GLSL -> MSL via the modern strict profile, then verify the MSL
 * with the Metal device compiler.  Returns 1 on full success. */
static int compile_and_verify_metal(const char *name,
                                    mgl_toolchain_stage stage,
                                    const char *glsl,
                                    MGLShaderProfile profile) {
    char *msl = NULL;
    size_t msl_len = 0;
    char *err = NULL;

    int rc = mgl_toolchain_glsl_to_msl_profile(stage, glsl, 0,
                                               &msl, &msl_len, &err, profile);
    if (rc != 0 || !msl) {
        char buf[256];
        snprintf(buf, sizeof(buf), "toolchain compile failed: %s",
                 err ? err : "(no message)");
        record_fail(name, buf);
        mgl_toolchain_free(err);
        mgl_toolchain_free(msl);
        return 0;
    }

    /* Verify with the Metal device compiler. */
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        record_fail(name, "MTLCreateSystemDefaultDevice returned NULL");
        mgl_toolchain_free(msl);
        return 0;
    }

    NSString *src = [[NSString alloc] initWithBytes:msl
                                            length:msl_len
                                          encoding:NSUTF8StringEncoding];
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
    NSError *nserr = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src
                                              options:opts
                                                error:&nserr];
    if (!lib) {
        NSString *msg = nserr ? [nserr localizedDescription] : @"(no error)";
        char buf[512];
        snprintf(buf, sizeof(buf), "Metal newLibraryWithSource failed: %s",
                 [msg UTF8String]);
        /* Dump MSL to /tmp for offline inspection. */
        char dump[256];
        snprintf(dump, sizeof(dump), "/tmp/mgl_modern_strict_%s.msl", name);
        FILE *fp = fopen(dump, "w");
        if (fp) { fputs(msl, fp); fclose(fp); }
        record_fail(name, buf);
        mgl_toolchain_free(msl);
        return 0;
    }

    record_pass(name);
    mgl_toolchain_free(msl);
    return 1;
}

/* Spec R9 / Task 9: compile a GL_KHR_shader_subgroup compute shader via
 * the modern strict profile and verify (a) SPIRV-Cross lowered the ops to
 * Metal simdgroup intrinsics and (b) the MSL compiles on the Metal device.
 *
 * The lowering check is a substring scan over the generated MSL for the
 * intrinsic tokens SPIRV-Cross emits for each subgroup op (see the table in
 * the kComputeSubgroup460 comment above).  This validates the auto-lowering
 * path independently of the Metal compiler, so a failure here pinpoints a
 * SPIRV-Cross regression rather than a generic compile error.  The Metal
 * compile step then confirms the lowered MSL is actually acceptable to the
 * device compiler (simdgroup intrinsics are core on Apple Silicon / Intel
 * dGPUs; on hardware without simdgroup support Metal rejects the MSL). */
static int compile_and_verify_subgroup(const char *name,
                                       const char *glsl) {
    char *msl = NULL;
    size_t msl_len = 0;
    char *err = NULL;

    int rc = mgl_toolchain_glsl_to_msl_profile(MGL_TOOLCHAIN_STAGE_COMPUTE,
                                               glsl, 0,
                                               &msl, &msl_len, &err,
                                               MGL_SHADER_PROFILE_MODERN_STRICT);
    if (rc != 0 || !msl) {
        char buf[256];
        snprintf(buf, sizeof(buf), "toolchain compile failed: %s",
                 err ? err : "(no message)");
        record_fail(name, buf);
        mgl_toolchain_free(err);
        mgl_toolchain_free(msl);
        return 0;
    }

    /* Dump MSL for offline inspection regardless of pass/fail. */
    char dump[256];
    snprintf(dump, sizeof(dump), "/tmp/mgl_modern_strict_%s.msl", name);
    FILE *fp = fopen(dump, "w");
    if (fp) { fputs(msl, fp); fclose(fp); }

    /* Lowering check: every expected simdgroup intrinsic token must appear in
     * the generated MSL.  Missing tokens mean SPIRV-Cross failed to lower a
     * subgroup op (e.g. emitted a GLSL-style fallback or threw and we caught
     * it elsewhere).  simd_max/quad_max are matched as bare identifiers by
     * checking for "simd_max(" and "quad_max(" to avoid colliding with
     * simd_prefix_* / quad_prefix_* names. */
    static const struct {
        const char *token;
        const char *what;
    } expected[] = {
        { "simdgroup_barrier",         "subgroupBarrier -> simdgroup_barrier" },
        { "simd_max(",                 "subgroupMax -> simd_max" },
        { "quad_max(",                 "subgroupClusteredMax -> quad_max" },
        { "thread_index_in_simdgroup", "gl_SubgroupInvocationID" },
        { "thread_execution_width",    "gl_SubgroupSize" },
    };
    int lowering_ok = 1;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (!strstr(msl, expected[i].token)) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "MSL lowering missing token '%s' (%s)",
                     expected[i].token, expected[i].what);
            record_fail(name, buf);
            lowering_ok = 0;
            break;
        }
    }
    if (!lowering_ok) {
        mgl_toolchain_free(msl);
        return 0;
    }

    /* Metal device compile of the lowered MSL.  On a simdgroup-capable device
     * (Apple Silicon / Intel dGPU) this succeeds; the GL_KHR_shader_subgroup*
     * extension strings are gated on threadExecutionWidth >= 32 in
     * MGLCapability / get.c, so a non-simdgroup device would not advertise
     * the extension and this compile path would never be exercised in
     * production.  Here we still attempt the compile and report failure
     * explicitly so a regression on simdgroup hardware is caught. */
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        record_fail(name, "MTLCreateSystemDefaultDevice returned NULL");
        mgl_toolchain_free(msl);
        return 0;
    }

    NSString *src = [[NSString alloc] initWithBytes:msl
                                            length:msl_len
                                          encoding:NSUTF8StringEncoding];
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
    NSError *nserr = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src
                                              options:opts
                                                error:&nserr];
    if (!lib) {
        NSString *msg = nserr ? [nserr localizedDescription] : @"(no error)";
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "Metal newLibraryWithSource failed (simdgroup MSL): %s",
                 [msg UTF8String]);
        record_fail(name, buf);
        mgl_toolchain_free(msl);
        return 0;
    }

    record_pass(name);
    mgl_toolchain_free(msl);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Profile detector unit tests                                        */
/* ------------------------------------------------------------------ */

static void test_profile_detector(void) {
    const char *name = "profile_detector";

    /* GLSL 460 compute -> MODERN_STRICT */
    MGLShaderProfile p = mglDetectShaderProfile(GL_COMPUTE_SHADER,
                                                kComputeBasic460,
                                                strlen(kComputeBasic460));
    if (p != MGL_SHADER_PROFILE_MODERN_STRICT) {
        record_fail(name, "460 compute should be MODERN_STRICT");
        return;
    }

    /* GLSL 460 vertex with SSBO -> MODERN_STRICT */
    const char *vs_ssbo =
        "#version 460 core\n"
        "layout(std430, binding = 0) readonly buffer B { float x[]; };\n"
        "void main() { gl_Position = vec4(x[0]); }\n";
    p = mglDetectShaderProfile(GL_VERTEX_SHADER, vs_ssbo, strlen(vs_ssbo));
    if (p != MGL_SHADER_PROFILE_MODERN_STRICT) {
        record_fail(name, "460 VS with SSBO should be MODERN_STRICT");
        return;
    }

    /* Legacy GLSL 330 fragment -> LEGACY */
    p = mglDetectShaderProfile(GL_FRAGMENT_SHADER, kLegacyFS330,
                               strlen(kLegacyFS330));
    if (p != MGL_SHADER_PROFILE_LEGACY) {
        record_fail(name, "330 FS should be LEGACY");
        return;
    }

    /* Legacy GLSL 120 -> LEGACY */
    const char *vs120 =
        "#version 120\n"
        "void main() { gl_Position = ftransform(); }\n";
    p = mglDetectShaderProfile(GL_VERTEX_SHADER, vs120, strlen(vs120));
    if (p != MGL_SHADER_PROFILE_LEGACY) {
        record_fail(name, "120 VS should be LEGACY");
        return;
    }

    record_pass(name);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    fprintf(stderr, "=== MGL modern strict profile compile test (Spec R12) ===\n\n");

    /* 1. Profile detector unit tests. */
    fprintf(stderr, "[1] Profile detector\n");
    test_profile_detector();

    /* 2. Compile each modern shader and verify with Metal compiler. */
    fprintf(stderr, "\n[2] Modern strict compile + Metal verification\n");
    compile_and_verify_metal("compute_basic_460",
                             MGL_TOOLCHAIN_STAGE_COMPUTE,
                             kComputeBasic460,
                             MGL_SHADER_PROFILE_MODERN_STRICT);
    compile_and_verify_metal("compute_ssbo_atomic_460",
                             MGL_TOOLCHAIN_STAGE_COMPUTE,
                             kComputeSSBOAtomic460,
                             MGL_SHADER_PROFILE_MODERN_STRICT);
    compile_and_verify_metal("compute_image_460",
                             MGL_TOOLCHAIN_STAGE_COMPUTE,
                             kComputeImage460,
                             MGL_SHADER_PROFILE_MODERN_STRICT);

    /* 3. Auto-detect path (mgl_toolchain_glsl_to_msl) should also pick
     *    MODERN_STRICT for a 460 compute shader. */
    fprintf(stderr, "\n[3] Auto-detect path\n");
    {
        char *msl = NULL;
        size_t msl_len = 0;
        char *err = NULL;
        int rc = mgl_toolchain_glsl_to_msl(MGL_TOOLCHAIN_STAGE_COMPUTE,
                                           kComputeBasic460, 0,
                                           &msl, &msl_len, &err);
        if (rc == 0 && msl) {
            record_pass("auto_detect_compute_460");
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "auto-detect compile failed: %s",
                     err ? err : "(no message)");
            record_fail("auto_detect_compute_460", buf);
        }
        mgl_toolchain_free(err);
        mgl_toolchain_free(msl);
    }

    /* 4. Subgroup lowering (Spec R9 / Task 9): GL_KHR_shader_subgroup ops
     *    must lower to Metal simdgroup intrinsics via SPIRV-Cross and the
     *    resulting MSL must compile on the Metal device. */
    fprintf(stderr, "\n[4] Subgroup -> simdgroup lowering (Spec R9)\n");
    compile_and_verify_subgroup("compute_subgroup_460", kComputeSubgroup460);

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
