#include "umd.h"
#include "validate.h"
#include "draw-bounds.h"
#include "draw-coverage.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <new>
#include <stdexcept>

namespace pi5 {
#ifdef _DEBUG
static SRWLOCK logLock=SRWLOCK_INIT;
static char logPending[8192];
static DWORD logPendingBytes=0;
static void FlushLogLocked(){
    if(!logPendingBytes)return;
    wchar_t path[MAX_PATH];swprintf_s(path,L"C:\\ProgramData\\Pi5GraphicsDiagnostics\\umd-%lu.txt",GetCurrentProcessId());
    HANDLE file=CreateFileW(path,FILE_APPEND_DATA|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){LARGE_INTEGER size;DWORD written;if(GetFileSizeEx(file,&size)&&size.QuadPart<2*1024*1024)WriteFile(file,logPending,logPendingBytes,&written,nullptr);CloseHandle(file);}
    logPendingBytes=0;
}
#endif
static void FlushLog(){
#ifdef _DEBUG
    AcquireSRWLockExclusive(&logLock);FlushLogLocked();ReleaseSRWLockExclusive(&logLock);
#endif
}
static void LogMessage(bool critical,const char*format,va_list args){
#ifdef _DEBUG
    // Bound all routine diagnostics, including debugger exceptions and stderr,
    // before formatting a message. Critical failures remain immediate.
    static volatile LONG count=0;LONG number=critical?0:InterlockedIncrement(&count);if(number>2048)return;
#else
    if(!critical)return;
#endif
    char message[768];vsnprintf_s(message,sizeof(message),_TRUNCATE,format,args);
    fputs(message,stderr);fflush(stderr);OutputDebugStringA(message);
#ifdef _DEBUG
    // Batch disk output; errors flush their preceding context immediately.
    DWORD bytes=static_cast<DWORD>(strlen(message));AcquireSRWLockExclusive(&logLock);
    if(bytes>sizeof(logPending)-logPendingBytes)FlushLogLocked();
    memcpy(logPending+logPendingBytes,message,bytes);logPendingBytes+=bytes;
    if(critical||number==2048)FlushLogLocked();
    ReleaseSRWLockExclusive(&logLock);
#else
    (void)critical;
#endif
}
static void Log(const char*format,...){va_list args;va_start(args,format);LogMessage(false,format,args);va_end(args);}
static void LogCritical(const char*format,...){va_list args;va_start(args,format);LogMessage(true,format,args);va_end(args);}

static void DiagnosticWrite(HANDLE file,const char*format,...){
    char line[1024]={};
    va_list args;va_start(args,format);
    vsnprintf_s(line,sizeof(line),_TRUNCATE,format,args);
    va_end(args);
    DWORD bytes=static_cast<DWORD>(strlen(line)),written=0;
    if(bytes)(void)WriteFile(file,line,bytes,&written,nullptr);
}
static uint32_t DiagnosticProgramHash(const Program*program){
    if(!program)return 0;
    uint32_t hash=2166136261u;
    for(UINT word:program->tokens)hash=(hash^word)*16777619u;
    return hash;
}
static void RecordCall(Device*d,const char*where){
    if(!d||!where)return;
    LONG sequence=InterlockedIncrement(&d->diagnosticSequence);
    auto&record=d->diagnosticCalls[(static_cast<ULONG>(sequence)-1u)%64u];
    InterlockedExchange(&record.sequence,0);
    record.tick=GetTickCount();
    record.thread=GetCurrentThreadId();
    record.failure=d->failure;
    strncpy_s(record.where,sizeof(record.where),where,_TRUNCATE);
    InterlockedExchange(&record.sequence,sequence);
}
static void PersistProcessEvent(const char*kind,UINT id){
    CreateDirectoryW(L"C:\\ProgramData\\Pi5GraphicsDiagnostics",nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path,L"C:\\ProgramData\\Pi5GraphicsDiagnostics\\umd-event-%lu-%u-%lu.txt",
        GetCurrentProcessId(),id,GetTickCount());
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    DiagnosticWrite(file,"Pi5D3D diagnostic event v1\r\nkind=%s\r\nid=%u\r\npid=%lu\r\ntid=%lu\r\ntick=%lu\r\n",
        kind?kind:"<null>",id,GetCurrentProcessId(),GetCurrentThreadId(),GetTickCount());
    CloseHandle(file);
}
static void PersistFirstFatal(Device*d,HRESULT hr,const char*where){
    if(!d||hr==DXGI_DDI_ERR_WASSTILLDRAWING||
       InterlockedCompareExchange(&d->fatalCaptured,1,0)!=0)return;
    CreateDirectoryW(L"C:\\ProgramData\\Pi5GraphicsDiagnostics",nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path,L"C:\\ProgramData\\Pi5GraphicsDiagnostics\\umd-first-fatal-%lu-%lu-%lu.txt",
        GetCurrentProcessId(),GetCurrentThreadId(),GetTickCount());
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){InterlockedExchange(&d->fatalCaptured,0);return;}

    DiagnosticWrite(file,"Pi5D3D first-fatal diagnostic v1\r\n");
    DiagnosticWrite(file,"pid=%lu tid=%lu tick=%lu\r\n",GetCurrentProcessId(),GetCurrentThreadId(),GetTickCount());
    DiagnosticWrite(file,"where=%s\r\nfatal_hr=0x%08lx\r\nprior_failure=0x%08lx\r\nnativeDisplay=%u\r\n",
        where?where:"<null>",static_cast<ULONG>(hr),static_cast<ULONG>(d->failure),d->nativeDisplay?1u:0u);
    DiagnosticWrite(file,
        "state topology=%u sampleMask=0x%08x stencilRef=%u target=%p layout=%p index=%p indexFormat=%u indexOffset=%u blend=%p depth=%p raster=%p\r\n",
        static_cast<UINT>(d->topology),d->sampleMask,d->stencilRef,d->target,d->layout,d->indexBuffer,
        static_cast<UINT>(d->indexFormat),d->indexOffset,d->blend,d->depth,d->raster);
    DiagnosticWrite(file,"viewport x=%g y=%g w=%g h=%g min=%g max=%g scissor=%ld,%ld,%ld,%ld\r\n",
        d->viewport.TopLeftX,d->viewport.TopLeftY,d->viewport.Width,d->viewport.Height,
        d->viewport.MinDepth,d->viewport.MaxDepth,d->scissor.left,d->scissor.top,d->scissor.right,d->scissor.bottom);
    DiagnosticWrite(file,"batch count=%u bytes=%llu vertices=%llu resources=%llu gathered=%llu\r\n",
        d->batch.Count,static_cast<unsigned long long>(d->batchBytes.size()),
        static_cast<unsigned long long>(d->batchVertices.size()),
        static_cast<unsigned long long>(d->batchResources.size()),
        static_cast<unsigned long long>(d->gatheredVertices.size()));

    const Resource*target=d->target?d->target->resource:nullptr;
    if(target)DiagnosticWrite(file,
        "target allocation=0x%x kernel=0x%x %ux%u pitch=%u format=%u bind=0x%x misc=0x%x levels=%u primary=%u mapped=%p\r\n",
        target->allocation,target->kernelResource,target->info.Width,target->info.Height,target->info.Pitch,
        target->info.Format,target->info.BindFlags,target->info.MiscFlags,target->info.Levels,target->primary?1u:0u,target->mapped);
    if(d->indexBuffer)DiagnosticWrite(file,
        "index allocation=0x%x width=%u bytes=%u format=%u bind=0x%x mapped=%p\r\n",
        d->indexBuffer->allocation,d->indexBuffer->info.Width,d->indexBuffer->info.Bytes,
        d->indexBuffer->info.Format,d->indexBuffer->info.BindFlags,d->indexBuffer->mapped);

    const Program*programs[2]={d->vs,d->ps};const char*names[2]={"vs","ps"};
    for(UINT i=0;i<2;++i){
        const Program*p=programs[i];
        if(p)DiagnosticWrite(file,
            "shader %s stage=%u compiled=%u loops=%u tokens=%llu hash=0x%08x ptr=%p\r\n",
            names[i],static_cast<UINT>(p->stage),p->compiled?1u:0u,p->loops?1u:0u,
            static_cast<unsigned long long>(p->tokens.size()),DiagnosticProgramHash(p),p);
        else DiagnosticWrite(file,"shader %s <null>\r\n",names[i]);
    }

    LONG end=InterlockedCompareExchange(&d->diagnosticSequence,0,0);
    LONG first=end>63?end-63:1;
    DiagnosticWrite(file,"call_ring first=%ld end=%ld\r\n",first,end);
    for(LONG sequence=first;sequence<=end;++sequence){
        auto&record=d->diagnosticCalls[(static_cast<ULONG>(sequence)-1u)%64u];
        LONG published=InterlockedCompareExchange(&record.sequence,0,0);
        if(published!=sequence)continue;
        DiagnosticWrite(file,"call seq=%ld tick=%lu tid=%lu prior=0x%08lx where=%s\r\n",
            sequence,record.tick,record.thread,static_cast<ULONG>(record.failure),record.where);
    }
    FlushFileBuffers(file);
    CloseHandle(file);
}
// DEBUG diagnostic: with this flag file present, unsupported draws are logged
// and skipped instead of failing the device, so one compositor run lists every gap.
static bool SkipUnsupportedDraws(){
#ifdef _DEBUG
    static const bool skip=GetFileAttributesW(L"C:\\ProgramData\\Pi5GraphicsDiagnostics\\skip-unsupported-draws")!=INVALID_FILE_ATTRIBUTES;
    return skip;
#else
    return false;
#endif
}
struct ErrorCode {HRESULT hr;};
static void CaptureShader(const UINT*code,UINT words,ShaderStage stage){
#ifdef _DEBUG
    uint32_t hash=2166136261u;for(UINT i=0;i<words;++i)hash=(hash^code[i])*16777619u;
    LogCritical("Pi5D3D shader stage=%u words=%u hash=%08x\n",unsigned(stage),words,hash);
    // Devices are recreated with the same shaders, so only distinct new captures count.
    static volatile LONG count=0;if(count>=256)return;
    wchar_t path[MAX_PATH];swprintf_s(path,L"C:\\ProgramData\\Pi5GraphicsDiagnostics\\shader-%u-%08x.bin",unsigned(stage),hash);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){InterlockedIncrement(&count);DWORD written;WriteFile(file,code,words*4,&written,nullptr);CloseHandle(file);}
#else
    (void)code;(void)words;(void)stage;
