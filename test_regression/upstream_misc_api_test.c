/* Focused checks for selected upstream API and packed-pixel fixes. */
#include <stdio.h>
#include <stdint.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include "MGLContext.h"
#include "MGLRenderer.h"

static int failures;
static void expect_error(const char *label, GLenum expected)
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

    GLfloat attenuation[3] = {1.0f, 0.0f, 0.0f};
    glPointParameterf(0x8126, 1.0f); /* compatibility POINT_SIZE_MIN */
    expect_error("point min", GL_INVALID_ENUM);
    glPointParameteri(0x8127, 1); /* compatibility POINT_SIZE_MAX */
    expect_error("point max", GL_INVALID_ENUM);
    glPointParameterfv(0x8129, attenuation); /* compatibility attenuation */
    expect_error("point attenuation", GL_INVALID_ENUM);
    glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, -1.0f);
    expect_error("negative point fade", GL_INVALID_VALUE);
    glPointParameterf(GL_POINT_SPRITE_COORD_ORIGIN, (GLfloat)GL_UPPER_LEFT);
    expect_error("point origin float", GL_NO_ERROR);

    glMemoryBarrierByRegion(GL_ALL_BARRIER_BITS);
    expect_error("region all bits", GL_NO_ERROR);
    glMemoryBarrierByRegion(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    expect_error("region invalid bit", GL_INVALID_VALUE);

    GLuint textures[2];
    glGenTextures(2, textures);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, textures[0]);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 2, GL_RGBA8, 1, 1, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, textures[1]);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, 1, 1, GL_TRUE);
    expect_error("sample texture setup", GL_NO_ERROR);
    glCopyImageSubData(textures[0], GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0,
                       textures[1], GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0, 1, 1, 1);
    expect_error("copy different samples", GL_INVALID_OPERATION);
    glDeleteTextures(2, textures);

    GLuint texture;
    const uint32_t packed_red = 0xfff00000u; /* BGRA: B=0,G=0,R=1023,A=3 */
    unsigned char rgba[4] = {0};
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, 1, 1, 0, GL_BGRA,
                 GL_UNSIGNED_INT_2_10_10_10_REV, &packed_red);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    expect_error("packed BGRA transfer", GL_NO_ERROR);
    if (rgba[0] != 255 || rgba[1] != 0 || rgba[2] != 0 || rgba[3] != 255) {
        fprintf(stderr, "packed BGRA red became %u,%u,%u,%u\n", rgba[0], rgba[1], rgba[2], rgba[3]);
        failures++;
    }
    glDeleteTextures(1, &texture);

    /* GL 4.6 Table 8.5 permits shared-exp packed data only with RGB. */
    const uint32_t shared_red = (16u << 27u) | 256u;
    uint32_t shared_readback = 0;
    GLfloat rgb[3] = {0};
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 1, 1, 0, GL_BGR,
                 GL_UNSIGNED_INT_5_9_9_9_REV, &shared_red);
    expect_error("illegal shared-exponent BGR upload", GL_INVALID_OPERATION);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_INT_5_9_9_9_REV, &shared_red);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGR, GL_FLOAT, rgb);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV,
                  &shared_readback);
    expect_error("shared-exponent RGB transfer", GL_NO_ERROR);
    if (rgb[0] != 0.0f || rgb[1] != 0.0f || rgb[2] != 1.0f ||
        shared_readback != shared_red) {
        fprintf(stderr, "shared BGR red became %g,%g,%g packed 0x%x\n",
                rgb[0], rgb[1], rgb[2], shared_readback);
        failures++;
    }
    glDeleteTextures(1, &texture);
    printf("upstream miscellaneous API: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
