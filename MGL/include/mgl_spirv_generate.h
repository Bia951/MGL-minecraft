#ifndef MGL_SPIRV_GENERATE_H
#define MGL_SPIRV_GENERATE_H

#include <glslang_c_interface.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Shared by linked-program and standalone translation. Keep OpName/debug
 * information used by GL reflection; validation matches glslang's default.
 * The bundled glslang requires optimize_size as well as disable_optimizer=false
 * to run its GLSL optimization passes. */
static inline void mglGenerateProgramSPIRV(glslang_program_t *program,
                                         glslang_stage_t stage)
{
    const char *enabled = getenv("MGL_SPIRV_OPTIMIZE");
    if (!enabled || strcmp(enabled, "1") != 0) {
        glslang_program_SPIRV_generate(program, stage);
        return;
    }
    glslang_spv_options_t options = {0};
    options.disable_optimizer = false;
    options.optimize_size = true;
    options.validate = true;
    glslang_program_SPIRV_generate_with_options(program, stage, &options);
    fprintf(stderr, "MGL SPIRV OPTIMIZE stage=%d words=%zu\n",
            (int)stage, glslang_program_SPIRV_get_size(program));
}

#endif
