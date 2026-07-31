/*
 * test_regression/persistent_map_test.c — Spec R10 / Task 10
 *
 * Persistent buffer mapping acceptance test.
 *
 * Verifies that glNamedBufferStorage + GL_MAP_PERSISTENT_BIT creates a
 * single shared backing store (MTLStorageModeShared on Apple Silicon) so
 * that CPU writes via the persistent mapping are directly visible to the
 * GPU and GPU writes are directly visible to the CPU — without the
 * CPU malloc shadow + separate MTLBuffer double backing that required
 * manual copies on every GPU/CPU hand-off.
 *
 * Two sub-tests:
 *   A. Upload path (CPU write -> flush -> GPU read):
 *      - glNamedBufferStorage(GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT |
 *        GL_MAP_FLUSH_EXPLICIT_BIT)
 *      - glMapNamedBufferRange(same flags)
 *      - CPU writes a known pattern via the persistent pointer.
 *      - glFlushMappedNamedBufferRange.
 *      - Compute shader reads from the SSBO and atomicAdd's a counter.
 *      - Counter must reflect the CPU-written values, proving the GPU
 *        sees CPU writes through the shared backing (not a stale copy).
 *
 *   B. Download path (GPU write -> barrier -> fence -> CPU read):
 *      - glNamedBufferStorage(GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT)
 *      - glMapNamedBufferRange(same flags)
 *      - Compute shader writes a known pattern into the SSBO.
 *      - glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT) (Task 4
 *        hazard model: commit+wait for CPU visibility).
 *      - glFenceSync + glClientWaitSync ensures GPU completion.
 *      - CPU reads the persistent mapping and verifies the GPU-written
 *        values, proving the CPU sees GPU writes through the shared
 *        backing (not a stale shadow).
 *
 * If the implementation maintained a separate CPU shadow + MTLBuffer
 * (the pre-Task-10 double backing), sub-test A would fail because GPU
 * reads the stale MTLBuffer copy (CPU writes landed in the shadow), and
 * sub-test B would fail because CPU reads the stale shadow (GPU writes
 * landed in the MTLBuffer).
 *
 * Build:  make test-persistent-map
 * Run:    build/test_persistent_map
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

/* Wait for a fence sync to signal. Returns 0 on success, nonzero on
 * timeout or error. */
static int wait_fence(GLsync sync)
{
    if (!sync) return 1;
    GLenum result = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
        return 0;
    }
    fprintf(stderr, "  [wait_fence] glClientWaitSync returned 0x%x (timeout or error)\n", result);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Shaders                                                             */
/* ------------------------------------------------------------------ */

/* Compute shader that reads from the SSBO at binding 0 and atomicAdd's
 * each element to a counter at binding 1.  Used by sub-test A to verify
 * the GPU sees CPU-written values through the persistent mapping. */
static const char *kReadAndCountCS =
    "#version 460 core\n"
    "layout(local_size_x = 4) in;\n"
    "layout(std430, binding = 0) readonly buffer InputData {\n"
    "    uint values[];\n"
    "} in_data;\n"
    "layout(std430, binding = 1) buffer Counter {\n"
    "    uint total;\n"
    "} counter;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    if (idx < 8u) {\n"
    "        atomicAdd(counter.total, in_data.values[idx]);\n"
    "    }\n"
    "}\n";

/* Compute shader that writes a known pattern into the SSBO at binding 0.
 * Used by sub-test B to verify the CPU sees GPU-written values through
 * the persistent mapping. */
static const char *kWritePatternCS =
    "#version 460 core\n"
    "layout(local_size_x = 4) in;\n"
    "layout(std430, binding = 0) writeonly buffer OutputData {\n"
    "    uint values[];\n"
    "} out_data;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    if (idx < 8u) {\n"
    "        out_data.values[idx] = 100u + idx * 10u;\n"
    "    }\n"
    "}\n";

#define NUM_VALUES 8u

