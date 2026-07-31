/*
 * test_regression/dispatch_indirect_gpu_test.c — Spec R5 / Task 5
 *
 * Verifies glDispatchComputeIndirect reads GPU-written dispatch arguments
 * via Metal's native dispatchThreadgroupsWithIndirectBuffer:... (no CPU
 * memcpy pre-read on the hot path).
 *
 * Flow under test (the Voxy pattern):
 *   1. Compute shader A writes {groups_x, groups_y, groups_z} into an SSBO.
 *   2. glMemoryBarrier(GL_COMMAND_BARRIER_BIT) — encoder ordering only,
 *      NO commit+wait (Task 4 hazard model).
 *   3. The SAME buffer is bound to GL_DISPATCH_INDIRECT_BUFFER.
 *   4. glDispatchComputeIndirect(0) dispatches compute shader B, which
 *      atomicAdd's a counter for every invocation.
 *   5. glFinish + readback → counter must equal groups_x * groups_y *
 *      groups_z * local_size.
 *
 * If the implementation CPU-read the indirect buffer on the hot path
 * (the pre-Task-5 behavior), the CPU shadow would still hold the
 * glBufferData zeros at step 4 (the GPU has not yet executed CS_A), so
 * zero threadgroups would dispatch and the counter would stay 0.  A
 * passing test therefore proves the dispatch reads GPU-written arguments
 * via Metal, not via a CPU shadow.
 *
 * Build:  make test-dispatch-indirect-gpu
 * Run:    build/test_dispatch_indirect_gpu
 *
 * Exit code: 0 if all PASS, 1 if any FAIL.
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

/* Read back SSBO data via glMapBufferRange(GL_MAP_READ_BIT).
 * Returns 0 on success, nonzero on failure. */
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

/* ------------------------------------------------------------------ */
/* Shaders                                                             */
/* ------------------------------------------------------------------ */

/* Writes the three group counts into the first three uints of the SSBO
 * bound at binding 0.  The values are baked into the shader so we don't
 * depend on uniform updates. */
static const char *kWriteGroupsCS =
    "#version 460 core\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) writeonly buffer IndirectArgs {\n"
    "    uint groups_x;\n"
    "    uint groups_y;\n"
    "    uint groups_z;\n"
    "} args;\n"
    "void main() {\n"
    "    args.groups_x = 3u;\n"
    "    args.groups_y = 2u;\n"
    "    args.groups_z = 1u;\n"
    "}\n";

/* Same as above but writes the counts at a non-zero offset (skipping
 * 3 uints of padding).  Used to verify indirectBufferOffset propagation. */
static const char *kWriteGroupsOffsetCS =
    "#version 460 core\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) writeonly buffer IndirectArgs {\n"
    "    uint pad[3];   /* bytes 0..11: ignored */\n"
    "    uint groups_x; /* bytes 12..15 */\n"
    "    uint groups_y; /* bytes 16..19 */\n"
    "    uint groups_z; /* bytes 20..23 */\n"
    "} args;\n"
    "void main() {\n"
    "    args.groups_x = 3u;\n"
    "    args.groups_y = 2u;\n"
    "    args.groups_z = 1u;\n"
    "}\n";

/* Writes {5, 1, 1} so a 1D dispatch with a different local_size is verified. */
static const char *kWriteGroups1DCS =
    "#version 460 core\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) writeonly buffer IndirectArgs {\n"
    "    uint groups_x;\n"
    "    uint groups_y;\n"
    "    uint groups_z;\n"
    "} args;\n"
    "void main() {\n"
    "    args.groups_x = 5u;\n"
    "    args.groups_y = 1u;\n"
    "    args.groups_z = 1u;\n"
    "}\n";

/* Consumer compute shader: each invocation atomicAdd's 1 to a counter in
 * the SSBO bound at binding 0.  local_size_x = 4, so each dispatched
 * threadgroup contributes 4 invocations. */