#endif
}
static void Require(bool value,HRESULT hr=E_INVALIDARG){if(!value)throw ErrorCode{hr};}
static void Check(HRESULT hr){if(FAILED(hr))throw ErrorCode{hr};}
template<class F> static void Guard(Device*d,const char*where,F f){
    RecordCall(d,where);
    try {if(SUCCEEDED(d->failure))f();}
    catch(const ErrorCode&e){d->Error(e.hr,where);}
    catch(const std::bad_alloc&){d->Error(E_OUTOFMEMORY,where);}
    catch(const std::exception&e){OutputDebugStringA(e.what());d->Error(E_FAIL,where);}
}
void Device::Error(HRESULT hr,const char*where){
    if(hr==DXGI_DDI_ERR_WASSTILLDRAWING){user.pfnSetErrorCb(core,hr);return;}
    RecordCall(this,where);
    PersistFirstFatal(this,hr,where);
    LogCritical("Pi5D3D %s failed: %08lx\n",where,hr);
    failure=hr;
    user.pfnSetErrorCb(core,hr);
}
void *Device::Lock(Resource*r,D3D10_DDI_MAP mode,UINT flags){
    Require(r&&r->allocation&&!r->mapped);
    FlushReference(r,mode!=D3D10_DDI_MAP_READ);
    D3DDDICB_LOCK a={};a.hAllocation=r->allocation;
    a.Flags.LockEntire=1;
    a.Flags.ReadOnly=mode==D3D10_DDI_MAP_READ;a.Flags.WriteOnly=mode==D3D10_DDI_MAP_WRITE;
    a.Flags.Discard=mode==D3D10_DDI_MAP_WRITE_DISCARD;
    // FlushReference has retired all pending references before a write lock.
    // VidMm can reuse retired discard instances without reporting allocation busy.
    a.Flags.NoExistingReference=a.Flags.Discard;
    a.Flags.DonotWait=(flags&D3D10_DDI_MAP_FLAG_DONOTWAIT)!=0;
    HRESULT hr=kernel.pfnLockCb(runtime.handle,&a);if(hr==D3DDDIERR_WASSTILLDRAWING)hr=DXGI_DDI_ERR_WASSTILLDRAWING;
    if(FAILED(hr)&&hr!=DXGI_DDI_ERR_WASSTILLDRAWING)LogCritical("Pi5D3D lock hr=%08lx allocation=%x flags=%x misc=%x size=%ux%u\n",hr,r->allocation,a.Flags.Value,r->info.MiscFlags,r->info.Width,r->info.Height);
    Check(hr);Require(a.pData!=nullptr,E_FAIL);
    r->allocation=a.hAllocation;r->mapped=a.pData;return a.pData;
}
void Device::Unlock(Resource*r){
    Require(r&&r->mapped);D3DDDICB_UNLOCK a={};a.NumAllocations=1;a.phAllocations=&r->allocation;
    Check(kernel.pfnUnlockCb(runtime.handle,&a));r->mapped=nullptr;
}
void Device::SubmitImmediate(const void *data,UINT bytes,Resource **resources,UINT count){
    Require(data&&bytes<=context.CommandBufferSize&&count<=context.AllocationListSize&&context.pCommandBuffer&&context.pAllocationList,E_OUTOFMEMORY);
    memcpy(context.pCommandBuffer,data,bytes);
    for(UINT i=0;i<count;++i){Require(resources[i]&&resources[i]->allocation&&!resources[i]->mapped);context.pAllocationList[i]={};context.pAllocationList[i].hAllocation=resources[i]->allocation;context.pAllocationList[i].WriteOperation=i==0;}
    D3DDDICB_RENDER a={};a.hContext=context.hContext;a.CommandLength=bytes;a.NumAllocations=count;
    HRESULT hr=kernel.pfnRenderCb(runtime.handle,&a);
    if(FAILED(hr)){auto h=static_cast<const Pi5CommandHeader*>(data);LogCritical("Pi5D3D render callback hr=%08lx operation=%u bytes=%u allocations=%u\n",hr,h->Operation,bytes,count);
        for(UINT i=0;i<count;++i)LogCritical("  allocation %u handle=%x dim=%u %ux%u levels=%u bytes=%u\n",i,resources[i]->allocation,resources[i]->info.Dimension,resources[i]->info.Width,resources[i]->info.Height,resources[i]->info.Levels,resources[i]->info.Bytes);}
    Check(hr);
    context.pCommandBuffer=a.pNewCommandBuffer;context.CommandBufferSize=a.NewCommandBufferSize;
    context.pAllocationList=a.pNewAllocationList;context.AllocationListSize=a.NewAllocationListSize;
    context.pPatchLocationList=a.pNewPatchLocationList;context.PatchLocationListSize=a.NewPatchLocationListSize;
}
static Pi5CommandHeader Header(UINT operation,UINT bytes){return {PI5_UMD_MAGIC,PI5_UMD_ABI,bytes,operation};}
// CPU reads need pending writes; CPU writes need every pending reference.
// Compare allocation handles as distinct Resource objects may alias a shared surface.
void Device::FlushReference(Resource*r,bool write){
    for(size_t i=0;i<batchResources.size();++i)if((write||i==0)&&batchResources[i]->allocation==r->allocation){FlushBatch();break;}
}
void Device::FlushBatch(){
    if(!batch.Count)return;
    Pi5BatchCommand header=batch;batch={};
    std::vector<BYTE> bytes,vertexData;std::vector<Resource*> resources;
    bytes.swap(batchBytes);vertexData.swap(batchVertices);resources.swap(batchResources);
    // Nothing queued still names an old scratch instance. This whole batch uses
    // the freshly renamed instance populated below, including every child offset.
    auto scratch=&vertexScratch;
    if(!scratch->allocation){D3DDDI_ALLOCATIONINFO info={};info.pPrivateDriverData=&scratch->info;info.PrivateDriverDataSize=sizeof(scratch->info);
        D3DDDICB_ALLOCATE request={};request.NumAllocations=1;request.pAllocationInfo=&info;Check(kernel.pfnAllocateCb(runtime.handle,&request));scratch->allocation=info.hAllocation;Require(scratch->allocation!=0,E_FAIL);}
    auto mapped=Lock(scratch,D3D10_DDI_MAP_WRITE_DISCARD);memcpy(mapped,vertexData.data(),vertexData.size());Unlock(scratch);
    header.Header=Header(Pi5DrawBatch,static_cast<UINT>(bytes.size()));memcpy(bytes.data(),&header,sizeof(header));
    if(header.Count==1){const auto&e=header.Entries[0];Resource*local[PI5_MAX_ALLOCATIONS]={};for(UINT i=0;i<e.Count;++i)local[i]=resources[e.References[i]];
        SubmitImmediate(bytes.data()+e.Offset,e.Bytes,local,e.Count);
    }else SubmitImmediate(bytes.data(),static_cast<UINT>(bytes.size()),resources.data(),static_cast<UINT>(resources.size()));
}
void Device::Submit(const void*data,UINT bytes,Resource**resources,UINT count){
    auto h=static_cast<const Pi5CommandHeader*>(data);
    if(h->Operation!=Pi5Draw){FlushBatch();SubmitImmediate(data,bytes,resources,count);return;}
    Require(count>=2&&count<=PI5_MAX_ALLOCATIONS&&resources[1]==&vertexScratch&&!gatheredVertices.empty());
    // A single maximum-size draw can still use the original unwrapped ABI.
    if(bytes>PI5_UMD_COMMAND_BYTES-sizeof(Pi5BatchCommand)){
        FlushBatch();auto scratch=&vertexScratch;
        if(!scratch->allocation){D3DDDI_ALLOCATIONINFO info={};info.pPrivateDriverData=&scratch->info;info.PrivateDriverDataSize=sizeof(scratch->info);D3DDDICB_ALLOCATE a={};a.NumAllocations=1;a.pAllocationInfo=&info;Check(kernel.pfnAllocateCb(runtime.handle,&a));scratch->allocation=info.hAllocation;}
        auto mapped=Lock(scratch,D3D10_DDI_MAP_WRITE_DISCARD);memcpy(mapped,gatheredVertices.data(),gatheredVertices.size());Unlock(scratch);SubmitImmediate(data,bytes,resources,count);return;
    }
    size_t newReferences=0;for(UINT i=2;i<count;++i){bool found=false;for(size_t j=2;j<batchResources.size();++j)if(batchResources[j]->allocation==resources[i]->allocation){found=true;break;}if(!found)++newReferences;}
    if(batch.Count&&(batchResources[0]->allocation!=resources[0]->allocation||batch.Count==PI5_MAX_BATCH_DRAWS||
       ((batchBytes.size()+7)&~size_t(7))+bytes>PI5_UMD_COMMAND_BYTES||batchVertices.size()+gatheredVertices.size()>vertexScratch.info.Bytes||batchResources.size()+newReferences>PI5_MAX_REFERENCES))FlushBatch();
    if(!batch.Count){batchBytes.resize(sizeof(Pi5BatchCommand));batchResources={resources[0],resources[1]};}
    auto&e=batch.Entries[batch.Count];e.Count=count;e.References[1]=1;
    for(UINT i=2;i<count;++i){UINT index=2;while(index<batchResources.size()&&batchResources[index]->allocation!=resources[i]->allocation)++index;
        if(index==batchResources.size())batchResources.push_back(resources[i]);e.References[i]=index;}
    e.Offset=static_cast<UINT>((batchBytes.size()+7)&~size_t(7));e.Bytes=bytes;batchBytes.resize(e.Offset+bytes);memcpy(batchBytes.data()+e.Offset,data,bytes);
    auto child=reinterpret_cast<Pi5DrawCommand*>(batchBytes.data()+e.Offset);child->VertexOffset=static_cast<UINT>(batchVertices.size());
    batchVertices.insert(batchVertices.end(),gatheredVertices.begin(),gatheredVertices.end());++batch.Count;
}
static uint32_t IntegerBufferKind(DXGI_FORMAT format){
    switch(format){
    case DXGI_FORMAT_R8_UINT:return Pi5BindingBytes;case DXGI_FORMAT_R8_SINT:return Pi5BindingSbyte;
    case DXGI_FORMAT_R8G8_UINT:return Pi5BindingUbyte2;case DXGI_FORMAT_R8G8_SINT:return Pi5BindingSbyte2;
    case DXGI_FORMAT_R8G8B8A8_UINT:return Pi5BindingUbyte4;case DXGI_FORMAT_R8G8B8A8_SINT:return Pi5BindingSbyte4;
    case DXGI_FORMAT_R16_UINT:return Pi5BindingUshorts;case DXGI_FORMAT_R16_SINT:return Pi5BindingShorts;
    case DXGI_FORMAT_R16G16_UINT:return Pi5BindingUshort2;case DXGI_FORMAT_R16G16_SINT:return Pi5BindingShort2;
    case DXGI_FORMAT_R16G16B16A16_UINT:return Pi5BindingUshort4;case DXGI_FORMAT_R16G16B16A16_SINT:return Pi5BindingShort4;
    case DXGI_FORMAT_R32_UINT:return Pi5BindingUint;case DXGI_FORMAT_R32_SINT:return Pi5BindingInt;
    case DXGI_FORMAT_R32G32_UINT:return Pi5BindingUint2;case DXGI_FORMAT_R32G32_SINT:return Pi5BindingInt2;
    case DXGI_FORMAT_R32G32B32A32_UINT:return Pi5BindingUint4;case DXGI_FORMAT_R32G32B32A32_SINT:return Pi5BindingInt4;
    default:return 0;}
}
static UINT Components(DXGI_FORMAT f){
    if(auto kind=IntegerBufferKind(f))return Pi5BufferComponents(kind);
    switch(f){case DXGI_FORMAT_R32_FLOAT:return 1;case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16_FLOAT:return 2;case DXGI_FORMAT_R32G32B32_FLOAT:return 3;case DXGI_FORMAT_R32G32B32A32_FLOAT:return 4;default:return 0;}
}
static bool ColorFormat(DXGI_FORMAT f){return f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_B8G8R8A8_UNORM||f==DXGI_FORMAT_B8G8R8X8_UNORM;}
static SIZE_T APIENTRY ResourceSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATERESOURCE*){return sizeof(Resource);}
static SIZE_T APIENTRY OpenedResourceSize(D3D10DDI_HDEVICE,const D3D10DDIARG_OPENRESOURCE*){return sizeof(Resource);}
static void APIENTRY OpenResource(D3D10DDI_HDEVICE h,const D3D10DDIARG_OPENRESOURCE*a,D3D10DDI_HRESOURCE out,D3D10DDI_HRTRESOURCE runtime){
    auto r=new(out.pDrvPrivate)Resource;
    Guard(Get(h),"OpenResource",[&]{
        Require(a&&a->NumAllocations==1&&a->pOpenAllocationInfo&&a->pPrivateDriverData&&a->PrivateDriverDataSize==sizeof(Pi5AllocationInfo));
        const auto&allocation=a->pOpenAllocationInfo[0];Require(allocation.hAllocation&&allocation.pPrivateDriverData&&allocation.PrivateDriverDataSize==sizeof(Pi5AllocationInfo));
        Pi5AllocationInfo info;memcpy(&info,allocation.pPrivateDriverData,sizeof(info));Require(ValidateAllocation(info)&&!memcmp(&info,a->pPrivateDriverData,sizeof(info))&&info.Dimension==D3D10DDIRESOURCE_TEXTURE2D);
        r->allocation=allocation.hAllocation;r->kernelResource=a->hKMResource.handle;r->runtime=runtime;r->info=info;
        Log("Pi5D3D opened shared allocation=%x resource=%x %ux%u\n",r->allocation,r->kernelResource,info.Width,info.Height);
    });
}
// Each subresource map shares one lock of the whole allocation.
static void APIENTRY Map(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE r,UINT sub,D3D10_DDI_MAP mode,UINT flags,D3D10DDI_MAPPED_SUBRESOURCE*out){
    if(out)*out={};Guard(Get(h),"Map",[&]{auto resource=Get(r);Pi5Level level={};Require(out&&resource&&Pi5AllocationLevel(resource->info,sub,level)&&!(resource->mappedLevels&(1u<<sub)));
        if(!resource->mappedLevels)Get(h)->Lock(resource,mode,flags);resource->mappedLevels|=1u<<sub;
        out->pData=static_cast<BYTE*>(resource->mapped)+level.Offset;out->RowPitch=level.Pitch;out->DepthPitch=resource->info.Dimension==D3D10DDIRESOURCE_BUFFER?resource->info.Bytes:level.Pitch*level.Height;});
}
static void APIENTRY Unmap(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE r,UINT sub){Guard(Get(h),"Unmap",[&]{auto resource=Get(r);Require(resource&&sub<32&&(resource->mappedLevels&(1u<<sub)));
    resource->mappedLevels&=~(1u<<sub);if(!resource->mappedLevels)Get(h)->Unlock(resource);});}
