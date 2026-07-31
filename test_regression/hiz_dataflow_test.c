/*
 * test_regression/hiz_dataflow_test.c — Spec R14 / Task 14
 *
 * Voxy Hi-Z 数据流复刻测试集（headless, non-interactive, 数值断言）。
 *
 * 复刻 Voxy Hi-Z 管线的 8 个关键可见性环节：
 *   1. 深度 texture sampling        — FBO 渲染深度 → sample 深度值
 *   2. textureGather                — 4 邻接 texel 采集
 *   3. R32F imageStore              — compute imageStore 写 R32F → 读回
 *   4. FBO mip level attachment     — 指定 mip 挂为颜色附件，只写该 mip
 *   5. render target → sampler 转换 — FBO 渲染到纹理 → 该纹理被采样（barrier）
 *   6. resize 后 texture/FBO 重建    — 重建不崩溃，新尺寸正确
 *   7. Y 轴方向                     — 渲染已知上下图案，验证 Y 轴与 OpenGL 一致
 *   8. reversed depth               — glDepthFunc(GL_GREATER) + clear 0.0
 *
 * Build:  make test-hiz
 * Run:    build/test_hiz_dataflow
 *
 * Exit code: 0 if all PASS, 1 if any FAIL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

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
        char log[4096];
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
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "  [program link FAIL] %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint link_program_vs_fs(const char *vs_src, const char *fs_src)
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
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "  [program link FAIL] %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint make_vbo(const void *data, size_t sz)
{
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sz, data, GL_STATIC_DRAW);
    return b;
}

/* Create an FBO with RGBA8 color texture + depth renderbuffer. */
static GLuint make_fbo(int w, int h, GLuint *out_tex)
{
    GLuint fbo, tex, rbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "  [FBO incomplete: 0x%x]\n", st);
        return 0;
    }
    if (out_tex) *out_tex = tex;
    return fbo;
}

/* Create an FBO with RGBA8 color texture + depth TEXTURE (for depth sampling). */
static GLuint make_fbo_depth_tex(int w, int h, GLuint *out_color, GLuint *out_depth)
{
    GLuint fbo, tex, dtex;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    glGenTextures(1, &dtex);
    glBindTexture(GL_TEXTURE_2D, dtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dtex, 0);

    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "  [depth-tex FBO incomplete: 0x%x]\n", st);
        return 0;
    }
    if (out_color) *out_color = tex;
    if (out_depth) *out_depth = dtex;
    return fbo;
}

static void drain_errors(void)
{
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {
        /* drain */
    }
}

/* Fullscreen quad (two triangles) with texcoords. */
static const float QUAD_POS[] = {
    -1.0f, -1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,
     1.0f,  1.0f, 0.0f,
};
static const float QUAD_TEX[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    0.0f, 1.0f,
    0.0f, 1.0f,
    1.0f, 0.0f,
    1.0f, 1.0f,
};

static GLuint make_quad_vao(void)
{
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint vbo_p = make_vbo(QUAD_POS, sizeof(QUAD_POS));
    GLuint vbo_t = make_vbo(QUAD_TEX, sizeof(QUAD_TEX));
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_p);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_t);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
    /* VBOs stay bound to the VAO's element buffer state; they will be
     * cleaned up when the test deletes them explicitly.  Return vao; the
     * caller owns vao + the two vbos (not tracked individually here for
     * simplicity — the context is destroyed at process end). */
    return vao;
}

/* ------------------------------------------------------------------ */
/* Shared shaders                                                     */
/* ------------------------------------------------------------------ */

/* Vertex shader: fullscreen quad with texcoords. */
static const char *VS_QUAD_TEX =
    "#version 460 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 1.0); }\n";

/* Vertex shader: fullscreen quad with texcoords + depth uniform. */
static const char *VS_QUAD_TEX_DEPTH =
    "#version 460 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "uniform float u_depth;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos.xy, u_depth, 1.0); }\n";

/* ================================================================== */
/* Test 1: 深度 texture sampling                                      */
/*                                                                    */
/* FBO 渲染一个已知深度（z=0.5）的 fullscreen quad 到深度纹理，再用   */
/* 该深度纹理作为 sampler2D 采样，验证采到的深度值 ≈ 0.5。           */
/* ================================================================== */

static const char *FS_SAMPLE_DEPTH =
    "#version 460 core\n"
    "in vec2 v_uv;\n"
    "layout(location = 0) out vec4 frag;\n"
    "uniform sampler2D u_depth_tex;\n"
    "void main() {\n"
    "    float d = texture(u_depth_tex, v_uv).r;\n"
    "    frag = vec4(d, d, d, 1.0);\n"
    "}\n";

