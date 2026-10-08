/* Focused sampler/texture validation and compressed PBO readback regression.
 * Build with the same headers/frameworks as test-mapped-vertex, linking libmgl.
 */
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include "MGLContext.h"
#include "MGLRenderer.h"

static int failures;
static void error_is(const char *label, GLenum expected)
{
    GLenum actual = glGetError();
    if (actual != expected) {
        fprintf(stderr, "%s: error 0x%x expected 0x%x\n", label, actual, expected);
        failures++;
    }
}
static void check(const char *label, int ok)
{
    if (!ok) { fprintf(stderr, "%s failed\n", label); failures++; }
}

int main(void)
{
    GLMContext ctx = createGLMContext(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                     GL_DEPTH_COMPONENT, GL_FLOAT, 0, 0);
    if (!ctx || !CppCreateMGLRendererHeadless(ctx)) return 1;
    MGLsetCurrentContext(ctx);
    GLuint sampler, texture, ms, buffer;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(0x7fffffffu, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    error_is("unknown sampler", GL_INVALID_OPERATION);
    check("unknown sampler not created", !glIsSampler(0x7fffffffu));
    const GLenum forbidden[] = {GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL,
        GL_DEPTH_STENCIL_TEXTURE_MODE, GL_TEXTURE_SWIZZLE_R,
        GL_TEXTURE_IMMUTABLE_FORMAT, GL_TEXTURE_IMMUTABLE_LEVELS};
    for (unsigned i = 0; i < sizeof(forbidden)/sizeof(forbidden[0]); ++i) {
        GLint value = 123;
        glSamplerParameteri(sampler, forbidden[i], 0);
        error_is("sampler texture pname setter", GL_INVALID_ENUM);
        glGetSamplerParameteriv(sampler, forbidden[i], &value);
        error_is("sampler texture pname getter", GL_INVALID_ENUM);
        check("rejected getter preserves output", value == 123);
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const GLfloat color[] = {0.0f, 0.25f, 0.5f, 1.0f};
    GLfloat actual[4] = {-2,-2,-2,-2};
    GLint integers[4] = {-2,-2,-2,-2};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color);
    glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, actual);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, integers);
    error_is("border query", GL_NO_ERROR);
    check("all four float colors", memcmp(color, actual, sizeof(color)) == 0);
    check("normalized integer colors", integers[0] == 0 && integers[3] == INT_MAX &&
        integers[1] > 536870000 && integers[1] < 536872000 &&
        integers[2] > 1073741000 && integers[2] < 1073743000);
    glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, color);
    glGetSamplerParameteriv(sampler, GL_TEXTURE_BORDER_COLOR, integers);
    error_is("sampler normalized border query", GL_NO_ERROR);
    check("sampler integer endpoint", integers[3] == INT_MAX);
    glGetTexParameterIiv(GL_TEXTURE_BUFFER, GL_TEXTURE_MIN_FILTER, integers);
    error_is("integer buffer target", GL_INVALID_ENUM);
    glGetTexParameterIuiv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_MIN_FILTER, (GLuint *)integers);
    error_is("integer cube-face target", GL_INVALID_ENUM);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    glGetTexParameterIiv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, integers);
    check("immutable integer query", integers[0] == GL_TRUE);
    glGetTexParameterIuiv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS, (GLuint *)integers);
    check("immutable levels query", integers[0] == 1);
    error_is("immutable queries", GL_NO_ERROR);
    glGenTextures(1, &ms);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, ms);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_BASE_LEVEL, 0);
    error_is("multisample zero base", GL_NO_ERROR);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_BASE_LEVEL, 1);
    error_is("multisample nonzero base", GL_INVALID_OPERATION);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MAX_LEVEL, -1);
    error_is("multisample negative max", GL_INVALID_VALUE);
    glTexParameteri(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    error_is("multisample sampler state", GL_INVALID_ENUM);
    unsigned char pixel[4];
    glGetTextureSubImage(ms, 0, 0, 0, 0, 1, 1, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, 4, pixel);
    error_is("multisample subimage rejected", GL_INVALID_OPERATION);
    glBindImageTexture(0, 0x7fffffffu, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    error_is("missing image texture", GL_INVALID_VALUE);
    glBindImageTexture(0, texture, 0, GL_FALSE, -1, GL_READ_ONLY, GL_RGBA8);
    error_is("negative layer on 2D", GL_INVALID_VALUE);
    GLint max_image_units = 0;
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &max_image_units);
    glBindImageTextures((GLuint)max_image_units, 0, NULL);
    error_is("zero multibind at image limit", GL_NO_ERROR);
    glBindImageTextures((GLuint)max_image_units, 1, NULL);
    error_is("image multibind exceeds limit", GL_INVALID_OPERATION);
    glBindImageTextures((GLuint)max_image_units + 1, 0, NULL);
    error_is("zero multibind past image limit", GL_INVALID_OPERATION);
    GLuint image_names[] = {0x7fffffffu, texture};
    glBindImageTextures(0, 2, image_names);
    error_is("mixed image multibind", GL_INVALID_OPERATION);
    GLint binding;
    glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 1, &binding);
    check("valid multibind entry applied", binding == (GLint)texture);
    glGetIntegeri_v(GL_IMAGE_BINDING_ACCESS, 1, &binding);
    check("multibind read-write access", binding == GL_READ_WRITE);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_IMAGE_FORMAT_COMPATIBILITY_TYPE, &binding);
    error_is("image compatibility query", GL_NO_ERROR);
    check("image compatibility BY_SIZE", binding == GL_IMAGE_FORMAT_COMPATIBILITY_BY_SIZE);
    GLuint buffer_texture;
    glGenTextures(1, &buffer_texture);
    glBindTexture(GL_TEXTURE_BUFFER, buffer_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_DEPTH_COMPONENT32F, 0);
    error_is("TexBuffer rejects depth format", GL_INVALID_ENUM);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, 0);
    error_is("TexBuffer allows RGBA8 detach", GL_NO_ERROR);
    glGetTextureSubImage(buffer_texture, 0, 0, 0, 0, 1, 1, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, 4, pixel);
    error_is("buffer subimage rejected", GL_INVALID_OPERATION);
    glDeleteTextures(1, &buffer_texture);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const unsigned char compressed[8] = {10,20,0,0,0,0,0,0};
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RED_RGTC1,
                           4, 4, 0, sizeof(compressed), compressed);
    error_is("compressed upload", GL_NO_ERROR);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    unsigned char initial[16]; memset(initial, 0xa5, sizeof(initial));
    glBufferData(GL_PIXEL_PACK_BUFFER, sizeof(initial), initial, GL_STATIC_READ);
    glGetCompressedTextureImage(texture, 0, 8, NULL);
    error_is("PBO offset zero", GL_NO_ERROR);
    unsigned char copied[16];
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(copied), copied);
    check("PBO zero data", memcmp(copied, compressed, 8) == 0);
    check("PBO zero bounds", memcmp(copied + 8, initial + 8, 8) == 0);
    glBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(initial), initial);
    glGetCompressedTextureImage(texture, 0, 8, (void *)4);
    error_is("PBO offset four", GL_NO_ERROR);
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(copied), copied);
    check("PBO offset data", memcmp(copied + 4, compressed, 8) == 0);
    check("PBO offset bounds", memcmp(copied, initial, 4) == 0 &&
        memcmp(copied + 12, initial + 12, 4) == 0);
    glGetCompressedTextureImage(texture, 0, 8, (void *)9);
    error_is("PBO write overflow", GL_INVALID_OPERATION);
    void *mapping = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 16, GL_MAP_READ_BIT);
    check("map succeeded", mapping != NULL);
    glGetCompressedTextureImage(texture, 0, 8, NULL);
    error_is("nonpersistent mapped PBO", GL_INVALID_OPERATION);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteBuffers(1, &buffer);
    glDeleteTextures(1, &texture);
    glDeleteTextures(1, &ms);
    glDeleteSamplers(1, &sampler);
    printf("upstream texture API: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
