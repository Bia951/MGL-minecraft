/*
 * test_regression/base_instance_test.c — Spec R7 / Task 7
 *
 * 16-command baseInstance acceptance test.
 *
 * Verifies that per-command baseInstance is correctly passed from indirect
 * draw commands through Metal's native draw APIs (drawIndexedPrimitives:
 * ...baseInstance: / drawPrimitives:...baseInstance:) and the ICB path
 * (issueIndirectCommandBufferBatch encodes per-command baseInstance) to the
 * vertex shader as gl_BaseInstance (Metal [[base_instance]], mapped by
 * SPIRV-Cross).
 *
 * Setup:
 *   - 16 DrawElementsIndirectCommands / DrawArraysIndirectCommands with
 *     baseInstance = 0..15.
 *   - SSBO with 16 uint atomic counters, all zero.
 *   - Vertex shader: full-screen triangle (gl_VertexID), passes
 *     gl_BaseInstance to fragment as flat varying.
 *   - Fragment shader: atomicAdd(ssbo.counters[v_baseInstance], 1u).
 *   - 1x1 FBO so each draw contributes exactly 1 fragment.
 *
 * After 16 draws, every counter[0..15] must equal 1.  If baseInstance is
 * ignored or globally emulated (e.g. always 0, the pre-Task-7 hack), all 16
 * increments land in counters[0] (=16) and counters[1..15] stay 0 — test
 * fails.  If baseInstance is wrong (e.g. always reads command[0]'s
 * baseInstance), the distribution is wrong.  If baseInstance exceeds 15,
 * the shader's bounds guard triggers an out-of-range counter write that the
 * test detects.
 *
 * This catches the Voxy failure modes:
 *   - LOD overlap (baseInstance ignored → all sections read slot 0)
 *   - SSBO out-of-bounds (baseInstance wrong → index > 15)
 *   - geometry stretch (baseInstance lost → wrong section metadata)
 *
 * Two sub-tests:
 *   A. glMultiDrawElementsIndirect (indexed) — exercises the ICB batch path
 *      (issueIndirectCommandBufferBatch) which encodes per-command
 *      baseInstance into MTLIndirectCommandBuffer.
 *   B. glMultiDrawArraysIndirect (arrays) — exercises the native
 *      drawPrimitives:...baseInstance: path.
 *
 * Build:  make test-base-instance
 * Run:    build/test_base_instance
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
 * Passes gl_BaseInstance to the fragment shader as a flat varying so the
 * fragment shader can index the counter SSBO by the per-command
 * baseInstance.  This is the key path under test: gl_BaseInstance must
 * reflect the baseInstance field of each individual indirect draw command. */
static const char *kBaseInstanceVS =
    "#version 460 core\n"
    "flat out uint v_baseInstance;\n"
    "void main() {\n"
    "    v_baseInstance = gl_BaseInstance;\n"
    "    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

/* Fragment shader: atomicAdd 1 to counter SSBO at index v_baseInstance.
 * A bounds guard writes to a sentinel slot (index 16) if baseInstance is
 * out of range [0, 15], which the test detects as a failure. */
static const char *kBaseInstanceFS =
    "#version 460 core\n"
    "flat in uint v_baseInstance;\n"
    "layout(std430, binding = 0) buffer Counters {\n"
    "    uint counters[17];\n"   /* [0..15] = per-baseInstance, [16] = OOB sentinel */
    "} ctr;\n"
    "void main() {\n"
    "    uint idx = (v_baseInstance < 16u) ? v_baseInstance : 16u;\n"
    "    atomicAdd(ctr.counters[idx], 1u);\n"
    "}\n";

#define NUM_COMMANDS   16u
#define OOB_SENTINEL   16u

/* ------------------------------------------------------------------ */
/* Test A: glMultiDrawElementsIndirect — 16 commands, baseInstance=0..15 */
/*                                                                    */
/* Exercises the ICB batch path (issueIndirectCommandBufferBatch)     */
/* which encodes per-command baseInstance into MTLIndirectCommandBuffer. */
/* ------------------------------------------------------------------ */

