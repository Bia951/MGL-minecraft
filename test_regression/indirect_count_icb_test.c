/*
 * test_regression/indirect_count_icb_test.c — Spec R6 / Task 6.2 + 6.3
 *
 * Verifies glMultiDrawElementsIndirectCount and glMultiDrawArraysIndirectCount
 * with a GPU-written draw count (the Voxy pattern).
 *
 * Flow under test:
 *   1. Compute shader writes the draw count into GL_PARAMETER_BUFFER.
 *   2. glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT)
 *      — encoder ordering, NO commit+wait (Task 4 hazard model).
 *   3. glMultiDrawElementsIndirectCount / glMultiDrawArraysIndirectCount
 *      reads the GPU-written count and dispatches the correct number of draws.
 *
 * Two sub-tests:
 *   A. GPU writes count=4, maxDrawCount=8 → exactly 4 draws execute.
 *   B. GPU writes count=6, maxDrawCount=3 → clamp to 3 draws (count > max).
 *
 * When MGL_ENABLE_INDIRECT_COUNT_GPU is set, the GPU path (compute kernel
 * patches a shadow indirect buffer, native Metal indirect draws, no CPU
 * readback) is exercised.  When unset, the CPU fallback (commit+wait+readback)
 * is used.  Both paths must produce the same functional result.
 *
 * Build:  make test-indirect-count-icb
 * Run:    build/test_indirect_count_icb
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

static GLuint link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { glDeleteShader(vs); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
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

/* Full-screen triangle vertex shader — no vertex attributes needed.
 * Each draw command renders one full-screen triangle covering the 1x1 FBO,
 * so each draw contributes exactly 1 fragment to the atomic counter. */
static const char *kFullscreenVS =
    "#version 460 core\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

/* Fragment shader: atomicAdd 1 to counter SSBO at binding 0. */
static const char *kCountFS =
    "#version 460 core\n"
    "layout(std430, binding = 0) buffer Counter { uint value; } counter;\n"
    "void main() {\n"
    "    atomicAdd(counter.value, 1u);\n"
    "}\n";

/* Compute shader: writes the draw count into the parameter buffer at offset 0. */
static const char *kWriteCountCS =
    "#version 460 core\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) writeonly buffer CountBuf { uint value; } cnt;\n"
    "layout(location = 0) uniform uint u_drawCount;\n"
    "void main() {\n"
    "    cnt.value = u_drawCount;\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Test A: GPU-written count=4, maxDrawCount=8 → 4 draws               */
/*                                                                   */
/* CS writes count=4 into GL_PARAMETER_BUFFER. After a command barrier, */
/* glMultiDrawElementsIndirectCount reads the count (GPU-side if the    */
/* GPU path is enabled, CPU-side otherwise) and dispatches 4 draws.    */
/* Each draw covers the 1x1 FBO, atomicAdd'ing the counter once.       */
/* Expected counter = 4.                                              */
/* ------------------------------------------------------------------ */

static void test_elements_count_gpu_written(void)
{
    const char *name = "elements_count_gpu_written_4_of_8";
    const uint32_t EXPECTED_DRAWS = 4u;
    const uint32_t MAX_DRAWS = 8u;

    GLuint draw_prog = link_program(kFullscreenVS, kCountFS);
    GLuint write_prog = link_program_compute(kWriteCountCS);
    if (!draw_prog || !write_prog) {
        CHECK(name, 0, "program link failed");
        if (draw_prog) glDeleteProgram(draw_prog);
        if (write_prog) glDeleteProgram(write_prog);
        return;
    }

    /* 1x1 FBO for counting fragments. */
    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    /* Element buffer: 3 indices for one triangle. */
    GLuint ibo;
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    uint16_t indices[3] = {0, 1, 2};
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    /* Indirect command buffer: 8 DrawElementsIndirectCommand entries. */
    GLuint cmd_buf;
    glGenBuffers(1, &cmd_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    struct DrawElementsCmd {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t  baseVertex;
        uint32_t baseInstance;
    } cmds[8];
    for (int i = 0; i < 8; i++) {
        cmds[i].count = 3;
        cmds[i].instanceCount = 1;
        cmds[i].firstIndex = 0;
        cmds[i].baseVertex = 0;
        cmds[i].baseInstance = 0;
    }
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmds), cmds, GL_STATIC_DRAW);

    /* Parameter buffer: zero-initialized; GPU will write the count.
     * Created with GL_SHADER_STORAGE_BUFFER target so MGL's SSBO binding
     * path correctly maps it for compute shader writes.  Later bound to
     * GL_PARAMETER_BUFFER for the indirect count draw. */
    GLuint param_buf;
    glGenBuffers(1, &param_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, param_buf);
    uint32_t zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), &zero, GL_DYNAMIC_COPY);

    /* Counter SSBO: zero-initialized. */
    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* Step 1: GPU-write count=4 into parameter buffer. */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, param_buf);
    glUseProgram(write_prog);
    glUniform1ui(glGetUniformLocation(write_prog, "u_drawCount"), EXPECTED_DRAWS);
    glDispatchCompute(1, 1, 1);

    /* Step 2: barrier — encoder ordering, no commit+wait.
     * GL_COMMAND_BARRIER_BIT covers the parameter buffer (indirect command
     * source); GL_SHADER_STORAGE_BARRIER_BIT covers the SSBO write. */
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    /* DEBUG: verify GPU write */
    if (getenv("MGL_DEBUG_INDIRECT_COUNT_SHADOW")) {
        glFinish();
        uint32_t debug_count = 0;
        readback_ssbo(param_buf, 0, sizeof(debug_count), &debug_count);
        fprintf(stderr, "  [DEBUG-TEST] After kWriteCountCS: param_buf[0]=%u (expected %u)\n",
                debug_count, EXPECTED_DRAWS);
    }

    /* Step 3: bind draw program, counter SSBO, draw. */
    glUseProgram(draw_prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    glBindBuffer(GL_PARAMETER_BUFFER, param_buf);

    /* Bind element buffer to VAO (need a VAO for element draws). */
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    drain_errors();
    glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void *)0,
                                     0, (GLsizei)MAX_DRAWS, 0);

    /* Step 4: wait + readback counter. */
    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t counter = 0;
    if (readback_ssbo(counter_buf, 0, sizeof(counter), &counter) != 0) {
        CHECK(name, 0, "readback_ssbo failed");
        goto cleanup;
    }

    CHECK(name, counter == EXPECTED_DRAWS,
          "expected counter=%u (4 draws on 1x1 FBO), got %u",
          EXPECTED_DRAWS, counter);

