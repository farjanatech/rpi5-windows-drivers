#include "precomp.h"
#include "gpu.h"
#include "../native/texture.h"
#define TRY(call) do {NTSTATUS result=(call);if(result!=STATUS_SUCCESS)return result;}while(0)
#include "shadow.inc"
#include "blit.inc"
#if DBG
#include "tile-check.inc"
void GpuPath::RecordJob(const Pi5CommandHeader*h,const Pi5AllocationInfo*r,LONGLONG ticks){
    PI5_JOB_SAMPLE value={};value.Ticks=ticks;value.Operation=h->Operation;value.Width=r[0].Width;value.Height=r[0].Height;value.ScissorWidth=value.Width;value.ScissorHeight=value.Height;
    value.SkippedLoads=jobSkippedLoads;value.EndTicks=KeQueryPerformanceCounter(nullptr).QuadPart;value.RenderTicks=jobRenderTicks;value.TileTicks=jobTileTicks;value.CopyTicks=jobCopyTicks;
    if(h->Operation==Pi5Draw){auto c=reinterpret_cast<const Pi5DrawCommand*>(h);value.Vertices=c->VertexCount;value.PixelWords=c->Pixel.CodeCount;value.Flags=c->Pipeline.Flags;
        if(value.Flags&8){value.ScissorWidth=c->Pipeline.Scissor[2]-c->Pipeline.Scissor[0];value.ScissorHeight=c->Pipeline.Scissor[3]-c->Pipeline.Scissor[1];}
        auto programCode=reinterpret_cast<const uint32_t*>(reinterpret_cast<const UCHAR*>(h)+c->Pixel.CodeOffset);value.PixelHash=2166136261u;
        for(ULONG i=0;i<c->Pixel.CodeCount*2;++i)value.PixelHash=(value.PixelHash^programCode[i])*16777619u;
        for(ULONG i=0;i<c->BindingCount;++i)value.BindingBytes+=r[c->Bindings[i].Allocation].Bytes;
        value.ScissorPixels=uint64_t(value.ScissorWidth)*value.ScissorHeight;value.WordPixels=value.ScissorPixels*value.PixelWords;
    }
    if(h->Operation==Pi5DrawBatch){const auto&batch=*reinterpret_cast<const Pi5BatchCommand*>(h);value.Vertices=batch.Count;
        ULONG bounds[4]={r[0].Width,r[0].Height,0,0};
        for(ULONG n=0;n<batch.Count;++n){auto c=reinterpret_cast<const Pi5DrawCommand*>(reinterpret_cast<const UCHAR*>(h)+batch.Entries[n].Offset);
            uint64_t pixels=(c->Pipeline.Flags&8)?uint64_t(c->Pipeline.Scissor[2]-c->Pipeline.Scissor[0])*(c->Pipeline.Scissor[3]-c->Pipeline.Scissor[1]):uint64_t(r[0].Width)*r[0].Height;
            value.ScissorPixels+=pixels;value.WordPixels+=pixels*c->Pixel.CodeCount;
            for(unsigned axis=0;axis<2;++axis){ULONG low=(c->Pipeline.Flags&8)?c->Pipeline.Scissor[axis]:0,high=(c->Pipeline.Flags&8)?c->Pipeline.Scissor[axis+2]:(axis?r[0].Height:r[0].Width);
                if(low<bounds[axis])bounds[axis]=low;if(high>bounds[axis+2])bounds[axis+2]=high;}
            if(c->Pixel.CodeCount<=value.PixelWords)continue;value.PixelWords=c->Pixel.CodeCount;value.PixelHash=2166136261u;
            auto instructions=reinterpret_cast<const uint32_t*>(reinterpret_cast<const UCHAR*>(c)+c->Pixel.CodeOffset);
            for(ULONG i=0;i<c->Pixel.CodeCount*2;++i)value.PixelHash=(value.PixelHash^instructions[i])*16777619u;
        }
        value.ScissorWidth=bounds[2]-bounds[0];value.ScissorHeight=bounds[3]-bounds[1];
    }
    if(h->Operation==Pi5CopyRegion){const auto&c=*reinterpret_cast<const Pi5RegionCommand*>(h);value.ScissorWidth=c.Width;value.ScissorHeight=c.Height;value.BindingBytes=r[1].Bytes;}
    if(activeShadow!=UINT32_MAX)value.Flags|=0x10000;
    if(sampledShadow)value.Flags|=0x20000;
    ExAcquireFastMutex(&jobLock);value.Sequence=++jobSequence;jobSamples[(value.Sequence-1)%512]=value;
    jobProfile.Sequence=value.Sequence;jobProfile.EndTicks=value.EndTicks;profileSamples[(value.Sequence-1)%128]=jobProfile;
    ExReleaseFastMutex(&jobLock);
}
void GpuPath::SnapshotJobs(PI5_JOB_STATUS*p){
    LARGE_INTEGER frequency;KeQueryPerformanceCounter(&frequency);p->Frequency=frequency.QuadPart;
    ExAcquireFastMutex(&jobLock);p->Sequence=jobSequence;RtlCopyMemory(p->Samples,jobSamples,sizeof(jobSamples));ExReleaseFastMutex(&jobLock);
}
void GpuPath::SnapshotProfile(PI5_GPU_PROFILE_STATUS*p){
    p->Enabled=profileCounters;p->Error=profileError;
    ExAcquireFastMutex(&jobLock);p->Sequence=jobSequence;RtlCopyMemory(p->Samples,profileSamples,sizeof(profileSamples));ExReleaseFastMutex(&jobLock);
}

#endif
NTSTATUS GpuPath::Io(ULONG control,void *in,ULONG inBytes,void *out,ULONG outBytes){
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL||KeAreAllApcsDisabled())return STATUS_INVALID_DEVICE_STATE;
    Pi5PerfScope timing(control==IOCTL_PI5_V3D_SUBMIT_CL||control==IOCTL_PI5_V3D_BUFFER_COPY||control==IOCTL_PI5_V3D_BUFFER_TILE?Pi5PerfGpuWait:Pi5PerfCount);
#if DBG
    LONGLONG started=KeQueryPerformanceCounter(nullptr).QuadPart;
    InterlockedExchange(&ioActive,static_cast<LONG>(control));InterlockedIncrement(&ioSequence);