static void test_elements_base_instance(void)
{
    const char *name = "elements_baseinstance_16_commands";

    GLuint prog = link_program(kBaseInstanceVS, kBaseInstanceFS);
    if (!prog) {
        CHECK(name, 0, "program link failed");
        return;
    }

    /* 1x1 FBO so each draw contributes exactly 1 fragment. */
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

    /* Indirect command buffer: 16 DrawElementsIndirectCommand entries.
     * Each command has baseInstance = 0..15. */
    GLuint cmd_buf;
    glGenBuffers(1, &cmd_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    struct DrawElementsCmd {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t  baseVertex;
        uint32_t baseInstance;
    } cmds[NUM_COMMANDS];
    for (uint32_t i = 0; i < NUM_COMMANDS; i++) {
        cmds[i].count = 3;
        cmds[i].instanceCount = 1;
        cmds[i].firstIndex = 0;
        cmds[i].baseVertex = 0;
        cmds[i].baseInstance = i;   /* <-- the key: 0..15 */
    }
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmds), cmds, GL_STATIC_DRAW);

    /* Counter SSBO: 17 uints (16 counters + 1 OOB sentinel), zero-initialized. */
    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counters[NUM_COMMANDS + 1] = {0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counters), counters, GL_DYNAMIC_COPY);

    /* VAO with element buffer bound. */
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);

    /* Draw. */
    glUseProgram(prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);

    drain_errors();
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, (const void *)0,
                                (GLsizei)NUM_COMMANDS, 0);

    /* Wait + readback counters. */
    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    memset(counters, 0, sizeof(counters));
    if (readback_ssbo(counter_buf, 0, sizeof(counters), counters) != 0) {
        CHECK(name, 0, "readback_ssbo failed");
        goto cleanup;
    }

    /* Verify: each counter[0..15] == 1, sentinel == 0. */
    int all_correct = 1;
    for (uint32_t i = 0; i < NUM_COMMANDS; i++) {
        if (counters[i] != 1u) {
            all_correct = 0;
            fprintf(stderr,
                    "  [%s] counter[%u]=%u (expected 1) — baseInstance not propagated per-command\n",
                    name, i, counters[i]);
            break;
        }
    }
    if (counters[OOB_SENTINEL] != 0u) {
        all_correct = 0;
        fprintf(stderr,
                "  [%s] OOB sentinel=%u (expected 0) — baseInstance exceeded [0,15]\n",
                name, counters[OOB_SENTINEL]);
    }

    /* Diagnostic: if all increments landed in slot 0, baseInstance is being
     * ignored globally (the pre-Task-7 failure mode). */
    if (!all_correct && counters[0] == NUM_COMMANDS) {
        fprintf(stderr,
                "  [%s] DIAGNOSTIC: counter[0]=%u == 16 — baseInstance globally "
                "ignored (all commands used baseInstance=0)\n",
                name, counters[0]);
    }

    CHECK(name, all_correct,
          "baseInstance per-command propagation failed (elements/ICB path)");

cleanup:
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &ibo);
    glDeleteBuffers(1, &cmd_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
}

/* ------------------------------------------------------------------ */
/* Test B: glMultiDrawArraysIndirect — 16 commands, baseInstance=0..15 */
/*                                                                    */
/* Exercises the native drawPrimitives:...baseInstance: path.          */
/* ------------------------------------------------------------------ */

static void test_arrays_base_instance(void)
{
    const char *name = "arrays_baseinstance_16_commands";

    GLuint prog = link_program(kBaseInstanceVS, kBaseInstanceFS);
    if (!prog) {
        CHECK(name, 0, "program link failed");
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
    } cmds[NUM_COMMANDS];
    for (uint32_t i = 0; i < NUM_COMMANDS; i++) {
        cmds[i].count = 3;
        cmds[i].instanceCount = 1;
        cmds[i].first = 0;
        cmds[i].baseInstance = i;   /* <-- the key: 0..15 */
    }
    glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmds), cmds, GL_STATIC_DRAW);

    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counters[NUM_COMMANDS + 1] = {0};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counters), counters, GL_DYNAMIC_COPY);

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glUseProgram(prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, counter_buf);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);

    drain_errors();
    glMultiDrawArraysIndirect(GL_TRIANGLES, (const void *)0,
                              (GLsizei)NUM_COMMANDS, 0);

    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    memset(counters, 0, sizeof(counters));
    if (readback_ssbo(counter_buf, 0, sizeof(counters), counters) != 0) {
        CHECK(name, 0, "readback_ssbo failed");
        goto cleanup;
    }

    int all_correct = 1;
    for (uint32_t i = 0; i < NUM_COMMANDS; i++) {
        if (counters[i] != 1u) {
            all_correct = 0;
            fprintf(stderr,
                    "  [%s] counter[%u]=%u (expected 1) — baseInstance not propagated per-command\n",
                    name, i, counters[i]);
            break;
        }
    }
    if (counters[OOB_SENTINEL] != 0u) {
        all_correct = 0;
        fprintf(stderr,
                "  [%s] OOB sentinel=%u (expected 0) — baseInstance exceeded [0,15]\n",
                name, counters[OOB_SENTINEL]);
    }

    if (!all_correct && counters[0] == NUM_COMMANDS) {
        fprintf(stderr,
                "  [%s] DIAGNOSTIC: counter[0]=%u == 16 — baseInstance globally "
                "ignored (all commands used baseInstance=0)\n",
                name, counters[0]);
    }

    CHECK(name, all_correct,
          "baseInstance per-command propagation failed (arrays/native path)");

cleanup:
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &cmd_buf);
    glDeleteBuffers(1, &counter_buf);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fprintf(stderr,
            "=== MGL 16-command baseInstance acceptance test (Spec R7 / Task 7) ===\n");
    fprintf(stderr,
            "Verifies per-command baseInstance → gl_BaseInstance (Metal [[base_instance]])\n\n");

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

    fprintf(stderr, "[A] glMultiDrawElementsIndirect: 16 commands, baseInstance=0..15 (ICB path)\n");
    test_elements_base_instance();
    drain_errors();

    fprintf(stderr, "\n[B] glMultiDrawArraysIndirect: 16 commands, baseInstance=0..15 (native path)\n");
    test_arrays_base_instance();
    drain_errors();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
