/* Reproduces the buffer upload and reuse pattern used by streaming chunk meshes.
 * Build: make test-mapped-vertex
 * Run: DYLD_LIBRARY_PATH=build build/test_mapped_vertex
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include "MGLContext.h"
#include "MGLRenderer.h"

typedef struct Vertex {
    float x, y;
    float r, g, b;
} Vertex;

static GLuint shader(GLenum type, const char *source)
{
    GLuint handle = glCreateShader(type);
    glShaderSource(handle, 1, &source, NULL);
    glCompileShader(handle);
    GLint ok = 0;
    glGetShaderiv(handle, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(handle, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile failed: %s\n", log);
        return 0;
    }
    return handle;
}

static GLuint program(void)
{
    const char *vs = "#version 330 core\n"
                     "layout(location=0) in vec2 position;\n"
                     "layout(location=1) in vec3 color;\n"
                     "out vec3 vertexColor;\n"
                     "void main() { gl_Position=vec4(position,0,1); vertexColor=color; }\n";
    const char *fs = "#version 330 core\n"
                     "in vec3 vertexColor; out vec4 fragmentColor;\n"
                     "void main() { fragmentColor=vec4(vertexColor,1); }\n";
    GLuint vert = shader(GL_VERTEX_SHADER, vs);
    GLuint frag = shader(GL_FRAGMENT_SHADER, fs);
    if (!vert || !frag) return 0;
    GLuint handle = glCreateProgram();
    glAttachShader(handle, vert);
    glAttachShader(handle, frag);
    glLinkProgram(handle);
    GLint ok = 0;
    glGetProgramiv(handle, GL_LINK_STATUS, &ok);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(handle, sizeof(log), NULL, log);
        fprintf(stderr, "program link failed: %s\n", log);
        return 0;
    }
    return handle;
}

static void triangle(Vertex *vertices, float center, float red, float green)
{
    const float xy[3][2] = {{-0.28f,-0.30f}, {0.28f,-0.30f}, {0.0f,0.30f}};
    for (int i = 0; i < 3; ++i) {
        vertices[i] = (Vertex){center + xy[i][0], xy[i][1], red, green, 0.0f};
    }
}

static int expect_color(const unsigned char *pixels, int x, int y,
                        int red, int green, const char *label)
{
    const unsigned char *p = pixels + ((y * 64 + x) * 4);
    int ok = red ? (p[0] > 200 && p[1] < 40) :
                   (p[1] > 200 && p[0] < 40);
    fprintf(stderr, "%s: %s (rgba=%u,%u,%u,%u)\n", label,
            ok ? "PASS" : "FAIL", p[0], p[1], p[2], p[3]);
    return ok;
}

int main(void)
{
    GLMContext ctx = createGLMContext(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                     GL_DEPTH_COMPONENT, GL_FLOAT, 0, 0);
    if (!ctx || !CppCreateMGLRendererHeadless(ctx)) return 1;
    MGLsetCurrentContext(ctx);

    GLuint fbo, texture, vao, vbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 2;
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);

    GLuint prog = program();
    if (!prog) return 3;
    glUseProgram(prog);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)(uintptr_t)8);

    /* Nonpersistent map/unmap must publish CPU shadow data to Metal. */
    glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 3, NULL, GL_STREAM_DRAW);
    Vertex *mapped = glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * 3,
                                      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!mapped) return 4;
    triangle(mapped, -0.48f, 0.0f, 1.0f);
    if (!glUnmapBuffer(GL_ARRAY_BUFFER)) return 5;
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    unsigned char pixels[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int ok = expect_color(pixels, 16, 30, 0, 1, "nonpersistent map/unmap VBO");
    glDeleteBuffers(1, &vbo);

    /* A fence must include deferred draws before the persistent ring is reused. */
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)(uintptr_t)8);
    glBufferStorage(GL_ARRAY_BUFFER, sizeof(Vertex) * 3, NULL,
                    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_CLIENT_STORAGE_BIT);
    mapped = glMapBufferRange(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * 3,
                              GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                              GL_MAP_FLUSH_EXPLICIT_BIT);
    if (!mapped) return 6;
    glClear(GL_COLOR_BUFFER_BIT);
    triangle(mapped, -0.48f, 0.0f, 1.0f);
    glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * 3);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLenum wait = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    if (wait != GL_ALREADY_SIGNALED && wait != GL_CONDITION_SATISFIED) {
        fprintf(stderr, "fence wait failed: 0x%x\n", wait);
        return 7;
    }
    glDeleteSync(fence);
    triangle(mapped, 0.48f, 1.0f, 0.0f);
    glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * 3);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ok &= expect_color(pixels, 16, 30, 0, 1, "persistent VBO before fence");
    ok &= expect_color(pixels, 47, 30, 1, 0, "persistent VBO after fence");
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) fprintf(stderr, "GL error: 0x%x\n", error);
    ok &= error == GL_NO_ERROR;
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    return ok ? 0 : 8;
}