static const char *kCountCS_local4 =
    "#version 460 core\n"
    "layout(local_size_x = 4) in;\n"
    "layout(std430, binding = 0) buffer Counter {\n"
    "    uint value;\n"
    "} counter;\n"
    "void main() {\n"
    "    atomicAdd(counter.value, 1u);\n"
    "}\n";

/* Consumer compute shader with local_size_x = 16 for the 1D dispatch test. */
static const char *kCountCS_local16 =
    "#version 460 core\n"
    "layout(local_size_x = 16) in;\n"
    "layout(std430, binding = 0) buffer Counter {\n"
    "    uint value;\n"
    "} counter;\n"
    "void main() {\n"
    "    atomicAdd(counter.value, 1u);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Test 1: GPU-written 3D group count → indirect dispatch             */
/*                                                                   */
/* CS_A writes {3, 2, 1} into the indirect-args SSBO.  After a        */
/* command-barrier (encoder ordering, no commit+wait), the SAME       */
/* buffer is bound to GL_DISPATCH_INDIRECT_BUFFER and a consumer CS   */
/* with local_size_x=4 atomicAdd's a counter.  Expected counter =     */
/* 3 * 2 * 1 * 4 = 24.                                               */
/* ------------------------------------------------------------------ */

static void test_gpu_written_groups_3d(void)
{
    const char *name = "gpu_written_groups_3d";
    GLuint write_prog = link_program_compute(kWriteGroupsCS);
    GLuint count_prog = link_program_compute(kCountCS_local4);
    if (!write_prog || !count_prog) {
        CHECK(name, 0, "program link failed");
        if (write_prog) glDeleteProgram(write_prog);
        if (count_prog) glDeleteProgram(count_prog);
        return;
    }

    /* Indirect-args buffer: zero-initialised via glBufferData.  The CPU
     * shadow now reads {0,0,0}.  If the implementation CPU-reads this
     * shadow on the hot path, zero threadgroups would dispatch. */
    GLuint args_buf;
    glGenBuffers(1, &args_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, args_buf);
    uint32_t args_zero[3] = {0, 0, 0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(args_zero), args_zero, GL_DYNAMIC_COPY);

    /* Counter buffer, also zero-initialised. */
    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* Step 1: GPU-write {3, 2, 1} into args_buf via compute shader. */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, args_buf);
    glUseProgram(write_prog);
    glDispatchCompute(1, 1, 1);

    /* Step 2: encoder-ordering barrier.  No commit+wait — this is the
     * Voxy hot path.  Task 4 hazard model guarantees the next encoder
     * observes the writes from the previous encoder within the same
     * command buffer. */
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    /* Step 3: bind args_buf as the dispatch indirect buffer and run the
     * consumer compute shader via glDispatchComputeIndirect.  The CPU
     * shadow of args_buf may still be {0,0,0} (GPU has not executed
     * CS_A yet from the CPU's perspective); only a GPU-side read via
     * dispatchThreadgroupsWithIndirectBuffer will see {3,2,1}. */
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, args_buf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glUseProgram(count_prog);
    drain_errors();
    glDispatchComputeIndirect(0);

    /* Step 4: wait for GPU completion and read back the counter. */
    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t counter = 0;
    if (readback_ssbo(counter_buf, 0, sizeof(counter), &counter) != 0) {
        CHECK(name, 0, "readback_ssbo failed for counter");
        glDeleteBuffers(1, &args_buf);
        glDeleteBuffers(1, &counter_buf);
        glDeleteProgram(write_prog);
        glDeleteProgram(count_prog);
        return;
    }

    /* 3 * 2 * 1 threadgroups * 4 invocations/threadgroup = 24. */
    CHECK(name, counter == 24u,
          "expected counter=24 (3*2*1 groups * local_size 4), got %u",
          counter);

    glDeleteBuffers(1, &args_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteProgram(write_prog);
    glDeleteProgram(count_prog);
}

/* ------------------------------------------------------------------ */
/* Test 2: GPU-written args at non-zero offset → indirect dispatch    */
/*                                                                   */
/* CS_A writes {3, 2, 1} at byte offset 12 (skipping 3 uints of       */
/* padding).  glDispatchComputeIndirect(12) must dispatch the same    */
/* 6 threadgroups.  Verifies indirectBufferOffset propagation to      */
/* Metal's dispatchThreadgroupsWithIndirectBuffer:                    */
/* indirectBufferOffset:.                                            */
/* ------------------------------------------------------------------ */

