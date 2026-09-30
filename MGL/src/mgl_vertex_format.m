/*
 * mgl_vertex_format.m
 * MGL
 *
 * Implementation of the Vertex Format / Pipeline Signature Subsystem.
 * See mgl_vertex_format.h for the API contract.
 *
 * Pure helpers for GL→Metal vertex format translation and pipeline state
 * signature computation.  No renderer state dependency.
 */

#import "mgl_vertex_format.h"

#include <string.h>

/* === Vertex format mapping (extern) === */

const char *mglVertexFormatName(MTLVertexFormat format)
{
    switch (format) {
        case MTLVertexFormatFloat: return "Float";
        case MTLVertexFormatFloat2: return "Float2";
        case MTLVertexFormatFloat3: return "Float3";
        case MTLVertexFormatFloat4: return "Float4";
        case MTLVertexFormatUChar4: return "UChar4";
        case MTLVertexFormatUChar4Normalized: return "UChar4Normalized";
        case MTLVertexFormatUChar3: return "UChar3";
        case MTLVertexFormatUChar3Normalized: return "UChar3Normalized";
        case MTLVertexFormatUChar2: return "UChar2";
        case MTLVertexFormatUChar2Normalized: return "UChar2Normalized";
        case MTLVertexFormatUChar: return "UChar";
        case MTLVertexFormatUCharNormalized: return "UCharNormalized";
        case MTLVertexFormatShort: return "Short";
        case MTLVertexFormatShort2: return "Short2";
        case MTLVertexFormatShort3: return "Short3";
        case MTLVertexFormatShort4: return "Short4";
        case MTLVertexFormatShortNormalized: return "ShortNormalized";
        case MTLVertexFormatShort2Normalized: return "Short2Normalized";
        case MTLVertexFormatShort3Normalized: return "Short3Normalized";
        case MTLVertexFormatShort4Normalized: return "Short4Normalized";
        case MTLVertexFormatUShort: return "UShort";
        case MTLVertexFormatUShort2: return "UShort2";
        case MTLVertexFormatUShort3: return "UShort3";
        case MTLVertexFormatUShort4: return "UShort4";
        case MTLVertexFormatUShortNormalized: return "UShortNormalized";
        case MTLVertexFormatUShort2Normalized: return "UShort2Normalized";
        case MTLVertexFormatUShort3Normalized: return "UShort3Normalized";
        case MTLVertexFormatUShort4Normalized: return "UShort4Normalized";
        case MTLVertexFormatUInt1010102Normalized: return "UInt1010102Normalized";
        case MTLVertexFormatInt1010102Normalized: return "Int1010102Normalized";
        default: return "Unknown";
    }
}

bool mglIntegerAttribNeedsConversion(GLenum srcType,
                                     GLuint shaderGlType,
                                     GLuint size,
                                     MTLVertexFormat *outFormat)
{
    if (outFormat) {
        *outFormat = MTLVertexFormatInvalid;
    }
    if (size < 1u || size > 4u) {
        return false;
    }

    bool shaderIsInt = (shaderGlType == GL_INT || shaderGlType == GL_INT_VEC2 ||
                        shaderGlType == GL_INT_VEC3 || shaderGlType == GL_INT_VEC4);
    bool shaderIsUint = (shaderGlType == GL_UNSIGNED_INT ||
                         shaderGlType == GL_UNSIGNED_INT_VEC2 ||
                         shaderGlType == GL_UNSIGNED_INT_VEC3 ||
                         shaderGlType == GL_UNSIGNED_INT_VEC4);
    if (!shaderIsInt && !shaderIsUint) {
        return false;
    }

    bool srcUnsigned = (srcType == GL_UNSIGNED_BYTE ||
                        srcType == GL_UNSIGNED_SHORT ||
                        srcType == GL_UNSIGNED_INT);
    bool srcSigned = (srcType == GL_BYTE || srcType == GL_SHORT || srcType == GL_INT);

    bool needConv = (shaderIsInt && srcUnsigned) || (shaderIsUint && srcSigned);
    if (!needConv) {
        return false;
    }

    MTLVertexFormat f = MTLVertexFormatInvalid;
    if (shaderIsInt) {
        switch (size) {
            case 1: f = MTLVertexFormatInt; break;
            case 2: f = MTLVertexFormatInt2; break;
            case 3: f = MTLVertexFormatInt3; break;
            case 4: f = MTLVertexFormatInt4; break;
        }
    } else {
        switch (size) {
            case 1: f = MTLVertexFormatUInt; break;
            case 2: f = MTLVertexFormatUInt2; break;
            case 3: f = MTLVertexFormatUInt3; break;
            case 4: f = MTLVertexFormatUInt4; break;
        }
    }
    if (outFormat) {
        *outFormat = f;
    }
    return f != MTLVertexFormatInvalid;
}

