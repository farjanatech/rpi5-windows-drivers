#pragma once
#define NOMINMAX
#define D3D_UMD_INTERFACE_VERSION 0x3004
#include <windows.h>
#include <d3d10umddi.h>
#include <type_traits>
#include <vector>
#include <array>
#include "abi.h"
#include "shader.h"
namespace pi5 {
struct Resource {
    D3DKMT_HANDLE allocation = 0;
    D3DKMT_HANDLE kernelResource = 0;
    D3D10DDI_HRTRESOURCE runtime = {};
    Pi5AllocationInfo info = {};
    void *mapped = nullptr;
    UINT mappedLevels = 0;
    bool primary = false;
};
struct LinkedProgram {
    Shader shader;
    ShaderSignature inputs={};
    bool valid=false;
};
struct Program {
    Shader first,second;
    std::array<LinkedProgram,4> linked;
    std::vector<UINT> tokens;
    ConstantBuffers constants;
    ShaderBlend blend;
    unsigned nextLinked=0;
    ShaderStage stage=ShaderStage::Pixel;
    bool compiled=false,loops=false;
};
struct RenderTarget {Resource *resource=nullptr;};
struct ShaderView {Resource *resource=nullptr;UINT firstElement=0,elements=0,components=0,elementBytes=0,firstLevel=0,levels=1;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;};
struct EventQuery {Resource marker;UINT serial=0;bool issued=false,ready=false;};
struct Layout {std::vector<D3D10DDIARG_INPUT_ELEMENT_DESC> elements;};
struct VertexBinding {Resource*resource=nullptr;UINT stride=0,offset=0;};
struct UmdCallRecord {
    volatile LONG sequence=0;
    DWORD tick=0;
    DWORD thread=0;
    HRESULT failure=S_OK;
    char where[48]={};
};
struct Device {
    D3D10DDI_HRTDEVICE runtime;
    D3D10DDI_HRTCORELAYER core;
    D3DDDI_DEVICECALLBACKS kernel;
    D3D10DDI_CORELAYER_DEVICECALLBACKS user;
    const DXGI_DDI_BASE_CALLBACKS *dxgi=nullptr;
    D3DDDICB_CREATECONTEXT context={};
    HRESULT failure=S_OK;
    volatile LONG diagnosticSequence=0;
    volatile LONG fatalCaptured=0;
    UmdCallRecord diagnosticCalls[64]={};
    bool nativeDisplay=false;
    Program *vs=nullptr,*ps=nullptr;
    Resource *constants[2][14]={};
    VertexBinding vertices[32];
    Resource *indexBuffer=nullptr;
    DXGI_FORMAT indexFormat=DXGI_FORMAT_UNKNOWN;
    UINT indexOffset=0;
    Resource vertexScratch;
    Resource uploadScratch;
    std::vector<BYTE> gatheredVertices,batchVertices,batchBytes;
    std::vector<Resource*> batchResources;
    Pi5BatchCommand batch={};
    void FlushBatch();
    void SubmitImmediate(const void *data,UINT bytes,Resource **resources,UINT count);
    void FlushReference(Resource *resource,bool write);

    RenderTarget *target=nullptr;
    Layout *layout=nullptr;
    D3D10_DDI_VIEWPORT viewport={};
    const D3D10_DDI_BLEND_DESC *blend=nullptr;
    const D3D10_DDI_DEPTH_STENCIL_DESC *depth=nullptr;
    const D3D10_DDI_RASTERIZER_DESC *raster=nullptr;
    const D3D10_DDI_SAMPLER_DESC *samplers[3][16]={};
    ShaderView *textures[3][128]={};
    FLOAT blendFactor[4]={1,1,1,1};
    UINT sampleMask=UINT32_MAX,stencilRef=0;
    RECT scissor={};
    D3D10_DDI_PRIMITIVE_TOPOLOGY topology=D3D10_DDI_PRIMITIVE_TOPOLOGY_UNDEFINED;
    void Error(HRESULT hr,const char *where);
    void Submit(const void *data,UINT bytes,Resource **resources,UINT count);
    void *Lock(Resource *r,D3D10_DDI_MAP mode,UINT flags=0);
    void Unlock(Resource *r);
};
inline Device *Get(D3D10DDI_HDEVICE h){return static_cast<Device*>(h.pDrvPrivate);}
inline Resource *Get(D3D10DDI_HRESOURCE h){return static_cast<Resource*>(h.pDrvPrivate);}
void SetDeviceFunctions(D3D10DDI_DEVICEFUNCS *functions);
}
