#include "mgl_spirv_sample_flip.h"

#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <new>
#include <set>
#include <utility>
#include <vector>

namespace {

using Inst = std::vector<uint32_t>; // opcode followed by operands
using Origins = std::set<uint32_t>;

static void logMessage(spv_message_level_t, const char *,
                       const spv_position_t &, const char *message)
{
    std::fprintf(stderr, "MGL sample-flip SPIR-V: %s\n",
                 message ? message : "(no detail)");
}

static void fail(const char *message)
{
    std::fprintf(stderr, "MGL sample-flip SPIR-V: %s\n", message);
}

static Inst makeInst(uint16_t op, std::initializer_list<uint32_t> args)
{
    Inst v;
    v.reserve(args.size() + 1);
    v.push_back(op);
    v.insert(v.end(), args.begin(), args.end());
    return v;
}

static uint16_t opcode(const Inst &i) { return static_cast<uint16_t>(i[0]); }

static bool isImageQuery(uint16_t op)
{
    return op == SpvOpImageQueryFormat || op == SpvOpImageQueryOrder ||
           op == SpvOpImageQuerySizeLod || op == SpvOpImageQuerySize ||
           op == SpvOpImageQueryLod || op == SpvOpImageQueryLevels ||
           op == SpvOpImageQuerySamples;
}

static bool isSampling(uint16_t op)
{
    switch (op) {
        case SpvOpImageSampleImplicitLod:
        case SpvOpImageSampleExplicitLod:
        case SpvOpImageSampleDrefImplicitLod:
        case SpvOpImageSampleDrefExplicitLod:
        case SpvOpImageSampleProjImplicitLod:
        case SpvOpImageSampleProjExplicitLod:
        case SpvOpImageSampleProjDrefImplicitLod:
        case SpvOpImageSampleProjDrefExplicitLod:
        case SpvOpImageFetch:
            return true;
        default: return false;
    }
}

static bool isProj(uint16_t op)
{
    return op == SpvOpImageSampleProjImplicitLod ||
           op == SpvOpImageSampleProjExplicitLod ||
           op == SpvOpImageSampleProjDrefImplicitLod ||
           op == SpvOpImageSampleProjDrefExplicitLod;
}

struct ImageType { uint32_t dim=0, depth=0, arrayed=0, ms=0, sampled=0; };
struct TypeInfo { uint32_t component=0, count=0, width=0; bool isFloat=false, isSigned=false; };
struct Resource { uint32_t id=0, imageType=0; bool eligible=false, rejected=false; };

// This transform intentionally follows only simple, statically evident opaque
// value flow. Anything more complex remains on the renderer's mirror path.
static bool parseModule(const std::vector<uint32_t> &words, std::vector<Inst> &ins)
{
    if (words.size() < 5 || words[0] != SpvMagicNumber) return false;
    for (size_t p=5; p<words.size();) {
        uint32_t h=words[p]; uint32_t n=h>>16;
        if (!n || p+n>words.size()) return false;
        Inst i; i.reserve(n); i.push_back(h&0xffffu);
        i.insert(i.end(), words.begin()+p+1, words.begin()+p+n);
        ins.push_back(std::move(i)); p+=n;
    }
    return true;
}

static void appendWords(std::vector<uint32_t> &out, const Inst &i)
{
    out.push_back((static_cast<uint32_t>(i.size())<<16) | opcode(i));
    out.insert(out.end(), i.begin()+1, i.end());
}

static uint32_t resultId(const Inst &i)
{
    uint16_t op=opcode(i);
    // Only operations which can propagate opaque values are needed here.
    switch (op) {
        case SpvOpLoad: case SpvOpSampledImage: case SpvOpImage:
        case SpvOpCopyObject: case SpvOpPhi: case SpvOpSelect:
        case SpvOpAccessChain: case SpvOpInBoundsAccessChain:
        case SpvOpPtrAccessChain: case SpvOpInBoundsPtrAccessChain:
            return i.size()>2 ? i[2] : 0;
        default: return 0;
    }
}

static bool hasOrigin(const Inst &i, const std::map<uint32_t,Origins> &origins,
                      Origins *combined)
{
    bool found=false;
    for (size_t n=1;n<i.size();++n) {
        auto it=origins.find(i[n]);
        if (it!=origins.end()) { found=true; if(combined) combined->insert(it->second.begin(),it->second.end()); }
    }
    return found;
}

static bool supportedSample(uint16_t op, const Inst &i, size_t &coordIndex,
                            size_t &maskIndex, SpvImageOperandsMask &mask)
{
    if (!isSampling(op) || op==SpvOpImageFetch || i.size()<5) return false;
    if(op==SpvOpImageSampleProjDrefImplicitLod||op==SpvOpImageSampleProjDrefExplicitLod) return false;
    coordIndex=4; // result type/id, sampled image, coordinate
    bool dref = op==SpvOpImageSampleDrefImplicitLod || op==SpvOpImageSampleDrefExplicitLod ||
                op==SpvOpImageSampleProjDrefImplicitLod || op==SpvOpImageSampleProjDrefExplicitLod;
    if (dref) coordIndex=4; // dref follows coordinate
    maskIndex=coordIndex+(dref?2:1);
    mask=SpvImageOperandsMaskNone;
    if(maskIndex<i.size()) {
        mask=static_cast<SpvImageOperandsMask>(i[maskIndex]);
        // Only one supported operand can be present. Offsets, min-lod, sample,
        // visibility and extension operands are deliberately excluded.
        if (op==SpvOpImageSampleImplicitLod || op==SpvOpImageSampleDrefImplicitLod ||
            op==SpvOpImageSampleProjImplicitLod || op==SpvOpImageSampleProjDrefImplicitLod) {
            if(mask!=SpvImageOperandsMaskNone && mask!=SpvImageOperandsBiasMask) return false;
        } else {
            if(mask!=SpvImageOperandsLodMask && mask!=SpvImageOperandsGradMask) return false;
        }
        if(isProj(op)&&mask==SpvImageOperandsGradMask) return false;
        uint32_t expected=maskIndex+1;
        if(mask==SpvImageOperandsBiasMask||mask==SpvImageOperandsLodMask) expected+=1;
        if(mask==SpvImageOperandsGradMask) expected+=2;
        if(i.size()!=expected) return false;
    }
    return true;
}

static bool typeVector(const std::map<uint32_t,TypeInfo> &types,uint32_t id,
                       uint32_t count, bool wantFloat)
{
    auto it=types.find(id);
    return it!=types.end() && it->second.count==count &&
           it->second.width==32 && it->second.isFloat==wantFloat &&
           (wantFloat || it->second.isSigned);
}

} // namespace