double mglDecodeVertexAttribComponent(const uint8_t *src,
                                      GLenum type,
                                      GLboolean normalized,
                                      NSUInteger component)
{
    if (!src) {
        return 0.0;
    }

    switch (type) {
        case GL_FLOAT: {
            float v = 0.0f;
            memcpy(&v, src + component * sizeof(float), sizeof(v));
            return (double)v;
        }
        case GL_UNSIGNED_BYTE: {
            uint8_t v = 0;
            memcpy(&v, src + component, sizeof(v));
            return normalized ? ((double)v / 255.0) : (double)v;
        }
        case GL_BYTE: {
            int8_t v = 0;
            memcpy(&v, src + component, sizeof(v));
            if (normalized) {
                double d = (double)v / 127.0;
                return d < -1.0 ? -1.0 : d;
            }
            return (double)v;
        }
        case GL_UNSIGNED_SHORT: {
            uint16_t v = 0;
            memcpy(&v, src + component * sizeof(uint16_t), sizeof(v));
            return normalized ? ((double)v / 65535.0) : (double)v;
        }
        case GL_SHORT: {
            int16_t v = 0;
            memcpy(&v, src + component * sizeof(int16_t), sizeof(v));
            if (normalized) {
                double d = (double)v / 32767.0;
                return d < -1.0 ? -1.0 : d;
            }
            return (double)v;
        }
        case GL_UNSIGNED_INT: {
            uint32_t v = 0;
            memcpy(&v, src + component * sizeof(uint32_t), sizeof(v));
            return normalized ? ((double)v / 4294967295.0) : (double)v;
        }
        case GL_INT: {
            int32_t v = 0;
            memcpy(&v, src + component * sizeof(int32_t), sizeof(v));
            if (normalized) {
                double d = (double)v / 2147483647.0;
                return d < -1.0 ? -1.0 : d;
            }
            return (double)v;
        }
        default:
            return 0.0;
    }
}

/* === Pipeline signature === */

uint64_t mglVertexDescriptorSignature(MTLVertexDescriptor *vertexDescriptor)
{
    uint64_t hash = 1469598103934665603ull;
    if (!vertexDescriptor) {
        return hash;
    }

    for (NSUInteger i = 0; i < MAX_ATTRIBS; i++) {
        MTLVertexAttributeDescriptor *attrib = vertexDescriptor.attributes[i];
        if (!attrib) {
            continue;
        }
        hash = mglHashStepU64(hash, (uint64_t)attrib.format);
        hash = mglHashStepU64(hash, (uint64_t)attrib.offset);
        hash = mglHashStepU64(hash, (uint64_t)attrib.bufferIndex);
    }

    /* kMGLMaxMetalVertexBufferCount = 31 (Metal vertex buffer slots 0..30).
     * Referenced as literal to avoid a circular include with MGLRenderer.m,
     * which defines its own static const version.  See mgl_buffer_slots.h. */
    for (NSUInteger i = 0; i < 31u; i++) {
        MTLVertexBufferLayoutDescriptor *layout = vertexDescriptor.layouts[i];
        if (!layout) {
            continue;
        }
        hash = mglHashStepU64(hash, (uint64_t)layout.stride);
        hash = mglHashStepU64(hash, (uint64_t)layout.stepFunction);
        hash = mglHashStepU64(hash, (uint64_t)layout.stepRate);
    }

    return hash;
}