#endif
    IO_STATUS_BLOCK io={};NTSTATUS status=ZwDeviceIoControlFile(file,nullptr,nullptr,nullptr,&io,control,in,inBytes,out,outBytes);
    if(status==STATUS_PENDING){status=ZwWaitForSingleObject(file,FALSE,nullptr);if(status==STATUS_SUCCESS)status=io.Status;}
#if DBG
    InterlockedExchange(&ioActive,0);
    uint64_t elapsed=KeQueryPerformanceCounter(nullptr).QuadPart-started;
    if(control==IOCTL_PI5_V3D_SUBMIT_CL)jobRenderTicks+=elapsed;
    else if(control==IOCTL_PI5_V3D_BUFFER_TILE)jobTileTicks+=elapsed;
    else if(control==IOCTL_PI5_V3D_BUFFER_COPY)jobCopyTicks+=elapsed;
#endif
    if(status==STATUS_SUCCESS&&io.Information!=outBytes)return STATUS_DEVICE_PROTOCOL_ERROR;
#if DBG
    if(status==STATUS_SUCCESS&&control==IOCTL_PI5_V3D_SUBMIT_CL&&profileCounters){
        PI5_V3D_PROFILE p={};p.Version=1;p.Mode=2;
        NTSTATUS collected=Io(IOCTL_PI5_V3D_PROFILE,&p,sizeof(p),&p,sizeof(p));
        if(collected==STATUS_SUCCESS&&p.Version==1&&p.Mode==2){++jobProfile.Lists;jobProfile.Overflow|=p.Overflow;
            for(unsigned i=0;i<32;++i)jobProfile.Counters[i]+=p.Counters[i];}
        else{profileError=collected==STATUS_SUCCESS?STATUS_DEVICE_PROTOCOL_ERROR:collected;profileCounters=FALSE;}
    }