static void test_gpu_written_groups_with_offset(void)
{
    const char *name = "gpu_written_groups_with_offset";
    GLuint write_prog = link_program_compute(kWriteGroupsOffsetCS);
    GLuint count_prog = link_program_compute(kCountCS_local4);
    if (!write_prog || !count_prog) {
        CHECK(name, 0, "program link failed");
        if (write_prog) glDeleteProgram(write_prog);
        if (count_prog) glDeleteProgram(count_prog);
        return;
    }

    /* 8 uints = 32 bytes; args live at bytes [12..24). */
    GLuint args_buf;
    glGenBuffers(1, &args_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, args_buf);
    uint32_t args_zero[8] = {0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(args_zero), args_zero, GL_DYNAMIC_COPY);

    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* GPU-write {3, 2, 1} at offset 12. */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, args_buf);
    glUseProgram(write_prog);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    /* Indirect dispatch with offset=12 — must read the GPU-written args
     * at that offset, not the leading zeros. */
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, args_buf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glUseProgram(count_prog);
    drain_errors();
    glDispatchComputeIndirect(12);

    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t counter = 0;
    if (readback_ssbo(counter_buf, 0, sizeof(counter), &counter) != 0) {
        CHECK(name, 0, "readback_ssbo failed for counter");
        glDeleteBuffers(1, &args_buf);
        glDeleteBuffers(1, &counter_buf);
        glDeleteProgram(write_prog);
        glDeleteProgram(count_prog);
        return;
    }

    /* Same expected count: 3 * 2 * 1 * 4 = 24. */
    CHECK(name, counter == 24u,
          "expected counter=24 (3*2*1 groups * local_size 4) with indirect offset=12, got %u",
          counter);

    glDeleteBuffers(1, &args_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteProgram(write_prog);
    glDeleteProgram(count_prog);
}

/* ------------------------------------------------------------------ */
/* Test 3: GPU-written 1D group count → indirect dispatch             */
/*                                                                   */
/* CS_A writes {5, 1, 1}.  Consumer CS has local_size_x=16.           */
/* Expected counter = 5 * 1 * 1 * 16 = 80.                           */
/* ------------------------------------------------------------------ */

static void test_gpu_written_groups_1d(void)
{
    const char *name = "gpu_written_groups_1d";
    GLuint write_prog = link_program_compute(kWriteGroups1DCS);
    GLuint count_prog = link_program_compute(kCountCS_local16);
    if (!write_prog || !count_prog) {
        CHECK(name, 0, "program link failed");
        if (write_prog) glDeleteProgram(write_prog);
        if (count_prog) glDeleteProgram(count_prog);
        return;
    }

    GLuint args_buf;
    glGenBuffers(1, &args_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, args_buf);
    uint32_t args_zero[3] = {0, 0, 0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(args_zero), args_zero, GL_DYNAMIC_COPY);

    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* GPU-write {5, 1, 1}. */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, args_buf);
    glUseProgram(write_prog);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    /* Indirect dispatch — local_size_x=16 consumer. */
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, args_buf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glUseProgram(count_prog);
    drain_errors();
    glDispatchComputeIndirect(0);

    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t counter = 0;
    if (readback_ssbo(counter_buf, 0, sizeof(counter), &counter) != 0) {
        CHECK(name, 0, "readback_ssbo failed for counter");
        glDeleteBuffers(1, &args_buf);
        glDeleteBuffers(1, &counter_buf);
        glDeleteProgram(write_prog);
        glDeleteProgram(count_prog);
        return;
    }

    /* 5 * 1 * 1 * 16 = 80. */
    CHECK(name, counter == 80u,
          "expected counter=80 (5*1*1 groups * local_size 16), got %u",
          counter);

    glDeleteBuffers(1, &args_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteProgram(write_prog);
    glDeleteProgram(count_prog);
}