uint64_t mglPipelineDescriptorSignature(MTLRenderPipelineDescriptor *pipelineStateDescriptor)
{
    uint64_t hash = 1469598103934665603ull;
    if (!pipelineStateDescriptor) {
        return hash;
    }

    hash = mglHashStepU64(hash, (uint64_t)pipelineStateDescriptor.rasterSampleCount);
    hash = mglHashStepU64(hash, (uint64_t)pipelineStateDescriptor.rasterizationEnabled);
    hash = mglHashStepU64(hash, (uint64_t)pipelineStateDescriptor.alphaToCoverageEnabled);
    hash = mglHashStepU64(hash, (uint64_t)pipelineStateDescriptor.alphaToOneEnabled);
    hash = mglHashStepU64(hash, (uint64_t)pipelineStateDescriptor.depthAttachmentPixelFormat);
    hash = mglHashStepU64(hash, (uint64_t)pipelineStateDescriptor.stencilAttachmentPixelFormat);

    for (NSUInteger i = 0; i < MAX_COLOR_ATTACHMENTS; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *attachment = pipelineStateDescriptor.colorAttachments[i];
        if (!attachment) {
            continue;
        }
        hash = mglHashStepU64(hash, (uint64_t)attachment.pixelFormat);
        hash = mglHashStepU64(hash, (uint64_t)attachment.blendingEnabled);
        hash = mglHashStepU64(hash, (uint64_t)attachment.sourceRGBBlendFactor);
        hash = mglHashStepU64(hash, (uint64_t)attachment.destinationRGBBlendFactor);
        hash = mglHashStepU64(hash, (uint64_t)attachment.rgbBlendOperation);
        hash = mglHashStepU64(hash, (uint64_t)attachment.sourceAlphaBlendFactor);
        hash = mglHashStepU64(hash, (uint64_t)attachment.destinationAlphaBlendFactor);
        hash = mglHashStepU64(hash, (uint64_t)attachment.alphaBlendOperation);
        hash = mglHashStepU64(hash, (uint64_t)attachment.writeMask);
    }

    return hash;
}

MTLWinding mglMaybeInvertMTLWinding(MTLWinding winding, BOOL invert)
{
    if (!invert) {
        return winding;
    }
    return (winding == MTLWindingClockwise)
        ? MTLWindingCounterClockwise
        : MTLWindingClockwise;
}

