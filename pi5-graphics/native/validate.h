#pragma once
#include <stdint.h>
#include "abi.h"
namespace pi5 {
enum class ProgramStage : uint32_t { Pixel, Coordinate, Vertex };
enum class LookupForm : uint32_t { Sample, Fetch2D, Fetch1D };
struct TexturePatch {uint32_t config0,config1,binding;LookupForm form;bool constant;};
struct TexturePatches {uint32_t count=0;TexturePatch lookups[PI5_MAX_LOOKUPS]={};};
struct ProgramRules {
    ProgramStage stage=ProgramStage::Pixel;
    uint32_t vertexScalars=0,varyingScalars=0,nonPerspectiveMask=0,flatMask=0;
    uint32_t bindingKinds[PI5_BINDINGS]={};
    bool constants=false,targetReads=false;
};
// 'registers' receives the mask of registers the program writes or reads.
bool ValidateProgram(const uint64_t *code,uint32_t words,const uint32_t *uniforms,uint32_t uniformCount,
                     const ProgramRules &rules,TexturePatches *patches=nullptr,uint64_t *registers=nullptr,uint8_t *checkedUniforms=nullptr,uint32_t *bindingsUsed=nullptr);
// Caller serializes access. Zero-initialize once; entries own exact program and
// checked-uniform copies. Ordinary numeric uniforms do not affect validation;
// every token, VPM index and tile-buffer format word is compared exactly.
struct ProgramValidationCache {
    static constexpr uint32_t Entries=32,Words=2048,Uniforms=512;
    struct Entry {
        uint64_t hash,age;
        uint32_t words,uniforms,bindings;
        uint64_t registers;
        ProgramRules rules;
        TexturePatches patches;
        uint64_t code[Words];
        uint32_t values[Uniforms];
        uint8_t checked[Uniforms];
    } entries[Entries];
    uint64_t age;
    bool Validate(const uint64_t *code,uint32_t words,const uint32_t *uniforms,uint32_t uniformCount,
                  const ProgramRules &rules,TexturePatches *patches=nullptr,uint64_t *registers=nullptr,uint32_t *bindingsUsed=nullptr);
};
// Rules for program 0 (coordinate), 1 (vertex) or 2 (pixel) of a structurally checked draw.
ProgramRules DrawProgramRules(const Pi5DrawCommand &draw,unsigned program);
bool ValidateAllocation(const Pi5AllocationInfo &allocation);
bool ValidateCommand(const void *data,uint32_t bytes,const Pi5AllocationInfo *allocations,
                     uint32_t count,ProgramValidationCache *cache=nullptr);
}
