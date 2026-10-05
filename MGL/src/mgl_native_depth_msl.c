/* Compile a private IR variant using the normal fragment compatibility passes.
 * GL reflection is read-only. Nothing from this temporary Program is published. */
#include "mgl_native_depth_ir.h"
#include "mgl_spirv_compile.h"
#include "mgl_ir_postprocess.h"
#include <stdlib.h>
#include <string.h>

/* Uniform/input/output structs must remain byte-for-byte identical after the
 * same layout repairs. A changed ABI is a fallback, never a new GL reflection. */
static bool identical_structs(const char *base, const char *variant)
{
    if (!base || !variant) return false;
    const char *p = base;
    while ((p = strstr(p, "struct "))) {
        const char *brace = strchr(p, '{');
        if (!brace || (size_t)(brace - p) > 256u) return false;
        const char *end = strstr(brace, "};");
        if (!end) return false;
        char declaration[257];
        size_t length = (size_t)(brace - p);
        memcpy(declaration, p, length); declaration[length] = 0;
        const char *other = strstr(variant, declaration);
        size_t size = (size_t)(end + 2 - p);
        if (!other || strncmp(p, other, size)) return false;
        p = end + 2;
    }
    return true;
}

char *mglNativeDepthMSL(GLMContext ctx, Program *program, uint64_t depth_mask, uint64_t flip_mask)
{
    const int stage = _FRAGMENT_SHADER;
    if (!program || !ctx || program->spirv[stage].uses_argument_buffers ||
        !program->spirv[stage].entry_point || !program->spirv[stage].msl_str) return NULL;
    SpirvResourceList *images = &program->spirv_resources_list[stage][SPVC_RESOURCE_TYPE_SAMPLED_IMAGE];
    if (!images->list || images->count > 64u) return NULL;
    uint32_t ids[64];
    for (GLuint i = 0; i < images->count; i++) ids[i] = images->list[i]._id;
    uint32_t *words = NULL;
    size_t count = 0;
    if (!mglNativeDepthIR(program->spirv[stage].ir, program->spirv[stage].size,
                          ids, images->count, depth_mask, flip_mask, &words, &count)) return NULL;
    Program *copy = malloc(sizeof(*copy));
    spvc_context context = NULL;
    spvc_compiler compiler = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler_options options = NULL;
    char *result = NULL;
    if (!copy) { free(words); return NULL; }
    memcpy(copy, program, sizeof(*copy));
    copy->spirv[stage].msl_str = NULL;
    /* Only the fragment resource records are writable by these passes.
     * Owned names/member layouts remain shared, read-only input. */
    for (int type = 0; type < _MAX_SPIRV_RES; type++) copy->spirv_resources_list[stage][type].list = NULL;
    for (int type = 0; type < _MAX_SPIRV_RES; type++) {
        SpirvResourceList *src = &program->spirv_resources_list[stage][type];
        if (!src->count) continue;
        if (!src->list || src->count > 4096u) goto done;
        copy->spirv_resources_list[stage][type].list = malloc(src->count * sizeof(SpirvResource));
        if (!copy->spirv_resources_list[stage][type].list) goto done;
        memcpy(copy->spirv_resources_list[stage][type].list, src->list, src->count * sizeof(SpirvResource));
    }
#define CHECK(expr) do { if ((expr) != SPVC_SUCCESS) goto done; } while (0)
    CHECK(spvc_context_create(&context));
    CHECK(spvc_context_parse_spirv(context, words, count, &ir));
    CHECK(spvc_context_create_compiler(context, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler));
    CHECK(spvc_compiler_msl_add_discrete_descriptor_set(compiler, 2));
    CHECK(spvc_compiler_msl_add_discrete_descriptor_set(compiler, 3));
    CHECK(spvc_compiler_create_compiler_options(compiler, &options));
    CHECK(spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_ARGUMENT_BUFFERS, SPVC_FALSE));
    CHECK(spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_TEXTURE_1D_AS_2D, SPVC_TRUE));
    CHECK(spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, SPVC_TRUE));
    CHECK(spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(3,1,0)));
    CHECK(spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_TEXEL_BUFFER_TEXTURE_WIDTH, MGL_TEXEL_BUFFER_TEXTURE_WIDTH));
    CHECK(spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_BUFFER_SIZE_BUFFER_INDEX, MGL_BUFFER_SIZE_BUFFER_INDEX));
    CHECK(spvc_compiler_install_compiler_options(compiler, options));
    CHECK(spvc_compiler_rename_entry_point(compiler, "main", program->spirv[stage].entry_point, SpvExecutionModelFragment));
    for (int type = 0; type < _MAX_SPIRV_RES; type++) {
        SpirvResourceList *list = &copy->spirv_resources_list[stage][type];
        for (GLuint i = 0; i < list->count; i++) {
            SpirvResource *res = &list->list[i];
            switch (type) {
                case SPVC_RESOURCE_TYPE_UNIFORM_BUFFER: case SPVC_RESOURCE_TYPE_UNIFORM_CONSTANT:
                case SPVC_RESOURCE_TYPE_STORAGE_BUFFER: case SPVC_RESOURCE_TYPE_ATOMIC_COUNTER:
                case SPVC_RESOURCE_TYPE_SAMPLED_IMAGE: case SPVC_RESOURCE_TYPE_SEPARATE_IMAGE:
                case SPVC_RESOURCE_TYPE_STORAGE_IMAGE: case SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS:
                    spvc_compiler_set_decoration(compiler, res->_id, SpvDecorationBinding, res->binding);
                    break;
                default: break;
            }
            if (res->name && !strcmp(res->name, "sampler")) spvc_compiler_set_name(compiler, res->_id, "mgl_sampler_tex");
        }
    }
    if (!mglRunIRPostprocessPipeline(ctx, copy, stage, compiler)) goto done;
    const char *msl = NULL;
    CHECK(spvc_compiler_compile(compiler, &msl));
    if (!msl) goto done;
    MSLPatchPipeline pipeline;
    char *owned = strdup(msl);
    if (!owned) goto done;
    if (!mslPipelineInit(&pipeline, copy, stage, owned)) { free(owned); goto done; }
    mslPipelineAddStep(&pipeline, "remove_restrict", mglPatchRemoveRestrict);
    mslPipelineAddStep(&pipeline, "fix_sampler_shadowing", mglPatchFixSamplerShadowing);
    mslPipelineAddStep(&pipeline, "fix_unknown_texture_type", mglPatchFixUnknownTextureType);
    mslPipelineAddStep(&pipeline, "strip_thread_const_ref", mglPatchStripThreadConstRef);
    mslPipelineAddStep(&pipeline, "rename_length_squared", mglPatchRenameLengthSquared);
    mslPipelineAddStep(&pipeline, "lower_double_types", mglPatchLowerDoubleTypes);
    mslPipelineAddStep(&pipeline, "fix_end_portal_layer", mglPatchFixEndPortalLayer);
    mslPipelineAddStep(&pipeline, "fragcoord_origin_fix", mglPatchFragCoordOriginFix);
    mslPipelineAddStep(&pipeline, "fix_plain_struct_pointer_array", mglPatchFixPlainStructPointerArray);
    mslPipelineAddStep(&pipeline, "inject_atomic_counter_args", mglPatchInjectAtomicCounterArgs);
    mslPipelineAddStep(&pipeline, "apply_resource_bindings", mglPatchApplyResourceBindings);
    mslPipelineAddStep(&pipeline, "fix_image2drect_imagesize", mglPatchFixImage2DRectImageSize);
    mslPipelineAddStep(&pipeline, "apply_resource_bindings_final", mglPatchApplyResourceBindings);
    bool patched = mslPipelineRun(&pipeline);
    copy->spirv[stage].msl_str = mslPipelineTakeResult(&pipeline);
    mslPipelineDestroy(&pipeline);
    if (!patched) goto done;
    applyMSLUniformBufferPacking(copy, stage);
    if (!identical_structs(program->spirv[stage].msl_str, copy->spirv[stage].msl_str)) goto done;
    if ((spvc_compiler_msl_needs_buffer_size_buffer(compiler) != SPVC_FALSE) !=
        (program->spirv[stage].needs_buffer_size_buffer != GL_FALSE)) goto done;
    for (int type = 0; type < _MAX_SPIRV_RES; type++) {
        SpirvResourceList *list = &copy->spirv_resources_list[stage][type];
        for (GLuint i = 0; i < list->count; i++) {
            if (list->list[i].binding != program->spirv_resources_list[stage][type].list[i].binding) goto done;
        }
    }
    result = copy->spirv[stage].msl_str;
    copy->spirv[stage].msl_str = NULL;
done:
    for (int type = 0; type < _MAX_SPIRV_RES; type++) free(copy->spirv_resources_list[stage][type].list);
    free(copy->spirv[stage].msl_str);
    free(copy); free(words);
    if (context) spvc_context_destroy(context);
    return result;
}