static Resource *UploadScratch(Device*d,UINT width,UINT height,UINT format){
    auto r=&d->uploadScratch;
    if(r->allocation&&(r->info.Width<width||r->info.Height<height||r->info.Format!=format)){
        width=std::max(width,r->info.Width);height=std::max(height,r->info.Height);
        D3DDDICB_DEALLOCATE a={};a.NumAllocations=1;a.HandleList=&r->allocation;Check(d->kernel.pfnDeallocateCb(d->runtime.handle,&a));*r={};
    }
    if(!r->allocation){UINT pitch=(width*4+63)&~63u,bytes=(pitch*height+4095)&~4095u;
        r->info={PI5_UMD_ABI,bytes,width,height,pitch,format,0,D3D10DDIRESOURCE_TEXTURE2D};Require(ValidateAllocation(r->info));
        D3DDDI_ALLOCATIONINFO info={};info.pPrivateDriverData=&r->info;info.PrivateDriverDataSize=sizeof(r->info);
        D3DDDICB_ALLOCATE a={};a.NumAllocations=1;a.pAllocationInfo=&info;Check(d->kernel.pfnAllocateCb(d->runtime.handle,&a));r->allocation=info.hAllocation;Require(r->allocation!=0,E_FAIL);
    }
    return r;
}
static void Update(Device*d,Resource*r,UINT sub,const D3D10_DDI_BOX*box,const void*data,UINT sourcePitch){
    Pi5Level level={};Require(r&&data&&Pi5AllocationLevel(r->info,sub,level));bool buffer=r->info.Dimension==D3D10DDIRESOURCE_BUFFER;
    UINT left=box?box->left:0,top=box?box->top:0,right=box?box->right:level.Width,bottom=box?box->bottom:level.Height;
    Require(left<=right&&top<=bottom&&right<=level.Width&&bottom<=level.Height&&(!box||(box->front==0&&box->back==1)));
    UINT bytes=(right-left)*(buffer?1u:4u);Require(buffer||bottom-top<=1||sourcePitch>=bytes);
    if(left==right||top==bottom)return;
    // Shared and GPU-only textures in the native segment cannot be CPU-visible.
    // Upload through a private allocation; VidMm orders its copy against both
    // devices and the next CPU lock before the scratch storage is reused.
    if(Pi5CpuInvisible(r->info)){
        Require(!buffer);auto staging=UploadScratch(d,right-left,bottom-top,r->info.Format);
        auto dst=static_cast<BYTE*>(d->Lock(staging,D3D10_DDI_MAP_WRITE));
        for(UINT y=0;y<bottom-top;++y)memcpy(dst+size_t(y)*staging->info.Pitch,static_cast<const BYTE*>(data)+size_t(y)*sourcePitch,bytes);
        d->Unlock(staging);
        alignas(8) Pi5RegionCommand c={};c.Header=Header(Pi5CopyRegion,sizeof(c));c.Source=1;c.DestinationX=left;c.DestinationY=top;c.DestinationLevel=sub;c.Width=right-left;c.Height=bottom-top;
        Pi5AllocationInfo infos[]={r->info,staging->info};Require(ValidateCommand(&c,sizeof(c),infos,2));Resource*resources[]={r,staging};d->Submit(&c,sizeof(c),resources,2);return;
    }
    auto dst=static_cast<BYTE*>(d->Lock(r,D3D10_DDI_MAP_WRITE))+level.Offset;
    for(UINT y=top;y<bottom;++y)memcpy(dst+size_t(y)*level.Pitch+left*(buffer?1u:4u),static_cast<const BYTE*>(data)+size_t(y-top)*sourcePitch,bytes);
    d->Unlock(r);
}
static void APIENTRY UpdateResource(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE r,UINT sub,const D3D10_DDI_BOX*box,const void*data,UINT pitch,UINT){Guard(Get(h),"UpdateResource",[&]{
    try{Update(Get(h),Get(r),sub,box,data,pitch);}
    catch(const ErrorCode&){
#ifdef _DEBUG
        auto resource=Get(r);if(resource)LogCritical("Pi5D3D update parameters dim=%u size=%ux%u levels=%u sub=%u data=%p pitch=%u mapped=%p mapped-levels=%x\n",resource->info.Dimension,resource->info.Width,resource->info.Height,resource->info.Levels,sub,data,pitch,resource->mapped,resource->mappedLevels);
        if(box)LogCritical("Pi5D3D update box=%u,%u,%u-%u,%u,%u\n",box->left,box->top,box->front,box->right,box->bottom,box->back);
#endif
        throw;
    }
});}
static void APIENTRY CreateResource(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATERESOURCE*a,D3D10DDI_HRESOURCE out,D3D10DDI_HRTRESOURCE runtime){
    auto r=new(out.pDrvPrivate) Resource;
    Guard(Get(h),"CreateResource",[&]{
        if(a)Log("Pi5D3D resource dim=%u format=%u usage=%u bind=%x misc=%x mip=%u array=%u samples=%u/%u primary=%p\n",a->ResourceDimension,a->Format,a->Usage,a->BindFlags,a->MiscFlags,a->MipLevels,a->ArraySize,a->SampleDesc.Count,a->SampleDesc.Quality,a->pPrimaryDesc);
        Require(a&&a->pMipInfoList&&a->MipLevels>=1&&a->MipLevels<=PI5_MAX_LEVELS&&a->ArraySize==1&&a->SampleDesc.Count==1&&a->SampleDesc.Quality==0,DXGI_DDI_ERR_UNSUPPORTED);
        bool buffer=a->ResourceDimension==D3D10DDIRESOURCE_BUFFER;
        Require(buffer?a->MipLevels==1:(a->ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D&&ColorFormat(a->Format)),DXGI_DDI_ERR_UNSUPPORTED);
        UINT width=a->pMipInfoList[0].TexelWidth,height=buffer?1:a->pMipInfoList[0].TexelHeight;
        Require(width&&height&&width<=(buffer?PI5_UMD_RESOURCE_BYTES:4096)&&height<=4096);
        for(UINT i=1;i<a->MipLevels;++i)Require(a->pMipInfoList[i].TexelWidth==std::max(width>>i,1u)&&a->pMipInfoList[i].TexelHeight==std::max(height>>i,1u),DXGI_DDI_ERR_UNSUPPORTED);
        if(a->MiscFlags&PI5_RESOURCE_DISPLAYABLE)Require(Get(h)->nativeDisplay&&!buffer&&(a->BindFlags&D3D10_DDI_BIND_PRESENT),DXGI_DDI_ERR_UNSUPPORTED);
        if(a->pPrimaryDesc){const auto&p=*a->pPrimaryDesc;
            Log("Pi5D3D primary flags=%x source=%u mode=%ux%u format=%u refresh=%u/%u\n",p.Flags,p.VidPnSourceId,p.ModeDesc.Width,p.ModeDesc.Height,p.ModeDesc.Format,p.ModeDesc.RefreshRate.Numerator,p.ModeDesc.RefreshRate.Denominator);
            Require(Get(h)->nativeDisplay&&!buffer&&(a->BindFlags&D3D10_DDI_BIND_PRESENT)&&a->Usage==D3D10_DDI_USAGE_DEFAULT&&!a->MapFlags&&
                p.VidPnSourceId<2&&!(p.Flags&~3u)&&width>=640&&height>=480&&height<=2160&&(a->Format==DXGI_FORMAT_B8G8R8A8_UNORM||a->Format==DXGI_FORMAT_B8G8R8X8_UNORM)&&
                p.ModeDesc.Width==width&&p.ModeDesc.Height==height&&p.ModeDesc.Format==a->Format,DXGI_DDI_ERR_UNSUPPORTED);
            a->pPrimaryDesc->DriverFlags=0;
        }
        UINT pitch=buffer?width:(width*4+63)&~63u;uint64_t layout=buffer?uint64_t(pitch):Pi5LevelChain(width,height,pitch,a->MipLevels,0,nullptr);
        uint64_t size=(layout+4095)&~UINT64_C(4095);Require(size<=PI5_UMD_RESOURCE_BYTES,E_OUTOFMEMORY);
        r->info={PI5_UMD_ABI,static_cast<UINT>(size),width,height,pitch,static_cast<UINT>(a->Format),a->BindFlags,static_cast<UINT>(a->ResourceDimension)};r->info.MiscFlags=a->MiscFlags|(a->pPrimaryDesc?PI5_RESOURCE_DISPLAYABLE:0);r->info.Levels=a->MipLevels;
        if(a->pPrimaryDesc){r->info.RefreshNumerator=a->pPrimaryDesc->ModeDesc.RefreshRate.Numerator;r->info.RefreshDenominator=a->pPrimaryDesc->ModeDesc.RefreshRate.Denominator;}
        if(!buffer&&!a->MapFlags&&(a->Usage==D3D10_DDI_USAGE_DEFAULT||a->Usage==D3D10_DDI_USAGE_IMMUTABLE))r->info.PrivateFlags=PI5_ALLOCATION_GPU_ONLY;
        Require(ValidateAllocation(r->info),DXGI_DDI_ERR_UNSUPPORTED);r->runtime=runtime;
        D3DDDI_ALLOCATIONINFO allocation={};allocation.pPrivateDriverData=&r->info;allocation.PrivateDriverDataSize=sizeof(r->info);
        if(a->pPrimaryDesc){allocation.Flags.Primary=1;allocation.VidPnSourceId=a->pPrimaryDesc->VidPnSourceId;}
        D3DDDICB_ALLOCATE request={};request.hResource=runtime.handle;request.NumAllocations=1;request.pAllocationInfo=&allocation;request.pPrivateDriverData=&r->info;request.PrivateDriverDataSize=sizeof(r->info);
        HRESULT allocated=Get(h)->kernel.pfnAllocateCb(Get(h)->runtime.handle,&request);
        Log("Pi5D3D Allocate hr=%08lx allocation=%x resource=%x %ux%u pitch=%u initial=%p data=%p\n",allocated,allocation.hAllocation,request.hKMResource,width,height,pitch,a->pInitialDataUP,a->pInitialDataUP?a->pInitialDataUP[0].pSysMem:nullptr);
        Check(allocated);r->allocation=allocation.hAllocation;r->kernelResource=request.hKMResource;r->primary=a->pPrimaryDesc!=nullptr;Require(r->allocation!=0,E_FAIL);
        if(a->pInitialDataUP)for(UINT i=0;i<a->MipLevels;++i)if(a->pInitialDataUP[i].pSysMem)Update(Get(h),r,i,nullptr,a->pInitialDataUP[i].pSysMem,a->pInitialDataUP[i].SysMemPitch);
    });
}
static void APIENTRY DestroyResource(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE r){
    auto resource=Get(r);if(!resource)return;
    Guard(Get(h),"DestroyResource flush",[&]{Get(h)->FlushReference(resource,true);});
    if(resource->mapped){try{Get(h)->Unlock(resource);}catch(...){Get(h)->Error(E_FAIL,"Destroy mapped resource");}}
    if(resource->allocation){D3DDDICB_DEALLOCATE a={};if(resource->kernelResource||(resource->info.MiscFlags&0x102u))a.hResource=resource->runtime.handle;else{a.NumAllocations=1;a.HandleList=&resource->allocation;}HRESULT hr=Get(h)->kernel.pfnDeallocateCb(Get(h)->runtime.handle,&a);if(FAILED(hr))Get(h)->Error(hr,"Deallocate");}
    resource->~Resource();
}
static SIZE_T APIENTRY TargetSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATERENDERTARGETVIEW*){return sizeof(RenderTarget);}
static void APIENTRY CreateTarget(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATERENDERTARGETVIEW*a,D3D10DDI_HRENDERTARGETVIEW out,D3D10DDI_HRTRENDERTARGETVIEW){
    auto target=new(out.pDrvPrivate)RenderTarget;
    Guard(Get(h),"CreateTarget",[&]{Require(a&&a->ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D&&a->Tex2D.MipSlice==0&&a->Tex2D.FirstArraySlice==0&&a->Tex2D.ArraySize==1&&ColorFormat(a->Format),DXGI_DDI_ERR_UNSUPPORTED);target->resource=Get(a->hDrvResource);Require(target->resource&&target->resource->info.Format==static_cast<UINT>(a->Format));});
}
static void APIENTRY DestroyTarget(D3D10DDI_HDEVICE,D3D10DDI_HRENDERTARGETVIEW h){static_cast<RenderTarget*>(h.pDrvPrivate)->~RenderTarget();}
static SIZE_T APIENTRY ViewSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATESHADERRESOURCEVIEW*){return sizeof(ShaderView);}
static void APIENTRY CreateView(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATESHADERRESOURCEVIEW*a,D3D10DDI_HSHADERRESOURCEVIEW out,D3D10DDI_HRTSHADERRESOURCEVIEW){
    auto view=new(out.pDrvPrivate)ShaderView;Guard(Get(h),"CreateShaderResourceView",[&]{
        Require(a);auto r=Get(a->hDrvResource);Require(r&&r->allocation&&(r->info.BindFlags&D3D10_DDI_BIND_SHADER_RESOURCE));
        if(a->ResourceDimension==D3D10DDIRESOURCE_BUFFER){
            LogCritical("Pi5D3D buffer view format=%u first=%u count=%u bytes=%u\n",a->Format,a->Buffer.FirstElement,a->Buffer.NumElements,r->info.Width);
            uint32_t kind=IntegerBufferKind(a->Format);
            UINT components=kind?Pi5BufferComponents(kind):a->Format==DXGI_FORMAT_R32_FLOAT?1:a->Format==DXGI_FORMAT_R32G32_FLOAT?2:a->Format==DXGI_FORMAT_R32G32B32_FLOAT?3:a->Format==DXGI_FORMAT_R32G32B32A32_FLOAT?4:0;
            Require(r->info.Dimension==D3D10DDIRESOURCE_BUFFER&&components&&a->Buffer.NumElements,DXGI_DDI_ERR_UNSUPPORTED);
            UINT elementBytes=kind?Pi5BufferElementBytes(kind):components*4;
            uint64_t end=uint64_t(a->Buffer.FirstElement)+a->Buffer.NumElements;Require(end*elementBytes<=r->info.Width);
            view->resource=r;view->firstElement=a->Buffer.FirstElement;view->elements=a->Buffer.NumElements;view->components=components;view->elementBytes=elementBytes;view->format=a->Format;return;
        }
        Log("Pi5D3D texture view dim=%u format=%u mip=%u/%u array=%u/%u resource-format=%u\n",a->ResourceDimension,a->Format,a->Tex2D.MostDetailedMip,a->Tex2D.MipLevels,a->Tex2D.FirstArraySlice,a->Tex2D.ArraySize,r->info.Format);
        UINT first=a->Tex2D.MostDetailedMip,levels=a->Tex2D.MipLevels==UINT_MAX&&first<r->info.Levels?r->info.Levels-first:a->Tex2D.MipLevels;
        Require(a->ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D&&r->info.Dimension==D3D10DDIRESOURCE_TEXTURE2D&&first<r->info.Levels&&levels&&levels<=r->info.Levels-first&&!a->Tex2D.FirstArraySlice&&a->Tex2D.ArraySize==1&&ColorFormat(a->Format),DXGI_DDI_ERR_UNSUPPORTED);
        Require(r->info.Format==static_cast<UINT>(a->Format));view->resource=r;view->firstLevel=first;view->levels=levels;
    });
}
static void APIENTRY DestroyView(D3D10DDI_HDEVICE,D3D10DDI_HSHADERRESOURCEVIEW h){static_cast<ShaderView*>(h.pDrvPrivate)->~ShaderView();}
template<unsigned Stage> static void APIENTRY SetViews(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HSHADERRESOURCEVIEW *views){Guard(Get(h),"SetShaderResources",[&]{Require(first<=128&&count<=128-first);for(UINT i=0;i<count;++i)Get(h)->textures[Stage][first+i]=static_cast<ShaderView*>(views[i].pDrvPrivate);});}
static void APIENTRY SetTargets(D3D10DDI_HDEVICE h,const D3D10DDI_HRENDERTARGETVIEW*targets,UINT count,UINT,D3D10DDI_HDEPTHSTENCILVIEW depth){Guard(Get(h),"SetTargets",[&]{Require(count<=1&&!depth.pDrvPrivate,DXGI_DDI_ERR_UNSUPPORTED);Get(h)->target=count?static_cast<RenderTarget*>(targets[0].pDrvPrivate):nullptr;});}
static unsigned Channel(float x){return static_cast<unsigned>(std::max(0.0f,std::min(1.0f,x))*255.0f+0.5f);}
static void APIENTRY ClearTarget(D3D10DDI_HDEVICE h,D3D10DDI_HRENDERTARGETVIEW target,FLOAT color[4]){Guard(Get(h),"ClearTarget",[&]{auto t=static_cast<RenderTarget*>(target.pDrvPrivate);Require(t&&t->resource);Pi5ClearCommand c={};c.Header=Header(Pi5Clear,sizeof(c));c.Color=Channel(color[0])|(Channel(color[1])<<8)|(Channel(color[2])<<16)|(Channel(color[3])<<24);Resource*resources[]={t->resource};Get(h)->Submit(&c,sizeof(c),resources,1);});}
static void APIENTRY CopyResource(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,D3D10DDI_HRESOURCE src){Guard(Get(h),"CopyResource",[&]{Require(Get(dst)&&Get(src)&&Get(dst)->info.Width==Get(src)->info.Width&&Get(dst)->info.Height==Get(src)->info.Height&&Get(dst)->info.Format==Get(src)->info.Format&&Get(dst)->info.Levels==Get(src)->info.Levels);Pi5CopyCommand c={};c.Header=Header(Pi5Copy,sizeof(c));c.Source=1;Resource*resources[]={Get(dst),Get(src)};Get(h)->Submit(&c,sizeof(c),resources,2);});}
static void APIENTRY CopyRegion(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,UINT dstSub,UINT x,UINT y,UINT z,D3D10DDI_HRESOURCE src,UINT srcSub,const D3D10_DDI_BOX*box){Guard(Get(h),"CopyRegion",[&]{
    auto d=Get(dst),s=Get(src);Pi5Level sourceLevel={};
    Require(d&&s&&!z&&d->allocation!=s->allocation&&d->info.Dimension==s->info.Dimension&&d->info.Format==s->info.Format&&Pi5AllocationLevel(s->info,srcSub,sourceLevel)&&dstSub<d->info.Levels);
    D3D10_DDI_BOX b=box?*box:D3D10_DDI_BOX{0,0,0,LONG(sourceLevel.Width),LONG(sourceLevel.Height),1};Require(b.left>=0&&b.top>=0&&b.front>=0&&b.left<=b.right&&b.top<=b.bottom&&b.front<=b.back&&b.right<=LONG(sourceLevel.Width)&&b.bottom<=LONG(sourceLevel.Height));
    if(b.left==b.right||b.top==b.bottom||b.front==b.back)return;Require(!b.front&&b.back==1);
    alignas(8) Pi5RegionCommand c={};c.Header=Header(Pi5CopyRegion,sizeof(c));c.Source=1;c.SourceX=b.left;c.SourceY=b.top;c.DestinationX=x;c.DestinationY=y;c.Width=b.right-b.left;c.Height=b.bottom-b.top;c.DestinationLevel=dstSub;c.SourceLevel=srcSub;
    Pi5AllocationInfo infos[]={d->info,s->info};Require(ValidateCommand(&c,sizeof(c),infos,2));Resource*resources[]={d,s};Get(h)->Submit(&c,sizeof(c),resources,2);
});}
static SIZE_T APIENTRY ShaderSize(D3D10DDI_HDEVICE,const UINT*,const D3D10DDIARG_STAGE_IO_SIGNATURES*){return sizeof(Program);}
template<ShaderStage S> static void APIENTRY CreateShader(D3D10DDI_HDEVICE h,const UINT*code,D3D10DDI_HSHADER out,D3D10DDI_HRTSHADER,const D3D10DDIARG_STAGE_IO_SIGNATURES*){
    auto program=new(out.pDrvPrivate)Program;program->stage=S;
    // Retain the runtime-validated DXBC. Compile on first use so unused effect
    // variants do not allocate QPU programs or prevent basic device setup.
    Guard(Get(h),"CreateShader",[&]{Require(code&&code[1]>=2&&code[1]<=65536&&code[0]==(S==ShaderStage::Pixel?0x40u:0x10040u));CaptureShader(code,code[1],S);program->tokens.assign(code,code+code[1]);
        for(UINT at=2;at<code[1];){UINT length=(code[at]>>24)&127;Require(length&&length<=code[1]-at);if((code[at]&0x7ff)==48)program->loops=true;at+=length;}
    });
    if(FAILED(Get(h)->failure))program->~Program();
}
static void APIENTRY DestroyShader(D3D10DDI_HDEVICE,D3D10DDI_HSHADER h){static_cast<Program*>(h.pDrvPrivate)->~Program();}
static void APIENTRY SetVs(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER s){Get(h)->vs=static_cast<Program*>(s.pDrvPrivate);}
static void APIENTRY SetPs(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER s){Get(h)->ps=static_cast<Program*>(s.pDrvPrivate);}
template<unsigned Stage> static void APIENTRY SetConstants(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HRESOURCE*r){Guard(Get(h),"SetConstants",[&]{Require(first<=14&&count<=14-first);for(UINT i=0;i<count;++i)Get(h)->constants[Stage][first+i]=Get(r[i]);});}
static SIZE_T APIENTRY LayoutSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEELEMENTLAYOUT*){return sizeof(Layout);}
static void APIENTRY CreateLayout(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATEELEMENTLAYOUT*a,D3D10DDI_HELEMENTLAYOUT out,D3D10DDI_HRTELEMENTLAYOUT){auto layout=new(out.pDrvPrivate)Layout;Guard(Get(h),"CreateLayout",[&]{
    if(a&&a->pVertexElements){Log("Pi5D3D layout elements=%u\n",a->NumElements);for(UINT i=0;i<std::min(a->NumElements,32u);++i){const auto&e=a->pVertexElements[i];Log("  slot=%u reg=%u format=%u offset=%u class=%u step=%u\n",e.InputSlot,e.InputRegister,e.Format,e.AlignedByteOffset,e.InputSlotClass,e.InstanceDataStepRate);}}
    Require(a&&a->NumElements<=32&&(!a->NumElements||a->pVertexElements),DXGI_DDI_ERR_UNSUPPORTED);UINT registers=0;
    for(UINT i=0;i<a->NumElements;++i){const auto&e=a->pVertexElements[i];Require(e.InputSlot<32&&e.InputRegister<32&&!(registers&(1u<<e.InputRegister))&&
        ((e.InputSlotClass==D3D10_DDI_INPUT_PER_VERTEX_DATA&&!e.InstanceDataStepRate)||e.InputSlotClass==D3D10_DDI_INPUT_PER_INSTANCE_DATA)&&(Components(e.Format)||e.Format==DXGI_FORMAT_R8G8B8A8_UNORM),DXGI_DDI_ERR_UNSUPPORTED);registers|=1u<<e.InputRegister;layout->elements.push_back(e);}
    });if(FAILED(Get(h)->failure))layout->~Layout();}
