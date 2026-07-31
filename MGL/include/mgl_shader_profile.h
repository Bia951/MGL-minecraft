/*
 * mgl_shader_profile.h
 * MGL
 *
 * Shader compile-profile selection (Spec R12 / Task 12).
 *
 * Splits the GLSL -> SPIR-V -> MSL pipeline into two profiles:
 *
 *   MGL_SHADER_PROFILE_LEGACY         Vanilla Minecraft shaders.
 *                                     Allows the existing finite GLSL text
 *                                     compatibility rewrites (version upgrade,
 *                                     UBO/SSBO binding injection, extension
 *                                     stripping, cull-distance rewrite, ...).
 *                                     SPIR-V target is SPV_1_0.
 *
 *   MGL_SHADER_PROFILE_MODERN_STRICT  Voxy-class GLSL 4.50/4.60 compute and
 *                                     SSBO/subgroup/image shaders.  Preserves
 *                                     original semantics: no GLSL text
 *                                     rewrites, newer SPIR-V target
 *                                     (SPV_1_3), glslang auto-map bindings
 *                                     and locations, SPIRV-Cross reflection
 *                                     for resource mapping, and the legacy
 *                                     MSL string-level patch pipeline is
 *                                     skipped.  int64 is never faked: if the
 *                                     shader uses int64 and the Metal device
 *                                     cannot compile it, the failure bubbles
 *                                     up so Voxy falls back to 32-bit.
 *
 * The profile is selected purely from the GLSL source text and shader type;
 * it does not depend on runtime state, so it can be computed at shader-source
 * time and reused at link time.
 */

#ifndef MGL_SHADER_PROFILE_H
#define MGL_SHADER_PROFILE_H

#include "glcorearb.h"
#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MGL_SHADER_PROFILE_LEGACY = 0,
    MGL_SHADER_PROFILE_MODERN_STRICT = 1
} MGLShaderProfile;

/* Selects the compile profile for a shader from its GLSL source.
 *
 * `type` is the GL shader type (GL_COMPUTE_SHADER, GL_VERTEX_SHADER, ...).
 * `src` is the shader source (need not be NUL-terminated; `src_len` is the
 *   number of bytes to inspect).  If `src` is NULL or `src_len` is 0, the
 *   function returns MGL_SHADER_PROFILE_LEGACY.
 *
 * Rule (Spec R12):
 *   MODERN_STRICT  when #version >= 450 AND (compute shader OR source uses
 *                  modern features: SSBO / runtime-sized array / image /
 *                  subgroup / int64).
 *   LEGACY         otherwise (vanilla Minecraft GLSL 120/330, ES, ...).
 *
 * Vanilla Minecraft shaders are GLSL 1.20/3.30, so they always take the
 * LEGACY path and keep the existing compatibility rewrites. */
MGLShaderProfile mglDetectShaderProfile(GLuint type, const char *src, size_t src_len);

/* Human-readable name for diagnostics. */
const char *mglShaderProfileName(MGLShaderProfile profile);

#ifdef __cplusplus
}
#endif

#endif /* MGL_SHADER_PROFILE_H */
