/* Private sampler2D variants. No GLSL/MSL text edits or reflection mutation. */
#include "mgl_native_depth_ir.h"
#include <spirv-tools/libspirv.h>
#include <spirv.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_BOUND (1024u * 1024u)
typedef struct { size_t at; uint32_t type; int owner; } Id;
typedef struct {
    const uint32_t *words;
    Id *ids;
    uint32_t bound;
} Parse;
typedef struct { uint32_t *words; size_t count, capacity; bool valid; } Stream;
typedef struct {
    uint32_t variable, pointer, sampled, image, scalar;
    uint32_t new_pointer, new_sampled, new_image, zero, one;
    bool depth, flip, declare_types;
} Resource;

static spv_result_t record(void *user, const spv_parsed_instruction_t *ins)
{
    Parse *p = user;
    if (ins->result_id) {
        if (ins->result_id >= p->bound) return SPV_ERROR_INVALID_ID;
        p->ids[ins->result_id].at = (size_t)(ins->words - p->words);
        p->ids[ins->result_id].type = ins->type_id;
    }
    return SPV_SUCCESS;
}

static void append(Stream *s, const uint32_t *words, size_t count)
{
    if (!s->valid || count > SIZE_MAX / sizeof(uint32_t) - s->count) { s->valid = false; return; }
    size_t needed = s->count + count;
    if (needed > s->capacity) {
        size_t cap = needed <= SIZE_MAX / (2u * sizeof(uint32_t)) ? needed * 2u : needed;
        uint32_t *new_words = realloc(s->words, cap * sizeof(uint32_t));
        if (!new_words) { s->valid = false; return; }
        s->words = new_words;
        s->capacity = cap;
    }
    memcpy(s->words + s->count, words, count * sizeof(uint32_t));
    s->count += count;
}
#define EMIT(S, OP, ...) do { uint32_t inst[] = {0, __VA_ARGS__}; \
    inst[0] = ((uint32_t)(sizeof(inst)/sizeof(inst[0])) << 16) | (OP); \
    append((S), inst, sizeof(inst)/sizeof(inst[0])); } while (0)

static const uint32_t *definition(const Parse *p, uint32_t id, SpvOp op)
{
    if (!id || id >= p->bound || !p->ids[id].at) return NULL;
    const uint32_t *w = p->words + p->ids[id].at;
    return (w[0] & 0xffffu) == (uint32_t)op ? w : NULL;
}

/* Only direct scalar sampler variables and their load/Image chains are
 * accepted. Other ID uses (functions, stores, phi, access chains, gather,
 * projection, comparison, offsets, sparse operations) reject the variant.
 * SPIRV-Tools operand metadata distinguishes IDs from coincident literals. */
static spv_result_t check_uses(void *user, const spv_parsed_instruction_t *ins)
{
    Parse *p = user;
    const uint32_t *w = ins->words;
    int owner = -1;
    uint32_t source = 0;
    switch (ins->opcode) {
        case SpvOpLoad: case SpvOpImage: source = w[3]; break;
        default: break;
    }
    if (source && source < p->bound) owner = p->ids[source].owner;
    if (owner >= 0 && ins->result_id) p->ids[ins->result_id].owner = owner;
    for (uint16_t k = 0; k < ins->num_operands; k++) {
        const spv_parsed_operand_t *operand = &ins->operands[k];
        if (operand->type != SPV_OPERAND_TYPE_ID) continue;
        uint32_t id = w[operand->offset];
        if (id >= p->bound || p->ids[id].owner < 0) continue;
        bool allowed = false;
        switch (ins->opcode) {
            case SpvOpName: case SpvOpDecorate: case SpvOpEntryPoint: allowed = true; break;
            case SpvOpLoad: case SpvOpImage: allowed = operand->offset == 3; break;
            case SpvOpImageSampleImplicitLod: case SpvOpImageSampleExplicitLod:
            case SpvOpImageFetch: case SpvOpImageQuerySizeLod:
            case SpvOpImageQuerySize: case SpvOpImageQueryLevels:
                allowed = operand->offset == 3; break;
            default: break;
        }
        if (!allowed) return SPV_ERROR_INVALID_DATA;
    }
    return SPV_SUCCESS;
}