static void APIENTRY DestroyLayout(D3D10DDI_HDEVICE,D3D10DDI_HELEMENTLAYOUT h){static_cast<Layout*>(h.pDrvPrivate)->~Layout();}
static void APIENTRY SetLayout(D3D10DDI_HDEVICE h,D3D10DDI_HELEMENTLAYOUT l){Get(h)->layout=static_cast<Layout*>(l.pDrvPrivate);}
static void APIENTRY SetVertices(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HRESOURCE*resources,const UINT*strides,const UINT*offsets){Guard(Get(h),"SetVertices",[&]{Require(first<=32&&count<=32-first&&(!count||(resources&&strides&&offsets)));for(UINT i=0;i<count;++i)Get(h)->vertices[first+i]={Get(resources[i]),strides[i],offsets[i]};});}
static void APIENTRY SetTopology(D3D10DDI_HDEVICE h,D3D10_DDI_PRIMITIVE_TOPOLOGY t){Get(h)->topology=t;}
static void APIENTRY SetViewports(D3D10DDI_HDEVICE h,UINT count,UINT,const D3D10_DDI_VIEWPORT*v){Guard(Get(h),"SetViewports",[&]{Require(count<=1&&(!count||v),DXGI_DDI_ERR_UNSUPPORTED);Get(h)->viewport=count?*v:D3D10_DDI_VIEWPORT{};});}
static void APIENTRY SetScissors(D3D10DDI_HDEVICE h,UINT count,UINT,const D3D10_DDI_RECT*r){Guard(Get(h),"SetScissors",[&]{Require(count<=1&&(!count||r),DXGI_DDI_ERR_UNSUPPORTED);Get(h)->scissor=count?RECT{r->left,r->top,r->right,r->bottom}:RECT{};});}
static uint16_t HalfUnit(float value){
    if(!(value>0))return 0;if(value>=1)return 0x3c00;
    uint32_t bits;memcpy(&bits,&value,4);int exponent=int(bits>>23)-127;if(exponent < -25)return 0;
    uint32_t mantissa=(bits&0x7fffff)|0x800000;unsigned shift=exponent < -14?unsigned(-exponent-1):13;
    uint32_t rounded=mantissa>>shift,remainder=mantissa&((1u<<shift)-1),half=1u<<(shift-1);
    rounded+=remainder>half||(remainder==half&&(rounded&1));
    return uint16_t(exponent < -14?rounded:((exponent+14)<<10)+rounded);
}
static uint32_t BlendFactor(D3D10_DDI_BLEND value,bool alpha){
    switch(unsigned(value)){
    case 1:return 0;case 2:return 1;case 3:return 2;case 4:return 3;case 5:return 6;case 6:return 7;
    case 7:return 8;case 8:return 9;case 9:return 4;case 10:return 5;case 11:return 14;
    case 14:return alpha?12:10;case 15:return alpha?13:11;case 20:return 12;case 21:return 13;
    default:throw ErrorCode{DXGI_DDI_ERR_UNSUPPORTED};}
}
static ShaderBlend ShaderBlendState(Device*d){
    ShaderBlend state;
    if(d->blend&&d->blend->BlendEnable[0]){const auto&b=*d->blend;
        auto Dual=[](D3D10_DDI_BLEND factor){return unsigned(factor)>=16&&unsigned(factor)<=19;};
        if(Dual(b.SrcBlend)||Dual(b.DestBlend)||Dual(b.SrcBlendAlpha)||Dual(b.DestBlendAlpha))
            state.functions={uint32_t(b.SrcBlend),uint32_t(b.DestBlend),uint32_t(b.BlendOp),uint32_t(b.SrcBlendAlpha),uint32_t(b.DestBlendAlpha),uint32_t(b.BlendOpAlpha)};
    }
    state.opaqueTarget=state.functions[2]&&d->target->resource->info.Format==DXGI_FORMAT_B8G8R8X8_UNORM;return state;
}
static bool Pipeline(Device*d,Pi5Pipeline&state){
    Require(d->sampleMask==UINT32_MAX&&(!d->blend||!d->blend->AlphaToCoverageEnable),DXGI_DDI_ERR_UNSUPPORTED);
    if(d->blend){state.ColorMask=d->blend->RenderTargetWriteMask[0];if(d->blend->BlendEnable[0]){
        const auto&b=*d->blend;Require(b.BlendOp>=1&&b.BlendOp<=5&&b.BlendOpAlpha>=1&&b.BlendOpAlpha<=5,DXGI_DDI_ERR_UNSUPPORTED);
        if(ShaderBlendState(d).functions[2])state.Flags|=PI5_PIPELINE_SHADER_BLEND;
        else{state.Flags|=1;state.Blend[0]=BlendFactor(b.SrcBlend,false);state.Blend[1]=BlendFactor(b.DestBlend,false);state.Blend[2]=b.BlendOp-1;
            state.Blend[3]=BlendFactor(b.SrcBlendAlpha,true);state.Blend[4]=BlendFactor(b.DestBlendAlpha,true);state.Blend[5]=b.BlendOpAlpha-1;}
        for(unsigned i=0;i<4;++i)state.Constant[i]=HalfUnit(d->blendFactor[i]);
    }}
    if(d->target->resource->info.Format==DXGI_FORMAT_B8G8R8X8_UNORM)for(unsigned i=0;i<6;++i)if(i%3!=2){if(state.Blend[i]==8)state.Blend[i]=1;else if(state.Blend[i]==9)state.Blend[i]=0;}
    state.CullMode=3;
    if(d->raster){const auto&r=*d->raster;Require(r.FillMode==D3D10_DDI_FILL_SOLID&&r.CullMode>=1&&r.CullMode<=3&&!r.DepthBias&&!r.DepthBiasClamp&&!r.SlopeScaledDepthBias&&!r.MultisampleEnable&&!r.AntialiasedLineEnable,DXGI_DDI_ERR_UNSUPPORTED);
        state.CullMode=r.CullMode;if(r.FrontCounterClockwise)state.Flags|=2;if(!r.DepthClipEnable)state.Flags&=~4u;
        if(r.ScissorEnable){state.Flags|=8;LONG width=LONG(d->target->resource->info.Width),height=LONG(d->target->resource->info.Height);
            state.Scissor[0]=UINT(std::max(0L,std::min(width,d->scissor.left)));state.Scissor[1]=UINT(std::max(0L,std::min(height,d->scissor.top)));
            state.Scissor[2]=UINT(std::max(0L,std::min(width,d->scissor.right)));state.Scissor[3]=UINT(std::max(0L,std::min(height,d->scissor.bottom)));
            if(state.Scissor[0]>=state.Scissor[2]||state.Scissor[1]>=state.Scissor[3])return false;
        }
    }
    return state.ColorMask!=0;
}
struct BindingSize {UINT elements,width,height;};
static Pi5Program AppendProgram(Device*d,const Shader&s,Resource **constants,std::vector<BYTE>&packet,const BindingSize *sizes=nullptr,const ConstantBuffers *snapshot=nullptr,const UINT *bindingMap=nullptr){
    Pi5Program result={};packet.resize((packet.size()+7)&~size_t(7));result.CodeOffset=static_cast<UINT>(packet.size());result.CodeCount=static_cast<UINT>(s.code.size());
    auto code=reinterpret_cast<const BYTE*>(s.code.data());packet.insert(packet.end(),code,code+s.code.size()*8);
    result.UniformOffset=static_cast<UINT>(packet.size());result.UniformCount=static_cast<UINT>(s.uniforms.size());
    ConstantBuffers loaded;const auto&data=snapshot?*snapshot:loaded;
    for(const auto&u:s.uniforms)if(!snapshot&&(u.kind==UniformKind::ConstantBuffer||u.kind==UniformKind::FloatConstantBuffer)&&u.slot<14&&constants[u.slot]&&loaded[u.slot].empty()){
        auto r=constants[u.slot];Require(r->info.Dimension==D3D10DDIRESOURCE_BUFFER&&r->info.Width<=65536);loaded[u.slot].resize(r->info.Width/4);
        auto mapped=d->Lock(r,D3D10_DDI_MAP_READ);memcpy(loaded[u.slot].data(),mapped,loaded[u.slot].size()*4);d->Unlock(r);
    }
    float viewport[]={d->viewport.Width*32,-d->viewport.Height*32,d->viewport.MaxDepth-d->viewport.MinDepth,d->viewport.MinDepth};
    // Allocate the uniform range once; per-word vector insertion repeatedly
    // creates checked iterators and reallocates while building a draw packet.
    packet.resize(packet.size()+s.uniforms.size()*4);UINT uniformIndex=0;
    for(const auto&u:s.uniforms){uint32_t value=0;if(u.kind==UniformKind::Literal||u.kind==UniformKind::ConstantTextureConfig||u.kind==UniformKind::ConstantSamplerConfig)value=u.value;
        else if(u.kind==UniformKind::TextureConfig||u.kind==UniformKind::SamplerConfig){Require(bindingMap&&u.slot<s.bindings.size()&&bindingMap[u.slot]<PI5_BINDINGS);value=(u.kind==UniformKind::TextureConfig?PI5_TEXTURE_TOKEN:PI5_SAMPLER_TOKEN)|(bindingMap[u.slot]<<PI5_BINDING_TOKEN_SHIFT);}
        else if(u.kind==UniformKind::Viewport){Require(u.value<4);memcpy(&value,&viewport[u.value],4);}
        else if(u.kind==UniformKind::BlendConstant){Require(u.value<4);memcpy(&value,&d->blendFactor[u.value],4);}
        else if(u.kind==UniformKind::BindingElements||u.kind==UniformKind::BindingWidth||u.kind==UniformKind::BindingHeight){Require(sizes&&bindingMap&&u.slot<s.bindings.size()&&bindingMap[u.slot]<PI5_BINDINGS);
            const auto&size=sizes[bindingMap[u.slot]];value=u.kind==UniformKind::BindingElements?size.elements:u.kind==UniformKind::BindingWidth?size.width:size.height;}
        else{Require((u.kind==UniformKind::ConstantBuffer||u.kind==UniformKind::FloatConstantBuffer)&&u.slot<14);if(u.value<data[u.slot].size())value=data[u.slot][u.value];if(u.kind==UniformKind::FloatConstantBuffer&&!(value&0x7f800000u))value=0;}
        memcpy(packet.data()+result.UniformOffset+size_t(uniformIndex++)*4,&value,4);
    }
    if(s.constantWords){Require(s.constantSlot<14&&s.constantWords<=4096&&s.constantDeclaredWords<=s.constantWords);result.ConstantOffset=static_cast<UINT>(packet.size());result.ConstantWords=s.constantWords;packet.resize(packet.size()+s.constantWords*4,0);
        if(snapshot){const auto&table=(*snapshot)[s.constantSlot];size_t words=std::min(table.size(),size_t(s.constantDeclaredWords));if(words)memcpy(packet.data()+result.ConstantOffset,table.data(),words*4);}
        else if(auto r=constants[s.constantSlot]){Require(r->info.Dimension==D3D10DDIRESOURCE_BUFFER&&r->info.Width<=65536);UINT bytes=std::min(r->info.Width,s.constantDeclaredWords*4);auto mapped=d->Lock(r,D3D10_DDI_MAP_READ);memcpy(packet.data()+result.ConstantOffset,mapped,bytes);d->Unlock(r);}
    }
    return result;
}
static void CompileProgram(Device*d,Program*p,Resource **resources){
    if(p->stage==ShaderStage::Pixel){auto blend=ShaderBlendState(d);if(blend.functions!=p->blend.functions||blend.opaqueTarget!=p->blend.opaqueTarget){p->compiled=false;p->blend=blend;}}
    if(p->loops){
        ConstantBuffers snapshot;
        for(unsigned slot=0;slot<14;++slot)if(auto r=resources[slot]){
            Require(r->info.Dimension==D3D10DDIRESOURCE_BUFFER&&r->info.Width<=65536);snapshot[slot].resize(r->info.Width/4);
            auto mapped=d->Lock(r,D3D10_DDI_MAP_READ);memcpy(snapshot[slot].data(),mapped,snapshot[slot].size()*4);d->Unlock(r);
        }
        auto ReadWord=[](const ConstantBuffers&data,const Uniform&word){const auto&buffer=data[word.slot];return word.value<buffer.size()?buffer[word.value]:0;};
        // Pixel compilation tracks exactly which words fixed a branch or index.
        // Vertex linking can use additional outputs, so retain its full snapshot key.
        if(p->stage==ShaderStage::Vertex&&snapshot!=p->constants)p->compiled=false;
        for(const auto&word:p->first.specialized)if(ReadWord(snapshot,word)!=ReadWord(p->constants,word))p->compiled=false;
        p->constants=std::move(snapshot);
    }
    if(p->compiled)return;
    // A linked vertex program also depends on the loop-specialization snapshot.
    for(auto&linked:p->linked)linked.valid=false;p->nextLinked=0;
    std::string error;ShaderSignature noVaryings={};auto constants=p->loops?&p->constants:nullptr;
    if(!CompileShaderTokens(p->tokens.data(),p->tokens.size(),p->stage,p->first,error,p->stage==ShaderStage::Vertex?&noVaryings:nullptr,constants,p->stage==ShaderStage::Pixel?&p->blend:nullptr)||
       (p->stage==ShaderStage::Vertex&&!CompileShaderTokens(p->tokens.data(),p->tokens.size(),ShaderStage::Coordinate,p->second,error,nullptr,constants))){CaptureShader(p->tokens.data(),static_cast<UINT>(p->tokens.size()),p->stage);LogCritical("Pi5D3D active shader: %s\n",error.c_str());
        if(p->loops)for(unsigned slot=0;slot<14;++slot)if(p->constants[slot].size()>=4)LogCritical("Pi5D3D loop constants slot=%u words=%zu first=%08x,%08x,%08x,%08x\n",slot,p->constants[slot].size(),p->constants[slot][0],p->constants[slot][1],p->constants[slot][2],p->constants[slot][3]);
        throw ErrorCode{DXGI_DDI_ERR_UNSUPPORTED};}
    if(p->loops)Log("Pi5D3D specialized shader code=%zu uniforms=%zu key-words=%zu\n",p->first.code.size(),p->first.uniforms.size(),p->first.specialized.size());
    p->compiled=true;
}
static UINT Float16Bits(uint16_t value){
    UINT sign=UINT(value&0x8000)<<16,mantissa=value&1023,exponent=(value>>10)&31;
    if(exponent==31)return sign|0x7f800000|(mantissa<<13);
    if(exponent)return sign|((exponent+112)<<23)|(mantissa<<13);
    if(!mantissa)return sign;int power=-14;while(!(mantissa&1024)){mantissa<<=1;--power;}return sign|(UINT(power+127)<<23)|((mantissa&1023)<<13);
}
struct VertexOrderState {UINT previous[2]={},seen=0;};
static std::vector<UINT> VertexOrder(Device*d,UINT count,UINT first,bool indexed,INT base,bool strip,VertexOrderState&state){
    Require(count<=4095,DXGI_DDI_ERR_UNSUPPORTED);std::vector<UINT> indices(count),order;UINT cut=UINT32_MAX;
    if(indexed){auto r=d->indexBuffer;Require(r&&r->info.Dimension==D3D10DDIRESOURCE_BUFFER);UINT bytes=d->indexFormat==DXGI_FORMAT_R16_UINT?2:4;
        uint64_t start=uint64_t(d->indexOffset)+uint64_t(first)*bytes;Require(start<=r->info.Width&&uint64_t(count)*bytes<=r->info.Width-start);
        auto data=static_cast<const BYTE*>(d->Lock(r,D3D10_DDI_MAP_READ))+start;
        for(UINT i=0;i<count;++i){if(bytes==2){uint16_t value;memcpy(&value,data+size_t(i)*2,2);indices[i]=value;}else memcpy(&indices[i],data+size_t(i)*4,4);}d->Unlock(r);cut=bytes==2?0xffffu:UINT32_MAX;
    }else{Require(uint64_t(first)+count<=UINT64_C(0x100000000));for(UINT i=0;i<count;++i)indices[i]=first+i;}
    auto&previous=state.previous;auto&seen=state.seen;
    for(UINT i=0;i<(strip?count:count/3*3);++i){UINT raw=indices[i];if(strip&&indexed&&raw==cut){seen=0;continue;}
        int64_t adjusted=int64_t(raw)+(indexed?base:0);Require(adjusted>=0&&adjusted<=UINT32_MAX);UINT vertex=UINT(adjusted);
        if(!strip){order.push_back(vertex);continue;}
        // Odd strip triangles are rotated, not swapped, so vertex n stays first
        // for Direct3D flat interpolation while the winding is still reversed.
        if(seen>=2){Require(order.size()<=4092,DXGI_DDI_ERR_UNSUPPORTED);bool odd=(seen-2)&1;order.push_back(previous[0]);order.push_back(odd?vertex:previous[1]);order.push_back(odd?previous[1]:vertex);}
        previous[0]=previous[1];previous[1]=vertex;++seen;
    }return order;
}
static UINT GatherVertices(Device*d,const std::vector<UINT>&order,UINT instances,UINT firstInstance,UINT instanceBase,UINT vertexIdBias){
    Require(!order.empty()&&instances&&uint64_t(order.size())*instances<=4095,DXGI_DDI_ERR_UNSUPPORTED);UINT orderCount=static_cast<UINT>(order.size()),outputCount=orderCount*instances;const UINT*orderData=order.data();
    const auto&masks=d->vs->first.inputs;UINT scalars=0;for(UINT mask:masks)for(UINT c=0;c<4;++c)scalars+=(mask>>c)&1;
    Require(scalars<=16,DXGI_DDI_ERR_UNSUPPORTED);scalars=std::max(scalars,1u);d->gatheredVertices.resize(size_t(outputCount)*scalars*4);auto packed=reinterpret_cast<UINT*>(d->gatheredVertices.data());memset(packed,0,d->gatheredVertices.size());
    UINT field=0;
    for(UINT reg=0;reg<masks.size();++reg){if(!masks[reg])continue;UINT fields=0,ordinary=masks[reg];
        for(UINT c=0;c<4;++c)if(masks[reg]&(1u<<c)){
            UINT system=d->vs->first.systemInputs[reg][c];
            if(system){Require(system==6||system==8);ordinary&=~(1u<<c);
                for(UINT vertex=0;vertex<outputCount;++vertex)packed[size_t(vertex)*scalars+field+fields]=system==6?orderData[vertex%orderCount]-vertexIdBias:instanceBase+vertex/orderCount;}
            ++fields;
        }
        if(!ordinary){field+=fields;continue;}
        Require(d->layout,DXGI_DDI_ERR_UNSUPPORTED);const D3D10DDIARG_INPUT_ELEMENT_DESC*element=nullptr;
        for(const auto&e:d->layout->elements)if(e.InputRegister==reg)element=&e;Require(element,DXGI_DDI_ERR_UNSUPPORTED);
        const auto&binding=d->vertices[element->InputSlot];auto r=binding.resource;Require(r&&r->info.Dimension==D3D10DDIRESOURCE_BUFFER&&binding.stride<=4096);
        uint32_t integerKind=IntegerBufferKind(element->Format);
        bool normalized=element->Format==DXGI_FORMAT_R8G8B8A8_UNORM,half=element->Format==DXGI_FORMAT_R16G16_FLOAT;
        bool integer=integerKind!=0,perInstance=element->InputSlotClass==D3D10_DDI_INPUT_PER_INSTANCE_DATA;
        UINT components=normalized?4:Components(element->Format),componentBytes=integer?Pi5BufferComponentBytes(integerKind):half?2:normalized?1:4;
        UINT elementBytes=components*componentBytes;bool signedInteger=Pi5BufferSigned(integerKind);
        uint64_t offset=uint64_t(binding.offset)+element->AlignedByteOffset;
        uint64_t last=perInstance?uint64_t(firstInstance)+(element->InstanceDataStepRate?(uint64_t(instanceBase)+instances-1)/element->InstanceDataStepRate:0):*std::max_element(order.begin(),order.end());
        Require(components&&offset<=r->info.Width&&last*binding.stride+elementBytes<=r->info.Width-offset);
        auto mapped=static_cast<const BYTE*>(d->Lock(r,D3D10_DDI_MAP_READ));
        for(UINT instance=0;instance<instances;++instance){
          uint64_t instanceRecord=uint64_t(firstInstance)+(element->InstanceDataStepRate?(uint64_t(instanceBase)+instance)/element->InstanceDataStepRate:0);
          auto output=packed+size_t(instance)*orderCount*scalars;
          for(UINT vertex=0;vertex<orderCount;++vertex){
            uint64_t record=perInstance?instanceRecord:orderData[vertex];
            auto source=mapped+offset+record*binding.stride;UINT lane=field;
            for(UINT component=0;component<4;++component)if(masks[reg]&(1u<<component)){if(!(ordinary&(1u<<component))){++lane;continue;}UINT value=component==3?(integer?1:0x3f800000):0;
                if(component<components){if(normalized){float converted=float(source[component])/255.0f;memcpy(&value,&converted,4);}else if(integer){value=0;memcpy(&value,source+component*componentBytes,componentBytes);if(signedInteger&&componentBytes<4){UINT sign=1u<<(componentBytes*8-1);value=(value^sign)-sign;}}else if(half){uint16_t v;memcpy(&v,source+component*2,2);value=Float16Bits(v);}else memcpy(&value,source+component*4,4);}
                output[size_t(vertex)*scalars+lane++]=value;
            }
          }
        }d->Unlock(r);field+=fields;
    }
    return scalars;
}
static void DrawVertices(Device*d,const std::vector<UINT>&order,UINT instances,UINT firstInstance,UINT instanceBase,UINT vertexIdBias){
    Require(!order.empty()&&uint64_t(order.size())*instances<=4095);UINT outputCount=static_cast<UINT>(order.size())*instances;
    Pi5DrawCommand c={};c.Header=Header(Pi5Draw,0);c.Vertices=1;c.VertexCount=outputCount;
    if(!Pipeline(d,c.Pipeline))return;
    CompileProgram(d,d->vs,d->constants[0]);CompileProgram(d,d->ps,d->constants[1]);c.VertexComponents=GatherVertices(d,order,instances,firstInstance,instanceBase,vertexIdBias);c.VertexStride=c.VertexComponents*4;
    // Retain four linked variants per vertex shader so alternating pixel inputs
    // do not repeatedly compile the same vertex program. Ordinary constants are
    // still read by AppendProgram on every draw; only code and uniform metadata
    // are cached. A changed pixel input signature must relink the vertex stage.
    LinkedProgram*linkedProgram=nullptr;
    for(auto&entry:d->vs->linked)if(entry.valid&&entry.inputs==d->ps->first.inputs){linkedProgram=&entry;break;}
    if(!linkedProgram){
        auto&entry=d->vs->linked[d->vs->nextLinked];d->vs->nextLinked=(d->vs->nextLinked+1)%unsigned(d->vs->linked.size());
        entry.valid=false;std::string error;Shader linked;
        if(!CompileShaderTokens(d->vs->tokens.data(),d->vs->tokens.size(),ShaderStage::Vertex,linked,error,&d->ps->first.inputs,d->vs->loops?&d->vs->constants:nullptr)){LogCritical("Pi5D3D linked shader: %s\n",error.c_str());throw ErrorCode{DXGI_DDI_ERR_UNSUPPORTED};}
        entry.shader=std::move(linked);entry.inputs=d->ps->first.inputs;entry.valid=true;linkedProgram=&entry;
        Log("Pi5D3D linked vertex code=%zu uniforms=%zu\n",entry.shader.code.size(),entry.shader.uniforms.size());
    }
    const auto&linkedVertex=linkedProgram->shader;
    c.VaryingScalars=d->ps->first.varyingScalars;c.NonPerspectiveMask=d->ps->first.nonPerspectiveMask;c.FlatMask=d->ps->first.flatMask;Require(linkedVertex.varyingScalars==c.VaryingScalars,DXGI_DDI_ERR_UNSUPPORTED);
    Resource*resources[PI5_MAX_ALLOCATIONS]={d->target->resource,&d->vertexScratch};UINT resourceCount=2;BindingSize sizes[PI5_BINDINGS]={};
    const Shader*shaderPrograms[]={&d->vs->second,&linkedVertex,&d->ps->first};UINT bindingMaps[3][PI5_BINDINGS]={};
    for(UINT program=0;program<3;++program){UINT stage=program==2?1:0;const auto&bindings=shaderPrograms[program]->bindings;
      Require(bindings.size()<=PI5_BINDINGS,DXGI_DDI_ERR_UNSUPPORTED);
      for(UINT k=0;k<bindings.size();++k){const auto&b=bindings[k];auto view=d->textures[stage][b.resource];Pi5Binding x={};BindingSize size={};
       auto PrepareBinding=[&](){
        // Identity rotation can leave distinct resources sharing one allocation, so compare allocations.
        Require(view&&view->resource&&view->resource->allocation&&view->resource->allocation!=d->target->resource->allocation,DXGI_DDI_ERR_UNSUPPORTED);
        UINT index=2;while(index<resourceCount&&resources[index]->allocation!=view->resource->allocation)++index;if(index==resourceCount){Require(resourceCount<PI5_MAX_ALLOCATIONS,DXGI_DDI_ERR_UNSUPPORTED);resources[resourceCount++]=view->resource;}x.Allocation=index;
        if(b.buffer){uint32_t kind=IntegerBufferKind(view->format);
            Require(view->resource->info.Dimension==D3D10DDIRESOURCE_BUFFER&&kind&&b.signedBuffer==Pi5BufferSigned(kind)&&view->elements<PI5_MAX_BYTE_ELEMENTS,DXGI_DDI_ERR_UNSUPPORTED);
            x.Kind=kind;x.First=view->firstElement;x.Count=view->elements;size.elements=view->elements;return;}
        Pi5Level level={};Require(view->resource->info.Dimension==D3D10DDIRESOURCE_TEXTURE2D&&Pi5AllocationLevel(view->resource->info,view->firstLevel,level),DXGI_DDI_ERR_UNSUPPORTED);
        x.Kind=Pi5BindingTexture;x.First=view->firstLevel;x.Count=view->levels;size.width=level.Width;size.height=level.Height;
        if(b.sampler==NoSampler)return;
        auto sampler=d->samplers[stage][b.sampler];
        if(sampler)Log("Pi5D3D sampler filter=%x address=%u,%u,%u lod=%g,%g bias=%g border=%g,%g,%g,%g view=%p levels=%u/%u\n",sampler->Filter,sampler->AddressU,sampler->AddressV,sampler->AddressW,sampler->MinLOD,sampler->MaxLOD,sampler->MipLODBias,sampler->BorderColor[0],sampler->BorderColor[1],sampler->BorderColor[2],sampler->BorderColor[3],view,view->firstLevel,view->levels);
        Require(sampler&&!sampler->MipLODBias&&!(UINT(sampler->Filter)&~0x15u),DXGI_DDI_ERR_UNSUPPORTED);
        auto Address=[](D3D10_DDI_TEXTURE_ADDRESS_MODE mode)->UINT{
            switch(mode){case D3D10_DDI_TEXTURE_ADDRESS_WRAP:return Pi5Wrap;case D3D10_DDI_TEXTURE_ADDRESS_MIRROR:return Pi5Mirror;
            case D3D10_DDI_TEXTURE_ADDRESS_CLAMP:return Pi5Clamp;case D3D10_DDI_TEXTURE_ADDRESS_BORDER:return Pi5Border;
            case D3D10_DDI_TEXTURE_ADDRESS_MIRRORONCE:return Pi5MirrorOnce;default:throw ErrorCode{DXGI_DDI_ERR_UNSUPPORTED};}
        };
        UINT filter=UINT(sampler->Filter);x.Filter=(filter&4?PI5_FILTER_MAG_LINEAR:0)|(filter&0x10?PI5_FILTER_MIN_LINEAR:0)|(filter&1?PI5_FILTER_MIP_LINEAR:0);
        // Level-of-detail limits are u4.8 values relative to the view's most detailed mip.
        auto Lod=[](float value){return static_cast<UINT>(std::max(0.0f,std::min(15.0f,value))*256.0f+0.5f);};
        Require(!std::isnan(sampler->MinLOD)&&!std::isnan(sampler->MaxLOD),DXGI_DDI_ERR_UNSUPPORTED);x.MinLod=Lod(sampler->MinLOD);x.MaxLod=std::max(x.MinLod,Lod(sampler->MaxLOD));
        x.AddressModes=Address(sampler->AddressU)|(Address(sampler->AddressV)<<3);
        if(Pi5UsesBorder(x.AddressModes))for(UINT i=0;i<4;++i){Require(std::isfinite(sampler->BorderColor[i]),DXGI_DDI_ERR_UNSUPPORTED);float component=std::max(0.0f,std::min(1.0f,sampler->BorderColor[i]));memcpy(&x.Border[i],&component,4);}
       };PrepareBinding();
       UINT mapped=0;while(mapped<c.BindingCount&&memcmp(&c.Bindings[mapped],&x,sizeof(x)))++mapped;
       if(mapped==c.BindingCount){Require(mapped<PI5_BINDINGS,DXGI_DDI_ERR_UNSUPPORTED);c.Bindings[mapped]=x;sizes[mapped]=size;++c.BindingCount;}
       bindingMaps[program][k]=mapped;
      }
    }
    c.Viewport[0]=d->viewport.TopLeftX;c.Viewport[1]=d->viewport.TopLeftY;c.Viewport[2]=d->viewport.Width;c.Viewport[3]=d->viewport.Height;c.Viewport[4]=d->viewport.MinDepth;c.Viewport[5]=d->viewport.MaxDepth;
    auto vertexConstants=d->vs->loops?&d->vs->constants:nullptr,pixelConstants=d->ps->loops?&d->ps->constants:nullptr;
    size_t packetBytes=sizeof(c);
    for(const Shader*s:shaderPrograms)packetBytes=((packetBytes+7)&~size_t(7))+s->code.size()*8+s->uniforms.size()*4+size_t(s->constantWords)*4;
    Require(packetBytes<=PI5_UMD_COMMAND_BYTES);std::vector<BYTE> packet;packet.reserve(packetBytes);packet.resize(sizeof(c));
    c.Coordinate=AppendProgram(d,d->vs->second,d->constants[0],packet,sizes,vertexConstants,bindingMaps[0]);c.Vertex=AppendProgram(d,linkedVertex,d->constants[0],packet,sizes,vertexConstants,bindingMaps[1]);c.Pixel=AppendProgram(d,d->ps->first,d->constants[1],packet,sizes,pixelConstants,bindingMaps[2]);
    uint32_t bounds[4];
    if(DrawBounds(d->vs->second.code.data(),c.Coordinate.CodeCount,reinterpret_cast<const uint32_t*>(packet.data()+c.Coordinate.UniformOffset),c.Coordinate.UniformCount,
                  reinterpret_cast<const uint32_t*>(d->gatheredVertices.data()),c.VertexCount,c.VertexComponents,c.Viewport,resources[0]->info.Width,resources[0]->info.Height,bounds)){
        if(c.Pipeline.Flags&8)for(unsigned axis=0;axis<2;++axis){bounds[axis]=std::max(bounds[axis],c.Pipeline.Scissor[axis]);bounds[axis+2]=std::min(bounds[axis+2],c.Pipeline.Scissor[axis+2]);}
        if(bounds[0]<bounds[2]&&bounds[1]<bounds[3]){c.Pipeline.Flags|=8;memcpy(c.Pipeline.Scissor,bounds,sizeof(bounds));}
    }
    if(DrawCoverage(d->vs->second.code.data(),c.Coordinate.CodeCount,reinterpret_cast<const uint32_t*>(packet.data()+c.Coordinate.UniformOffset),c.Coordinate.UniformCount,
                    reinterpret_cast<const uint32_t*>(d->gatheredVertices.data()),c.VertexCount,c.VertexComponents,c.Viewport,resources[0]->info.Width,resources[0]->info.Height,c.Pipeline,bounds)){
        c.Pipeline.Flags|=PI5_PIPELINE_COVERAGE;memcpy(c.Pipeline.Coverage,bounds,sizeof(bounds));
    }
    Require(packet.size()<=PI5_UMD_COMMAND_BYTES);c.Header.Bytes=static_cast<UINT>(packet.size());memcpy(packet.data(),&c,sizeof(c));
#ifdef _DEBUG
    // Run the kernel's structural validation first so a rejected draw names its interface.
    Pi5AllocationInfo infos[PI5_MAX_ALLOCATIONS]={};for(UINT i=0;i<resourceCount;++i)infos[i]=resources[i]->info;
    if(!ValidateCommand(packet.data(),c.Header.Bytes,infos,resourceCount)){
        LogCritical("Pi5D3D invalid draw bytes=%u allocations=%u vertices=%u stride=%u components=%u varyings=%u noperspective=%x flat=%x bindings=%u viewport=%g,%g,%g,%g,%g,%g\n",
            c.Header.Bytes,resourceCount,c.VertexCount,c.VertexStride,c.VertexComponents,c.VaryingScalars,c.NonPerspectiveMask,c.FlatMask,c.BindingCount,c.Viewport[0],c.Viewport[1],c.Viewport[2],c.Viewport[3],c.Viewport[4],c.Viewport[5]);
        for(UINT i=0;i<resourceCount;++i)LogCritical("  allocation %u dim=%u %ux%u pitch=%u format=%u levels=%u bytes=%u bind=%x misc=%x\n",i,infos[i].Dimension,infos[i].Width,infos[i].Height,infos[i].Pitch,infos[i].Format,infos[i].Levels,infos[i].Bytes,infos[i].BindFlags,infos[i].MiscFlags);
        for(UINT k=0;k<c.BindingCount;++k){const auto&x=c.Bindings[k];LogCritical("  binding %u kind=%u allocation=%u first=%u count=%u filter=%u border=%u lod=%u-%u\n",k,x.Kind,x.Allocation,x.First,x.Count,x.Filter,x.AddressModes,x.MinLod,x.MaxLod);}
        const Pi5Program*programs[]={&c.Coordinate,&c.Vertex,&c.Pixel};
        for(UINT i=0;i<3;++i){const auto&p=*programs[i];uint32_t registers=0;
            bool valid=ValidateProgram(reinterpret_cast<const uint64_t*>(packet.data()+p.CodeOffset),p.CodeCount,reinterpret_cast<const uint32_t*>(packet.data()+p.UniformOffset),p.UniformCount,DrawProgramRules(c,i),nullptr,&registers);
            LogCritical("  program %u code=%u uniforms=%u constants=%u valid=%u registers=%08x\n",i,p.CodeCount,p.UniformCount,p.ConstantWords,valid,registers);}
        throw ErrorCode{DXGI_DDI_ERR_UNSUPPORTED};
    }
#endif
    d->Submit(packet.data(),c.Header.Bytes,resources,resourceCount);
}
static void DrawBody(D3D10DDI_HDEVICE h,UINT count,UINT first,bool indexed,INT base,UINT instances,UINT firstInstance){auto d=Get(h);
    Log("Pi5D3D Draw count=%u first=%u topology=%u target=%p layout=%p viewport=%g,%g,%g,%g mask=%x\n",count,first,d->topology,d->target,d->layout,d->viewport.TopLeftX,d->viewport.TopLeftY,d->viewport.Width,d->viewport.Height,d->sampleMask);
    bool strip=d->topology==D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;Require(d->vs&&d->ps&&d->target&&d->target->resource&&(strip||d->topology==D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST),DXGI_DDI_ERR_UNSUPPORTED);
    if(count<3||!instances||!d->viewport.Width||!d->viewport.Height)return;
    Require(uint64_t(first)+count<=UINT64_C(0x100000000));
    // The validated kernel packet is bounded to 4095 expanded vertices. Split
    // larger API draws on triangle boundaries without changing draw order,
    // system IDs, instance divisors, or strip restart/provoking-vertex state.
    UINT block=strip?1365u:4095u,vertexIdBias=indexed?UINT(base):first;
    if(count<=block){
        VertexOrderState state;auto order=VertexOrder(d,count,first,indexed,base,strip,state);if(order.empty())return;
        UINT perBatch=4095u/static_cast<UINT>(order.size());
        for(UINT instance=0;instance<instances;){UINT batch=std::min(perBatch,instances-instance);
            DrawVertices(d,order,batch,firstInstance,instance,vertexIdBias);instance+=batch;}
    }else for(UINT instance=0;instance<instances;++instance){
        VertexOrderState state;
        for(UINT cursor=0;cursor<count;){UINT take=std::min(block,count-cursor);
            auto order=VertexOrder(d,take,first+cursor,indexed,base,strip,state);
            if(!order.empty())DrawVertices(d,order,1,firstInstance,instance,vertexIdBias);cursor+=take;
        }
    }
}
static void DrawCommon(D3D10DDI_HDEVICE h,UINT count,UINT first,bool indexed,INT base,UINT instances,UINT firstInstance){Guard(Get(h),"Draw",[&]{
    try{DrawBody(h,count,first,indexed,base,instances,firstInstance);}
    catch(const ErrorCode&e){
#ifdef _DEBUG
        auto d=Get(h);LogCritical("Pi5D3D rejected draw count=%u first=%u instances=%u indexed=%u layout=%p viewport=%g,%g,%g,%g\n",count,first,instances,indexed,d->layout,d->viewport.TopLeftX,d->viewport.TopLeftY,d->viewport.Width,d->viewport.Height);
        for(auto program:{d->vs,d->ps})if(program){uint32_t hash=2166136261u;for(auto word:program->tokens)hash=(hash^word)*16777619u;
            LogCritical("Pi5D3D active shader stage=%u hash=%08x compiled=%u inputs=%x,%x,%x,%x,%x,%x,%x,%x\n",unsigned(program->stage),hash,program->compiled,program->first.inputs[0],program->first.inputs[1],program->first.inputs[2],program->first.inputs[3],program->first.inputs[4],program->first.inputs[5],program->first.inputs[6],program->first.inputs[7]);}
#endif
        if(e.hr!=DXGI_DDI_ERR_UNSUPPORTED||!SkipUnsupportedDraws())throw;LogCritical("Pi5D3D skipped unsupported draw\n");}
});}
static void APIENTRY Draw(D3D10DDI_HDEVICE h,UINT count,UINT first){DrawCommon(h,count,first,false,0,1,0);}
static void APIENTRY DrawIndexed(D3D10DDI_HDEVICE h,UINT count,UINT first,INT base){DrawCommon(h,count,first,true,base,1,0);}
static void APIENTRY DrawInstanced(D3D10DDI_HDEVICE h,UINT count,UINT instances,UINT first,UINT firstInstance){DrawCommon(h,count,first,false,0,instances,firstInstance);}
static void APIENTRY DrawIndexedInstanced(D3D10DDI_HDEVICE h,UINT count,UINT instances,UINT first,INT base,UINT firstInstance){DrawCommon(h,count,first,true,base,instances,firstInstance);}
static void APIENTRY Flush(D3D10DDI_HDEVICE h){Guard(Get(h),"Flush",[&]{Get(h)->FlushBatch();});}
static void APIENTRY Hazard(D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE){}
static void APIENTRY ViewHazard(D3D10DDI_HDEVICE,D3D10DDI_HSHADERRESOURCEVIEW,D3D10DDI_HRESOURCE){}
static void APIENTRY Predication(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY q,BOOL){Guard(Get(h),"SetPredication",[&]{Require(!q.pDrvPrivate,DXGI_DDI_ERR_UNSUPPORTED);});}
static void APIENTRY GeometryShader(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER s){Guard(Get(h),"SetGeometryShader",[&]{Require(!s.pDrvPrivate,DXGI_DDI_ERR_UNSUPPORTED);});}
static void APIENTRY StreamOutput(D3D10DDI_HDEVICE h,UINT count,UINT,const D3D10DDI_HRESOURCE*r,const UINT*){Guard(Get(h),"SetStreamOutput",[&]{Require(count<=4);for(UINT i=0;i<count;++i)Require(!r[i].pDrvPrivate,DXGI_DDI_ERR_UNSUPPORTED);});}
static void APIENTRY IndexBuffer(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE resource,DXGI_FORMAT format,UINT offset){Guard(Get(h),"SetIndexBuffer",[&]{
    auto d=Get(h);auto r=Get(resource);if(r){Require(r->info.Dimension==D3D10DDIRESOURCE_BUFFER&&(r->info.BindFlags&D3D10_DDI_BIND_INDEX_BUFFER)&&(format==DXGI_FORMAT_R16_UINT||format==DXGI_FORMAT_R32_UINT),DXGI_DDI_ERR_UNSUPPORTED);UINT bytes=format==DXGI_FORMAT_R16_UINT?2:4;Require(!(offset%bytes)&&offset<=r->info.Width);}
    d->indexBuffer=r;d->indexFormat=format;d->indexOffset=offset;
});}
static BOOL APIENTRY Busy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE resource){auto d=Get(h);if(FAILED(d->failure))return TRUE;
    try{d->Lock(Get(resource),D3D10_DDI_MAP_READ,D3D10_DDI_MAP_FLAG_DONOTWAIT);d->Unlock(Get(resource));return FALSE;}
    catch(const ErrorCode&e){if(e.hr!=DXGI_DDI_ERR_WASSTILLDRAWING)d->Error(e.hr,"IsStagingBusy");return TRUE;}
}
static SIZE_T APIENTRY QuerySize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEQUERY*){return sizeof(EventQuery);}
static void APIENTRY CreateQuery(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATEQUERY*a,D3D10DDI_HQUERY out,D3D10DDI_HRTQUERY){
    auto q=new(out.pDrvPrivate)EventQuery;auto d=Get(h);
    Guard(d,"CreateQuery",[&]{Require(a&&a->Query==D3D10DDI_QUERY_EVENT&&!a->MiscFlags,DXGI_DDI_ERR_UNSUPPORTED);
        q->marker.info={PI5_UMD_ABI,4096,1,1,64,28,0,D3D10DDIRESOURCE_TEXTURE2D};D3DDDI_ALLOCATIONINFO info={};info.pPrivateDriverData=&q->marker.info;info.PrivateDriverDataSize=sizeof(q->marker.info);
        D3DDDICB_ALLOCATE request={};request.NumAllocations=1;request.pAllocationInfo=&info;Check(d->kernel.pfnAllocateCb(d->runtime.handle,&request));q->marker.allocation=info.hAllocation;Require(info.hAllocation!=0,E_FAIL);
    });
}
static void APIENTRY EndQuery(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY query){auto d=Get(h);auto q=static_cast<EventQuery*>(query.pDrvPrivate);
    Guard(d,"EndQuery",[&]{Require(q&&q->marker.allocation);q->issued=true;q->ready=false;if(!++q->serial)++q->serial;
        Pi5ClearCommand c={};c.Header=Header(Pi5Clear,sizeof(c));c.Color=q->serial;Resource*resources[]={&q->marker};d->Submit(&c,sizeof(c),resources,1);
    });
}
static void APIENTRY GetQueryData(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY query,void *data,UINT bytes,UINT){auto d=Get(h);auto q=static_cast<EventQuery*>(query.pDrvPrivate);
    Guard(d,"QueryGetData",[&]{Require(q&&((!data&&!bytes)||(data&&bytes==sizeof(BOOL))));if(!q->issued)throw ErrorCode{DXGI_DDI_ERR_WASSTILLDRAWING};
        if(!q->ready){auto p=static_cast<const volatile UINT*>(d->Lock(&q->marker,D3D10_DDI_MAP_READ,D3D10_DDI_MAP_FLAG_DONOTWAIT));UINT value=*p;d->Unlock(&q->marker);Require(value==q->serial,E_FAIL);q->ready=true;}
        if(data)*static_cast<BOOL*>(data)=TRUE;
    });
}
static void APIENTRY DestroyQuery(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY query){auto d=Get(h);auto q=static_cast<EventQuery*>(query.pDrvPrivate);if(!q)return;
    if(q->marker.allocation){D3DDDICB_DEALLOCATE a={};a.NumAllocations=1;a.HandleList=&q->marker.allocation;HRESULT hr=d->kernel.pfnDeallocateCb(d->runtime.handle,&a);if(FAILED(hr))d->Error(hr,"DestroyQuery");}q->~EventQuery();
}
template<class T> static SIZE_T APIENTRY StateSize(D3D10DDI_HDEVICE,const T*){return sizeof(T);}
template<class T,class H,class R> static void APIENTRY CreateState(D3D10DDI_HDEVICE h,const T*desc,H state,R){Guard(Get(h),"CreateState",[&]{Require(desc&&state.pDrvPrivate);new(state.pDrvPrivate)T(*desc);});}
template<class H> static void APIENTRY DestroyState(D3D10DDI_HDEVICE,H){}
static void APIENTRY SetBlend(D3D10DDI_HDEVICE h,D3D10DDI_HBLENDSTATE state,const FLOAT factor[4],UINT mask){auto d=Get(h);d->blend=static_cast<const D3D10_DDI_BLEND_DESC*>(state.pDrvPrivate);memcpy(d->blendFactor,factor,sizeof(d->blendFactor));d->sampleMask=mask;}
static void APIENTRY SetDepth(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILSTATE state,UINT ref){auto d=Get(h);d->depth=static_cast<const D3D10_DDI_DEPTH_STENCIL_DESC*>(state.pDrvPrivate);d->stencilRef=ref;}
static void APIENTRY SetRaster(D3D10DDI_HDEVICE h,D3D10DDI_HRASTERIZERSTATE state){Get(h)->raster=static_cast<const D3D10_DDI_RASTERIZER_DESC*>(state.pDrvPrivate);}
template<unsigned Stage> static void APIENTRY SetSamplers(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HSAMPLER *samplers){Guard(Get(h),"SetSamplers",[&]{Require(first<=16&&count<=16-first);for(UINT i=0;i<count;++i)Get(h)->samplers[Stage][first+i]=static_cast<const D3D10_DDI_SAMPLER_DESC*>(samplers[i].pDrvPrivate);});}
static void APIENTRY CounterInfo(D3D10DDI_HDEVICE,D3D10DDI_COUNTER_INFO *info){*info={};}
static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE h){auto d=Get(h);Guard(d,"DestroyDevice flush",[&]{d->FlushBatch();});for(auto r:{&d->vertexScratch,&d->uploadScratch})if(r->allocation){D3DDDICB_DEALLOCATE a={};a.NumAllocations=1;a.HandleList=&r->allocation;HRESULT hr=d->kernel.pfnDeallocateCb(d->runtime.handle,&a);if(FAILED(hr))d->Error(hr,"Destroy scratch allocation");}if(d->context.hContext){D3DDDICB_DESTROYCONTEXT a={};a.hContext=d->context.hContext;HRESULT hr=d->kernel.pfnDestroyContextCb(d->runtime.handle,&a);if(FAILED(hr))d->Error(hr,"DestroyContext");}FlushLog();d->~Device();}
static void APIENTRY CheckFormat(D3D10DDI_HDEVICE,DXGI_FORMAT format,UINT*out){*out=ColorFormat(format)?(D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET|D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE|D3D10_DDI_FORMAT_SUPPORT_BLENDABLE):0;}
static void APIENTRY Multisample(D3D10DDI_HDEVICE,DXGI_FORMAT format,UINT count,UINT*out){*out=ColorFormat(format)&&count==1?1:0;}
template<class Function,unsigned Id> struct Unsupported;
template<class R,class...A,unsigned Id> struct Unsupported<R(APIENTRY*)(D3D10DDI_HDEVICE,A...),Id>{static R APIENTRY Call(D3D10DDI_HDEVICE h,A...){char name[64];sprintf_s(name,"unsupported DDI %u",Id);Get(h)->Error(DXGI_DDI_ERR_UNSUPPORTED,name);if constexpr(!std::is_void_v<R>)return R{};}};
static void APIENTRY Relocate(D3D10DDI_HDEVICE,D3D10DDI_DEVICEFUNCS *f){SetDeviceFunctions(f);}
void SetDeviceFunctions(D3D10DDI_DEVICEFUNCS*f){
    *f={};
#include "ddi-defaults.inc"
    f->pfnCalcPrivateResourceSize=ResourceSize;f->pfnCreateResource=CreateResource;f->pfnDestroyResource=DestroyResource;
    f->pfnCalcPrivateOpenedResourceSize=OpenedResourceSize;f->pfnOpenResource=OpenResource;
    f->pfnResourceMap=f->pfnStagingResourceMap=f->pfnDynamicIABufferMapNoOverwrite=f->pfnDynamicIABufferMapDiscard=f->pfnDynamicConstantBufferMapDiscard=f->pfnDynamicResourceMapDiscard=Map;
    f->pfnResourceUnmap=f->pfnStagingResourceUnmap=f->pfnDynamicIABufferUnmap=f->pfnDynamicConstantBufferUnmap=f->pfnDynamicResourceUnmap=Unmap;
    f->pfnResourceUpdateSubresourceUP=f->pfnDefaultConstantBufferUpdateSubresourceUP=UpdateResource;
    f->pfnCalcPrivateRenderTargetViewSize=TargetSize;f->pfnCreateRenderTargetView=CreateTarget;f->pfnDestroyRenderTargetView=DestroyTarget;f->pfnSetRenderTargets=SetTargets;f->pfnClearRenderTargetView=ClearTarget;
    f->pfnCalcPrivateShaderResourceViewSize=ViewSize;f->pfnCreateShaderResourceView=CreateView;f->pfnDestroyShaderResourceView=DestroyView;f->pfnVsSetShaderResources=SetViews<0>;f->pfnPsSetShaderResources=SetViews<1>;f->pfnGsSetShaderResources=SetViews<2>;
    f->pfnCalcPrivateShaderSize=ShaderSize;f->pfnCreateVertexShader=CreateShader<ShaderStage::Vertex>;f->pfnCreatePixelShader=CreateShader<ShaderStage::Pixel>;f->pfnDestroyShader=DestroyShader;f->pfnVsSetShader=SetVs;f->pfnPsSetShader=SetPs;
    f->pfnVsSetConstantBuffers=SetConstants<0>;f->pfnPsSetConstantBuffers=SetConstants<1>;
    f->pfnCalcPrivateElementLayoutSize=LayoutSize;f->pfnCreateElementLayout=CreateLayout;f->pfnDestroyElementLayout=DestroyLayout;f->pfnIaSetInputLayout=SetLayout;f->pfnIaSetVertexBuffers=SetVertices;f->pfnIaSetTopology=SetTopology;
    f->pfnSetViewports=SetViewports;f->pfnSetScissorRects=SetScissors;f->pfnDraw=Draw;f->pfnDrawIndexed=DrawIndexed;f->pfnDrawInstanced=DrawInstanced;f->pfnDrawIndexedInstanced=DrawIndexedInstanced;f->pfnResourceCopy=CopyResource;f->pfnResourceCopyRegion=CopyRegion;f->pfnFlush=Flush;f->pfnResourceReadAfterWriteHazard=Hazard;f->pfnShaderResourceViewReadAfterWriteHazard=ViewHazard;f->pfnResourceIsStagingBusy=Busy;
    f->pfnCheckFormatSupport=CheckFormat;f->pfnCheckMultisampleQualityLevels=Multisample;f->pfnDestroyDevice=DestroyDevice;
    f->pfnRelocateDeviceFuncs=Relocate;
    f->pfnSetPredication=Predication;f->pfnGsSetShader=GeometryShader;f->pfnSoSetTargets=StreamOutput;f->pfnIaSetIndexBuffer=IndexBuffer;
    f->pfnCalcPrivateQuerySize=QuerySize;f->pfnCreateQuery=CreateQuery;f->pfnDestroyQuery=DestroyQuery;f->pfnQueryEnd=EndQuery;f->pfnQueryGetData=GetQueryData;
    f->pfnSetBlendState=SetBlend;f->pfnSetDepthStencilState=SetDepth;f->pfnSetRasterizerState=SetRaster;
    f->pfnVsSetSamplers=SetSamplers<0>;f->pfnPsSetSamplers=SetSamplers<1>;f->pfnGsSetSamplers=SetSamplers<2>;f->pfnCheckCounterInfo=CounterInfo;
    f->pfnCalcPrivateBlendStateSize=StateSize<D3D10_DDI_BLEND_DESC>;f->pfnCreateBlendState=CreateState<D3D10_DDI_BLEND_DESC,D3D10DDI_HBLENDSTATE,D3D10DDI_HRTBLENDSTATE>;f->pfnDestroyBlendState=DestroyState<D3D10DDI_HBLENDSTATE>;
    f->pfnCalcPrivateDepthStencilStateSize=StateSize<D3D10_DDI_DEPTH_STENCIL_DESC>;f->pfnCreateDepthStencilState=CreateState<D3D10_DDI_DEPTH_STENCIL_DESC,D3D10DDI_HDEPTHSTENCILSTATE,D3D10DDI_HRTDEPTHSTENCILSTATE>;f->pfnDestroyDepthStencilState=DestroyState<D3D10DDI_HDEPTHSTENCILSTATE>;
    f->pfnCalcPrivateRasterizerStateSize=StateSize<D3D10_DDI_RASTERIZER_DESC>;f->pfnCreateRasterizerState=CreateState<D3D10_DDI_RASTERIZER_DESC,D3D10DDI_HRASTERIZERSTATE,D3D10DDI_HRTRASTERIZERSTATE>;f->pfnDestroyRasterizerState=DestroyState<D3D10DDI_HRASTERIZERSTATE>;
    f->pfnCalcPrivateSamplerSize=StateSize<D3D10_DDI_SAMPLER_DESC>;f->pfnCreateSampler=CreateState<D3D10_DDI_SAMPLER_DESC,D3D10DDI_HSAMPLER,D3D10DDI_HRTSAMPLER>;f->pfnDestroySampler=DestroyState<D3D10DDI_HSAMPLER>;
}
template<class T,UINT Id>static HRESULT APIENTRY UnsupportedDxgi(T*){
    static volatile LONG captured=0;
    if(InterlockedCompareExchange(&captured,1,0)==0)PersistProcessEvent("unsupported-dxgi",Id);
    return DXGI_DDI_ERR_UNSUPPORTED;
}
static HRESULT APIENTRY Present(DXGI_DDI_ARG_PRESENT*a){
    auto d=reinterpret_cast<Device*>(a->hDevice);auto src=reinterpret_cast<Resource*>(a->hSurfaceToPresent),dst=reinterpret_cast<Resource*>(a->hDstResource);
    RecordCall(d,"DXGI Present");
    if(!src||!src->allocation||src->mapped||a->SrcSubResourceIndex||a->DstSubResourceIndex||!d->dxgi||!d->dxgi->pfnPresentCb||(dst&&(!dst->allocation||dst->mapped))){
        PersistFirstFatal(d,E_INVALIDARG,"DXGI Present validation");return E_INVALIDARG;
    }
    Guard(d,"Present flush",[&]{d->FlushBatch();});if(FAILED(d->failure))return d->failure;
    DXGIDDICB_PRESENT present={};present.hSrcAllocation=src->allocation;present.hDstAllocation=dst?dst->allocation:0;present.pDXGIContext=a->pDXGIContext;present.hContext=d->context.hContext;
    HRESULT hr=d->dxgi->pfnPresentCb(d->runtime.handle,&present);
    if(FAILED(hr)){PersistFirstFatal(d,hr,"DXGI Present callback");LogCritical("Pi5D3D Present failed: %08lx src=%x dst=%x\n",hr,present.hSrcAllocation,present.hDstAllocation);}
    else Log("Pi5D3D Present hr=%08lx src=%x dst=%x\n",hr,present.hSrcAllocation,present.hDstAllocation);
    return hr;
}
static HRESULT APIENTRY ResolveShared(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE*a){
    auto d=reinterpret_cast<Device*>(a->hDevice);auto r=reinterpret_cast<Resource*>(a->hResource);
    Guard(d,"ResolveShared flush",[&]{d->FlushBatch();});
    return r&&r->allocation&&!r->mapped?d->failure:E_INVALIDARG;
}
static HRESULT APIENTRY RotateResources(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES*a){
    auto d=reinterpret_cast<Device*>(a->hDevice);
    Guard(d,"RotateResourceIdentities",[&]{d->FlushBatch();Require(a->Resources&&a->Resources<=16&&a->pResources);
        for(UINT i=0;i<a->Resources;++i){auto r=reinterpret_cast<Resource*>(a->pResources[i]);Require(r&&r->allocation&&!r->mapped);for(UINT j=0;j<i;++j)Require(a->pResources[j]!=a->pResources[i]);}
        auto first=reinterpret_cast<Resource*>(a->pResources[0]);Resource saved=*first;
        for(UINT i=0;i<a->Resources;++i){auto dst=reinterpret_cast<Resource*>(a->pResources[i]);auto src=i+1<a->Resources?reinterpret_cast<Resource*>(a->pResources[i+1]):&saved;
            dst->allocation=src->allocation;dst->kernelResource=src->kernelResource;dst->info=src->info;dst->primary=src->primary;
        }
    });return d->failure;
}
static HRESULT APIENTRY Priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *a){
    auto d=reinterpret_cast<Device*>(a->hDevice);auto r=reinterpret_cast<Resource*>(a->hResource);if(!r||!d->kernel.pfnSetPriorityCb)return E_INVALIDARG;
    D3DDDICB_SETPRIORITY p={};p.NumAllocations=1;p.HandleList=&r->allocation;p.pPriorities=&a->Priority;return d->kernel.pfnSetPriorityCb(d->runtime.handle,&p);
}
static HRESULT APIENTRY Residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *a){
    auto d=reinterpret_cast<Device*>(a->hDevice);if(!d->kernel.pfnQueryResidencyCb)return E_INVALIDARG;
    for(SIZE_T i=0;i<a->Resources;++i){auto r=reinterpret_cast<Resource*>(a->pResources[i]);if(!r)return E_INVALIDARG;D3DDDI_RESIDENCYSTATUS status={};D3DDDICB_QUERYRESIDENCY q={};q.NumAllocations=1;q.HandleList=&r->allocation;q.pResidencyStatus=&status;HRESULT hr=d->kernel.pfnQueryResidencyCb(d->runtime.handle,&q);if(FAILED(hr))return hr;a->pStatus[i]=static_cast<DXGI_DDI_RESIDENCY>(status);}return S_OK;
}
static HRESULT APIENTRY DisplayMode(DXGI_DDI_ARG_SETDISPLAYMODE*a){
    if(!a||!a->hDevice)return E_INVALIDARG;auto d=reinterpret_cast<Device*>(a->hDevice);auto r=reinterpret_cast<Resource*>(a->hResource);
    if(!d->nativeDisplay||!r||!r->allocation||!r->primary||r->mapped||a->SubResourceIndex||!d->kernel.pfnSetDisplayModeCb)return DXGI_DDI_ERR_UNSUPPORTED;
    Guard(d,"DisplayMode flush",[&]{d->FlushBatch();});if(FAILED(d->failure))return d->failure;D3DDDICB_SETDISPLAYMODE mode={};mode.hPrimaryAllocation=r->allocation;
    HRESULT hr=d->kernel.pfnSetDisplayModeCb(d->runtime.handle,&mode);Log("Pi5D3D SetDisplayMode hr=%08lx primary=%x\n",hr,r->allocation);return hr;
}
struct Adapter {Pi5AdapterInfo info;};
static SIZE_T APIENTRY DeviceSize(D3D10DDI_HADAPTER,const D3D10DDIARG_CALCPRIVATEDEVICESIZE*){Log("Pi5D3D DeviceSize\n");return sizeof(Device);}
static HRESULT APIENTRY CreateDevice(D3D10DDI_HADAPTER adapter,D3D10DDIARG_CREATEDEVICE*a){
    if(a)Log("Pi5D3D CreateDevice interface=%08x version=%08x\n",a->Interface,a->Version);
    if(!a||(a->Interface!=D3D10_0_DDI_INTERFACE_VERSION&&a->Interface!=D3D10_0_7_DDI_INTERFACE_VERSION)||!a->pKTCallbacks||!a->pUMCallbacks||!a->hDrvDevice.pDrvPrivate)return E_INVALIDARG;
    auto d=new(a->hDrvDevice.pDrvPrivate)Device;d->runtime=a->hRTDevice;d->core=a->hRTCoreLayer;d->kernel=*a->pKTCallbacks;d->user=*a->pUMCallbacks;d->dxgi=a->DXGIBaseDDI.pDXGIBaseCallbacks;
    d->vertexScratch.info={PI5_UMD_ABI,262144,262144,1,262144,0,1,D3D10DDIRESOURCE_BUFFER};
    d->nativeDisplay=(static_cast<Adapter*>(adapter.pDrvPrivate)->info.Caps&PI5_ADAPTER_NATIVE_DISPLAY)!=0;
    if(!d->kernel.pfnCreateContextCb||!d->kernel.pfnDestroyContextCb||!d->kernel.pfnAllocateCb||!d->kernel.pfnDeallocateCb||!d->kernel.pfnLockCb||!d->kernel.pfnUnlockCb||!d->kernel.pfnRenderCb||!d->user.pfnSetErrorCb){d->~Device();return E_INVALIDARG;}
    d->context.EngineAffinity=1;HRESULT hr=d->kernel.pfnCreateContextCb(d->runtime.handle,&d->context);
    if(FAILED(hr)){d->~Device();return hr;}
    SetDeviceFunctions(a->pDeviceFuncs);
    if(a->DXGIBaseDDI.pDXGIDDIBaseFunctions){auto f=a->DXGIBaseDDI.pDXGIDDIBaseFunctions;*f={};f->pfnPresent=Present;f->pfnGetGammaCaps=UnsupportedDxgi<DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS,1>;f->pfnSetDisplayMode=DisplayMode;f->pfnSetResourcePriority=Priority;f->pfnQueryResourceResidency=Residency;f->pfnRotateResourceIdentities=RotateResources;f->pfnBlt=UnsupportedDxgi<DXGI_DDI_ARG_BLT,2>;}
    if(a->Interface==D3D10_0_7_DDI_INTERFACE_VERSION&&a->DXGIBaseDDI.pDXGIDDIBaseFunctions2)a->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnResolveSharedResource=ResolveShared;
    return S_OK;
}
static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER h){delete static_cast<Adapter*>(h.pDrvPrivate);return S_OK;}
static HRESULT APIENTRY Versions(D3D10DDI_HADAPTER,UINT*count,UINT64*versions){
    Log("Pi5D3D Versions\n");if(!count)return E_INVALIDARG;
    if(versions&&*count<2){*count=2;return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);}
    *count=2;if(versions){versions[0]=D3D10_0_7_DDI_SUPPORTED;versions[1]=D3D10_0_DDI_SUPPORTED;}return S_OK;
}
static HRESULT APIENTRY Caps(D3D10DDI_HADAPTER,const D3D10_2DDIARG_GETCAPS*a){if(!a||!a->pData)return E_INVALIDARG;Log("Pi5D3D Caps type=%u bytes=%u\n",a->Type,a->DataSize);if((a->Type==D3D11DDICAPS_THREADING||a->Type==D3D11DDICAPS_SHADER)&&a->DataSize==sizeof(UINT)){*static_cast<UINT*>(a->pData)=0;return S_OK;}if(a->Type==D3D11DDICAPS_3DPIPELINESUPPORT&&a->DataSize==sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS)){static_cast<D3D11DDI_3DPIPELINESUPPORT_CAPS*>(a->pData)->Caps=D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0);return S_OK;}return E_NOTIMPL;}
}
extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER*a){
    if(a)pi5::Log("Pi5D3D OpenAdapter10_2 interface=%08x version=%08x\n",a->Interface,a->Version);
    if(!a||!a->pAdapterCallbacks||!a->pAdapterCallbacks->pfnQueryAdapterInfoCb||!a->pAdapterFuncs_2)return E_INVALIDARG;
    auto adapter=new(std::nothrow)pi5::Adapter;if(!adapter)return E_OUTOFMEMORY;D3DDDICB_QUERYADAPTERINFO query={};query.pPrivateDriverData=&adapter->info;query.PrivateDriverDataSize=sizeof(adapter->info);
    HRESULT hr=a->pAdapterCallbacks->pfnQueryAdapterInfoCb(a->hRTAdapter.handle,&query);
    if(FAILED(hr)||adapter->info.Magic!=PI5_UMD_MAGIC||adapter->info.Version!=PI5_UMD_ABI||adapter->info.V3dVersion!=71){delete adapter;return FAILED(hr)?hr:E_NOINTERFACE;}
    *a->pAdapterFuncs_2={pi5::DeviceSize,pi5::CreateDevice,pi5::CloseAdapter,pi5::Versions,pi5::Caps};a->hAdapter.pDrvPrivate=adapter;return S_OK;
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER*a){
    if(!a||a->Interface!=D3D10_0_DDI_INTERFACE_VERSION||!a->pAdapterFuncs)return E_NOINTERFACE;
    auto original=a->pAdapterFuncs;D3D10_2DDI_ADAPTERFUNCS functions={};a->pAdapterFuncs_2=&functions;HRESULT hr=OpenAdapter10_2(a);a->pAdapterFuncs=original;
    if(SUCCEEDED(hr))*original={functions.pfnCalcPrivateDeviceSize,functions.pfnCreateDevice,functions.pfnCloseAdapter};return hr;
}
