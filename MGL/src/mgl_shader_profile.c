/*
 * mgl_shader_profile.c
 * MGL
 *
 * Implementation of the shader compile-profile selector (Spec R12 / Task 12).
 * See mgl_shader_profile.h for the profile contract and selection rule.
 */

#include "mgl_shader_profile.h"

#include <string.h>
#include <stddef.h>

static int mgl_is_ident_char(int c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

/* Returns 1 if `src[..]` contains `token` as a standalone identifier token
 * (not a substring of a larger identifier).  `src_len` bounds the search. */
static int mgl_source_contains_token(const char *src, size_t src_len,
                                     const char *token)
{
    if (!src || !token) {
        return 0;
    }

    size_t token_len = strlen(token);
    if (token_len == 0 || token_len > src_len) {
        return 0;
    }

    for (size_t i = 0; i + token_len <= src_len; i++) {
        if (memcmp(src + i, token, token_len) != 0) {
            continue;
        }

        /* Check the char before the match is not an identifier char. */
        if (i > 0 && mgl_is_ident_char((unsigned char)src[i - 1])) {
            continue;
        }

        /* Check the char after the match is not an identifier char. */
        size_t end = i + token_len;
        if (end < src_len && mgl_is_ident_char((unsigned char)src[end])) {
            continue;
        }

        return 1;
    }

    return 0;
}

/* Parse the GLSL #version number.  Returns 0 if not found. */
static int mgl_parse_glsl_version(const char *src, size_t src_len)
{
    if (!src || src_len == 0) {
        return 0;
    }

    /* Search for "#version" within the first 256 bytes (it must be near the
     * top of the source per the GLSL spec).  A plain substring search is
     * fine here — false positives later in the file are harmless because a
     * valid #version line always starts at the first non-whitespace. */
    size_t scan = src_len < 256 ? src_len : 256;
    const char *p = NULL;
    for (size_t i = 0; i + 8 <= scan; i++) {
        if (memcmp(src + i, "#version", 8) == 0) {
            /* Make sure it's not part of a larger token (e.g. x_version). */
            if (i > 0 && mgl_is_ident_char((unsigned char)src[i - 1])) {
                continue;
            }
            p = src + i + 8;
            break;
        }
    }
    if (!p) {
        return 0;
    }

    /* Skip whitespace between "#version" and the number. */
    while (p < src + src_len && (*p == ' ' || *p == '\t')) {
        p++;
    }

    int version = 0;
    while (p < src + src_len && *p >= '0' && *p <= '9') {
        version = version * 10 + (*p - '0');
        if (version > 9999) {
            break;
        }
        p++;
    }

    return version;
}

/* Returns 1 if the source uses any modern GLSL feature that should force the
 * MODERN_STRICT profile (under a #version >= 450 shader). */
static int mgl_source_uses_modern_features(const char *src, size_t src_len)
{
    /* SSBO: GLSL `buffer` block qualifier.  `buffer` as a standalone token
     * in a layout/block decl is the SSBO storage qualifier (GLSL 4.30+). */
    if (mgl_source_contains_token(src, src_len, "buffer")) {
        return 1;
    }

    /* std430 is the SSBO packing rule and only appears with SSBOs. */
    if (mgl_source_contains_token(src, src_len, "std430")) {
        return 1;
    }

    /* Image load/store types (GL_ARB_shader_image_load_store, GLSL 4.20+). */
    static const char *const image_types[] = {
        "image1D", "image2D", "image3D", "imageCube",
        "image2DRect", "image1DArray", "image2DArray", "imageCubeArray",
        "imageBuffer", "iimage1D", "iimage2D", "iimage3D", "iimageCube",
        "uimage1D", "uimage2D", "uimage3D", "uimageCube",
        "image2DMS", "image2DMSArray",
        NULL
    };
    for (int i = 0; image_types[i]; i++) {
        if (mgl_source_contains_token(src, src_len, image_types[i])) {
            return 1;
        }
    }

    /* imageLoad / imageStore built-ins. */
    if (mgl_source_contains_token(src, src_len, "imageLoad")) {
        return 1;
    }
    if (mgl_source_contains_token(src, src_len, "imageStore")) {
        return 1;
    }

    /* Subgroup ops (GL_KHR_shader_subgroup / GL_NV_shader_thread_*). */
    if (mgl_source_contains_token(src, src_len, "subgroup")) {
        return 1;
    }
    if (mgl_source_contains_token(src, src_len, "gl_Subgroup")) {
        return 1;
    }
    if (mgl_source_contains_token(src, src_len, "gl_SUBGROUP_SIZE")) {
        return 1;
    }

    /* int64 (GL_ARB_gpu_shader_int64).  If a Voxy shader uses int64 and the
     * Metal device cannot compile it, the MODERN_STRICT path lets the failure
     * bubble up rather than papering over it. */
    if (mgl_source_contains_token(src, src_len, "int64_t")) {
        return 1;
    }
    if (mgl_source_contains_token(src, src_len, "uint64_t")) {
        return 1;
    }
    if (mgl_source_contains_token(src, src_len, "GL_ARB_gpu_shader_int64")) {
        return 1;
    }

    /* Compute-only shared memory qualifier. */
    if (mgl_source_contains_token(src, src_len, "shared")) {
        return 1;
    }

    /* Compute workgroup layout qualifier (also a strong compute signal). */
    if (mgl_source_contains_token(src, src_len, "local_size_x")) {
        return 1;
    }

    return 0;
}

MGLShaderProfile mglDetectShaderProfile(GLuint type, const char *src, size_t src_len)
{
    if (!src || src_len == 0) {
        return MGL_SHADER_PROFILE_LEGACY;
    }

    int version = mgl_parse_glsl_version(src, src_len);
    if (version < 450) {
        return MGL_SHADER_PROFILE_LEGACY;
    }

    /* #version >= 450: decide MODERN_STRICT vs LEGACY. */
    if (type == GL_COMPUTE_SHADER) {
        return MGL_SHADER_PROFILE_MODERN_STRICT;
    }

    if (mgl_source_uses_modern_features(src, src_len)) {
        return MGL_SHADER_PROFILE_MODERN_STRICT;
    }

    /* A GLSL 4.50/4.60 shader without any modern feature marker is treated
     * as LEGACY so the existing compatibility rewrites still apply.  Vanilla
     * Minecraft never emits 4.50/4.60, so this only affects shaders that
     * explicitly request 4.50+ without SSBO/image/subgroup/int64. */
    return MGL_SHADER_PROFILE_LEGACY;
}

const char *mglShaderProfileName(MGLShaderProfile profile)
{
    switch (profile) {
        case MGL_SHADER_PROFILE_MODERN_STRICT:
            return "modern-strict";
        case MGL_SHADER_PROFILE_LEGACY:
        default:
            return "legacy";
    }
}