/* ------------------------------------------------------------------ */
/* Test A: CPU write -> flush -> GPU read (upload path)               */
/*                                                                    */
/* Creates a persistent write mapping, CPU writes a known pattern,    */
/* flushes, then a compute shader reads the values and sums them.     */
/* The sum must match the CPU-written values, proving the GPU reads   */
/* from the same shared backing store (not a stale copy).            */
/* ------------------------------------------------------------------ */

static void test_persistent_upload(void)
{
    const char *name = "persistent_map_upload";
    GLuint prog = link_program_compute(kReadAndCountCS);
    if (!prog) {
        CHECK(name, 0, "program link failed");
        return;
    }

    /* Persistent write storage.  GL_MAP_FLUSH_EXPLICIT_BIT is a map-only
     * flag (not valid for glNamedBufferStorage), so storage uses just
     * PERSISTENT | WRITE and the map adds FLUSH_EXPLICIT. */
    GLuint data_buf;
    glGenBuffers(1, &data_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, data_buf);
    GLbitfield storage_flags = GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT;
    glNamedBufferStorage(data_buf, NUM_VALUES * sizeof(uint32_t), NULL, storage_flags);
    drain_errors();

    /* Persistent write map with explicit flush. */
    GLbitfield map_flags = GL_MAP_PERSISTENT_BIT | GL_MAP_WRITE_BIT |
                           GL_MAP_FLUSH_EXPLICIT_BIT;
    uint32_t *mapped = (uint32_t *)glMapNamedBufferRange(
        data_buf, 0, NUM_VALUES * sizeof(uint32_t), map_flags);
    if (!mapped) {
        CHECK(name, 0, "glMapNamedBufferRange failed for persistent write");
        glDeleteProgram(prog);
        glDeleteBuffers(1, &data_buf);
        return;
    }

    /* CPU writes a known pattern: 1, 2, 3, ..., 8.  Sum = 36. */
    for (uint32_t i = 0; i < NUM_VALUES; i++) {
        mapped[i] = i + 1u;
    }

    /* Flush the entire mapped range so GPU can see the writes. */
    glFlushMappedNamedBufferRange(data_buf, 0, NUM_VALUES * sizeof(uint32_t));
    drain_errors();

    /* Counter buffer, zero-initialized. */
    GLuint counter_buf;
    glGenBuffers(1, &counter_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    uint32_t counter_zero = 0;
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(counter_zero), &counter_zero, GL_DYNAMIC_COPY);

    /* Dispatch: 2 threadgroups of 4 = 8 invocations, one per value. */
    glUseProgram(prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, data_buf);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, counter_buf);
    drain_errors();
    glDispatchCompute(2, 1, 1);

    /* Wait + readback counter. */
    glFinish();
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, counter_buf);
    void *counter_mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, sizeof(uint32_t),
                                             GL_MAP_READ_BIT);
    if (!counter_mapped) {
        CHECK(name, 0, "glMapBufferRange failed for counter readback");
        glUnmapNamedBuffer(data_buf);
        glDeleteProgram(prog);
        glDeleteBuffers(1, &data_buf);
        glDeleteBuffers(1, &counter_buf);
        return;
    }
    uint32_t counter = *(const uint32_t *)counter_mapped;
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    /* Expected: 1+2+3+4+5+6+7+8 = 36. */
    uint32_t expected = 36u;
    CHECK(name, counter == expected,
          "expected counter=%u (sum 1..8), got %u — GPU did not see CPU-written persistent data",
          expected, counter);

    /* Unmap the persistent mapping.  Persistent maps don't strictly need
     * unmap, but it's good hygiene for test cleanup. */
    glUnmapNamedBuffer(data_buf);

    glDeleteProgram(prog);
    glDeleteBuffers(1, &data_buf);
    glDeleteBuffers(1, &counter_buf);
}

/* ------------------------------------------------------------------ */
/* Test B: GPU write -> barrier -> fence -> CPU read (download path)  */
/*                                                                    */
/* Creates a persistent read mapping, GPU writes a known pattern,     */
/* barrier + fence ensures completion, then CPU reads the persistent  */
/* mapping.  The values must match the GPU-written pattern, proving   */
/* the CPU reads from the same shared backing store (not a stale      */
/* shadow).                                                           */
/* ------------------------------------------------------------------ */