cleanup:
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &ibo);
    glDeleteBuffers(1, &cmd_buf);
    glDeleteBuffers(1, &param_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(draw_prog);
    glDeleteProgram(write_prog);
}

/* ------------------------------------------------------------------ */
/* Test B: GPU-written count=6, maxDrawCount=3 → clamp to 3 draws      */
/*                                                                   */
/* CS writes count=6 into GL_PARAMETER_BUFFER. maxDrawCount=3, so     */
/* only 3 draws should execute (clamped). Expected counter = 3.       */
/* ------------------------------------------------------------------ */

static void test_elements_count_clamp(void)
{
    const char *name = "elements_count_clamp_6_to_3";
    const uint32_t GPU_WRITTEN_COUNT = 6u;
    const uint32_t MAX_DRAWS = 3u;
    const uint32_t EXPECTED_DRAWS = 3u;

    GLuint draw_prog = link_program(kFullscreenVS, kCountFS);
    GLuint write_prog = link_program_compute(kWriteCountCS);
    if (!draw_prog || !write_prog) {
        CHECK(name, 0, "program link failed");
        if (draw_prog) glDeleteProgram(draw_prog);
        if (write_prog) glDeleteProgram(write_prog);
        return;
    }

    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLuint ibo;
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    uint16_t indices[3] = {0, 1, 2};
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    /* 6 commands (GPU will write count=6, but maxDrawCount=3). */
    GLuint cmd_buf;
    glGenBuffers(1, &cmd_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    struct DrawElementsCmd {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t  baseVertex;
        uint32_t baseInstance;
    } cmds[6];
    for (int i = 0; i < 6; i++) {
        cmds[i].count = 3;
        cmds[i].instanceCount = 1;
        cmds[i].firstIndex = 0;
        cmds[i].baseVertex = 0;
        cmds[i].baseInstance = 0;
    }
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmds), cmds, GL_STATIC_DRAW);

    GLuint param_buf;
    glGenBuffers(1, &param_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, param_buf);
    uint32_t zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), &zero, GL_DYNAMIC_COPY);

    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* GPU-write count=6. */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, param_buf);
    glUseProgram(write_prog);
    glUniform1ui(glGetUniformLocation(write_prog, "u_drawCount"), GPU_WRITTEN_COUNT);
    glDispatchCompute(1, 1, 1);

    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    /* DEBUG: verify GPU write */
    if (getenv("MGL_DEBUG_INDIRECT_COUNT_SHADOW")) {
        glFinish();
        uint32_t debug_count = 0;
        readback_ssbo(param_buf, 0, sizeof(debug_count), &debug_count);
        fprintf(stderr, "  [DEBUG-TEST-B] After kWriteCountCS: param_buf[0]=%u (expected %u)\n",
                debug_count, GPU_WRITTEN_COUNT);
    }

    /* Draw with maxDrawCount=3 — should clamp to 3. */
    glUseProgram(draw_prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    glBindBuffer(GL_PARAMETER_BUFFER, param_buf);

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    drain_errors();
    glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void *)0,
                                     0, (GLsizei)MAX_DRAWS, 0);

    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t counter = 0;
    if (readback_ssbo(counter_buf, 0, sizeof(counter), &counter) != 0) {
        CHECK(name, 0, "readback_ssbo failed");
        goto cleanup;
    }

    CHECK(name, counter == EXPECTED_DRAWS,
          "expected counter=%u (clamped to maxDrawCount=3), got %u",
          EXPECTED_DRAWS, counter);

