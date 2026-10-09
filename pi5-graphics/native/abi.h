#pragma once
#include <stdint.h>
#define PI5_UMD_MAGIC 0x55443550u
#define PI5_UMD_ABI 21u
#define PI5_TEXTURE_TOKEN 0xf005000fu
#define PI5_SAMPLER_TOKEN 0xf0050021u
#define PI5_CONSTANT_TEXTURE_TOKEN 0xf005010fu
#define PI5_CONSTANT_SAMPLER_TOKEN 0xf0050121u
#define PI5_BINDING_TOKEN_SHIFT 12u
#define PI5_BINDINGS 4u
#define PI5_MAX_ALLOCATIONS (2u+PI5_BINDINGS)
#define PI5_MAX_BATCH_DRAWS 32u
#define PI5_MAX_REFERENCES 32u
#define PI5_MAX_LEVELS 13u
// This compiler keeps live vertex inputs in the 32-register QPU bank.
// V3D 7.1 can expose more attributes, but 32 scalar inputs are the safe
// implementation limit until the UMD grows vertex-input spilling.
#define PI5_MAX_VERTEX_SCALARS 32u
// One flat-shade or non-perspective flags packet covers 24 varyings.
#define PI5_MAX_VARYINGS 24u
#define PI5_MAX_PROGRAM_WORDS 32768u
#define PI5_MAX_PROGRAM_UNIFORMS 8191u
#define PI5_MAX_LOOKUPS 96u
// Three uniform streams precede aligned texture/sampler descriptors.
#define PI5_UNIFORM_DESCRIPTORS 98304u
#define PI5_MAX_BYTE_ELEMENTS (16u*1024u*1024u)
#define PI5_UMD_COMMAND_BYTES (256u*1024u)
#define PI5_UMD_RESOURCE_BYTES (64u*1024u*1024u)
#define PI5_ADAPTER_NATIVE_DISPLAY 1u
#define PI5_RESOURCE_DISPLAYABLE 0x20000u
#define PI5_ALLOCATION_GPU_ONLY 1u
struct Pi5AdapterInfo {uint32_t Magic,Version,V3dVersion,Caps;};
#define PI5_SCHEDULER_QUERY_MAGIC 0x50355343u
struct Pi5SchedulerStatus {
    uint32_t Magic,Version,Submitted,Completed,Fault,QueueHighWater,Head,Tail;
    uint32_t Diagnostics[12];
};
static_assert(sizeof(Pi5SchedulerStatus)==80,"scheduler diagnostic ABI layout");
struct Pi5AllocationInfo {
    uint32_t Version,Bytes,Width,Height,Pitch,Format,BindFlags,Dimension;
    uint32_t MiscFlags=0,Levels=1,PrivateFlags=0,Reserved=0;
    uint32_t RefreshNumerator=0,RefreshDenominator=0;
};
inline bool Pi5CpuInvisible(const Pi5AllocationInfo&info){return (info.MiscFlags&0x102u)||(info.PrivateFlags&PI5_ALLOCATION_GPU_ONLY);}
// Texture mip levels are stored consecutively in the allocation. Level 0 uses
// the allocation pitch; smaller levels use 64-byte aligned pitches and start
// on 256-byte boundaries.
struct Pi5Level {uint32_t Offset,Pitch,Width,Height;};
// Decode bounded nonnegative IEEE floats without using kernel floating point.
// The result preserves 16 fractional bits for viewport-centre calculation.
inline bool Pi5ViewportFixed(uint32_t bits,uint32_t &fixed){
    if((bits&0x7fffffff)==0){fixed=0;return true;}
    if(bits>0x45800000u)return false; // finite, nonnegative, at most 4096
    int shift=int((bits>>23)&255)-127-7;
    uint32_t mantissa=(bits&0x7fffff)|0x800000;
    fixed=shift>=0?mantissa<<shift:shift<=-32?0:mantissa>>-shift;
    return true;
}
inline bool Pi5ViewportRect(const uint32_t bits[4],uint32_t width,uint32_t height,uint32_t fixed[4]){
    for(unsigned i=0;i<4;++i)if(!Pi5ViewportFixed(bits[i],fixed[i]))return false;
    return fixed[2]&&fixed[3]&&uint64_t(fixed[0])+fixed[2]<=(uint64_t(width)<<16)&&
           uint64_t(fixed[1])+fixed[3]<=(uint64_t(height)<<16);
}
inline uint64_t Pi5LevelChain(uint32_t width,uint32_t height,uint32_t pitch,uint32_t levels,uint32_t level,Pi5Level *out){
    uint64_t offset=0;
    for(uint32_t i=0;i<levels;++i){
        if(i==level&&out){out->Offset=static_cast<uint32_t>(offset);out->Pitch=pitch;out->Width=width;out->Height=height;}
        uint64_t end=offset+uint64_t(pitch)*height;if(i+1==levels)return end;
        offset=(end+255)&~uint64_t(255);width=width>1?width/2:1;height=height>1?height/2:1;pitch=(width*4+63)&~63u;
    }
    return 0;
}
inline bool Pi5AllocationLevel(const Pi5AllocationInfo &r,uint32_t level,Pi5Level &out){
    if(!r.Levels||level>=r.Levels)return false;
    if(r.Dimension!=3){out={0,r.Pitch,r.Width,r.Height};return !level;}
    return Pi5LevelChain(r.Width,r.Height,r.Pitch,r.Levels,level,&out)<=r.Bytes;
}
enum Pi5Operation : uint32_t {Pi5Clear=1,Pi5Copy=2,Pi5Draw=3,Pi5CopyRegion=4,Pi5DrawBatch=5};
struct Pi5CommandHeader {uint32_t Magic,Version,Bytes,Operation;};
struct Pi5ClearCommand {Pi5CommandHeader Header;uint32_t Target,Color,Reserved[2];};
struct Pi5CopyCommand {Pi5CommandHeader Header;uint32_t Destination,Source,Reserved[2];};
struct Pi5RegionCommand {Pi5CommandHeader Header;uint32_t Destination,Source,SourceX,SourceY,DestinationX,DestinationY,Width,Height,DestinationLevel,SourceLevel,Reserved[2];};
struct Pi5Program {uint32_t CodeOffset,CodeCount,UniformOffset,UniformCount,ConstantOffset,ConstantWords;};
struct Pi5Pipeline {
    uint32_t Flags=4,CullMode=1,ColorMask=15; // blend, front-CCW, depth-clip, scissor
    uint32_t Scissor[4]={},Blend[6]={1,0,0,1,0,0},Reserved=0;
    uint16_t Constant[4]={};
    uint32_t Coverage[4]={};
};
#define PI5_PIPELINE_SHADER_BLEND 16u
#define PI5_PIPELINE_COVERAGE 32u
inline bool Pi5ValidCoverage(const Pi5Pipeline&s,uint32_t width,uint32_t height){
    if(!(s.Flags&PI5_PIPELINE_COVERAGE)){for(auto v:s.Coverage)if(v)return false;return true;}
    if((s.Flags&(1|PI5_PIPELINE_SHADER_BLEND))||s.ColorMask!=15||s.Coverage[0]>=s.Coverage[2]||s.Coverage[1]>=s.Coverage[3]||s.Coverage[2]>width||s.Coverage[3]>height)return false;
    if(s.Flags&8)for(unsigned axis=0;axis<2;++axis)if(s.Coverage[axis]<s.Scissor[axis]||s.Coverage[axis+2]>s.Scissor[axis+2])return false;
    return true;
}
enum Pi5BindingKind : uint32_t {
    Pi5BindingTexture=1,Pi5BindingBytes,Pi5BindingUshorts,Pi5BindingShorts,
    Pi5BindingUshort2,Pi5BindingShort2,Pi5BindingUshort4,Pi5BindingShort4,
    Pi5BindingUint,Pi5BindingInt,Pi5BindingSbyte,Pi5BindingUbyte2,Pi5BindingSbyte2,
    Pi5BindingUbyte4,Pi5BindingSbyte4,Pi5BindingUint2,Pi5BindingInt2,Pi5BindingUint4,Pi5BindingInt4
};
inline uint32_t Pi5BufferComponents(uint32_t kind){
    switch(kind){case Pi5BindingBytes:case Pi5BindingUshorts:case Pi5BindingShorts:case Pi5BindingUint:case Pi5BindingInt:case Pi5BindingSbyte:return 1;
    case Pi5BindingUshort2:case Pi5BindingShort2:case Pi5BindingUbyte2:case Pi5BindingSbyte2:case Pi5BindingUint2:case Pi5BindingInt2:return 2;
    case Pi5BindingUshort4:case Pi5BindingShort4:case Pi5BindingUbyte4:case Pi5BindingSbyte4:case Pi5BindingUint4:case Pi5BindingInt4:return 4;default:return 0;}
}
inline uint32_t Pi5BufferComponentBytes(uint32_t kind){
    switch(kind){case Pi5BindingBytes:case Pi5BindingSbyte:case Pi5BindingUbyte2:case Pi5BindingSbyte2:case Pi5BindingUbyte4:case Pi5BindingSbyte4:return 1;
    case Pi5BindingUshorts:case Pi5BindingShorts:case Pi5BindingUshort2:case Pi5BindingShort2:case Pi5BindingUshort4:case Pi5BindingShort4:return 2;
    case Pi5BindingUint:case Pi5BindingInt:case Pi5BindingUint2:case Pi5BindingInt2:case Pi5BindingUint4:case Pi5BindingInt4:return 4;default:return 0;}
}
inline uint32_t Pi5BufferElementBytes(uint32_t kind){return Pi5BufferComponents(kind)*Pi5BufferComponentBytes(kind);}
inline bool Pi5BufferSigned(uint32_t kind){return kind==Pi5BindingShorts||kind==Pi5BindingShort2||kind==Pi5BindingShort4||kind==Pi5BindingInt||kind==Pi5BindingInt2||kind==Pi5BindingInt4||kind==Pi5BindingSbyte||kind==Pi5BindingSbyte2||kind==Pi5BindingSbyte4;}
#define PI5_FILTER_MAG_LINEAR 1u
#define PI5_FILTER_MIN_LINEAR 2u
#define PI5_FILTER_MIP_LINEAR 4u
enum Pi5AddressMode : uint32_t {Pi5Clamp=0,Pi5Border=1,Pi5Wrap=2,Pi5Mirror=3,Pi5MirrorOnce=4};
inline bool Pi5ValidAddressModes(uint32_t modes){return !(modes&~63u)&&(modes&7)<=Pi5MirrorOnce&&(modes>>3)<=Pi5MirrorOnce;}
inline bool Pi5UsesBorder(uint32_t modes){return (modes&7)==Pi5Border||(modes>>3)==Pi5Border;}
// Textures: First/Count select view mip levels; LODs are view-relative u4.8.
// Integer buffers: First/Count select view elements of the declared format.
struct Pi5Binding {
    uint32_t Kind,Allocation,First,Count;
    uint32_t Filter,AddressModes,MinLod,MaxLod; // U in bits 0-2, V in bits 3-5.
    uint32_t Border[4];
};
struct Pi5DrawCommand {
    Pi5CommandHeader Header;
    uint32_t Target,Vertices,VertexOffset,VertexStride,VertexCount,VertexComponents;
    uint32_t VaryingScalars,NonPerspectiveMask,FlatMask,BindingCount;
    Pi5Binding Bindings[PI5_BINDINGS];
    float Viewport[6];
    Pi5Program Coordinate,Vertex,Pixel;
    Pi5Pipeline Pipeline;
};
static_assert(sizeof(Pi5AdapterInfo)==16 && sizeof(Pi5AllocationInfo)==56 && sizeof(Pi5Binding)==48 &&
              sizeof(Pi5RegionCommand)==64 && sizeof(Pi5DrawCommand)==424 && sizeof(Pi5Pipeline)==80,"shared graphics ABI layout");

// Every child is a complete, independently validated draw. Target and gathered
// vertices are shared; texture references are remapped through each entry.
struct Pi5BatchEntry {uint32_t Offset,Bytes,Count,Reserved,References[PI5_MAX_ALLOCATIONS];};
struct Pi5BatchCommand {Pi5CommandHeader Header;uint32_t Count,Reserved[3];Pi5BatchEntry Entries[PI5_MAX_BATCH_DRAWS];};
static_assert(sizeof(Pi5BatchEntry)==40&&sizeof(Pi5BatchCommand)==1312,"batch ABI layout");
