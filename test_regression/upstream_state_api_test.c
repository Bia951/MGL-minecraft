/* Focused GL API regressions for the adapted upstream state fixes.
 * Build against libmgl, then run with DYLD_LIBRARY_PATH=build. */
#include <stdio.h>
#include <string.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include "MGLContext.h"
#include "MGLRenderer.h"

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}
static void error(GLenum expected, const char *what)
{
    GLenum actual = glGetError();
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: error 0x%x expected 0x%x\n", what, actual, expected);
        failures++;
    }
    while (glGetError() != GL_NO_ERROR) {}
}
static GLuint shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(sh, sizeof(log), NULL, log); fprintf(stderr, "%s\n", log); }
    check(ok, "shader compilation");
    return sh;
}
static void buffers(void)
{
    GLuint names[2];
    GLuint data[4] = {17, 23, 41, 59}, out[4] = {0};
    glGenBuffers(2, names);
    glBindBuffer(GL_ARRAY_BUFFER, names[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
    GLint v = 0;
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_ACCESS, &v);
    check(v == GL_READ_WRITE, "BufferData initial ACCESS");
    check(glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY) != NULL, "MapBuffer read only");
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_ACCESS_FLAGS, &v);
    check(v == GL_MAP_READ_BIT, "MapBuffer ACCESS_FLAGS");
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_ACCESS, &v);
    check(v == GL_READ_ONLY, "UnmapBuffer preserves ACCESS");
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_ACCESS_FLAGS, &v);
    check(v == 0, "UnmapBuffer clears ACCESS_FLAGS");
    check(glMapNamedBufferRange(names[0], 0, sizeof(data), GL_MAP_WRITE_BIT) != NULL, "named write mapping");
    glGetNamedBufferParameteriv(names[0], GL_BUFFER_ACCESS, &v);
    check(v == GL_WRITE_ONLY, "MapNamedBufferRange ACCESS");
    glUnmapNamedBuffer(names[0]);
    glGetNamedBufferParameteriv(names[0], GL_BUFFER_ACCESS, &v);
    check(v == GL_WRITE_ONLY, "UnmapNamedBuffer preserves ACCESS");
    error(GL_NO_ERROR, "buffer access sequence");
    glBindBuffer(GL_COPY_WRITE_BUFFER, names[1]);
    glBufferStorage(GL_COPY_WRITE_BUFFER, sizeof(data), data, 0);
    GLuint replacement = 99;
    glBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof(replacement), &replacement);
    error(GL_INVALID_OPERATION, "immutable SubData denied");
    glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof(out), out);
    check(memcmp(data, out, sizeof(data)) == 0, "denied SubData leaves bytes unchanged");
    glBindBuffer(GL_COPY_READ_BUFFER, names[0]);
    glBufferSubData(GL_COPY_READ_BUFFER, 0, sizeof(replacement), &replacement);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, sizeof(data));
    glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof(out), out);
    check(out[0] == replacement && out[1] == data[1], "CopyBufferSubData publishes immutable destination");
    error(GL_NO_ERROR, "immutable copy is allowed");
    glDeleteBuffers(2, names);
    glGetIntegerv(GL_MAX_TRANSFORM_FEEDBACK_BUFFERS, &v);
    glBindBuffersBase(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)v, 1, NULL);
    error(GL_INVALID_OPERATION, "BindBuffersBase per-target limit");
    glBindBuffersRange(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)v, 1, NULL, NULL, NULL);
    error(GL_INVALID_OPERATION, "BindBuffersRange per-target limit");
    glBindBuffersBase(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)v, 0, NULL);
    glBindBuffersRange(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)v, 0, NULL, NULL, NULL);
    error(GL_NO_ERROR, "empty bindings at max");
    glBindBuffersBase(GL_TRANSFORM_FEEDBACK_BUFFER, (GLuint)v + 1, 0, NULL);
    error(GL_INVALID_OPERATION, "empty bindings past max");
}
static void stages(void)
{
    GLuint vs = shader(GL_VERTEX_SHADER, "#version 330 core\nvoid main(){gl_Position=vec4(0);}");
    GLuint fs = shader(GL_FRAGMENT_SHADER, "#version 330 core\nout vec4 c;void main(){c=vec4(1);}");
    GLuint cs = shader(GL_COMPUTE_SHADER, "#version 430 core\nlayout(local_size_x=4,local_size_y=2)in;void main(){}");
    GLuint graphics = glCreateProgram(), compute = glCreateProgram();
    glAttachShader(graphics, vs); glAttachShader(graphics, fs); glLinkProgram(graphics);
    glAttachShader(compute, cs); glLinkProgram(compute);
    GLint ok; glGetProgramiv(graphics, GL_LINK_STATUS, &ok); check(ok, "graphics link");
    glGetProgramiv(compute, GL_LINK_STATUS, &ok); check(ok, "compute link");
    static const GLenum missing[] = {GL_TESS_CONTROL_OUTPUT_VERTICES, GL_TESS_GEN_MODE,
        GL_TESS_GEN_SPACING, GL_TESS_GEN_VERTEX_ORDER, GL_TESS_GEN_POINT_MODE};
    for (unsigned i=0; i<sizeof(missing)/sizeof(missing[0]); i++) {
        GLint value = 1234; glGetProgramiv(graphics, missing[i], &value);
        error(GL_INVALID_OPERATION, "missing tessellation stage query");
        check(value == 1234, "invalid stage query preserves output");
    }
    GLint size[3] = {1234,1234,1234};
    glAttachShader(graphics, cs);
    glGetProgramiv(graphics, GL_COMPUTE_WORK_GROUP_SIZE, size);
    error(GL_INVALID_OPERATION, "attached unlinked compute stage excluded");
    check(size[0] == 1234 && size[2] == 1234, "invalid compute query preserves output");
    glDetachShader(graphics, cs);
    glDetachShader(compute, cs);
    glGetProgramiv(compute, GL_COMPUTE_WORK_GROUP_SIZE, size);
    error(GL_NO_ERROR, "detached linked compute query");
    check(size[0] == 4 && size[1] == 2 && size[2] == 1, "linked compute workgroup retained");
    glDeleteProgram(graphics); glDeleteProgram(compute);
    glDeleteShader(vs); glDeleteShader(fs); glDeleteShader(cs);
}
static void framebuffers(void)
{
    GLuint fbo; glCreateFramebuffers(1, &fbo);
    GLint sentinel = 1234;
    glGetNamedFramebufferAttachmentParameteriv(fbo, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &sentinel);
    error(GL_INVALID_OPERATION, "unattached image property");
    check(sentinel == 1234, "unattached query preserves output");
    glGetNamedFramebufferParameteriv(0, GL_FRAMEBUFFER_DEFAULT_WIDTH, &sentinel);
    error(GL_INVALID_OPERATION, "default framebuffer WIDTH invalid");
    GLenum token = GL_FRONT;
    glNamedFramebufferDrawBuffer(0, token); error(GL_NO_ERROR, "single DrawBuffer FRONT legal");
    glNamedFramebufferDrawBuffers(0, 1, &token); error(GL_INVALID_ENUM, "DrawBuffers FRONT forbidden");
    glNamedFramebufferDrawBuffer(fbo, token); error(GL_INVALID_OPERATION, "DrawBuffer FRONT on FBO");
    glNamedFramebufferReadBuffer(fbo, 0xdead); error(GL_INVALID_ENUM, "unknown ReadBuffer token");
    glNamedFramebufferReadBuffer(0, GL_COLOR_ATTACHMENT0); error(GL_INVALID_OPERATION, "default read COLOR_ATTACHMENT");
    glInvalidateNamedFramebufferData(fbo, 1, &token); error(GL_INVALID_ENUM, "invalid FBO invalidate attachment");
    GLint max; glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max);
    token = GL_COLOR_ATTACHMENT0 + max;
    glNamedFramebufferDrawBuffers(fbo, 1, &token); error(GL_INVALID_OPERATION, "DrawBuffers color attachment past max");
    glInvalidateNamedFramebufferData(fbo, 1, &token); error(GL_INVALID_OPERATION, "invalidate color attachment past max");
    glNamedFramebufferRenderbuffer(fbo, GL_COLOR_ATTACHMENT0, 0xdead, 0);
    error(GL_INVALID_ENUM, "renderbuffer target validation");
    glDeleteFramebuffers(1, &fbo);
}
int main(void)
{
    GLMContext ctx = createGLMContext(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                    GL_DEPTH_COMPONENT, GL_FLOAT, 0, 0);
    if (!ctx || !CppCreateMGLRendererHeadless(ctx)) return 2;
    MGLsetCurrentContext(ctx);
    while (glGetError() != GL_NO_ERROR) {}
    buffers(); stages(); framebuffers();
    printf("upstream_state_api_test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
