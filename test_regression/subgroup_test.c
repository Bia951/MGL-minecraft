/*
 * test_regression/subgroup_test.c — Spec R9 / Task 9
 *
 * Headless, non-interactive runtime test for GL_KHR_shader_subgroup lowering
 * to Metal simdgroup intrinsics.
 *
 * Verifies the end-to-end data flow that Voxy's Hi-Z shader depends on:
 *   - subgroupMax          (GL_KHR_shader_subgroup_arithmetic) reduces to a
 *                           correct per-subgroup maximum via Metal simd_max.
 *   - subgroupClusteredMax (GL_KHR_shader_subgroup_clustered, cluster size 4)
 *                           reduces to a correct per-quad maximum via
 *                           Metal quad_max.
 *   - gl_SubgroupInvocationID / gl_SubgroupSize are populated correctly.
 *
 * The test is gated on the GL_KHR_shader_subgroup extension being advertised
 * by the MGL context — which in turn is gated on MGLCapability detecting a
 * real simdgroup (threadExecutionWidth >= 32, see MGLCapability /
 * MGLRenderer init).  On non-simdgroup hardware the test reports SKIP
 * rather than FAIL, because the extension is intentionally not advertised
 * there and the simdgroup MSL would not compile.
 *
 * Build:  make test-subgroup
 * Run:    build/test_subgroup
 *
 * Exit code: 0 if all PASS (or all SKIP on non-subgroup hardware), 1 on FAIL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>

#include "MGLContext.h"
#include "MGLRenderer.h"

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

#define CHECK(name, cond, ...)                                                \
    do {                                                                      \
        if (cond) {                                                           \
            fprintf(stderr, "  [PASS] %s\n", name);                           \
            g_pass++;                                                         \
        } else {                                                              \
            fprintf(stderr, "  [FAIL] %s: ", name);                           \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* GL helpers                                                          */
/* ------------------------------------------------------------------ */

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "  [shader compile FAIL] %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program_compute(const char *cs_src)
{
    GLuint cs = compile_shader(GL_COMPUTE_SHADER, cs_src);
    if (!cs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, cs);
    glLinkProgram(p);
    glDeleteShader(cs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "  [program link FAIL] %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void drain_errors(void)
{
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "  [drained GL error 0x%x]\n", e);
    }
}

/* Read back SSBO data via glMapBufferRange(GL_MAP_READ_BIT) — same path
 * std430_test.c uses; this returns GPU-written data, not the CPU-side
 * buffer_data shadow.  Returns 0 on success, nonzero on failure. */
static int readback_ssbo(GLuint buffer, GLintptr offset, GLsizeiptr size, void *out)
{
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
    void *mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, offset, size, GL_MAP_READ_BIT);
    if (!mapped) {
        fprintf(stderr, "  [readback_ssbo] glMapBufferRange failed for buffer %u\n", buffer);
        return 1;
    }
    memcpy(out, mapped, (size_t)size);
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    return 0;
}

/* Return 1 if the named extension is advertised by the current MGL context,
 * 0 otherwise.  Scans GL_EXTENSIONS via glGetStringi so the dynamic subgroup
 * extension list (appended in get.c when subgroup_supported is set) is
 * observed. */