static uint32_t flip_vector(Stream *s, uint32_t type, uint32_t scalar,
                             uint32_t value, uint32_t one, bool gradient,
                             uint32_t *next)
{
    uint32_t x = (*next)++, y = (*next)++, flipped = (*next)++, result = (*next)++;
    EMIT(s, SpvOpCompositeExtract, scalar, x, value, 0);
    EMIT(s, SpvOpCompositeExtract, scalar, y, value, 1);
    if (gradient) EMIT(s, SpvOpFNegate, scalar, flipped, y);
    else EMIT(s, SpvOpFSub, scalar, flipped, one, y);
    EMIT(s, SpvOpCompositeConstruct, type, result, x, flipped);
    return result;
}

bool mglNativeDepthIR(const uint32_t *words, size_t count,
                      const uint32_t *resources, size_t resource_count,
                      uint64_t depth_mask, uint64_t flip_mask,
                      uint32_t **output, size_t *output_count)
{
    if (output) *output = NULL;
    if (output_count) *output_count = 0;
    uint64_t selected = depth_mask | flip_mask;
    if (!output || !output_count || !words || count < 5 || !resources ||
        resource_count > 64 || !selected || words[0] != SpvMagicNumber ||
        words[3] == 0 || words[3] > MAX_BOUND ||
        (resource_count < 64 && (selected >> resource_count))) return false;
    spv_context context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    if (!context) return false;
    spv_diagnostic diagnostic = NULL;
    bool ok = false;
    Parse p = {words, calloc(words[3], sizeof(Id)), words[3]};
    Stream stream = {NULL, 0, 0, true};
    Resource plans[64] = {{0}};
    uint32_t next = words[3];
    bool query_capability = false, fragment = false;
    if (!p.ids) goto done;
    for (uint32_t i = 0; i < p.bound; i++) p.ids[i].owner = -1;
    if (spvValidateBinary(context, words, count, &diagnostic) != SPV_SUCCESS) goto done;
    if (spvBinaryParse(context, &p, words, count, NULL, record, &diagnostic) != SPV_SUCCESS) goto done;
    for (size_t at = 5; at < count; at += words[at] >> 16) {
        const uint32_t *w = words + at;
        if ((w[0] & 0xffffu) == SpvOpEntryPoint) {
            if (w[1] != SpvExecutionModelFragment) goto done;
            fragment = true;
        }
        if ((w[0] & 0xffffu) == SpvOpCapability && w[1] == SpvCapabilityImageQuery) query_capability = true;
    }
    if (!fragment) goto done;
    for (size_t i = 0; i < resource_count; i++) {
        if (!(selected & (UINT64_C(1) << i))) continue;
        Resource *r = &plans[i];
        r->variable = resources[i];
        const uint32_t *var = definition(&p, r->variable, SpvOpVariable);
        if (!var || var[3] != SpvStorageClassUniformConstant || (var[0] >> 16) != 4) goto done;
        r->pointer = var[1];
        const uint32_t *ptr = definition(&p, r->pointer, SpvOpTypePointer);
        if (!ptr || ptr[2] != SpvStorageClassUniformConstant) goto done;
        r->sampled = ptr[3];
        const uint32_t *sampled = definition(&p, r->sampled, SpvOpTypeSampledImage);
        if (!sampled) goto done;
        r->image = sampled[2];
        const uint32_t *image = definition(&p, r->image, SpvOpTypeImage);
        if (!image || image[3] != SpvDim2D || image[4] != 0 || image[5] || image[6] || image[7] != 1) goto done;
        r->scalar = image[2];
        const uint32_t *scalar = definition(&p, r->scalar, SpvOpTypeFloat);
        if (!scalar || scalar[2] != 32) goto done;
        r->depth = (depth_mask & (UINT64_C(1) << i)) != 0;
        r->flip = (flip_mask & (UINT64_C(1) << i)) != 0;
        r->new_image = next++; r->new_sampled = next++; r->new_pointer = next++;
        r->zero = next++; r->one = next++;
        if (p.ids[r->variable].owner >= 0) goto done;
        p.ids[r->variable].owner = (int)i;
    }
    /* SPIR-V forbids duplicate non-aggregate types. Share the private depth
     * clone between selected resources with the same original image type,
     * but never change the original types used by unselected color samplers.
     * Emit at the first variable in module order, not mask/reflection order. */
    for (size_t i = 0; i < resource_count; i++) {
        Resource *r = &plans[i];
        if (!r->variable) continue;
        if (!r->depth) {
            r->new_image = r->image; r->new_sampled = r->sampled; r->new_pointer = r->pointer;
            continue;
        }
        size_t first = i;
        for (size_t j = 0; j < resource_count; j++) {
            if (plans[j].variable && plans[j].depth && plans[j].image == r->image &&
                p.ids[plans[j].variable].at < p.ids[plans[first].variable].at) first = j;
        }
        r->new_image = plans[first].new_image;
        r->new_sampled = plans[first].new_sampled;
        r->new_pointer = plans[first].new_pointer;
        r->declare_types = first == i;
    }
    if (spvBinaryParse(context, &p, words, count, NULL, check_uses, &diagnostic) != SPV_SUCCESS) goto done;
    append(&stream, words, 5);
    if (!query_capability && flip_mask) EMIT(&stream, SpvOpCapability, SpvCapabilityImageQuery);
    for (size_t at = 5; at < count; at += words[at] >> 16) {
        const uint32_t *w = words + at;
        uint32_t wc = w[0] >> 16, op = w[0] & 0xffffu;
        if (op == SpvOpVariable && w[2] < p.bound && p.ids[w[2]].owner >= 0) {
            Resource *r = &plans[p.ids[w[2]].owner];
            if (r->declare_types) {
                const uint32_t *image = definition(&p, r->image, SpvOpTypeImage);
                uint32_t clone[10];
                if ((image[0] >> 16) > 10) goto done;
                memcpy(clone, image, (image[0] >> 16) * 4u);
                clone[1] = r->new_image; clone[4] = 1u;
                append(&stream, clone, image[0] >> 16);
                EMIT(&stream, SpvOpTypeSampledImage, r->new_sampled, r->new_image);
                EMIT(&stream, SpvOpTypePointer, r->new_pointer, SpvStorageClassUniformConstant, r->new_sampled);
            }
            EMIT(&stream, SpvOpConstant, r->scalar, r->zero, 0);
            EMIT(&stream, SpvOpConstant, r->scalar, r->one, 0x3f800000u);
            EMIT(&stream, SpvOpVariable, r->new_pointer, r->variable, SpvStorageClassUniformConstant);
            continue;
        }
        if ((op == SpvOpLoad || op == SpvOpImage) && w[3] < p.bound && p.ids[w[3]].owner >= 0) {
            Resource *r = &plans[p.ids[w[3]].owner];
            if (wc != 4) goto done;
            EMIT(&stream, op, op == SpvOpLoad ? r->new_sampled : r->new_image, w[2], w[3]);
            continue;
        }
        if ((op == SpvOpImageSampleImplicitLod || op == SpvOpImageSampleExplicitLod || op == SpvOpImageFetch) &&
            w[3] < p.bound && p.ids[w[3]].owner >= 0) {
            Resource *r = &plans[p.ids[w[3]].owner];
            if (wc > 10 || wc < 5) goto done;
            uint32_t edited[10]; memcpy(edited, w, wc * 4u);
            uint32_t operands = wc > 5 ? w[5] : 0;
            if (operands & ~(SpvImageOperandsBiasMask | SpvImageOperandsLodMask | SpvImageOperandsGradMask)) goto done;
            const uint32_t *result = definition(&p, w[1], SpvOpTypeVector);
            if (!result || result[2] != r->scalar || result[3] != 4) goto done;
            if (r->flip) {
                uint32_t coord_type = p.ids[w[4]].type;
                const uint32_t *coord = definition(&p, coord_type, SpvOpTypeVector);
                if (!coord || coord[3] != 2) goto done;
                if (op == SpvOpImageFetch) {
                    /* Query at GL-relative LOD on the bound mip-range view. */
                    const uint32_t *integer = definition(&p, coord[2], SpvOpTypeInt);
                    if (!integer || integer[2] != 32 || integer[3] != 1 || operands != SpvImageOperandsLodMask || wc != 7) goto done;
                    uint32_t size = next++, x = next++, y = next++, height = next++;
                    uint32_t edge = next++, fy = next++, out = next++;
                    /* height - (y + 1), using an integer one converted from the
                     * existing float constant avoids adding late global types. */
                    uint32_t one = next++, y1 = next++;
                    EMIT(&stream, SpvOpConvertFToS, coord[2], one, r->one);
                    EMIT(&stream, SpvOpImageQuerySizeLod, coord_type, size, w[3], w[6]);
                    EMIT(&stream, SpvOpCompositeExtract, coord[2], x, w[4], 0);
                    EMIT(&stream, SpvOpCompositeExtract, coord[2], y, w[4], 1);
                    EMIT(&stream, SpvOpCompositeExtract, coord[2], height, size, 1);
                    EMIT(&stream, SpvOpIAdd, coord[2], y1, y, one);
                    EMIT(&stream, SpvOpISub, coord[2], edge, height, y1);
                    /* Keep the coordinate SSA explicit for validator/debugging. */
                    EMIT(&stream, SpvOpCopyObject, coord[2], fy, edge);
                    EMIT(&stream, SpvOpCompositeConstruct, coord_type, out, x, fy);
                    edited[4] = out;
                } else {
                    if (coord[2] != r->scalar) goto done;
                    edited[4] = flip_vector(&stream, coord_type, r->scalar, w[4], r->one, false, &next);
                    if (operands & SpvImageOperandsGradMask) {
                        if (operands != SpvImageOperandsGradMask || wc != 8 || p.ids[w[6]].type != coord_type || p.ids[w[7]].type != coord_type) goto done;
                        edited[6] = flip_vector(&stream, coord_type, r->scalar, w[6], r->one, true, &next);
                        edited[7] = flip_vector(&stream, coord_type, r->scalar, w[7], r->one, true, &next);
                    }
                }
            }
            if (r->depth) edited[2] = next++;
            append(&stream, edited, wc);
            if (r->depth) {
                uint32_t d = next++;
                EMIT(&stream, SpvOpCompositeExtract, r->scalar, d, edited[2], 0);
                EMIT(&stream, SpvOpCompositeConstruct, w[1], w[2], d, r->zero, r->zero, r->one);
            }
            continue;
        }
        append(&stream, w, wc);
    }
    if (!stream.valid || next > MAX_BOUND) goto done;
    stream.words[3] = next;
    spvDiagnosticDestroy(diagnostic); diagnostic = NULL;
    if (spvValidateBinary(context, stream.words, stream.count, &diagnostic) != SPV_SUCCESS) goto done;
    *output = stream.words; *output_count = stream.count; stream.words = NULL;
    ok = true;
done:
    if (!ok && diagnostic && getenv("MGL_NATIVE_DEPTH_IR_DEBUG"))
        fprintf(stderr, "MGL native depth IR: %s\n", diagnostic->error);
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    free(p.ids); free(stream.words);
    return ok;
}