MTLVertexFormat glTypeSizeToMtlType(GLuint type, GLuint size, bool normalized)
{
    switch(type)
    {
        case GL_UNSIGNED_BYTE:
            if (normalized)
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatUCharNormalized;
                    case 2: return MTLVertexFormatUChar2Normalized;
                    case 3: return MTLVertexFormatUChar3Normalized;
                    case 4: return MTLVertexFormatUChar4Normalized;
                }
            }
            else
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatUChar;
                    case 2: return MTLVertexFormatUChar2;
                    case 3: return MTLVertexFormatUChar3;
                    case 4: return MTLVertexFormatUChar4;
                }
            }
            break;

        case GL_BYTE:
            if (normalized)
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatCharNormalized;
                    case 2: return MTLVertexFormatChar2Normalized;
                    case 3: return MTLVertexFormatChar3Normalized;
                    case 4: return MTLVertexFormatChar4Normalized;
                }
            }
            else
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatChar;
                    case 2: return MTLVertexFormatChar2;
                    case 3: return MTLVertexFormatChar3;
                    case 4: return MTLVertexFormatChar4;
                }
            }
            break;

        case GL_UNSIGNED_SHORT:
            if (normalized)
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatUShortNormalized;
                    case 2: return MTLVertexFormatUShort2Normalized;
                    case 3: return MTLVertexFormatUShort3Normalized;
                    case 4: return MTLVertexFormatUShort4Normalized;
                }
            }
            else
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatUShort;
                    case 2: return MTLVertexFormatUShort2;
                    case 3: return MTLVertexFormatUShort3;
                    case 4: return MTLVertexFormatUShort4;
                }
            }
            break;

        case GL_SHORT:
            if (normalized)
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatShortNormalized;
                    case 2: return MTLVertexFormatShort2Normalized;
                    case 3: return MTLVertexFormatShort3Normalized;
                    case 4: return MTLVertexFormatShort4Normalized;
                }
            }
            else
            {
                switch(size)
                {
                    case 1: return MTLVertexFormatShort;
                    case 2: return MTLVertexFormatShort2;
                    case 3: return MTLVertexFormatShort3;
                    case 4: return MTLVertexFormatShort4;
                }
            }
            break;

            case GL_HALF_FLOAT:
                switch(size)
                {
                    case 1: return MTLVertexFormatHalf;
                    case 2: return MTLVertexFormatHalf2;
                    case 3: return MTLVertexFormatHalf3;
                    case 4: return MTLVertexFormatHalf4;
                }
                break;

            case GL_FLOAT:
                switch(size)
                {
                    case 1: return MTLVertexFormatFloat;
                    case 2: return MTLVertexFormatFloat2;
                    case 3: return MTLVertexFormatFloat3;
                    case 4: return MTLVertexFormatFloat4;
                }
                break;

            case GL_INT:
                switch(size)
                {
                    case 1: return MTLVertexFormatInt;
                    case 2: return MTLVertexFormatInt2;
                    case 3: return MTLVertexFormatInt3;
                    case 4: return MTLVertexFormatInt4;
                }
                break;

            case GL_UNSIGNED_INT:
                switch(size)
                {
                    case 1: return MTLVertexFormatUInt;
                    case 2: return MTLVertexFormatUInt2;
                    case 3: return MTLVertexFormatUInt3;
                    case 4: return MTLVertexFormatUInt4;
                }
                break;

            case GL_INT_2_10_10_10_REV:
                if (normalized)
                    return MTLVertexFormatInt1010102Normalized;
                break;

            case GL_UNSIGNED_INT_10_10_10_2:
            case GL_UNSIGNED_INT_2_10_10_10_REV:
                if (normalized)
                    return MTLVertexFormatUInt1010102Normalized;
                break;
        }

    return MTLVertexFormatInvalid;
}


MGLVertexAttributePlan mglVertexAttributePlan(const VertexAttrib *attrib,
                                             GLuint shaderType, bool currentValue)
{
    MGLVertexAttributePlan plan = {MTLVertexFormatInvalid, MGLVertexConversionNone};
    bool signedInput = shaderType == GL_INT || shaderType == GL_INT_VEC2 ||
                       shaderType == GL_INT_VEC3 || shaderType == GL_INT_VEC4;
    bool unsignedInput = shaderType == GL_UNSIGNED_INT || shaderType == GL_UNSIGNED_INT_VEC2 ||
                         shaderType == GL_UNSIGNED_INT_VEC3 || shaderType == GL_UNSIGNED_INT_VEC4;
    if (currentValue) {
        /* Current attributes are complete four-component values independent
         * of the disabled array's format, stride, offset or divisor. */
        plan.format = signedInput ? MTLVertexFormatInt4 : unsignedInput
            ? MTLVertexFormatUInt4 : MTLVertexFormatFloat4;
    } else if (attrib->type == GL_DOUBLE || (!attrib->integer &&
               (attrib->type == GL_INT || attrib->type == GL_UNSIGNED_INT))) {
        plan.format = mglDoubleVertexAttribFloatFormat(attrib->size);
        plan.conversion = MGLVertexConversionFloat;
    } else if (attrib->integer && mglIntegerAttribNeedsConversion(
                   attrib->type, shaderType, attrib->size, &plan.format)) {
        plan.conversion = signedInput ? MGLVertexConversionInt : MGLVertexConversionUInt;
    } else {
        plan.format = glTypeSizeToMtlType(attrib->type, attrib->size,
                                        !attrib->integer && attrib->normalized);
    }
    return plan;
}