#endif
    return status;
}
NTSTATUS GpuPath::Request(ULONG control){PI5_V3D_REQUEST r={1,0};return Io(control,&r,sizeof(r),nullptr,0);}
NTSTATUS GpuPath::Buffer(PI5_V3D_BUFFER *b,ULONG bytes,BOOLEAN writable){*b={1,0,0,bytes,writable?PI5_V3D_BUFFER_WRITABLE:0u,0};return Io(IOCTL_PI5_V3D_BUFFER_CREATE,b,sizeof(*b),b,sizeof(*b));}
NTSTATUS GpuPath::Transfer(const PI5_V3D_BUFFER &b,void *data,ULONG bytes,BOOLEAN write,ULONG bufferOffset){
    Pi5PerfScope timing(b.Handle==texture.Handle?Pi5PerfTextureUpload:Pi5PerfOtherTransfer,bytes);
    if(bufferOffset>b.Bytes||bytes>b.Bytes-bufferOffset||((bytes|bufferOffset)&3))return STATUS_INVALID_PARAMETER;
    if(write&&b.Handle==texture.Handle)textureCache.WriteArena(bufferOffset,bytes);
    for(ULONG offset=0;offset<bytes;){ULONG count=bytes-offset;if(count>PI5_V3D_TRANSFER_MAX_BYTES)count=PI5_V3D_TRANSFER_MAX_BYTES;
        auto request=reinterpret_cast<PI5_V3D_TRANSFER*>(scratch);*request={1,b.Handle,bufferOffset+offset,count};
        if(write){CopyGraphicsMemory(request+1,static_cast<PUCHAR>(data)+offset,count);TRY(Io(IOCTL_PI5_V3D_BUFFER_WRITE,request,sizeof(*request)+count,nullptr,0));}
        else{TRY(Io(IOCTL_PI5_V3D_BUFFER_READ,request,sizeof(*request),request,sizeof(*request)+count));CopyGraphicsMemory(static_cast<PUCHAR>(data)+offset,request+1,count);}
        offset+=count;
    }return STATUS_SUCCESS;
}
NTSTATUS GpuPath::Start(PUCHAR memory,ULONG bytes){
    PAGED_CODE();textureCache={};textureCursor=0;RtlZeroMemory(shadows,sizeof(shadows));shadowAge=shadowCursor=0;activeShadow=UINT32_MAX;sampledShadow=false;
    linearWidth=linearHeight=linearPitch=0;
#if DBG
    ExInitializeFastMutex(&jobLock);jobSkippedLoads=0;jobSequence=jobRenderTicks=jobTileTicks=jobCopyTicks=0;RtlZeroMemory(jobSamples,sizeof(jobSamples));
    profileError=STATUS_SUCCESS;jobProfile={};RtlZeroMemory(profileSamples,sizeof(profileSamples));
#endif
    scratch=static_cast<PUCHAR>(Allocate(sizeof(PI5_V3D_TRANSFER)+PI5_V3D_TRANSFER_MAX_BYTES));commands=static_cast<PUCHAR>(Allocate(PI5_UMD_COMMAND_BYTES));
    blitCommand=static_cast<PUCHAR>(Allocate(4096));
    programCache=static_cast<pi5::ProgramValidationCache*>(Allocate(sizeof(pi5::ProgramValidationCache)));
    if(!scratch||!commands||!blitCommand||!programCache)return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(programCache,sizeof(*programCache));
    OBJECT_ATTRIBUTES attrs;IO_STATUS_BLOCK io;UNICODE_STRING name=RTL_CONSTANT_STRING(PI5_V3D_NAME);
    InitializeObjectAttributes(&attrs,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,nullptr,nullptr);
    TRY(ZwCreateFile(&file,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&attrs,&io,nullptr,0,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,nullptr,0));
    PI5_V3D_STATUS status={};TRY(Io(IOCTL_PI5_V3D_QUERY,nullptr,0,&status,sizeof(status)));
    if(!status.Online||status.Owner||status.Fault)return STATUS_DEVICE_NOT_READY;
    TRY(Request(IOCTL_PI5_V3D_MEMORY_BEGIN));session=TRUE;
#if DBG
    if(profileCounters){PI5_V3D_PROFILE p={};p.Version=1;p.Mode=1;TRY(Io(IOCTL_PI5_V3D_PROFILE,&p,sizeof(p),&p,sizeof(p)));}
#endif
    TRY(Io(IOCTL_PI5_V3D_QUERY,nullptr,0,&status,sizeof(status)));
    if(status.TechVersion!=71)return STATUS_NOT_SUPPORTED;
    v3dRevision=(status.HubIdent[3]>>8)&255u;
    if(!v3dRevision)return STATUS_DEVICE_PROTOCOL_ERROR;
    Pi5Trace(164,STATUS_SUCCESS,status.TechVersion,v3dRevision);
    TRY(Buffer(&code,PI5_UMD_COMMAND_BYTES,FALSE));TRY(Buffer(&uniform,131072,FALSE));TRY(Buffer(&cl,PI5_UMD_COMMAND_BYTES,FALSE));
    codeStaging=static_cast<PUCHAR>(Allocate(code.Bytes));uniformStaging=static_cast<PUCHAR>(Allocate(uniform.Bytes));
    if(!codeStaging||!uniformStaging)return STATUS_INSUFFICIENT_RESOURCES;
    TRY(Buffer(&tile,4*1024*1024,TRUE));TRY(Buffer(&state,2*1024*1024,TRUE));
    TRY(Buffer(&target,PI5_UMD_RESOURCE_BYTES,TRUE));TRY(Buffer(&source,PI5_UMD_RESOURCE_BYTES,FALSE));TRY(Buffer(&texture,PI5_UMD_RESOURCE_BYTES,TRUE));
    for(auto&buffer:constants)TRY(Buffer(&buffer,131072,FALSE));
    segmentMdl=IoAllocateMdl(memory,bytes,FALSE,FALSE,nullptr);
    if(!segmentMdl)return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(segmentMdl);
    PI5_V3D_BUFFER_IMPORT imported={};imported.Version=1;imported.Flags=PI5_V3D_BUFFER_WRITABLE;imported.Mdl=reinterpret_cast<ULONG_PTR>(segmentMdl);
    TRY(Io(IOCTL_PI5_V3D_BUFFER_IMPORT,&imported,sizeof(imported),&segment,sizeof(segment)));
    if(segment.Version!=1||!segment.Handle||segment.Bytes!=bytes||segment.Flags!=PI5_V3D_BUFFER_WRITABLE||!segment.Address||uint64_t(segment.Address)+bytes>UINT64_C(0x100000000))return STATUS_DEVICE_PROTOCOL_ERROR;
    segmentCpu=memory;
#if DBG
    // The imported memory never goes through the buffered transfer interface.
    PI5_V3D_TRANSFER read={1,segment.Handle,0,4};
    if(Io(IOCTL_PI5_V3D_BUFFER_READ,&read,sizeof(read),scratch,sizeof(read)+4)!=STATUS_INVALID_PARAMETER)return STATUS_DATA_ERROR;
    PI5_V3D_BUFFER_COPY overlap={};overlap.Version=1;overlap.Source=overlap.Destination=segment.Handle;overlap.Width=overlap.Height=1;
    if(Io(IOCTL_PI5_V3D_BUFFER_COPY,&overlap,sizeof(overlap),&overlap,sizeof(overlap))!=STATUS_INVALID_PARAMETER)return STATUS_DATA_ERROR;
    PI5_V3D_BUFFER unused={};imported.Reserved[0]=1;
    if(Io(IOCTL_PI5_V3D_BUFFER_IMPORT,&imported,sizeof(imported),&unused,sizeof(unused))!=STATUS_INVALID_PARAMETER)return STATUS_DATA_ERROR;
    Pi5Trace(76,STATUS_SUCCESS,segment.Bytes,3);
#endif
    textureData=static_cast<PUCHAR>(Allocate(PI5_UMD_RESOURCE_BYTES));if(!textureData)return STATUS_INSUFFICIENT_RESOURCES;
#if DBG
    TRY(CheckTiles());
#endif
    return STATUS_SUCCESS;
}
NTSTATUS GpuPath::Recover(){
    PAGED_CODE();TRY(Request(IOCTL_PI5_V3D_RESET));
    // VidMm discards GPU allocations during TDR. Cached aliases must not
    // survive that ownership transition or write old data into reused pages.
    textureCache={};textureCursor=0;RtlZeroMemory(shadows,sizeof(shadows));
    shadowAge=shadowCursor=0;activeShadow=UINT32_MAX;sampledShadow=false;ResetBatch();
    return STATUS_SUCCESS;
}
NTSTATUS GpuPath::Stop(){
    PAGED_CODE();
    if(session){TRY(Request(IOCTL_PI5_V3D_END));session=FALSE;}
    // END has retired all GPU access. A failed END deliberately retains the
    // MDL, and Adapter::Stop consequently retains the segment backing memory.
    if(segmentMdl){IoFreeMdl(segmentMdl);segmentMdl=nullptr;}segmentCpu=nullptr;segment={};
    if(scanoutMdl){IoFreeMdl(scanoutMdl);scanoutMdl=nullptr;}scanout={};
    if(file){ZwClose(file);file=nullptr;}Free(scratch);scratch=nullptr;Free(commands);commands=nullptr;Free(textureData);textureData=nullptr;Free(blitCommand);blitCommand=nullptr;
    Free(codeStaging);codeStaging=nullptr;Free(uniformStaging);uniformStaging=nullptr;Free(programCache);programCache=nullptr;return STATUS_SUCCESS;
}
NTSTATUS GpuPath::AttachScanout(PVOID memory,ULONG bytes){
    PAGED_CODE();
    if(!session||scanoutMdl||!memory||!bytes||bytes>PI5_UMD_RESOURCE_BYTES||((reinterpret_cast<ULONG_PTR>(memory)|bytes)&4095))return STATUS_INVALID_PARAMETER;
    scanoutMdl=IoAllocateMdl(memory,bytes,FALSE,FALSE,nullptr);if(!scanoutMdl)return STATUS_INSUFFICIENT_RESOURCES;
    // The display's HAL common buffer is resident for the lifetime of this
    // adapter. END retires and unmaps it before the display owner releases it.
    MmBuildMdlForNonPagedPool(scanoutMdl);
    PI5_V3D_BUFFER_IMPORT imported={};imported.Version=1;imported.Flags=PI5_V3D_BUFFER_WRITABLE;imported.Mdl=reinterpret_cast<ULONG_PTR>(scanoutMdl);
    TRY(Io(IOCTL_PI5_V3D_BUFFER_IMPORT,&imported,sizeof(imported),&scanout,sizeof(scanout)));
    if(scanout.Version!=1||!scanout.Handle||scanout.Bytes!=bytes||scanout.Flags!=PI5_V3D_BUFFER_WRITABLE)return STATUS_DEVICE_PROTOCOL_ERROR;
    return Pi5Trace(78,STATUS_SUCCESS,scanout.Bytes,scanout.Handle);
}
NTSTATUS GpuPath::CopyScanout(ULONGLONG sourceOffset,ULONG pitch,ULONG height,ULONG destinationOffset){
    ULONG64 bytes=ULONG64(pitch)*height;
    if(!scanout.Handle||!pitch||(pitch&3)||pitch>16384||!height||height>4096||sourceOffset>segment.Bytes||bytes>segment.Bytes-sourceOffset||destinationOffset>scanout.Bytes||bytes>scanout.Bytes-destinationOffset||(destinationOffset&4095))return STATUS_INVALID_PARAMETER;
    ULONG input=segment.Address+static_cast<ULONG>(sourceOffset),rows=0;
    uint32_t cached=FindShadow(0,sourceOffset);
    if(cached!=UINT32_MAX){const auto&e=shadows[cached];if(e.info.Width!=pitch/4||e.info.Height!=height||e.info.Pitch!=pitch)return STATUS_INVALID_PARAMETER;input=target.Address+e.arena;rows=e.rows;}
    KeMemoryBarrier();TRY(CopySurface(input,scanout.Address+destinationOffset,pitch/4,height,pitch,pitch,rows,0));KeMemoryBarrier();return STATUS_SUCCESS;
}
NTSTATUS GpuPath::CopySurface(ULONG sourceAddress,ULONG destinationAddress,ULONG width,ULONG height,ULONG sourcePitch,ULONG destinationPitch,ULONG sourceRows,ULONG destinationRows){
    pi5::Draw input={},output={};input.width=output.width=width;input.height=output.height=height;
    input.target=sourceAddress;output.target=destinationAddress;input.pitch=sourcePitch;output.pitch=destinationPitch;input.tiledRows=sourceRows;output.tiledRows=destinationRows;
    pi5::EncodedCommands encoded;const char*error=nullptr;
    if(!pi5::EncodeCopy(input,output,cl.Address,commands,PI5_UMD_COMMAND_BYTES,encoded,error))return STATUS_INVALID_PARAMETER;
#if DBG
    jobSkippedLoads+=encoded.skippedLoads;
#endif
    ULONG aligned=(encoded.bytes+3)&~3u;RtlZeroMemory(commands+encoded.bytes,aligned-encoded.bytes);TRY(Transfer(cl,commands,aligned,TRUE));
    PI5_V3D_SUBMIT_CL submit={};submit.Version=1;submit.RclStart=encoded.renderStart;submit.RclEnd=encoded.renderEnd;
    return Io(IOCTL_PI5_V3D_SUBMIT_CL,&submit,sizeof(submit),&submit,sizeof(submit));
}
NTSTATUS GpuPath::Region(PUCHAR destination,ULONG destinationPitch,const UCHAR *sourceData,ULONG sourcePitch,ULONG width,ULONG height,ULONG pixelBytes){
    Pi5PerfScope timing(Pi5PerfRegion,ULONGLONG(width)*height*pixelBytes);
    ULONG rowBytes=width*pixelBytes;
    // Preserve pitches and overlapping origins by gathering the source first.
    // Both allocations are already CPU-visible. Once gathered, uploading the
    // same pixels for a TFU copy and downloading them again adds no work that
    // the final scatter needs. Earlier GPU jobs have retired on this worker.
    for(ULONG y=0;y<height;++y)CopyGraphicsMemory(textureData+SIZE_T(y)*rowBytes,sourceData+SIZE_T(y)*sourcePitch,rowBytes);
    for(ULONG y=0;y<height;++y)CopyGraphicsMemory(destination+SIZE_T(y)*destinationPitch,textureData+SIZE_T(y)*rowBytes,rowBytes);
    KeMemoryBarrier();return STATUS_SUCCESS;
}
// Places every bound view in the texture arena and writes its descriptors to
// the uniform page: binding b uses PI5_UNIFORM_DESCRIPTORS+b*64 and +32.
NTSTATUS GpuPath::Bindings(const Pi5DrawCommand &c,const Pi5AllocationInfo *r,void *const *cpu,const uint64_t *identities){
    for(ULONG b=0;b<c.BindingCount;++b){
        const auto&x=c.Bindings[b];const auto&a=r[x.Allocation];auto data=static_cast<const UCHAR*>(cpu[x.Allocation]);auto descriptor=commands+PI5_UNIFORM_DESCRIPTORS+b*64;
        uint64_t sourceOffset=static_cast<uint64_t>(data-segmentCpu),identity=identities[x.Allocation];
        ULONG elementBytes=Pi5BufferElementBytes(x.Kind);
        pi5::TextureChain chain={};ULONG bytes=(x.Count+1)*elementBytes;
        if(!Pi5BufferElementBytes(x.Kind)){Pi5Level first={};if(!Pi5AllocationLevel(a,x.First,first)||!pi5::LayoutTextureChain(first.Width,first.Height,x.Count,chain))return STATUS_INVALID_PARAMETER;bytes=chain.bytes;}
        if(!Pi5BufferElementBytes(x.Kind)&&x.First==0&&x.Count==1){uint32_t cached=FindShadow(identity,sourceOffset);
            if(cached!=UINT32_MAX){const auto&e=shadows[cached];bool bgra=a.Format==87||a.Format==88;
                if(!pi5::ChainDescriptor(chain,target.Address+e.arena,bgra,a.Format==88,descriptor)||!pi5::SamplerDescriptor(x.Filter,x.AddressModes,x.Border,x.MinLod,x.MaxLod,bgra,descriptor+32))return STATUS_INVALID_PARAMETER;
                sampledShadow=true;continue;
            }
        }
        bool cacheable=!Pi5BufferElementBytes(x.Kind)&&Pi5CpuInvisible(a);
        pi5::TextureView view={identity,sourceOffset,x.Kind,x.First,x.Count,0,bytes};bool reused=false;
        for(ULONG i=0;i<viewCount;++i)if(pi5::SameTextureView(views[i],view)){reused=true;break;}
        if(!pi5::PlaceTextureView(textureCache,texture.Bytes,textureCursor,view,cacheable,views,viewCount))return STATUS_INSUFFICIENT_RESOURCES;
        ULONG arena=view.arena;
        if(elementBytes){
            ULONG elements=x.Count+1;if(bytes>PI5_UMD_RESOURCE_BYTES-arena)return STATUS_INSUFFICIENT_RESOURCES;
            ULONG copied=x.Count*elementBytes,packed=(bytes+3)&~3u;
            if(!reused){CopyGraphicsMemory(textureData,data+SIZE_T(x.First)*elementBytes,copied);RtlZeroMemory(textureData+copied,packed-copied);
                TRY(Transfer(texture,textureData,packed,TRUE,arena));}
            if(!pi5::BufferDescriptors(elements,x.Kind,texture.Address+arena,descriptor,descriptor+32))return STATUS_INVALID_PARAMETER;
            arena+=bytes;
        }else{
            if(chain.bytes>PI5_UMD_RESOURCE_BYTES-arena)return STATUS_INSUFFICIENT_RESOURCES;
            // Shared/GDI allocations are not CPU-visible in this segment. All
            // writes reach this worker, including writes through another device.
            if(!reused&&(!cacheable||!textureCache.Find(identity,sourceOffset,x.First,x.Count,arena,chain.bytes))){
            textureCache.WriteArena(arena,chain.bytes);
            Pi5PerfScope timing(Pi5PerfTile,chain.bytes);
            for(ULONG i=0;i<x.Count;++i){Pi5Level level={};if(!Pi5AllocationLevel(a,x.First+i,level))return STATUS_INVALID_PARAMETER;
                const auto&out=chain.level[i];PI5_V3D_BUFFER_TILE convert={};convert.Version=1;convert.Source=segment.Handle;convert.Destination=texture.Handle;
                convert.SourceOffset=static_cast<ULONG>(data-segmentCpu)+level.Offset;convert.DestinationOffset=arena+out.offset;
                convert.Width=level.Width;convert.Height=level.Height;convert.SourcePitch=level.Pitch;
                convert.Layout=out.tiling==pi5::Tiling::Linear?3:out.tiling==pi5::Tiling::UbLinear1?4:out.tiling==pi5::Tiling::UbLinear2?5:out.xorEnabled?7:6;
                convert.OutputRows=out.tiling==pi5::Tiling::Uif?out.paddedHeight/8:0;
                TRY(Io(IOCTL_PI5_V3D_BUFFER_TILE,&convert,sizeof(convert),&convert,sizeof(convert)));}
            if(cacheable)textureCache.Store(identity,sourceOffset,a.Bytes,x.First,x.Count,arena,chain.bytes);
            }
            bool bgra=a.Format==87||a.Format==88;
            if(!pi5::ChainDescriptor(chain,texture.Address+arena,bgra,a.Format==88,descriptor)||!pi5::SamplerDescriptor(x.Filter,x.AddressModes,x.Border,x.MinLod,x.MaxLod,bgra,descriptor+32))return STATUS_INVALID_PARAMETER;
            arena+=chain.bytes;
        }
        arena=(arena+4095)&~4095u;if(arena>PI5_UMD_RESOURCE_BYTES)return STATUS_INSUFFICIENT_RESOURCES;
        if(!reused){if(viewCount>=RTL_NUMBER_OF(views))return STATUS_INSUFFICIENT_RESOURCES;views[viewCount++]=view;}
    }
    return STATUS_SUCCESS;
}
void GpuPath::ResetBatch(){batchCount=batchCodeCount=codeUsed=uniformUsed=viewCount=0;RtlZeroMemory(constantUsed,sizeof(constantUsed));
    RtlZeroMemory(uniformStaging+PI5_UNIFORM_DESCRIPTORS,PI5_MAX_BATCH_DRAWS*(PI5_BINDINGS+3)*64);}