/* ------------------------------------------------------------------ */
/* Test 4: Diagnostic — GPU-written args with explicit glFinish sync  */
/*                                                                   */
/* Same as test 1 but inserts glFinish() + readback after CS_A to    */
/* force GPU completion before the indirect dispatch.  This isolates  */
/* whether the indirect dispatch path itself works when the GPU-     */
/* written args are guaranteed visible, vs. whether the barrier-     */
/* only path (encoder ordering, no commit+wait) is sufficient.       */
/* ------------------------------------------------------------------ */

static void test_gpu_written_groups_with_finish_sync(void)
{
    const char *name = "gpu_written_groups_with_finish_sync";
    GLuint write_prog = link_program_compute(kWriteGroupsCS);
    GLuint count_prog = link_program_compute(kCountCS_local4);
    if (!write_prog || !count_prog) {
        CHECK(name, 0, "program link failed");
        if (write_prog) glDeleteProgram(write_prog);
        if (count_prog) glDeleteProgram(count_prog);
        return;
    }

    GLuint args_buf;
    glGenBuffers(1, &args_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, args_buf);
    uint32_t args_zero[3] = {0, 0, 0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(args_zero), args_zero, GL_DYNAMIC_COPY);

    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* Step 1: GPU-write {3, 2, 1} into args_buf. */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, args_buf);
    glUseProgram(write_prog);
    glDispatchCompute(1, 1, 1);

    /* Step 2: force GPU completion + readback to verify CS_A wrote. */
    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t args_readback[3] = {0};
    if (readback_ssbo(args_buf, 0, sizeof(args_readback), args_readback) != 0) {
        CHECK(name, 0, "readback_ssbo failed for args_buf");
        glDeleteBuffers(1, &args_buf);
        glDeleteBuffers(1, &counter_buf);
        glDeleteProgram(write_prog);
        glDeleteProgram(count_prog);
        return;
    }
    if (args_readback[0] != 3u || args_readback[1] != 2u || args_readback[2] != 1u) {
        CHECK(name, 0,
              "CS_A did not write args correctly: got {%u, %u, %u}, expected {3, 2, 1}",
              args_readback[0], args_readback[1], args_readback[2]);
        glDeleteBuffers(1, &args_buf);
        glDeleteBuffers(1, &counter_buf);
        glDeleteProgram(write_prog);
        glDeleteProgram(count_prog);
        return;
    }

    /* Step 3: indirect dispatch — GPU is synced, args are visible. */
    glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, args_buf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glUseProgram(count_prog);
    drain_errors();
    glDispatchComputeIndirect(0);

    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t counter = 0;
    if (readback_ssbo(counter_buf, 0, sizeof(counter), &counter) != 0) {
        CHECK(name, 0, "readback_ssbo failed for counter");
        glDeleteBuffers(1, &args_buf);
        glDeleteBuffers(1, &counter_buf);
        glDeleteProgram(write_prog);
        glDeleteProgram(count_prog);
        return;
    }

    CHECK(name, counter == 24u,
          "expected counter=24 (3*2*1 groups * local_size 4) with glFinish sync, got %u",
          counter);

    glDeleteBuffers(1, &args_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteProgram(write_prog);
    glDeleteProgram(count_prog);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fprintf(stderr,
            "=== MGL glDispatchComputeIndirect GPU-native test (Spec R5 / Task 5) ===\n\n");

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

    fprintf(stderr, "[1] GPU-written 3D group count -> indirect dispatch\n");
    test_gpu_written_groups_3d();
    drain_errors();

    fprintf(stderr, "\n[2] GPU-written args at non-zero offset -> indirect dispatch\n");
    test_gpu_written_groups_with_offset();
    drain_errors();

    fprintf(stderr, "\n[3] GPU-written 1D group count -> indirect dispatch\n");
    test_gpu_written_groups_1d();
    drain_errors();

    fprintf(stderr, "\n[4] Diagnostic: GPU-written args with glFinish sync before indirect dispatch\n");
    test_gpu_written_groups_with_finish_sync();
    drain_errors();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