static int has_extension(const char *name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const GLubyte *ext = glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (ext && strcmp((const char *)ext, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subgroup reduction compute shader                                   */
/* ------------------------------------------------------------------ */

/* One workgroup = local_size_x 32 = one simdgroup on Apple Silicon
 * (threadExecutionWidth == 32).  Each lane contributes its
 * gl_SubgroupInvocationID as the value, then:
 *   m  = subgroupMax(v)                 -> 31 for a full 32-wide subgroup
 *   cm = subgroupClusteredMax(v, 4)     -> per-quad max; lane L is in quad
 *                                          [L/4*4 .. L/4*4+3], so cm = quad*4+3
 *
 * Lane 0 writes three packed uints per workgroup into a single runtime-sized
 * array:  result[base+0]=max, result[base+1]=cluster_max, result[base+2]=size.
 * A single runtime array keeps the C-side readback indexing trivially in sync
 * with the GLSL layout (no per-array stride arithmetic to drift on). */
static const char *kSubgroupReduceCS =
    "#version 460 core\n"
    "#extension GL_KHR_shader_subgroup_basic : enable\n"
    "#extension GL_KHR_shader_subgroup_arithmetic : enable\n"
    "#extension GL_KHR_shader_subgroup_clustered : enable\n"
    "layout(local_size_x = 32) in;\n"
    "layout(std430, binding = 0) writeonly buffer OutBuf {\n"
    "    uint result[];\n"
    "} out_buf;\n"
    "void main() {\n"
    "    uint v = gl_SubgroupInvocationID;\n"
    "    uint m = subgroupMax(v);\n"
    "    uint cm = subgroupClusteredMax(v, 4);\n"
    "    if (gl_SubgroupInvocationID == 0u) {\n"
    "        uint base = gl_WorkGroupID.x * 3u;\n"
    "        out_buf.result[base + 0u] = m;\n"
    "        out_buf.result[base + 1u] = cm;\n"
    "        out_buf.result[base + 2u] = gl_SubgroupSize;\n"
    "    }\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Test: subgroupMax / subgroupClusteredMax reduction                  */
/* ------------------------------------------------------------------ */

static void test_subgroup_reduction(void)
{
    const char *name = "subgroup_reduction_runtime";

    /* 4 workgroups x 32 lanes.  Each workgroup's lane 0 writes 3 uints. */
    const GLuint N_WORKGROUPS = 4;
    const GLuint SSBO_UINTS = N_WORKGROUPS * 3; /* max_val + cluster_max + size */
    const GLsizeiptr SSBO_BYTES = (GLsizeiptr)(SSBO_UINTS * sizeof(uint32_t));

    GLuint prog = link_program_compute(kSubgroupReduceCS);
    if (!prog) {
        CHECK(name, 0, "program link failed (subgroup lowering rejected)");
        return;
    }

    GLuint ssbo;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    {
        uint32_t zero[16] = {0};
        /* Clear the whole region we will read back. */
        uint32_t *clr = (uint32_t *)calloc(SSBO_UINTS, sizeof(uint32_t));
        if (clr) {
            glBufferData(GL_SHADER_STORAGE_BUFFER, SSBO_BYTES, clr, GL_DYNAMIC_COPY);
            free(clr);
        } else {
            glBufferData(GL_SHADER_STORAGE_BUFFER, SSBO_BYTES, zero, GL_DYNAMIC_COPY);
        }
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);

    glUseProgram(prog);
    glDispatchCompute(N_WORKGROUPS, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t *out = (uint32_t *)calloc(SSBO_UINTS, sizeof(uint32_t));
    int rb = out ? readback_ssbo(ssbo, 0, SSBO_BYTES, out) : 1;
    if (rb != 0) {
        CHECK(name, 0, "SSBO readback failed");
        free(out);
        glDeleteProgram(prog);
        glDeleteBuffers(1, &ssbo);
        return;
    }

    /* Expected results for a 32-wide subgroup, packed as
     *   result[wg*3 + 0] = subgroupMax(v)             = 31
     *   result[wg*3 + 1] = subgroupClusteredMax(v, 4) = 3  (lane 0's quad max)
     *   result[wg*3 + 2] = gl_SubgroupSize            = 32 (threadExecutionWidth)
     *
     * subgroup_size is the runtime probe of the actual simdgroup width; we
     * accept >= 16 so the test is not brittle on hypothetical narrower
     * hardware, but it must match across all workgroups. */
    int failures = 0;
    uint32_t first_size = out[2]; /* subgroup_size for wg 0 */
    for (GLuint wg = 0; wg < N_WORKGROUPS; wg++) {
        uint32_t base = wg * 3u;
        uint32_t m  = out[base + 0];
        uint32_t cm = out[base + 1];
        uint32_t sz = out[base + 2];

        if (m != 31u) {
            fprintf(stderr,
                    "  [detail] wg=%u max_val=%u (expected 31)\n", wg, m);
            failures++;
        }
        if (cm != 3u) {
            fprintf(stderr,
                    "  [detail] wg=%u cluster_max_lane0=%u (expected 3)\n", wg, cm);
            failures++;
        }
        if (sz < 16u) {
            fprintf(stderr,
                    "  [detail] wg=%u subgroup_size=%u (expected >= 16)\n", wg, sz);
            failures++;
        }
        if (wg > 0 && sz != first_size) {
            fprintf(stderr,
                    "  [detail] wg=%u subgroup_size=%u differs from wg0=%u\n",
                    wg, sz, first_size);
            failures++;
        }
    }

    CHECK(name, failures == 0, "%d subgroup reduction mismatch(es)", failures);

    free(out);
    glDeleteProgram(prog);
    glDeleteBuffers(1, &ssbo);
}

/* ------------------------------------------------------------------ */
/* Test: extension advertisement on simdgroup hardware                 */
/* ------------------------------------------------------------------ */

static void test_extension_advertised(void)
{
    const char *name = "subgroup_extension_advertised";
    int has_basic = has_extension("GL_KHR_shader_subgroup_basic");
    int has_arith = has_extension("GL_KHR_shader_subgroup_arithmetic");
    int has_cluster = has_extension("GL_KHR_shader_subgroup_clustered");
    int has_root = has_extension("GL_KHR_shader_subgroup");

    /* On simdgroup-capable hardware all four strings must be present (they
     * are appended together in get.c).  On non-subgroup hardware none are
     * advertised; the runtime reduction test then SKIPs. */
    CHECK(name,
          (has_basic && has_arith && has_cluster && has_root) ||
          (!has_basic && !has_arith && !has_cluster && !has_root),
          "inconsistent subgroup extension set: basic=%d arith=%d cluster=%d root=%d",
          has_basic, has_arith, has_cluster, has_root);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fprintf(stderr,
            "=== MGL subgroup -> simdgroup lowering test (Spec R9 / Task 9) ===\n\n");

    GLMContext glm_ctx = createGLMContext(
        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
        GL_DEPTH_COMPONENT, GL_FLOAT, 0, 0);
    if (!glm_ctx) {
        fprintf(stderr, "FATAL: createGLMContext failed\n");
        return 1;
    }
    void *renderer = CppCreateMGLRendererHeadless(glm_ctx);
    if (!renderer) {
        fprintf(stderr, "FATAL: CppCreateMGLRendererHeadless failed\n");
        return 1;
    }
    MGLsetCurrentContext(glm_ctx);

    /* [1] Extension advertisement consistency.  On simdgroup hardware all
     *     four GL_KHR_shader_subgroup* strings appear; otherwise none do. */
    fprintf(stderr, "[1] Extension advertisement\n");
    test_extension_advertised();
    drain_errors();

    /* [2] Runtime reduction — only meaningful where the extension is
     *     advertised (simdgroup hardware).  SKIP otherwise. */
    fprintf(stderr, "\n[2] Subgroup reduction runtime\n");
    if (has_extension("GL_KHR_shader_subgroup_arithmetic")) {
        test_subgroup_reduction();
    } else {
        fprintf(stderr, "  [SKIP] subgroup_reduction_runtime: "
                        "GL_KHR_shader_subgroup not advertised "
                        "(non-simdgroup device)\n");
        g_skip++;
    }
    drain_errors();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  PASS: %d   FAIL: %d   SKIP: %d\n", g_pass, g_fail, g_skip);
    fprintf(stderr, "========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