static void test_depth_texture_sampling(void)
{
    const char *name = "depth_texture_sampling";
    const int W = 64, H = 64;

    GLuint color_tex, depth_tex;
    GLuint fbo = make_fbo_depth_tex(W, H, &color_tex, &depth_tex);
    if (!fbo) { CHECK(name, 0, "FBO creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Render a fullscreen quad at depth 0.5. */
    GLuint prog_fill = link_program_vs_fs(VS_QUAD_TEX_DEPTH,
        "#version 460 core\n"
        "layout(location = 0) out vec4 frag;\n"
        "void main() { frag = vec4(1.0, 1.0, 1.0, 1.0); }\n");
    if (!prog_fill) { CHECK(name, 0, "fill program link failed"); return; }
    glUseProgram(prog_fill);
    /* NDC z=0.0 maps to depth-buffer value 0.5 under the default depth
     * range (0,1):  depth = (z_ndc + 1) / 2 = 0.5. */
    glUniform1f(glGetUniformLocation(prog_fill, "u_depth"), 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    GLuint vao = make_quad_vao();
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_DEPTH_TEST);
    glFinish();

    /* Diagnostic: read back the depth texture directly via glGetTexImage to
     * determine whether depth was actually written (0.5) or not (1.0 clear).
     * This distinguishes a depth-write bug from a depth-sampling bug. */
    float depth_direct[W * H];
    memset(depth_direct, 0, sizeof(depth_direct));
    glBindTexture(GL_TEXTURE_2D, depth_tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth_direct);
    float depth_center = depth_direct[(H / 2) * W + (W / 2)];

    /* Barrier so the depth texture is visible to subsequent texture fetch. */
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

    /* Now sample the depth texture in a second pass. */
    GLuint result_tex;
    GLuint fbo2 = make_fbo(W, H, &result_tex);
    if (!fbo2) { CHECK(name, 0, "result FBO creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint prog_sample = link_program_vs_fs(VS_QUAD_TEX, FS_SAMPLE_DEPTH);
    if (!prog_sample) { CHECK(name, 0, "sample program link failed"); return; }
    glUseProgram(prog_sample);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth_tex);
    glUniform1i(glGetUniformLocation(prog_sample, "u_depth_tex"), 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    /* Read back center pixel.  The depth was 0.5, so the sampled value
     * encoded as RGBA8 (0.5*255 ≈ 128) should appear in all channels. */
    uint8_t pixel[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

    /* Allow ±2 for unorm rounding.  Include the direct depth-texture readback
     * value in the failure message to distinguish a depth-write bug (direct
     * readback wrong) from a depth-sampling bug (direct readback correct but
     * shader sample wrong). */
    int ok = (pixel[0] >= 126 && pixel[0] <= 130) &&
             (pixel[1] >= 126 && pixel[1] <= 130) &&
             (pixel[2] >= 126 && pixel[2] <= 130);
    CHECK(name, ok,
          "expected depth ≈ 0.5 (≈128), got r=%d g=%d b=%d "
          "(direct depth tex readback center=%f, clear was 1.0)",
          pixel[0], pixel[1], pixel[2], depth_center);

    glDeleteProgram(prog_fill);
    glDeleteProgram(prog_sample);
    glDeleteFramebuffers(1, &fbo);
    glDeleteFramebuffers(1, &fbo2);
    glDeleteTextures(1, &color_tex);
    glDeleteTextures(1, &depth_tex);
    glDeleteTextures(1, &result_tex);
}

/* ================================================================== */
/* Test 2: textureGather                                              */
/*                                                                    */
/* Create a 2×2 R32F texture with 4 distinct values, use              */
/* textureGather to collect the 4 texels, write them to a 1×1 RGBA   */
/* output, and verify all 4 gathered values are present.              */
/* ================================================================== */

static const char *FS_GATHER =
    "#version 460 core\n"
    "in vec2 v_uv;\n"
    "layout(location = 0) out vec4 frag;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    vec4 g = textureGather(u_tex, v_uv, 0);\n"
    "    frag = vec4(g.x, g.y, g.z, g.w);\n"
    "}\n";

static void test_texture_gather(void)
{
    const char *name = "texture_gather";
    const int W = 64, H = 64;

    /* 2x2 R32F texture with 4 distinct values. */
    float tex_data[4] = { 0.10f, 0.20f, 0.30f, 0.40f };
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 2, 2, 0, GL_RED, GL_FLOAT, tex_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint result_tex;
    GLuint fbo = make_fbo(W, H, &result_tex);
    if (!fbo) { CHECK(name, 0, "FBO creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint prog = link_program_vs_fs(VS_QUAD_TEX, FS_GATHER);
    if (!prog) { CHECK(name, 0, "gather program link failed"); return; }
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    /* make_quad_vao binds the VAO as a side effect; the returned id is not
     * needed separately because the VAO stays bound until replaced. */
    (void)make_quad_vao();
    /* Sample at the center of the 2x2 texture so all 4 texels are gathered. */
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    /* Read back center pixel as float (FBO is RGBA8, so values are unorm). */
    uint8_t pixel[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

    /* Expected unorm values: 0.10→26, 0.20→51, 0.30→76, 0.40→102.
     * textureGather returns 4 components; verify all 4 distinct values
     * are present (in any order — gather component order is defined by
     * the spec but we just check the set matches). */
    int expected[4] = { 26, 51, 76, 102 };
    int got[4] = { pixel[0], pixel[1], pixel[2], pixel[3] };

    /* Check each expected value is present among the 4 gathered. */
    int all_present = 1;
    for (int i = 0; i < 4; i++) {
        int found = 0;
        for (int j = 0; j < 4; j++) {
            if (abs(got[j] - expected[i]) <= 2) { found = 1; break; }
        }
        if (!found) { all_present = 0; break; }
    }
    CHECK(name, all_present,
          "expected gather set {26,51,76,102}, got {%d,%d,%d,%d}",
          got[0], got[1], got[2], got[3]);

    glDeleteProgram(prog);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteTextures(1, &result_tex);
}

/* ================================================================== */
/* Test 3: R32F imageStore                                            */
/*                                                                    */
/* Create an R32F texture, compute imageStore writes a known float    */
/* (3.14) to every texel, then read back via glGetTexImage and       */
/* verify.                                                            */
/* ================================================================== */

static void test_r32f_image_store(void)
{
    const char *name = "r32f_image_store";
    const int W = 8, H = 8;

    const char *cs_src =
        "#version 460 core\n"
        "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n"
        "layout(r32f, binding = 0) writeonly uniform image2D u_img;\n"
        "void main() {\n"
        "    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);\n"
        "    imageStore(u_img, coord, vec4(3.14, 0.0, 0.0, 0.0));\n"
        "}\n";

    GLuint prog = link_program_compute(cs_src);
    if (!prog) { CHECK(name, 0, "compute program link failed"); return; }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* Initialize to zero. */
    float zero_data[W * H];
    memset(zero_data, 0, sizeof(zero_data));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, W, H, 0, GL_RED, GL_FLOAT, zero_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindImageTexture(0, tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);

    glUseProgram(prog);
    glDispatchCompute(1, 1, 1);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    /* Read back via glGetTexImage. */
    float readback[W * H];
    memset(readback, 0, sizeof(readback));
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, readback);

    int ok = 1;
    int bad = -1;
    for (int i = 0; i < W * H; i++) {
        if (fabsf(readback[i] - 3.14f) > 0.001f) { ok = 0; bad = i; break; }
    }
    CHECK(name, ok, ok ? "ok" : "texel %d = %f (expected 3.14)", bad, readback[bad]);

    glDeleteProgram(prog);
    glDeleteTextures(1, &tex);
}

/* ================================================================== */
/* Test 4: FBO mip level attachment                                   */
/*                                                                    */
/* Create a multi-mip RGBA8 texture.  Fill mip 0 with red, mip 1     */
/* with green via glTexSubImage2D.  Then attach mip 1 as an FBO       */
/* color attachment and render blue to it.  Verify mip 0 is still    */
/* red (unchanged) and mip 1 is now blue (overwritten).               */
/* ================================================================== */

static void test_fbo_mip_attachment(void)
{
    const char *name = "fbo_mip_attachment";
    const int baseW = 16, baseH = 16;
    const int levels = 3;  /* mip 0 (16), mip 1 (8), mip 2 (4) */

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, baseW, baseH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Fill mip 0 with red. */
    uint8_t red[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; i++) {
        red[i * 4 + 0] = 255; red[i * 4 + 1] = 0;
        red[i * 4 + 2] = 0;   red[i * 4 + 3] = 255;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, red);

    /* Fill mip 1 with green. */
    uint8_t green[8 * 8 * 4];
    for (int i = 0; i < 8 * 8; i++) {
        green[i * 4 + 0] = 0;   green[i * 4 + 1] = 255;
        green[i * 4 + 2] = 0;   green[i * 4 + 3] = 255;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, green);

    /* Attach mip 1 to an FBO and render blue to it. */
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 1);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        CHECK(name, 0, "mip-level FBO incomplete");
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        return;
    }

    glViewport(0, 0, 8, 8);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);  /* blue */
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    /* Read back mip 0 — should still be red. */
    uint8_t mip0[16 * 16 * 4];
    memset(mip0, 0, sizeof(mip0));
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, mip0);

    /* Read back mip 1 — should be blue (overwritten). */
    uint8_t mip1[8 * 8 * 4];
    memset(mip1, 0, sizeof(mip1));
    glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, mip1);

    int mip0_ok = (mip0[0] == 255 && mip0[1] == 0 && mip0[2] == 0);
    int mip1_ok = (mip1[0] == 0 && mip1[1] == 0 && mip1[2] == 255);

    CHECK(name, mip0_ok && mip1_ok,
          "mip0=(%d,%d,%d) %s; mip1=(%d,%d,%d) %s",
          mip0[0], mip0[1], mip0[2], mip0_ok ? "OK(red)" : "WRONG",
          mip1[0], mip1[1], mip1[2], mip1_ok ? "OK(blue)" : "WRONG");

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

/* ================================================================== */
/* Test 5: render target → sampler conversion                         */
/*                                                                    */
/* FBO A renders a known pattern (left=red, right=blue) to texture T. */
/* glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT).  FBO B samples T    */
/* and renders it fullscreen.  Verify left=red, right=blue.           */
/* ================================================================== */

static void test_render_target_to_sampler(void)
{
    const char *name = "render_target_to_sampler";
    const int W = 64, H = 64;

    /* Pass 1: render left=red / right=blue to texture T. */
    GLuint texT;
    GLuint fboA = make_fbo(W, H, &texT);
    if (!fboA) { CHECK(name, 0, "FBO A creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fboA);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint prog_split = link_program_vs_fs(VS_QUAD_TEX,
        "#version 460 core\n"
        "in vec2 v_uv;\n"
        "layout(location = 0) out vec4 frag;\n"
        "void main() {\n"
        "    if (v_uv.x < 0.5) frag = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "    else              frag = vec4(0.0, 0.0, 1.0, 1.0);\n"
        "}\n");
    if (!prog_split) { CHECK(name, 0, "split program link failed"); return; }
    glUseProgram(prog_split);
    GLuint vao = make_quad_vao();
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    /* Barrier: render target writes to T must be visible to texture fetch. */
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

    /* Pass 2: FBO B samples T and draws it fullscreen. */
    GLuint texResult;
    GLuint fboB = make_fbo(W, H, &texResult);
    if (!fboB) { CHECK(name, 0, "FBO B creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fboB);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint prog_sample = link_program_vs_fs(VS_QUAD_TEX,
        "#version 460 core\n"
        "in vec2 v_uv;\n"
        "layout(location = 0) out vec4 frag;\n"
        "uniform sampler2D u_tex;\n"
        "void main() { frag = texture(u_tex, v_uv); }\n");
    if (!prog_sample) { CHECK(name, 0, "sample program link failed"); return; }
    glUseProgram(prog_sample);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texT);
    glUniform1i(glGetUniformLocation(prog_sample, "u_tex"), 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    /* Read back: left quarter should be red, right quarter blue. */
    uint8_t left[4] = {0, 0, 0, 0}, right[4] = {0, 0, 0, 0};
    glReadPixels(W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
    glReadPixels(3 * W / 4, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);

    int left_ok = (left[0] == 255 && left[1] == 0 && left[2] == 0);
    int right_ok = (right[0] == 0 && right[1] == 0 && right[2] == 255);
    CHECK(name, left_ok && right_ok,
          "left=(%d,%d,%d) %s; right=(%d,%d,%d) %s",
          left[0], left[1], left[2], left_ok ? "OK(red)" : "WRONG",
          right[0], right[1], right[2], right_ok ? "OK(blue)" : "WRONG");

    glDeleteProgram(prog_split);
    glDeleteProgram(prog_sample);
    glDeleteFramebuffers(1, &fboA);
    glDeleteFramebuffers(1, &fboB);
    glDeleteTextures(1, &texT);
    glDeleteTextures(1, &texResult);
}

/* ================================================================== */
/* Test 6: resize 后 texture/FBO 重建                                 */
/*                                                                    */
/* Create texture+FBO at 32×32, render, read back.  Delete, create   */
/* new at 128×128, render, read back.  Verify no crash and new       */
/* dimensions are correct.                                            */
/* ================================================================== */

static void test_resize_rebuild(void)
{
    const char *name = "resize_rebuild";

    /* Phase 1: 32x32. */
    const int W1 = 32, H1 = 32;
    GLuint tex1;
    GLuint fbo1 = make_fbo(W1, H1, &tex1);
    if (!fbo1) { CHECK(name, 0, "FBO 32x32 creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo1);
    glViewport(0, 0, W1, H1);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    uint8_t p1[4] = {0, 0, 0, 0};
    glReadPixels(W1 / 2, H1 / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p1);
    int phase1_ok = (p1[0] == 255 && p1[1] == 0 && p1[2] == 0);

    /* Tear down phase 1. */
    glDeleteFramebuffers(1, &fbo1);
    glDeleteTextures(1, &tex1);

    /* Phase 2: 128x128 — rebuild at new size. */
    const int W2 = 128, H2 = 128;
    GLuint tex2;
    GLuint fbo2 = make_fbo(W2, H2, &tex2);
    if (!fbo2) { CHECK(name, 0, "FBO 128x128 creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
    glViewport(0, 0, W2, H2);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    uint8_t p2[4] = {0, 0, 0, 0};
    glReadPixels(W2 / 2, H2 / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p2);
    int phase2_ok = (p2[0] == 0 && p2[1] == 0 && p2[2] == 255);

    /* Verify the new FBO dimensions via querying the texture level. */
    int tex_w = 0, tex_h = 0;
    glBindTexture(GL_TEXTURE_2D, tex2);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_h);
    int dims_ok = (tex_w == W2 && tex_h == H2);

    CHECK(name, phase1_ok && phase2_ok && dims_ok,
          "phase1=%s (p1=%d,%d,%d) phase2=%s (p2=%d,%d,%d) dims=%dx%d (exp %dx%d)",
          phase1_ok ? "OK" : "FAIL", p1[0], p1[1], p1[2],
          phase2_ok ? "OK" : "FAIL", p2[0], p2[1], p2[2],
          tex_w, tex_h, W2, H2);

    glDeleteFramebuffers(1, &fbo2);
    glDeleteTextures(1, &tex2);
}

/* ================================================================== */
/* Test 7: Y 轴方向                                                   */
/*                                                                    */
/* Render top half (NDC y > 0) red, bottom half (NDC y < 0) green.    */
/* In OpenGL, glReadPixels origin is bottom-left, so row 0 is bottom. */
/* Verify: bottom row (y=0) = green, top row (y=H-1) = red — proves   */
/* Y axis is NOT flipped (OpenGL convention).                         */
/* ================================================================== */

static void test_y_axis_direction(void)
{
    const char *name = "y_axis_direction";
    const int W = 64, H = 64;

    GLuint tex;
    GLuint fbo = make_fbo(W, H, &tex);
    if (!fbo) { CHECK(name, 0, "FBO creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint prog = link_program_vs_fs(VS_QUAD_TEX,
        "#version 460 core\n"
        "in vec2 v_uv;\n"
        "layout(location = 0) out vec4 frag;\n"
        "void main() {\n"
        "    /* v_uv.y = 0 at bottom (NDC y=-1), 1 at top (NDC y=+1) */\n"
        "    if (v_uv.y > 0.5) frag = vec4(1.0, 0.0, 0.0, 1.0);\n"  /* top = red */
        "    else              frag = vec4(0.0, 1.0, 0.0, 1.0);\n"  /* bottom = green */
        "}\n");
    if (!prog) { CHECK(name, 0, "program link failed"); return; }
    glUseProgram(prog);
    (void)make_quad_vao();
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    /* OpenGL glReadPixels: origin is bottom-left.
     * Row 0 (y=0)        = bottom of image = should be green.
     * Row H-1 (y=H-1)    = top of image    = should be red. */
    uint8_t bottom[4] = {0, 0, 0, 0}, top[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom);
    glReadPixels(W / 2, H - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);

    int bottom_ok = (bottom[0] == 0 && bottom[1] == 255 && bottom[2] == 0);   /* green */
    int top_ok = (top[0] == 255 && top[1] == 0 && top[2] == 0);               /* red */
    CHECK(name, bottom_ok && top_ok,
          "bottom=(%d,%d,%d) %s; top=(%d,%d,%d) %s",
          bottom[0], bottom[1], bottom[2], bottom_ok ? "OK(green)" : "WRONG",
          top[0], top[1], top[2], top_ok ? "OK(red)" : "WRONG");

    glDeleteProgram(prog);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

/* ================================================================== */
/* Test 8: reversed depth                                             */
/*                                                                    */
/* Clear depth to 0.0, glDepthFunc(GL_GREATER).  Draw red at z=0.5   */
/* (passes: 0.5 > 0.0).  Draw green at z=0.9 (passes: 0.9 > 0.5,     */
/* wins).  Draw blue at z=0.1 (fails: 0.1 < 0.9, occluded).  Center  */
/* should be green.  This is the Voxy reversed-depth convention.      */
/* ================================================================== */

static void test_reversed_depth(void)
{
    const char *name = "reversed_depth";
    const int W = 64, H = 64;

    GLuint color_tex, depth_tex;
    GLuint fbo = make_fbo_depth_tex(W, H, &color_tex, &depth_tex);
    if (!fbo) { CHECK(name, 0, "FBO creation failed"); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, W, H);

    /* Reversed depth: clear to 0.0 (farthest), GL_GREATER means higher z wins. */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);

    /* Shared program: fullscreen quad with adjustable depth + color. */
    GLuint prog = link_program_vs_fs(
        "#version 460 core\n"
        "layout(location = 0) in vec3 a_pos;\n"
        "uniform float u_depth;\n"
        "void main() { gl_Position = vec4(a_pos.xy, u_depth, 1.0); }\n",
        "#version 460 core\n"
        "layout(location = 0) out vec4 frag;\n"
        "uniform vec4 u_color;\n"
        "void main() { frag = u_color; }\n");
    if (!prog) { CHECK(name, 0, "program link failed"); return; }
    glUseProgram(prog);

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    /* Fullscreen quad (pos only). */
    static const float FS_POS[] = {
        -1.0f, -1.0f, 0.0f,  1.0f, -1.0f, 0.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,  1.0f, -1.0f, 0.0f,  1.0f, 1.0f, 0.0f,
    };
    GLuint vbo = make_vbo(FS_POS, sizeof(FS_POS));
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

    GLint depth_loc = glGetUniformLocation(prog, "u_depth");
    GLint color_loc = glGetUniformLocation(prog, "u_color");

    /* Draw red at z=0.5 (passes: 0.5 > 0.0 clear). */
    glUniform1f(depth_loc, 0.5f);
    glUniform4f(color_loc, 1.0f, 0.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    /* Draw green at z=0.9 (passes: 0.9 > 0.5, wins). */
    glUniform1f(depth_loc, 0.9f);
    glUniform4f(color_loc, 0.0f, 1.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    /* Draw blue at z=0.1 (fails: 0.1 < 0.9, occluded). */
    glUniform1f(depth_loc, 0.1f);
    glUniform4f(color_loc, 0.0f, 0.0f, 1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisable(GL_DEPTH_TEST);
    glFinish();

    uint8_t pixel[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

    /* Center should be green (z=0.9 won, blue at z=0.1 occluded). */
    int ok = (pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0);
    CHECK(name, ok, "expected green (z=0.9 won), got (%d,%d,%d)",
          pixel[0], pixel[1], pixel[2]);

    glDeleteProgram(prog);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color_tex);
    glDeleteTextures(1, &depth_tex);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fprintf(stderr,
            "=== MGL Voxy Hi-Z Data-Flow Test (Spec R14 / Task 14) ===\n\n");

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

    fprintf(stderr, "[1] 深度 texture sampling\n");
    test_depth_texture_sampling();
    drain_errors();

    fprintf(stderr, "\n[2] textureGather\n");
    test_texture_gather();
    drain_errors();

    fprintf(stderr, "\n[3] R32F imageStore\n");
    test_r32f_image_store();
    drain_errors();

    fprintf(stderr, "\n[4] FBO mip level attachment\n");
    test_fbo_mip_attachment();
    drain_errors();

    fprintf(stderr, "\n[5] render target → sampler conversion\n");
    test_render_target_to_sampler();
    drain_errors();

    fprintf(stderr, "\n[6] resize 后 texture/FBO 重建\n");
    test_resize_rebuild();
    drain_errors();

    fprintf(stderr, "\n[7] Y 轴方向\n");
    test_y_axis_direction();
    drain_errors();

    fprintf(stderr, "\n[8] reversed depth\n");
    test_reversed_depth();
    drain_errors();

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