static void test_persistent_download(void)
{
    const char *name = "persistent_map_download";
    GLuint prog = link_program_compute(kWritePatternCS);
    if (!prog) {
        CHECK(name, 0, "program link failed");
        return;
    }

    /* Persistent read storage.  No FLUSH_EXPLICIT since CPU only reads. */
    GLuint data_buf;
    glGenBuffers(1, &data_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, data_buf);
    GLbitfield storage_flags = GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT;
    /* Zero-initialize so we can detect if GPU writes actually landed. */
    uint32_t zeros[NUM_VALUES] = {0};
    glNamedBufferStorage(data_buf, NUM_VALUES * sizeof(uint32_t), zeros, storage_flags);
    drain_errors();

    /* Persistent read map. */
    GLbitfield map_flags = GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT;
    uint32_t *mapped = (uint32_t *)glMapNamedBufferRange(
        data_buf, 0, NUM_VALUES * sizeof(uint32_t), map_flags);
    if (!mapped) {
        CHECK(name, 0, "glMapNamedBufferRange failed for persistent read");
        glDeleteProgram(prog);
        glDeleteBuffers(1, &data_buf);
        return;
    }

    /* Verify initial zeros (sanity check the mapping). */
    int initial_ok = 1;
    for (uint32_t i = 0; i < NUM_VALUES; i++) {
        if (mapped[i] != 0u) {
            initial_ok = 0;
            break;
        }
    }
    if (!initial_ok) {
        fprintf(stderr, "  [%s] WARNING: persistent mapping not zero-initialized\n", name);
    }

    /* GPU writes the pattern: 100, 110, 120, ..., 170. */
    glUseProgram(prog);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, data_buf);
    drain_errors();
    glDispatchCompute(2, 1, 1);

    /* Task 4 hazard model: GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT triggers
     * commit+wait so CPU-visible GPU writes are flushed to the shared
     * backing.  On Apple Silicon shared storage the writes are already
     * coherent, but the barrier + fence ensures GPU completion. */
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT |
                    GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT);

    /* Fence sync to guarantee GPU completion before CPU reads. */
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (wait_fence(fence) != 0) {
        CHECK(name, 0, "fence wait failed");
        glDeleteSync(fence);
        glUnmapNamedBuffer(data_buf);
        glDeleteProgram(prog);
        glDeleteBuffers(1, &data_buf);
        return;
    }
    glDeleteSync(fence);

    /* CPU reads the persistent mapping.  No re-map needed — the pointer
     * is still valid and now reflects GPU-written data. */
    int all_correct = 1;
    for (uint32_t i = 0; i < NUM_VALUES; i++) {
        uint32_t expected = 100u + i * 10u;
        if (mapped[i] != expected) {
            all_correct = 0;
            fprintf(stderr,
                    "  [%s] mapped[%u]=%u (expected %u) — CPU did not see GPU-written persistent data\n",
                    name, i, mapped[i], expected);
        }
    }

    CHECK(name, all_correct,
          "persistent read mapping did not reflect GPU-written data (stale shadow?)");

    glUnmapNamedBuffer(data_buf);

    glDeleteProgram(prog);
    glDeleteBuffers(1, &data_buf);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fprintf(stderr,
            "=== MGL persistent buffer mapping test (Spec R10 / Task 10) ===\n");
    fprintf(stderr,
            "Verifies glNamedBufferStorage + GL_MAP_PERSISTENT_BIT shares a single backing store\n");
    fprintf(stderr,
            "between CPU and GPU (MTLStorageModeShared on Apple Silicon).\n\n");

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

    fprintf(stderr, "[A] CPU write -> flush -> GPU read (upload path)\n");
    test_persistent_upload();
    drain_errors();

    fprintf(stderr, "\n[B] GPU write -> barrier -> fence -> CPU read (download path)\n");
    test_persistent_download();
    drain_errors();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