cleanup:
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &ibo);
    glDeleteBuffers(1, &cmd_buf);
    glDeleteBuffers(1, &param_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(draw_prog);
    glDeleteProgram(write_prog);
}

/* ------------------------------------------------------------------ */
/* Test C: Same as A but with glMultiDrawArraysIndirectCount (no EBO)  */
/* ------------------------------------------------------------------ */

static void test_arrays_count_gpu_written(void)
{
    const char *name = "arrays_count_gpu_written_4_of_8";
    const uint32_t EXPECTED_DRAWS = 4u;
    const uint32_t MAX_DRAWS = 8u;

    GLuint draw_prog = link_program(kFullscreenVS, kCountFS);
    GLuint write_prog = link_program_compute(kWriteCountCS);
    if (!draw_prog || !write_prog) {
        CHECK(name, 0, "program link failed");
        if (draw_prog) glDeleteProgram(draw_prog);
        if (write_prog) glDeleteProgram(write_prog);
        return;
    }

    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    /* DrawArraysIndirectCommand: {count, primCount, first, baseInstance} */
    GLuint cmd_buf;
    glGenBuffers(1, &cmd_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    struct DrawArraysCmd {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t first;
        uint32_t baseInstance;
    } cmds[8];
    for (int i = 0; i < 8; i++) {
        cmds[i].count = 3;
        cmds[i].instanceCount = 1;
        cmds[i].first = 0;
        cmds[i].baseInstance = 0;
    }
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmds), cmds, GL_STATIC_DRAW);

    GLuint param_buf;
    glGenBuffers(1, &param_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, param_buf);
    uint32_t zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), &zero, GL_DYNAMIC_COPY);

    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* GPU-write count=4. */
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, param_buf);
    glUseProgram(write_prog);
    glUniform1ui(glGetUniformLocation(write_prog, "u_drawCount"), EXPECTED_DRAWS);
    glDispatchCompute(1, 1, 1);

    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    /* DEBUG: verify GPU write */
    if (getenv("MGL_DEBUG_INDIRECT_COUNT_SHADOW")) {
        glFinish();
        uint32_t debug_count = 0;
        readback_ssbo(param_buf, 0, sizeof(debug_count), &debug_count);
        fprintf(stderr, "  [DEBUG-TEST-C] After kWriteCountCS: param_buf[0]=%u (expected %u)\n",
                debug_count, EXPECTED_DRAWS);
    }

    glUseProgram(draw_prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    glBindBuffer(GL_PARAMETER_BUFFER, param_buf);

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    drain_errors();
    glMultiDrawArraysIndirectCount(GL_TRIANGLES, (const void *)0,
                                   0, (GLsizei)MAX_DRAWS, 0);

    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    uint32_t counter = 0;
    if (readback_ssbo(counter_buf, 0, sizeof(counter), &counter) != 0) {
        CHECK(name, 0, "readback_ssbo failed");
        goto cleanup;
    }

    CHECK(name, counter == EXPECTED_DRAWS,
          "expected counter=%u (4 array draws on 1x1 FBO), got %u",
          EXPECTED_DRAWS, counter);

cleanup:
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &cmd_buf);
    glDeleteBuffers(1, &param_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(draw_prog);
    glDeleteProgram(write_prog);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Enable the GPU path (compute kernel + native indirect draws, no CPU
     * readback) if available. Falls through to CPU fallback if the flag
     * is not supported or prerequisites fail. */
    setenv("MGL_ENABLE_INDIRECT_COUNT_GPU", "1", 1);

    fprintf(stderr,
            "=== MGL MultiDraw*IndirectCount GPU path test (Spec R6 / Task 6.2 + 6.3) ===\n");
    fprintf(stderr, "MGL_ENABLE_INDIRECT_COUNT_GPU=%s\n\n",
            getenv("MGL_ENABLE_INDIRECT_COUNT_GPU") ? "1" : "(unset)");

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

    fprintf(stderr, "[A] glMultiDrawElementsIndirectCount: GPU-written count=4, maxDrawCount=8\n");
    test_elements_count_gpu_written();
    drain_errors();

    fprintf(stderr, "\n[B] glMultiDrawElementsIndirectCount: count clamp 6→3\n");
    test_elements_count_clamp();
    drain_errors();

    fprintf(stderr, "\n[C] glMultiDrawArraysIndirectCount: GPU-written count=4, maxDrawCount=8\n");
    test_arrays_count_gpu_written();
    drain_errors();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