bool GpuPath::FitsDraw(const Pi5DrawCommand&c,const Pi5AllocationInfo*r,void *const *cpu,const uint64_t *identities){
    if(batchCount>=PI5_MAX_BATCH_DRAWS)return false;
    ULONG codeBytes=0,uniformBytes=0;const Pi5Program*p[]={&c.Coordinate,&c.Vertex,&c.Pixel};
    for(unsigned i=0;i<3;++i){codeBytes+=p[i]->CodeCount*8;uniformBytes+=p[i]->UniformCount*4;
        if(p[i]->ConstantWords){ULONG packed=pi5::TextureBytes(p[i]->ConstantWords,1);if(!packed||((packed+4095)&~4095u)>constants[i].Bytes-constantUsed[i])return false;}}
    if(codeBytes>code.Bytes-codeUsed||uniformBytes>PI5_UNIFORM_DESCRIPTORS-uniformUsed)return false;
    uint32_t cursor=textureCursor,plannedCount=0;pi5::TextureView planned[PI5_BINDINGS]={};
    for(ULONG b=0;b<c.BindingCount;++b){const auto&x=c.Bindings[b];const auto&a=r[x.Allocation];auto data=static_cast<const UCHAR*>(cpu[x.Allocation]);
        if(!Pi5BufferElementBytes(x.Kind)&&x.First==0&&x.Count==1&&FindShadow(identities[x.Allocation],uint64_t(data-segmentCpu))!=UINT32_MAX)continue;
        ULONG bytes=(x.Count+1)*Pi5BufferElementBytes(x.Kind);if(!Pi5BufferElementBytes(x.Kind)){Pi5Level first={};pi5::TextureChain chain;if(!Pi5AllocationLevel(a,x.First,first)||!pi5::LayoutTextureChain(first.Width,first.Height,x.Count,chain))return false;bytes=chain.bytes;}
        planned[plannedCount]={identities[x.Allocation],uint64_t(data-segmentCpu),x.Kind,x.First,x.Count,0,bytes};
        if(!pi5::PlaceTextureView(textureCache,texture.Bytes,cursor,planned[plannedCount],!Pi5BufferElementBytes(x.Kind)&&Pi5CpuInvisible(a),views,viewCount,planned,plannedCount))return false;
        ++plannedCount;
    }return true;
}
NTSTATUS GpuPath::PrepareDraw(const Pi5DrawCommand&command,const Pi5AllocationInfo*r,void *const *cpu,const uint64_t *identities){
    auto&draw=batchDraws[batchCount];draw={};draw.width=r[0].Width;draw.height=r[0].Height;draw.pitch=r[0].Pitch;draw.tile=tile.Address;draw.bgra=r[0].Format==87||r[0].Format==88;
    ULONG addresses[PI5_MAX_ALLOCATIONS]={};for(ULONG i=0;i<2;++i)addresses[i]=segment.Address+static_cast<ULONG>(static_cast<const UCHAR*>(cpu[i])-segmentCpu);draw.target=addresses[0];
    if(activeShadow!=UINT32_MAX){const auto&e=shadows[activeShadow];draw.target=target.Address+e.arena;draw.tiledRows=e.rows;}
            auto c=&command;const void*data=c;const Pi5Program *p[]={&c->Coordinate,&c->Vertex,&c->Pixel};
            uint32_t *codeAddress[]={&draw.coordinateCode,&draw.vertexCode,&draw.pixelCode},*uniformAddress[]={&draw.coordinateUniforms,&draw.vertexUniforms,&draw.pixelUniforms};
            ULONG uniformBytes=0;
            // Many draws share their programs but have different uniforms.
            // Retain one exact copy per batch and upload each bank only once.
            for(unsigned i=0;i<3;++i){ULONG bytes=p[i]->CodeCount*8,index=0;auto instructions=static_cast<const UCHAR*>(data)+p[i]->CodeOffset;
                for(;index<batchCodeCount;++index){const auto&e=batchCode[index];if(e.bytes==bytes&&!memcmp(codeStaging+e.offset,instructions,bytes))break;}
                if(index==batchCodeCount){if(batchCodeCount>=RTL_NUMBER_OF(batchCode)||bytes>code.Bytes-codeUsed)return STATUS_INSUFFICIENT_RESOURCES;
                    batchCode[batchCodeCount++]={codeUsed,bytes};RtlCopyMemory(codeStaging+codeUsed,instructions,bytes);codeUsed+=bytes;}
                *codeAddress[i]=code.Address+batchCode[index].offset;
            }
            // The uniform streams occupy only their actual words. Descriptors
            // have a fixed GPU address; the unused gap is never read by shaders.
            RtlZeroMemory(commands+PI5_UNIFORM_DESCRIPTORS,(PI5_BINDINGS+3)*64);
            for(unsigned i=0;i<3;++i){*uniformAddress[i]=uniform.Address+uniformUsed+uniformBytes;RtlCopyMemory(commands+uniformBytes,static_cast<const UCHAR*>(data)+p[i]->UniformOffset,p[i]->UniformCount*4);uniformBytes+=p[i]->UniformCount*4;}
            TRY(Bindings(*c,r,cpu,identities));ULONG descriptorBytes=c->BindingCount*64;
            draw.fourThreadMask=0;draw.finalThreadMask=0;
            for(unsigned i=0;i<3;++i){
                pi5::TexturePatches patches;const auto*program=p[i];uint64_t registers=0;
                if(!programCache->Validate(reinterpret_cast<const uint64_t*>(static_cast<const UCHAR*>(data)+program->CodeOffset),program->CodeCount,reinterpret_cast<const uint32_t*>(static_cast<const UCHAR*>(data)+program->UniformOffset),program->UniformCount,pi5::DrawProgramRules(*c,i),&patches,&registers))return STATUS_INVALID_PARAMETER;
                // V3D 7.1 is always at least 2-way threaded; the 4-way flag
                // halves the physical RF space to RF0-RF31.
                const bool highRegisters=(registers&UINT64_C(0xffffffff00000000))!=0;
                if(!highRegisters)draw.fourThreadMask|=1u<<i;
                // The UMD's 64-register fallback is vertex-only. If it has no
                // TMU lookup, its corrected epilogue contains no "last THRSW"
                // pair, so spawn it directly in the final thread section.
                else if(i==1&&!patches.count)draw.finalThreadMask|=1u<<i;
                if(!program->ConstantWords&&!patches.count)continue;
                ULONG descriptor=PI5_UNIFORM_DESCRIPTORS+(PI5_BINDINGS+i)*64,sampler=descriptor+32;
                if(program->ConstantWords){uint32_t packed=0;
                    if(!pi5::EncodeTexture(program->ConstantWords,1,program->ConstantWords*4,static_cast<const UCHAR*>(data)+program->ConstantOffset,textureData,PI5_UMD_RESOURCE_BYTES,packed))return STATUS_INVALID_PARAMETER;
                    TRY(Transfer(constants[i],textureData,packed,TRUE,constantUsed[i]));
                    if(!pi5::TextureDescriptors(program->ConstantWords,1,constants[i].Address+constantUsed[i],false,false,commands+descriptor,commands+sampler,false,true))return STATUS_INVALID_PARAMETER;
                    constantUsed[i]+=(packed+4095)&~4095u;
                    ULONG end=(PI5_BINDINGS+i+1)*64;if(descriptorBytes<end)descriptorBytes=end;
                }
                auto u=reinterpret_cast<ULONG*>(commands+*uniformAddress[i]-uniform.Address-uniformUsed);
                for(ULONG j=0;j<patches.count;++j){const auto&l=patches.lookups[j];ULONG textureState=l.constant?descriptor:PI5_UNIFORM_DESCRIPTORS+l.binding*64,samplerState=l.constant?sampler:textureState+32;
                    // All lookups return four 32-bit components, including typed integer buffers.
                    u[l.config0]=(uniform.Address+textureState+batchCount*(PI5_BINDINGS+3)*64)|15;u[l.config1]=(uniform.Address+samplerState+batchCount*(PI5_BINDINGS+3)*64)|1;}
            }
            if(uniformBytes)RtlCopyMemory(uniformStaging+uniformUsed,commands,uniformBytes);
            if(descriptorBytes)RtlCopyMemory(uniformStaging+PI5_UNIFORM_DESCRIPTORS+batchCount*(PI5_BINDINGS+3)*64,commands+PI5_UNIFORM_DESCRIPTORS,descriptorBytes);
            draw.vertexAddress=addresses[1]+c->VertexOffset;draw.vertexStride=c->VertexStride;draw.vertexCount=c->VertexCount;draw.vertexScalars=c->VertexComponents;draw.varyingScalars=c->VaryingScalars;draw.nonPerspectiveMask=c->NonPerspectiveMask;draw.flatMask=c->FlatMask;draw.v3dRevision=v3dRevision;draw.pipeline=c->Pipeline;if(!opaqueLoads){draw.pipeline.Flags&=~PI5_PIPELINE_COVERAGE;RtlZeroMemory(draw.pipeline.Coverage,sizeof(draw.pipeline.Coverage));}draw.loadTarget=true;
            RtlCopyMemory(draw.viewport,c->Viewport,sizeof(draw.viewport));
            uniformUsed+=uniformBytes;++batchCount;return STATUS_SUCCESS;
}
NTSTATUS GpuPath::SubmitBatch(){
    if(!batchCount)return STATUS_SUCCESS;
    TRY(Transfer(code,codeStaging,codeUsed,TRUE));
    if(uniformUsed)TRY(Transfer(uniform,uniformStaging,uniformUsed,TRUE));
    TRY(Transfer(uniform,uniformStaging+PI5_UNIFORM_DESCRIPTORS,batchCount*(PI5_BINDINGS+3)*64,TRUE,PI5_UNIFORM_DESCRIPTORS));
    pi5::EncodedCommands encoded;const char*error=nullptr;
    if(!pi5::EncodeDrawBatch(batchDraws,batchCount,cl.Address,commands,PI5_UMD_COMMAND_BYTES,encoded,error)||encoded.tileBytes>tile.Bytes||encoded.stateBytes>state.Bytes)return STATUS_INVALID_PARAMETER;
#if DBG
    jobSkippedLoads+=encoded.skippedLoads;
#endif
    ULONG aligned=(encoded.bytes+3)&~3u;RtlZeroMemory(commands+encoded.bytes,aligned-encoded.bytes);TRY(Transfer(cl,commands,aligned,TRUE));
    PI5_V3D_SUBMIT_CL submit={};submit.Version=1;submit.BclStart=encoded.binStart;submit.BclEnd=encoded.binEnd;submit.RclStart=encoded.renderStart;submit.RclEnd=encoded.renderEnd;
    submit.TileAddress=tile.Address;submit.TileBytes=tile.Bytes;submit.StateAddress=state.Address;submit.StateBytes=state.Bytes;
    TRY(Io(IOCTL_PI5_V3D_SUBMIT_CL,&submit,sizeof(submit),&submit,sizeof(submit)));
    if(activeShadow!=UINT32_MAX)shadows[activeShadow].dirty=true;
    return STATUS_SUCCESS;
}
NTSTATUS GpuPath::Execute(const void *data,ULONG bytes,const Pi5AllocationInfo *r,void *const *cpu,ULONG count,const uint64_t *identities){
    PAGED_CODE();
    Pi5PerfScope timing(Pi5PerfExecute);
    if(!pi5::ValidateCommand(data,bytes,r,count,programCache))return STATUS_INVALID_PARAMETER;
    ULONG addresses[PI5_MAX_REFERENCES]={};
    for(ULONG i=0;i<count;++i){
        ULONG_PTR pointer=reinterpret_cast<ULONG_PTR>(cpu[i]),base=reinterpret_cast<ULONG_PTR>(segmentCpu);
        if(!segmentCpu||pointer<base||pointer-base>segment.Bytes||r[i].Bytes>segment.Bytes-(pointer-base))return STATUS_INVALID_PARAMETER;
        addresses[i]=segment.Address+static_cast<ULONG>(pointer-base);
    }
    textureCache.WriteSource(addresses[0]-segment.Address,r[0].Bytes);
    auto h=static_cast<const Pi5CommandHeader*>(data);
#if DBG
    LONGLONG operationStarted=KeQueryPerformanceCounter(nullptr).QuadPart;
    jobSkippedLoads=0;jobRenderTicks=jobTileTicks=jobCopyTicks=0;jobProfile={};
#endif
    activeShadow=UINT32_MAX;sampledShadow=false;
    bool wholeCopy=h->Operation==Pi5Copy;
    if(h->Operation==Pi5CopyRegion){const auto&c=*static_cast<const Pi5RegionCommand*>(data);
        wholeCopy=!c.SourceLevel&&!c.DestinationLevel&&!c.SourceX&&!c.SourceY&&!c.DestinationX&&!c.DestinationY&&
            c.Width==r[0].Width&&c.Width==r[1].Width&&c.Height==r[0].Height&&c.Height==r[1].Height;
    }
    if(wholeCopy&&r[0].Dimension==3&&r[0].Levels==1&&r[1].Levels==1){
        if(addresses[0]==addresses[1])return STATUS_SUCCESS;
        // Keep whole-surface GPU copies in the tiled cache. CPU readbacks can
        // resolve straight into their destination, without an intermediate
        // writeback into the source allocation or invalidating its texture.
        TRY(AcquireShadow(identities[0],addresses[0]-segment.Address,r[0],activeShadow,false));
        if(activeShadow==UINT32_MAX)TRY(SyncShadows(addresses[0]-segment.Address,r[0].Bytes,true));
        // Acquiring the destination may have evicted the source. Find it only
        // after acquisition, when all evictions have been resolved.
        ULONG input=addresses[1],output=addresses[0],inputRows=0,outputRows=0;
        uint32_t sourceShadow=FindShadow(identities[1],addresses[1]-segment.Address);
        if(sourceShadow!=UINT32_MAX){const auto&e=shadows[sourceShadow];input=target.Address+e.arena;inputRows=e.rows;sampledShadow=true;}
        if(activeShadow!=UINT32_MAX){const auto&e=shadows[activeShadow];output=target.Address+e.arena;outputRows=e.rows;}
        KeMemoryBarrier();TRY(CopySurface(input,output,r[0].Width,r[0].Height,r[1].Pitch,r[0].Pitch,inputRows,outputRows));
        if(activeShadow!=UINT32_MAX)shadows[activeShadow].dirty=true;
#if DBG
        RecordJob(h,r,KeQueryPerformanceCounter(nullptr).QuadPart-operationStarted);
#endif
        KeMemoryBarrier();return STATUS_SUCCESS;
    }
    if(h->Operation==Pi5CopyRegion&&r[0].Dimension==3&&r[0].Levels==1&&r[1].Levels==1&&
       addresses[0]!=addresses[1]&&(Pi5CpuInvisible(r[0])||Pi5CpuInvisible(r[1]))){
        NTSTATUS status=BlitRegion(*static_cast<const Pi5RegionCommand*>(data),r,cpu,identities);
#if DBG
        RecordJob(h,r,KeQueryPerformanceCounter(nullptr).QuadPart-operationStarted);
#endif
        return status;
    }
    if(h->Operation==Pi5Draw||h->Operation==Pi5DrawBatch||h->Operation==Pi5Clear){
        TRY(AcquireShadow(identities[0],addresses[0]-segment.Address,r[0],activeShadow,h->Operation!=Pi5Clear));
        if(activeShadow==UINT32_MAX)TRY(SyncShadows(addresses[0]-segment.Address,r[0].Bytes,true));
    }else{
        for(ULONG i=1;i<count;++i)TRY(SyncShadows(addresses[i]-segment.Address,r[i].Bytes));
        TRY(SyncShadows(addresses[0]-segment.Address,r[0].Bytes,true));
    }
    if(h->Operation==Pi5Draw||h->Operation==Pi5DrawBatch){
        ResetBatch();KeMemoryBarrier();
        if(h->Operation==Pi5Draw){const auto&c=*static_cast<const Pi5DrawCommand*>(data);if(!FitsDraw(c,r,cpu,identities))return STATUS_INSUFFICIENT_RESOURCES;TRY(PrepareDraw(c,r,cpu,identities));}
        else{auto batch=static_cast<const Pi5BatchCommand*>(data);
            for(ULONG n=0;n<batch->Count;++n){const auto&e=batch->Entries[n];auto c=reinterpret_cast<const Pi5DrawCommand*>(static_cast<const UCHAR*>(data)+e.Offset);
                Pi5AllocationInfo local[PI5_MAX_ALLOCATIONS]={};void*localCpu[PI5_MAX_ALLOCATIONS]={};uint64_t localIds[PI5_MAX_ALLOCATIONS]={};
                for(ULONG i=0;i<e.Count;++i){local[i]=r[e.References[i]];localCpu[i]=cpu[e.References[i]];localIds[i]=identities[e.References[i]];}
                if(!FitsDraw(*c,local,localCpu,localIds)){TRY(SubmitBatch());ResetBatch();if(!FitsDraw(*c,local,localCpu,localIds))return STATUS_INSUFFICIENT_RESOURCES;}
                TRY(PrepareDraw(*c,local,localCpu,localIds));
            }
        }
        TRY(SubmitBatch());
#if DBG
        RecordJob(h,r,KeQueryPerformanceCounter(nullptr).QuadPart-operationStarted);
#endif
        KeMemoryBarrier();return STATUS_SUCCESS;
    }
    if(h->Operation==Pi5CopyRegion){
        const auto*c=static_cast<const Pi5RegionCommand*>(data);ULONG pixelBytes=r[0].Dimension==1?1:4;Pi5Level dst={},src={};
        if(!Pi5AllocationLevel(r[0],c->DestinationLevel,dst)||!Pi5AllocationLevel(r[1],c->SourceLevel,src))return STATUS_INVALID_PARAMETER;
        NTSTATUS status=Region(static_cast<PUCHAR>(cpu[0])+dst.Offset+SIZE_T(c->DestinationY)*dst.Pitch+SIZE_T(c->DestinationX)*pixelBytes,dst.Pitch,
                      static_cast<const UCHAR*>(cpu[1])+src.Offset+SIZE_T(c->SourceY)*src.Pitch+SIZE_T(c->SourceX)*pixelBytes,src.Pitch,c->Width,c->Height,pixelBytes);
#if DBG
        RecordJob(h,r,KeQueryPerformanceCounter(nullptr).QuadPart-operationStarted);
#endif
        return status;
    }
    if(h->Operation==Pi5Copy&&r[0].Levels>1){
        for(ULONG i=0;i<r[0].Levels;++i){Pi5Level level={};if(!Pi5AllocationLevel(r[0],i,level))return STATUS_INVALID_PARAMETER;
            TRY(Region(static_cast<PUCHAR>(cpu[0])+level.Offset,level.Pitch,static_cast<const UCHAR*>(cpu[1])+level.Offset,level.Pitch,level.Width,level.Height,4));}
        return STATUS_SUCCESS;
    }
    // VidMm owns these pages; the provider maps the same pages in the V3D MMU.
    // Submission retires GPU writes before the scheduler exposes completion.
    KeMemoryBarrier();
    if(h->Operation==Pi5Copy){
        if(r[0].Dimension==3)return CopySurface(addresses[1],addresses[0],r[0].Pitch/4,r[0].Height,r[1].Pitch,r[0].Pitch);
        PI5_V3D_BUFFER_COPY copy={};copy.Version=1;copy.Source=copy.Destination=segment.Handle;copy.SourceOffset=addresses[1]-segment.Address;copy.DestinationOffset=addresses[0]-segment.Address;copy.Width=r[0].Pitch/4;copy.Height=r[0].Height;
        TRY(Io(IOCTL_PI5_V3D_BUFFER_COPY,&copy,sizeof(copy),&copy,sizeof(copy)));
    }else{
        pi5::Draw draw={};draw.width=r[0].Width;draw.height=r[0].Height;draw.pitch=r[0].Pitch;draw.target=addresses[0];draw.tile=tile.Address;draw.bgra=r[0].Format==87||r[0].Format==88;
        if(activeShadow!=UINT32_MAX){const auto&e=shadows[activeShadow];draw.target=target.Address+e.arena;draw.tiledRows=e.rows;}
        pi5::EncodedCommands encoded;const char *error=nullptr;bool built;
        if(h->Operation==Pi5Clear){draw.clearColor=static_cast<const Pi5ClearCommand*>(data)->Color;built=pi5::EncodeClear(draw,cl.Address,commands,PI5_UMD_COMMAND_BYTES,encoded,error);}
        else{
            return STATUS_INVALID_PARAMETER;
        }
        if(!built||encoded.tileBytes>tile.Bytes||encoded.stateBytes>state.Bytes)return STATUS_INVALID_PARAMETER;
        ULONG aligned=(encoded.bytes+3)&~3u;RtlZeroMemory(commands+encoded.bytes,aligned-encoded.bytes);TRY(Transfer(cl,commands,aligned,TRUE));
        PI5_V3D_SUBMIT_CL submit={};submit.Version=1;submit.BclStart=encoded.binStart;submit.BclEnd=encoded.binEnd;submit.RclStart=encoded.renderStart;submit.RclEnd=encoded.renderEnd;
        if(submit.BclStart){submit.TileAddress=tile.Address;submit.TileBytes=tile.Bytes;submit.StateAddress=state.Address;submit.StateBytes=state.Bytes;}
        TRY(Io(IOCTL_PI5_V3D_SUBMIT_CL,&submit,sizeof(submit),&submit,sizeof(submit)));
        if(activeShadow!=UINT32_MAX)shadows[activeShadow].dirty=true;
#if DBG
        RecordJob(h,r,KeQueryPerformanceCounter(nullptr).QuadPart-operationStarted);
#endif
    }
    KeMemoryBarrier();return STATUS_SUCCESS;
}