extern "C" bool mglTransformSPIRVSampleFlip(
    const uint32_t *words, size_t word_count,
    uint32_t **out_words, size_t *out_word_count,
    MGLSpirvSampleFlipResource *out_resources, size_t resource_capacity,
    size_t *out_resource_count, MGLSpirvSampleFlipStats *out_stats)
{
    if (out_words) *out_words=nullptr;
    if (out_word_count) *out_word_count=0;
    if (out_resource_count) *out_resource_count=0;
    if (out_stats) std::memset(out_stats,0,sizeof(*out_stats));
    if (!words || !word_count || !out_words || !out_word_count ||
        !out_resource_count || (!out_resources && resource_capacity)) {
        fail("invalid arguments"); return false;
    }

    try {
        // Inline only opaque-resource helper functions; preserve the original
        // module and all IDs/names in the caller's storage.
        std::vector<uint32_t> inlined;
        spvtools::Optimizer optimizer(SPV_ENV_OPENGL_4_5);
        optimizer.SetMessageConsumer(logMessage);
        optimizer.RegisterPass(spvtools::CreateInlineOpaquePass());
        spvtools::OptimizerOptions options;
        options.set_preserve_bindings(true);
        options.set_preserve_spec_constants(true);
        options.set_run_validator(true);
        if (!optimizer.Run(words,word_count,&inlined,options) || inlined.empty()) {
            fail("opaque-function inlining failed"); return false;
        }

        std::vector<Inst> ins;
        if(!parseModule(inlined,ins)) { fail("malformed module"); return false; }
        std::map<uint32_t,ImageType> imageTypes;
        std::map<uint32_t,uint32_t> sampledImageTypes, pointerPointee, valueTypes;
        std::map<uint32_t,TypeInfo> types;
        std::map<uint32_t,Resource> resources;
        std::set<uint32_t> usedSpecIds;
        uint32_t bound=inlined[3], boolType=0, floatType=0, intType=0, vec2Int=0;
        for(const Inst &i:ins) {
            uint16_t op=opcode(i);
            bool hasResult=false,hasType=false;
            SpvHasResultAndType(static_cast<SpvOp>(op),&hasResult,&hasType);
            if(hasResult&&hasType&&i.size()>=3) valueTypes[i[2]]=i[1];
            if(op==SpvOpTypeFloat && i.size()==3) { types[i[1]]={0,1,i[2],true,false}; if(i[2]==32) floatType=i[1]; }
            else if(op==SpvOpTypeInt && i.size()==4) { types[i[1]]={0,1,i[2],false,i[3]!=0}; if(i[2]==32&&i[3]) intType=i[1]; }
            else if(op==SpvOpTypeBool && i.size()==2) boolType=i[1];
            else if(op==SpvOpTypeVector && i.size()==4) {
                auto c=types.find(i[2]); if(c!=types.end()) types[i[1]]={i[2],i[3],c->second.width,c->second.isFloat,c->second.isSigned};
                if(c!=types.end()&&c->second.width==32&&!c->second.isFloat&&c->second.isSigned&&i[3]==2)vec2Int=i[1];
            } else if(op==SpvOpTypeImage && i.size()>=9) imageTypes[i[1]]={i[3],i[4],i[5],i[6],i[7]};
            else if(op==SpvOpTypeSampledImage&&i.size()==3) sampledImageTypes[i[1]]=i[2];
            else if(op==SpvOpTypePointer&&i.size()==4) pointerPointee[i[1]]=i[3];
            else if(op==SpvOpDecorate&&i.size()>=4&&i[2]==SpvDecorationSpecId) usedSpecIds.insert(i[3]);
            else if((op==SpvOpLoad||op==SpvOpSampledImage||op==SpvOpImage||op==SpvOpCopyObject)&&i.size()>=3) valueTypes[i[2]]=i[1];
            else if(op==SpvOpVariable&&i.size()>=4&&i[3]==SpvStorageClassUniformConstant) {
                uint32_t pointee=pointerPointee[i[1]], imageId=pointee;
                auto sit=sampledImageTypes.find(pointee); if(sit!=sampledImageTypes.end()) imageId=sit->second;
                auto im=imageTypes.find(imageId);
                if(im!=imageTypes.end()) {
                    Resource r; r.id=i[2];r.imageType=imageId;
                    r.eligible=im->second.dim==SpvDim2D&&im->second.arrayed==0&&im->second.ms==0&&im->second.sampled==1&&im->second.depth!=2;
                    resources[r.id]=r;
                }
            }
        }
        std::map<uint32_t,Origins> origins;
        for(auto &r:resources) origins[r.first].insert(r.first);
        std::map<uint32_t,bool> rejected;
        std::map<uint32_t,uint32_t> sampleCounts,fetchCounts;
        auto mergeOrigin=[&](uint32_t destination,uint32_t source) {
            auto it=origins.find(source); if(!destination||it==origins.end()) return false;
            Origins &to=origins[destination]; size_t old=to.size(); to.insert(it->second.begin(),it->second.end()); return to.size()!=old;
        };
        // Resolve opaque SSA flow before classifying uses. Phi/select operands
        // can legally reference values defined later in the same block.
        bool changed=true; size_t passes=0;
        while(changed&&passes++<=ins.size()) {
            changed=false; bool active=false;
            for(const Inst &i:ins) {
                uint16_t op=opcode(i); if(op==SpvOpFunction) active=true; if(!active) continue;
                uint32_t rid=resultId(i);
                if(op==SpvOpLoad&&i.size()>=4) changed|=mergeOrigin(rid,i[3]);
                else if((op==SpvOpSampledImage||op==SpvOpImage||op==SpvOpCopyObject)&&i.size()>=4) changed|=mergeOrigin(rid,i[3]);
                else if((op==SpvOpAccessChain||op==SpvOpInBoundsAccessChain||op==SpvOpPtrAccessChain||op==SpvOpInBoundsPtrAccessChain)&&i.size()>=4) changed|=mergeOrigin(rid,i[3]);
                else if(op==SpvOpSelect&&i.size()>=6) {changed|=mergeOrigin(rid,i[4]);changed|=mergeOrigin(rid,i[5]);}
                else if(op==SpvOpPhi&&i.size()>=5) for(size_t n=3;n<i.size();n+=2) changed|=mergeOrigin(rid,i[n]);
            }
        }
        auto originsAt=[&](const Inst &i,size_t operand,Origins *out) {
            if(operand>=i.size()) return false;
            auto it=origins.find(i[operand]); if(it==origins.end()) return false;
            if(out) out->insert(it->second.begin(),it->second.end()); return true;
        };
        bool inFunction=false;
        for(const Inst &i:ins) {
            uint16_t op=opcode(i); uint32_t rid=resultId(i);
            if(op==SpvOpFunction) inFunction=true;
            if(!inFunction) continue; // Names, decorations, entry-point interfaces and globals are not value flow.
            if(op==SpvOpLoad||op==SpvOpSampledImage||op==SpvOpImage||op==SpvOpCopyObject) continue;
            if(op==SpvOpAccessChain||op==SpvOpInBoundsAccessChain||op==SpvOpPtrAccessChain||op==SpvOpInBoundsPtrAccessChain||op==SpvOpPhi||op==SpvOpSelect) {
                Origins o; if(rid) o=origins[rid]; for(uint32_t r:o) rejected[r]=true;
                continue;
            }
            Origins used;
            if(isSampling(op)||isImageQuery(op)) originsAt(i,3,&used);
            else hasOrigin(i,origins,&used);
            if(used.empty()) continue;
            if(isImageQuery(op)) continue;
            if(isSampling(op)) {
                if(used.size()!=1) {for(uint32_t r:used)rejected[r]=true;continue;}
                if(i.size()<4) {for(uint32_t r:used)rejected[r]=true;continue;}
                for(uint32_t r:used) {
                    auto ri=resources.find(r); if(ri==resources.end()||!ri->second.eligible) {rejected[r]=true;continue;}
            if(op==SpvOpImageFetch) {
                        if(i.size()<5) {rejected[r]=true;continue;}
                        auto coordTypeIt=valueTypes.find(i[4]);
                        if(coordTypeIt==valueTypes.end() || !typeVector(types,coordTypeIt->second,2,false) ||
                           (i.size()>5 && (i[5]!=SpvImageOperandsLodMask || i.size()!=7))) {rejected[r]=true;continue;}
                        ++fetchCounts[r];
                    } else {
                        size_t ci=0,mi=0; SpvImageOperandsMask mask;
                        if(!supportedSample(op,i,ci,mi,mask)) {rejected[r]=true;continue;}
                        auto coordType=valueTypes.find(i[ci]);
                        if(coordType==valueTypes.end()||!typeVector(types,coordType->second,isProj(op)?3:2,true)) {rejected[r]=true;continue;}
                        if(mask==SpvImageOperandsGradMask) {
                            auto gx=valueTypes.find(i[mi+1]), gy=valueTypes.find(i[mi+2]);
                            if(gx==valueTypes.end()||gy==valueTypes.end()||!typeVector(types,gx->second,2,true)||!typeVector(types,gy->second,2,true)) {rejected[r]=true;continue;}
                        }
                        ++sampleCounts[r];
                    }
                }
                continue;
            }
            // Any non-query operation which consumes an opaque-derived ID is
            // outside the understood flow (including gather, stores, calls).
            for(uint32_t r:used) rejected[r]=true;
        }

        std::vector<uint32_t> accepted;
        for(auto &p:resources) if(p.second.eligible&&!rejected[p.first]&&(sampleCounts[p.first]||fetchCounts[p.first])) accepted.push_back(p.first);
        if(out_stats) {out_stats->candidate_resources=static_cast<uint32_t>(resources.size());out_stats->rejected_resources=static_cast<uint32_t>(resources.size()-accepted.size());}
        if(accepted.size()>resource_capacity||accepted.size()>MGL_SPIRV_SAMPLE_FLIP_MAX_RESOURCES) {
            accepted.resize(std::min(resource_capacity,static_cast<size_t>(MGL_SPIRV_SAMPLE_FLIP_MAX_RESOURCES)));
        }
        if(accepted.empty()) {
            const size_t bytes=inlined.size()*sizeof(uint32_t); auto *copy=static_cast<uint32_t*>(std::malloc(bytes));
            if(!copy) {fail("allocation failed");return false;} std::memcpy(copy,inlined.data(),bytes); *out_words=copy;*out_word_count=inlined.size(); return true;
        }

        uint32_t next=bound;
        auto alloc=[&](){return next++;};
        if(!boolType) boolType=alloc();
        uint32_t oneFloat=0, oneInt=0, zeroInt=0;
        std::map<uint32_t,uint32_t> specFor;
        std::vector<Inst> annotations, typeDeclarations, declarations, preFunction;
        if(std::find_if(ins.begin(),ins.end(),[](const Inst&i){return opcode(i)==SpvOpTypeBool;})==ins.end()) typeDeclarations.push_back(makeInst(SpvOpTypeBool,{boolType}));
        bool needsImageQuery=std::any_of(accepted.begin(),accepted.end(),[&](uint32_t r){return fetchCounts[r]!=0;});
        bool needsSample=std::any_of(accepted.begin(),accepted.end(),[&](uint32_t r){return sampleCounts[r]!=0;});
        if(needsSample) {
            if(!floatType) {fail("32-bit float type unavailable for sample coordinates");return false;}
            oneFloat=alloc();
            declarations.push_back(makeInst(SpvOpConstant,{floatType,oneFloat,0x3f800000u}));
        }
        if(needsImageQuery) {
            oneInt=alloc(); zeroInt=alloc();
            declarations.push_back(makeInst(SpvOpConstant,{intType,oneInt,1u}));
            declarations.push_back(makeInst(SpvOpConstant,{intType,zeroInt,0u}));
        }
        bool hasImageQuery=std::any_of(ins.begin(),ins.end(),[](const Inst&i){return opcode(i)==SpvOpCapability&&i.size()>1&&i[1]==SpvCapabilityImageQuery;});
        if(needsImageQuery&&!hasImageQuery) preFunction.push_back(makeInst(SpvOpCapability,{SpvCapabilityImageQuery}));
        for(uint32_t r:accepted) {
            uint32_t sid=0; while(usedSpecIds.count(sid)) ++sid; usedSpecIds.insert(sid);
            uint32_t spec=alloc(); specFor[r]=spec;
            annotations.push_back(makeInst(SpvOpDecorate,{spec,SpvDecorationSpecId,sid}));
            declarations.push_back(makeInst(SpvOpSpecConstantFalse,{boolType,spec}));
            if(*out_resource_count<MGL_SPIRV_SAMPLE_FLIP_MAX_RESOURCES && *out_resource_count<resource_capacity) {
                out_resources[*out_resource_count]={r,sid}; ++*out_resource_count;
            }
        }
        std::vector<Inst> transformed;
        bool insertedAnn=false,insertedTypes=false,insertedDecl=false,insertedCap=false;
        for(const Inst &orig:ins) {
            uint16_t op=opcode(orig);
            if(!insertedCap&&op!=SpvOpCapability) {transformed.insert(transformed.end(),preFunction.begin(),preFunction.end());insertedCap=true;}
            if(!insertedAnn&&(op==SpvOpTypeVoid||op==SpvOpTypeBool||op==SpvOpTypeInt||op==SpvOpTypeFloat||op==SpvOpTypeVector||op==SpvOpTypeImage||op==SpvOpTypeSampler||op==SpvOpTypeSampledImage||op==SpvOpTypeArray||op==SpvOpTypeRuntimeArray||op==SpvOpTypeStruct||op==SpvOpTypePointer||op==SpvOpTypeFunction)) {transformed.insert(transformed.end(),annotations.begin(),annotations.end());insertedAnn=true;}
            if(!insertedTypes&&op==SpvOpFunction) {transformed.insert(transformed.end(),typeDeclarations.begin(),typeDeclarations.end());insertedTypes=true;}
            if(!insertedDecl&&op==SpvOpFunction) {transformed.insert(transformed.end(),declarations.begin(),declarations.end());insertedDecl=true;}
            Inst i=orig; Origins used;
            if(isSampling(op)) originsAt(orig,3,&used);
            if(isSampling(op)&&!used.empty()) {
                uint32_t root=*used.begin(); auto sf=specFor.find(root);
                if(sf!=specFor.end()) {
                    if(op==SpvOpImageFetch) {
                        uint32_t coord=i[4], lod=(i.size()>6)?i[6]:0;
                        uint32_t imageId=i[3], queryImage=imageId;
                        auto vt=valueTypes.find(imageId);
                        if(vt!=valueTypes.end()) {
                            auto si=sampledImageTypes.find(vt->second);
                            if(si!=sampledImageTypes.end()) {
                                uint32_t extracted=alloc();
                                transformed.push_back(makeInst(SpvOpImage,{si->second,extracted,imageId}));
                                queryImage=extracted;
                            }
                        }
                        uint32_t vec=alloc(),height=alloc(),base=alloc(),originalY=alloc(),flippedY=alloc(),sel=alloc(),newCoord=alloc();
                        transformed.push_back(makeInst(SpvOpImageQuerySizeLod,{vec2Int,vec,queryImage,lod?lod:zeroInt}));
                        transformed.push_back(makeInst(SpvOpCompositeExtract,{intType,height,vec,1}));
                        transformed.push_back(makeInst(SpvOpISub,{intType,base,height,oneInt}));
                        transformed.push_back(makeInst(SpvOpCompositeExtract,{intType,originalY,coord,1}));
                        transformed.push_back(makeInst(SpvOpISub,{intType,flippedY,base,originalY}));
                        transformed.push_back(makeInst(SpvOpSelect,{intType,sel,sf->second,flippedY,originalY}));
                        transformed.push_back(makeInst(SpvOpCompositeInsert,{vec2Int,newCoord,sel,coord,1})); i[4]=newCoord;
                        if(out_stats) ++out_stats->rewritten_fetch_ops;
                    } else {
                        size_t ci=0,mi=0;SpvImageOperandsMask mask; supportedSample(op,i,ci,mi,mask);
                        uint32_t coord=i[ci], originalY=alloc(), transformedY=alloc(), select=alloc(), newCoord=alloc();
                        auto coordTypeIt=valueTypes.find(coord);
                        if(coordTypeIt==valueTypes.end()) {rejected[root]=true;continue;}
                        uint32_t coordType=coordTypeIt->second;
                        transformed.push_back(makeInst(SpvOpCompositeExtract,{floatType,originalY,coord,1u}));
                        if(isProj(op)) {
                            uint32_t q=alloc(); transformed.push_back(makeInst(SpvOpCompositeExtract,{floatType,q,coord,2u}));
                            transformed.push_back(makeInst(SpvOpFSub,{floatType,transformedY,q,originalY}));
                        } else transformed.push_back(makeInst(SpvOpFSub,{floatType,transformedY,oneFloat,originalY}));
                        transformed.push_back(makeInst(SpvOpSelect,{floatType,select,sf->second,transformedY,originalY}));
                        transformed.push_back(makeInst(SpvOpCompositeInsert,{coordType,newCoord,select,coord,1})); i[ci]=newCoord;
                        if(mask==SpvImageOperandsGradMask) {
                            for(size_t gi=mi+1;gi<=mi+2;++gi) {
                                auto gradTypeIt=valueTypes.find(i[gi]);
                                if(gradTypeIt==valueTypes.end()||!typeVector(types,gradTypeIt->second,2,true)) {rejected[root]=true;break;}
                                uint32_t gy=alloc(), neg=alloc(), gv=alloc();
                                transformed.push_back(makeInst(SpvOpCompositeExtract,{floatType,gy,i[gi],1}));
                                transformed.push_back(makeInst(SpvOpFNegate,{floatType,neg,gy}));
                                // The specialization false arm must preserve the original gradient.
                                uint32_t gs=alloc(); transformed.push_back(makeInst(SpvOpSelect,{floatType,gs,sf->second,neg,gy}));
                                transformed.push_back(makeInst(SpvOpCompositeInsert,{gradTypeIt->second,gv,gs,i[gi],1})); i[gi]=gv;
                            }
                        }
                        if(out_stats) ++out_stats->rewritten_sample_ops;
                    }
                }
            }
            transformed.push_back(std::move(i));
        }
        if(!insertedCap) transformed.insert(transformed.end(),preFunction.begin(),preFunction.end());
        if(!insertedAnn) transformed.insert(transformed.begin(),annotations.begin(),annotations.end());
        if(!insertedTypes) transformed.insert(transformed.end(),typeDeclarations.begin(),typeDeclarations.end());
        if(!insertedDecl) transformed.insert(transformed.end(),declarations.begin(),declarations.end());

        std::vector<uint32_t> result(inlined.begin(),inlined.begin()+5); result[3]=next;
        for(const Inst &i:transformed) appendWords(result,i);
        spvtools::SpirvTools validator(SPV_ENV_OPENGL_4_5); validator.SetMessageConsumer(logMessage);
        if(!validator.Validate(result.data(),result.size())) {fail("transformed module failed OpenGL 4.5 validation");*out_resource_count=0;return false;}
        if(result.size()>static_cast<size_t>(-1)/sizeof(uint32_t)) {fail("result size overflow");*out_resource_count=0;return false;}
        auto *published=static_cast<uint32_t*>(std::malloc(result.size()*sizeof(uint32_t)));
        if(!published) {fail("allocation failed");*out_resource_count=0;return false;}
        std::memcpy(published,result.data(),result.size()*sizeof(uint32_t));
        *out_words=published;*out_word_count=result.size();
        if(out_stats) {out_stats->accepted_resources=static_cast<uint32_t>(*out_resource_count);out_stats->rejected_resources=out_stats->candidate_resources-out_stats->accepted_resources;}
        return true;
    } catch(const std::bad_alloc &) {fail("out of memory");*out_resource_count=0;return false;}
      catch(...) {fail("unexpected transform failure");*out_resource_count=0;return false;}
}
