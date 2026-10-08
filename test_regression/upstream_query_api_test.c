/* Focused regression for upstream generic queries and MultiDraw validation. */
#include <stdio.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include "MGLContext.h"
#include "MGLRenderer.h"

static int failures;
static void expect_error(GLenum expected, const char *label)
{
    GLenum actual = glGetError();
    if (actual != expected) {
        fprintf(stderr, "%s: error 0x%x, expected 0x%x\n", label, actual, expected);
        failures++;
    }
}

int main(void)
{
    GLMContext ctx = createGLMContext(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                     GL_DEPTH_COMPONENT, GL_FLOAT, 0, 0);
    if (!ctx || !CppCreateMGLRendererHeadless(ctx)) return 2;
    MGLsetCurrentContext(ctx);
    while (glGetError() != GL_NO_ERROR) {}
    const GLenum targets[] = {GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
        GL_ATOMIC_COUNTER_BUFFER, GL_TEXTURE_BUFFER, GL_QUERY_BUFFER, GL_PARAMETER_BUFFER};
    const GLenum pnames[] = {GL_COPY_READ_BUFFER_BINDING, GL_COPY_WRITE_BUFFER_BINDING,
        GL_ATOMIC_COUNTER_BUFFER_BINDING, GL_TEXTURE_BUFFER_BINDING,
        GL_QUERY_BUFFER_BINDING, GL_PARAMETER_BUFFER_BINDING};
    GLuint buffers[6];
    glGenBuffers(6, buffers);
    for (unsigned i = 0; i < 6; i++) {
        glBindBuffer(targets[i], buffers[i]);
        GLint value = -1;
        GLint64 wide = -1;
        GLfloat floating = -1;
        GLdouble doubled = -1;
        GLboolean boolean = GL_FALSE;
        glGetIntegerv(pnames[i], &value);
        glGetInteger64v(pnames[i], &wide);
        glGetFloatv(pnames[i], &floating);
        glGetDoublev(pnames[i], &doubled);
        glGetBooleanv(pnames[i], &boolean);
        if (value != (GLint)buffers[i] || wide != buffers[i] ||
            floating != buffers[i] || doubled != buffers[i] || boolean != GL_TRUE) failures++;
        glBindBuffer(targets[i], 0);
        value = -1;
        glGetIntegerv(pnames[i], &value);
        if (value != 0) failures++;
    }
    expect_error(GL_NO_ERROR, "generic bindings");
    glViewport(3, 7, 19, 23);
    glBlendColor(0.2f, 0.4f, 0.6f, 0.8f);
    const GLfloat inner[2] = {2, 3}, outer[4] = {4, 5, 6, 7};
    glPatchParameterfv(GL_PATCH_DEFAULT_INNER_LEVEL, inner);
    glPatchParameterfv(GL_PATCH_DEFAULT_OUTER_LEVEL, outer);
    const GLenum vectors[] = {GL_VIEWPORT, GL_BLEND_COLOR,
        GL_PATCH_DEFAULT_INNER_LEVEL, GL_PATCH_DEFAULT_OUTER_LEVEL};
    const unsigned counts[] = {4, 4, 2, 4};
    for (unsigned i = 0; i < 4; i++) {
        GLint normal[5] = {-99, -99, -99, -99, -99};
        GLint64 wide[5] = {-99, -99, -99, -99, -99};
        glGetIntegerv(vectors[i], normal);
        glGetInteger64v(vectors[i], wide);
        for (unsigned j = 0; j < counts[i]; j++) {
            if (wide[j] != normal[j]) failures++;
        }
        if (wide[counts[i]] != -99) failures++;
    }
    expect_error(GL_NO_ERROR, "vector queries");
    glMultiDrawElements(GL_TRIANGLES, NULL, GL_FLOAT, NULL, 0);
    expect_error(GL_INVALID_ENUM, "zero-count MultiDrawElements type");
    glMultiDrawElementsBaseVertex(GL_TRIANGLES, NULL, GL_FLOAT, NULL, 0, NULL);
    expect_error(GL_INVALID_ENUM, "zero-count MultiDrawElementsBaseVertex type");
    glMultiDrawElements(GL_TRIANGLES, NULL, GL_UNSIGNED_INT, NULL, 0);
    expect_error(GL_NO_ERROR, "valid zero-count MultiDrawElements");
    glDeleteBuffers(6, buffers);
    MGLsetCurrentContext(NULL);
    destroyGLMContext(ctx);
    fprintf(stderr, "upstream query API: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
