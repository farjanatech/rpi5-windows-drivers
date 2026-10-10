// Display integration for the same WDDM adapter that owns the rendering context.
// Physical source/target IDs stay fixed; the firmware POST owner may be either HDMI output.
struct PresentPacket {ULONG flags,color,count,interval;RECT source,destination,rects[64];};
struct DirectScanoutRequest {Adapter*adapter;Reference primary;ULONG flags,source;bool sourceCall;NTSTATUS status;};
static BOOLEAN SynchronizeDirectScanout(PVOID context){
    auto r=static_cast<DirectScanoutRequest*>(context);auto a=r->adapter;
    r->status=a->display->SourceNative(r->source).QueueDirectScanout(r->primary.offset);
    if(r->status==STATUS_SUCCESS){
        if(r->source){a->secondaryPrimary=r->primary;a->secondaryDisplayed=r->primary;}
        else{a->primary=r->primary;a->displayed=r->primary;}
        if(r->sourceCall&&r->source==a->display->PostSource()){a->primaryFlags=r->flags;++a->primaryCalls;}}
    return TRUE;
}
static NTSTATUS DirectScanout(Adapter*a,const Reference&r,ULONG flags,bool sourceCall,ULONG source=0){
    auto&native=a->display->SourceNative(source);
    DirectScanoutRequest request={a,r,flags,source,sourceCall,STATUS_UNSUCCESSFUL};
    // The MMIO DDI is already serialized with the display ISR by dxgkrnl.
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL){(void)SynchronizeDirectScanout(&request);return request.status;}
    NTSTATUS status=native.DrainFlips();if(status!=STATUS_SUCCESS)return status;
    // Mode and primary ownership transitions must finish before returning.
    // The caller owns gpuLock, and mode-sized allocations have no shadows.
    status=a->gpu.SyncShadows(r.offset,r.info.Bytes);if(status!=STATUS_SUCCESS)return status;
    BOOLEAN called=FALSE;status=a->dxgk.DxgkCbSynchronizeExecution(a->dxgk.DeviceHandle,SynchronizeDirectScanout,&request,0,&called);
    if(status!=STATUS_SUCCESS)return status;if(request.status!=STATUS_SUCCESS)return request.status;
    return native.DrainFlips();
}
static NTSTATUS Scanout(Adapter*a,const Reference&r,ULONG interval,bool noWait,ULONG fence,bool complete){
    Pi5PerfScope timing(Pi5PerfScanout,r.info.Bytes);
    if(!a->display->IsDriverActive()||!Range(a,r.offset,r.info.Bytes))return STATUS_INVALID_PARAMETER;
    const auto &mode=a->display->GetCurrentMode(a->display->PostSource())->DispInfo;
    if(r.info.Width!=mode.Width||r.info.Height!=mode.Height||r.info.Pitch!=mode.Pitch||(r.info.Format!=87&&r.info.Format!=88))return STATUS_NOT_SUPPORTED;
    if(a->mmioFlips){NTSTATUS status=DirectScanout(a,r,0,false,a->display->PostSource());if(status==STATUS_SUCCESS&&interval>1)status=a->display->PostNative().WaitForRefresh(interval-1);return status;}
    auto&native=a->display->PostNative();ULONG offset=0;
    const BOOLEAN c1=a->siliconRevision==0;
    const BOOLEAN flip=c1?TRUE:(interval!=0);
    NTSTATUS status=native.PrepareScanout(flip,&offset);if(status!=STATUS_SUCCESS)return status;
    if(c1){
        // Final Damian-reference isolation: a displayable C1 primary is forced
        // linear, so no tiled shadow may exist. Bypass V3D CopyScanout entirely
        // and CPU-copy the linear Windows primary into the inactive native
        // scanout buffer before publishing the flip.
        uint32_t shadow=a->gpu.FindShadow(r.identity,r.offset);
        if(shadow!=UINT32_MAX){BddTrace(254,STATUS_DEVICE_CONFIGURATION_ERROR,shadow,static_cast<ULONG>(r.offset));return STATUS_DEVICE_CONFIGURATION_ERROR;}
        SIZE_T bytes=SIZE_T(mode.Pitch)*mode.Height;
        PUCHAR src=a->memory+r.offset;
        PUCHAR dst=static_cast<PUCHAR>(native.ScanoutMemory())+offset;
        CopyGraphicsMemory(dst,src,bytes);KeMemoryBarrier();
        ULONG srcHash=2166136261u,dstHash=2166136261u;
        const ULONG words=static_cast<ULONG>(bytes/4);
        const volatile ULONG *sw=reinterpret_cast<const volatile ULONG*>(src);
        const volatile ULONG *dw=reinterpret_cast<const volatile ULONG*>(dst);
        const ULONG samples=1024u;
        for(ULONG i=0;i<samples;++i){ULONG at=static_cast<ULONG>((ULONGLONG(i)*words)/samples);srcHash=(srcHash^sw[at])*16777619u;dstHash=(dstHash^dw[at])*16777619u;}
        if(!(a->scanoutCopies&63u))BddTrace(254,srcHash==dstHash?STATUS_SUCCESS:STATUS_DATA_ERROR,srcHash,dstHash);
        if(srcHash!=dstHash)return STATUS_DATA_ERROR;
    }else{
        status=a->gpu.CopyScanout(r.offset,mode.Pitch,mode.Height,offset);if(status!=STATUS_SUCCESS)return status;
    }
    status=native.Presented(r.info.Width,r.info.Height);if(status!=STATUS_SUCCESS)return status;
    if(c1&&!(a->scanoutCopies&63u))BddTrace(252,STATUS_SUCCESS,interval,offset);
    status=native.CommitScanout(offset,static_cast<LONGLONG>(r.offset),flip,noWait,interval,complete?CompleteFlipDma:nullptr,a,fence);if(status!=STATUS_SUCCESS)return status;
    a->displayed=r;++a->scanoutCopies;
    return STATUS_SUCCESS;
}
static NTSTATUS RefreshIfPrimary(Adapter*a,const Reference&r){
    if(a->mmioFlips)return STATUS_SUCCESS; // rendering already writes the scanned allocation
    // VidMm can reuse the same segment offset for unrelated allocations.
    // The binding must refer to the live allocation, not just its address.
    LockPrimary(a);bool current=r.identity&&r.identity==a->primary.identity&&r.offset==a->primary.offset;
    NTSTATUS status=current?Scanout(a,r):STATUS_SUCCESS;UnlockPrimary(a);return status;
}
static bool RectInside(const RECT&r,const Pi5AllocationInfo&info){return r.left>=0&&r.top>=0&&r.right>=r.left&&r.bottom>=r.top&&static_cast<ULONG>(r.right)<=info.Width&&static_cast<ULONG>(r.bottom)<=info.Height;}
static NTSTATUS PresentReferences(Device*d,Dma*out,const DXGK_ALLOCATIONLIST*list,UINT count,bool resident){
    if(!list||out->bytes!=sizeof(PresentPacket))return STATUS_INVALID_PARAMETER;auto p=reinterpret_cast<const PresentPacket*>(out->data);
    bool fill=(p->flags&2)!=0,flip=(p->flags&4)!=0;
    if(out->count!=(fill||flip?1u:2u))return STATUS_INVALID_PARAMETER;
    for(UINT i=0;i<out->count;++i){UINT index=fill||i?DXGK_PRESENT_DESTINATION_INDEX:DXGK_PRESENT_SOURCE_INDEX;bool write=fill||i!=0;if(index>=count)return STATUS_INVALID_PARAMETER;
        const auto&entry=list[index];auto opened=static_cast<OpenAllocation*>(entry.hDeviceSpecificAllocation);
        if(!opened||opened->device!=d||!opened->allocation||opened->allocation->adapter!=d->adapter||(write&&opened->readOnly)||entry.SegmentId>1||(resident&&entry.SegmentId!=1))return STATUS_INVALID_HANDLE;
        const auto&info=opened->allocation->info;if(!pi5::ValidateAllocation(info)||info.Dimension!=3||(info.Format!=87&&info.Format!=88))return STATUS_NOT_SUPPORTED;
        if(entry.SegmentId==1&&!Range(d->adapter,entry.PhysicalAddress.QuadPart,info.Bytes))return STATUS_INVALID_PARAMETER;
        out->refs[i]={info,entry.SegmentId==1?static_cast<ULONGLONG>(entry.PhysicalAddress.QuadPart):MAXULONGLONG,opened->allocation->identity};
    }
    if(out->count==2&&out->refs[0].offset!=MAXULONGLONG&&out->refs[1].offset!=MAXULONGLONG&&out->refs[0].offset<out->refs[1].offset+out->refs[1].info.Bytes&&out->refs[1].offset<out->refs[0].offset+out->refs[0].info.Bytes)return STATUS_NOT_SUPPORTED;
    if(flip)return STATUS_SUCCESS;
    const auto&dst=out->refs[fill?0:1].info;
    if(!fill&&(!RectInside(p->source,out->refs[0].info)||LONGLONG(p->source.right)-p->source.left!=LONGLONG(p->destination.right)-p->destination.left||LONGLONG(p->source.bottom)-p->source.top!=LONGLONG(p->destination.bottom)-p->destination.top))return STATUS_NOT_SUPPORTED;
    for(UINT i=0;i<p->count;++i){const RECT&r=p->rects[i];if(!RectInside(r,dst)||r.left<p->destination.left||r.top<p->destination.top||r.right>p->destination.right||r.bottom>p->destination.bottom)return STATUS_INVALID_PARAMETER;}
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY FullPresent(HANDLE context,DXGKARG_PRESENT*a){
    auto c=static_cast<Context*>(context);
    if(!a->pDmaBuffer&&c->device->adapter->mmioFlips){
        if(!a->Flags.Flip||a->Flags.Blt||a->Flags.ColorFill||(a->Flags.Value&~15u)||!a->pAllocationList||a->MultipassOffset)return Pi5Trace(143,STATUS_NOT_SUPPORTED,a->Flags.Value);
        auto opened=static_cast<OpenAllocation*>(a->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].hDeviceSpecificAllocation);
        if(!opened||opened->device!=c->device||!opened->allocation||opened->allocation->adapter!=c->device->adapter)return STATUS_INVALID_HANDLE;
        const auto&i=opened->allocation->info;bool modeMatches=false;
        for(UINT source=0;source<MAX_VIEWS;++source){auto display=c->device->adapter->display;const auto&mode=display->GetCurrentMode(source)->DispInfo;
            if(display->SourceNative(source).Active()&&i.Width==mode.Width&&i.Height==mode.Height&&i.Pitch==mode.Pitch)modeMatches=true;}
        if(!(i.MiscFlags&PI5_RESOURCE_DISPLAYABLE)||i.Dimension!=3||i.Levels!=1||!modeMatches||(i.Format!=87&&i.Format!=88))return Pi5Trace(143,STATUS_NOT_SUPPORTED,i.Width,i.Height);
        // No DMA or patch output: Windows schedules SetVidPnSourceAddress
        // after this allocation's earlier rendering fences have completed.
        return Pi5Trace(143,STATUS_SUCCESS,a->Flags.Value);
    }
    if(!a->pDmaBuffer||a->DmaSize<sizeof(PresentPacket))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if(!a->pDmaBufferPrivateData||a->DmaBufferPrivateDataSize<sizeof(Dma)||a->MultipassOffset||a->SubRectCnt>64||(a->SubRectCnt&&!a->pDstSubRects)||(a->Flags.Value&~15u)||unsigned(a->Flags.Blt)+a->Flags.ColorFill+a->Flags.Flip!=1)return Pi5Trace(132,STATUS_NOT_SUPPORTED,a->Flags.Value,a->SubRectCnt);
    auto p=static_cast<PresentPacket*>(a->pDmaBuffer);RtlZeroMemory(p,sizeof(*p));p->flags=a->Flags.Value;p->color=a->Color;p->source=a->SrcRect;p->destination=a->DstRect;p->count=a->SubRectCnt;p->interval=static_cast<ULONG>(a->FlipInterval);
    if(p->count)RtlCopyMemory(p->rects,a->pDstSubRects,p->count*sizeof(RECT));else if(!a->Flags.Flip){p->rects[0]=a->DstRect;p->count=1;}
    auto out=static_cast<Dma*>(a->pDmaBufferPrivateData);RtlZeroMemory(out,sizeof(*out));out->magic=DmaMagic;out->kind=3;out->bytes=sizeof(*p);out->data=reinterpret_cast<PUCHAR>(p);out->device=c->device;out->count=a->Flags.Flip||a->Flags.ColorFill?1:2;
    NTSTATUS status=PresentReferences(c->device,out,a->pAllocationList,DXGK_PRESENT_MAX_INDEX+1,false);if(status!=STATUS_SUCCESS)return Pi5Trace(133,status,a->Flags.Value);
    if(a->PatchLocationListOutSize<out->count||!a->pPatchLocationListOut)return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    for(UINT i=0;i<out->count;++i){auto &patch=a->pPatchLocationListOut[i];RtlZeroMemory(&patch,sizeof(patch));patch.AllocationIndex=a->Flags.ColorFill||i?DXGK_PRESENT_DESTINATION_INDEX:DXGK_PRESENT_SOURCE_INDEX;patch.PatchOffset=16+i*4;}
    a->pPatchLocationListOut+=out->count;a->pDmaBuffer=static_cast<PUCHAR>(a->pDmaBuffer)+sizeof(*p);return Pi5Trace(134,STATUS_SUCCESS,a->Flags.Value,p->count);
}
static NTSTATUS PresentWork(Adapter*a,const Submission&s){
    if(s.start||s.end!=sizeof(PresentPacket)||s.dma.bytes!=sizeof(PresentPacket)||!s.dma.count||s.dma.count>2)return STATUS_INVALID_PARAMETER;
    const auto&p=*reinterpret_cast<const PresentPacket*>(s.dma.data);if(p.interval>4||p.count>64)return STATUS_INVALID_PARAMETER;
    for(UINT i=0;i<s.dma.count;++i)if(!Range(a,s.dma.refs[i].offset,s.dma.refs[i].info.Bytes))return STATUS_INVALID_PARAMETER;
    if(p.flags&4){LockPrimary(a);NTSTATUS status=Scanout(a,s.dma.refs[0],p.interval,(p.flags&8)!=0,s.fence,!(p.flags&8)&&p.interval==1);if(status==STATUS_SUCCESS)a->primary=s.dma.refs[0];UnlockPrimary(a);return status;}
    bool fill=(p.flags&2)!=0;const auto&dst=s.dma.refs[fill?0:1];auto output=a->memory+dst.offset;
    for(UINT i=0;i<s.dma.count;++i){const auto&r=s.dma.refs[i];NTSTATUS status=a->gpu.SyncShadows(r.offset,r.info.Bytes,fill||i!=0);if(status!=STATUS_SUCCESS)return status;}
    a->gpu.textureCache.WriteSource(dst.offset,dst.info.Bytes);
    for(UINT i=0;i<p.count;++i){const auto&r=p.rects[i];for(LONG y=r.top;y<r.bottom;++y){auto row=output+SIZE_T(y)*dst.info.Pitch+SIZE_T(r.left)*4;
        if(fill){auto pixels=reinterpret_cast<volatile ULONG*>(row);for(LONG x=r.left;x<r.right;++x)*pixels++=p.color;}
        else{const auto&src=s.dma.refs[0];auto input=a->memory+src.offset+SIZE_T(LONGLONG(p.source.top)+y-p.destination.top)*src.info.Pitch+SIZE_T(LONGLONG(p.source.left)+r.left-p.destination.left)*4;CopyGraphicsMemory(row,input,SIZE_T(r.right-r.left)*4);}
    }}KeMemoryBarrier();
    return RefreshIfPrimary(a,dst);
}
static NTSTATUS APIENTRY StandardAllocation(HANDLE,DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA*a){
    if(!a->pAllocationPrivateDriverData&&!a->pResourcePrivateDriverData){a->AllocationPrivateDriverDataSize=a->ResourcePrivateDriverDataSize=sizeof(Pi5AllocationInfo);return STATUS_SUCCESS;}
    if(!a->pAllocationPrivateDriverData||!a->pResourcePrivateDriverData||a->AllocationPrivateDriverDataSize!=sizeof(Pi5AllocationInfo)||a->ResourcePrivateDriverDataSize!=sizeof(Pi5AllocationInfo))return STATUS_INVALID_PARAMETER;
    ULONG width=0,height=0,misc=0;D3DDDI_RATIONAL refresh={};D3DDDIFORMAT format=D3DDDIFMT_UNKNOWN;UINT*pitch=nullptr;
    switch(a->StandardAllocationType){
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:{auto p=a->pCreateSharedPrimarySurfaceData;if(!p||p->VidPnSourceId>=MAX_VIEWS)return STATUS_INVALID_PARAMETER;width=p->Width;height=p->Height;format=p->Format;refresh=p->RefreshRate;misc=2|PI5_RESOURCE_DISPLAYABLE;break;}
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:{auto p=a->pCreateShadowSurfaceData;if(!p)return STATUS_INVALID_PARAMETER;width=p->Width;height=p->Height;format=p->Format;pitch=&p->Pitch;break;}
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:{auto p=a->pCreateStagingSurfaceData;if(!p)return STATUS_INVALID_PARAMETER;width=p->Width;height=p->Height;format=D3DDDIFMT_X8R8G8B8;pitch=&p->Pitch;break;}
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE:{auto p=a->pCreateGdiSurfaceData;if(!p||(p->Type!=D3DKMDT_GDISURFACE_TEXTURE&&p->Type!=D3DKMDT_GDISURFACE_STAGING&&p->Type!=D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE))return STATUS_NOT_SUPPORTED;width=p->Width;height=p->Height;format=p->Format;pitch=&p->Pitch;break;}
    default:return Pi5Trace(137,STATUS_NOT_SUPPORTED,a->StandardAllocationType);
    }
    if(!width||!height||width>4096||height>4096||(format!=D3DDDIFMT_A8R8G8B8&&format!=D3DDDIFMT_X8R8G8B8&&format!=D3DDDIFMT_A8B8G8R8))return Pi5Trace(138,STATUS_NOT_SUPPORTED,format,a->StandardAllocationType);
    ULONG row=(width*4+63)&~63u;Pi5AllocationInfo info={PI5_UMD_ABI,(row*height+4095)&~4095u,width,height,row,format==D3DDDIFMT_X8R8G8B8?88u:format==D3DDDIFMT_A8B8G8R8?28u:87u,0xa8,3};info.MiscFlags=misc;
    info.RefreshNumerator=refresh.Numerator;info.RefreshDenominator=refresh.Denominator;
    if(!pi5::ValidateAllocation(info))return STATUS_INVALID_PARAMETER;RtlCopyMemory(a->pAllocationPrivateDriverData,&info,sizeof(info));RtlCopyMemory(a->pResourcePrivateDriverData,&info,sizeof(info));if(pitch)*pitch=row;return Pi5Trace(139,STATUS_SUCCESS,a->StandardAllocationType,format);
}
static NTSTATUS APIENTRY SourceAddress(HANDLE adapter,const DXGKARG_SETVIDPNSOURCEADDRESS*p){
    auto a=static_cast<Adapter*>(adapter);if(p->VidPnSourceId>=MAX_VIEWS||a->stopping||!a->display->SourceNative(p->VidPnSourceId).Active())return STATUS_INVALID_PARAMETER;
    if(p->PrimarySegment!=1)return STATUS_NOT_SUPPORTED;const auto&mode=a->display->GetCurrentMode(p->VidPnSourceId)->DispInfo;
    if(!Range(a,p->PrimaryAddress.QuadPart,SIZE_T(mode.Pitch)*mode.Height))return STATUS_INVALID_PARAMETER;
    Reference primary={};primary.offset=static_cast<ULONGLONG>(p->PrimaryAddress.QuadPart);
    primary.info={PI5_UMD_ABI,(mode.Pitch*mode.Height+4095)&~4095u,mode.Width,mode.Height,mode.Pitch,87,0,3};
    if(p->hAllocation){auto allocation=static_cast<Allocation*>(p->hAllocation);const auto&i=allocation->info;if(allocation->adapter!=a||i.Width!=mode.Width||i.Height!=mode.Height||i.Pitch!=mode.Pitch||(i.Format!=87&&i.Format!=88))return STATUS_NOT_SUPPORTED;primary.info=i;primary.identity=allocation->identity;}
    if(a->mmioFlips){
        if(primary.info.Levels!=1||(p->hAllocation&&!(primary.info.MiscFlags&PI5_RESOURCE_DISPLAYABLE)))return STATUS_NOT_SUPPORTED;
        bool passive=KeGetCurrentIrql()==PASSIVE_LEVEL;if(passive)LockGpu(a);
        NTSTATUS status=DirectScanout(a,primary,p->Flags.Value,true,p->VidPnSourceId);
        if(passive)UnlockGpu(a);return Pi5Trace(144,status,p->PrimaryAddress.LowPart,p->Flags.Value);
    }
    // FlipOnVSyncMmIo is not advertised: this DDI runs at PASSIVE_LEVEL.
    LockGpu(a);NTSTATUS drained=a->display->PostNative().DrainFlips();
    if(drained!=STATUS_SUCCESS){UnlockGpu(a);return drained;}
    // The mode/primary handoff is complete when this DDI returns. Deferring
    // it until the submission queue becomes idle leaves VSync reporting the
    // previous primary after Windows has switched its allocation ownership.
    // MMIO flips are not advertised, so this copy can complete at PASSIVE.
    LockPrimary(a);NTSTATUS status=Scanout(a,primary);
    if(status==STATUS_SUCCESS){a->primary=primary;a->primaryFlags=p->Flags.Value;++a->primaryCalls;InterlockedExchange(&a->primaryPending,0);}
    UnlockPrimary(a);UnlockGpu(a);return Pi5Trace(140,status,p->PrimaryAddress.LowPart,p->Flags.Value);
}
static NTSTATUS APIENTRY FullEscape(HANDLE adapter,const DXGKARG_ESCAPE*e){
    if(e->PrivateDriverDataSize==sizeof(Pi5SchedulerStatus))return SchedulerEscape(adapter,e);
#if DBG
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_NATIVE_STATUS)){
        auto p=static_cast<PI5_NATIVE_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_NATIVE_QUERY_MAGIC||p->Version!=1)return STATUS_INVALID_PARAMETER;
        auto a=static_cast<Adapter*>(adapter);LockGpu(a);a->display->PostNative().SnapshotNative(p);UnlockGpu(a);return STATUS_SUCCESS;
    }
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_HDMI_STATUS)){
        auto p=static_cast<PI5_HDMI_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_HDMI_QUERY_MAGIC||p->Version!=1)return STATUS_INVALID_PARAMETER;
        static_cast<Adapter*>(adapter)->display->PostNative().SnapshotHdmi(p);return STATUS_SUCCESS;
    }
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_GPU_PROFILE_STATUS)){
        auto p=static_cast<PI5_GPU_PROFILE_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_GPU_PROFILE_MAGIC||p->Version!=1)return STATUS_INVALID_PARAMETER;
        static_cast<Adapter*>(adapter)->gpu.SnapshotProfile(p);return STATUS_SUCCESS;
    }
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_RECOVERY_STATUS)){
        auto p=static_cast<PI5_RECOVERY_STATUS*>(e->pPrivateDriverData);auto a=static_cast<Adapter*>(adapter);
        if(p->Magic!=PI5_RECOVERY_QUERY_MAGIC||p->Version!=1||p->ArmFailure>1||p->Reserved)return STATUS_INVALID_PARAMETER;
        // Failure injection requires an administrator's one-use volatile key.
        if(p->ArmFailure){if(a->fault||a->recovering||!ConsumeFlipTrial(L"FailNextDraw"))return STATUS_ACCESS_DENIED;InterlockedExchange(&a->failNextDraw,1);}
        p->Armed=a->failNextDraw;p->Fault=a->fault;p->Recovering=a->recovering;p->Recoveries=a->recoveries;p->Submitted=a->submitted;p->Completed=a->completed;
        return STATUS_SUCCESS;
    }
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_FRAME_STATUS)){
        auto p=static_cast<PI5_FRAME_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_FRAME_QUERY_MAGIC||p->Version!=1)return STATUS_INVALID_PARAMETER;
        static_cast<Adapter*>(adapter)->display->PostNative().SnapshotFrames(p);return STATUS_SUCCESS;
    }
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_JOB_STATUS)){
        auto p=static_cast<PI5_JOB_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_JOB_QUERY_MAGIC||p->Version!=3)return STATUS_INVALID_PARAMETER;
        static_cast<Adapter*>(adapter)->gpu.SnapshotJobs(p);return STATUS_SUCCESS;
    }
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_PERF_STATUS)){
        auto p=static_cast<PI5_PERF_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_PERF_QUERY_MAGIC||p->Version!=1)return STATUS_INVALID_PARAMETER;
        PerfSnapshot(p);return STATUS_SUCCESS;
    }
    if(e->pPrivateDriverData&&e->PrivateDriverDataSize==sizeof(PI5_PRIMARY_STATUS)){
        auto p=static_cast<PI5_PRIMARY_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_PRIMARY_QUERY_MAGIC||p->Version!=1)return STATUS_INVALID_PARAMETER;
        auto a=static_cast<Adapter*>(adapter);LockPrimary(a);
        RtlZeroMemory(p,sizeof(*p));p->Magic=PI5_PRIMARY_QUERY_MAGIC;p->Version=1;p->Flags=a->primaryFlags;p->Pending=a->primaryPending;p->SourceCalls=a->primaryCalls;p->Copies=a->scanoutCopies;
        const auto&primary=a->display->PostSource()?a->secondaryPrimary:a->primary;
        const auto&displayed=a->display->PostSource()?a->secondaryDisplayed:a->displayed;
        p->Submitted=a->submitted;p->Completed=a->completed;p->SourceOffset=primary.offset;p->SourceIdentity=primary.identity;p->DisplayedOffset=displayed.offset;p->DisplayedIdentity=displayed.identity;
        const auto&mode=a->display->GetCurrentMode(a->display->PostSource())->DispInfo;p->Width=mode.Width;p->Height=mode.Height;
        p->Reserved[0]=a->display->PostNative().BlackoutCount();p->Reserved[1]=a->display->PostNative().LastBlackoutReason();p->Reserved[2]=a->display->GetCurrentMode(a->display->PostSource())->Flags.SourceNotVisible;
        p->Reserved[3]=(a->asyncFlips?1u:0u)|(a->mmioFlips?2u:0u);
        if(mode.Width>480&&mode.Height>270){SIZE_T pixel=SIZE_T(mode.Pitch)*270+480*4;
            if(primary.offset!=MAXULONGLONG&&Range(a,primary.offset,pixel+4))p->SourcePixel=*reinterpret_cast<volatile ULONG*>(a->memory+primary.offset+pixel);
            p->FramebufferPixel=*reinterpret_cast<volatile ULONG*>(static_cast<PUCHAR>(a->display->PostNative().Framebuffer())+pixel);}
        UnlockPrimary(a);return STATUS_SUCCESS;
    }
#endif
    if(!e->pPrivateDriverData||e->PrivateDriverDataSize!=sizeof(PI5_DISPLAY_STATUS))return STATUS_INVALID_PARAMETER;auto p=static_cast<PI5_DISPLAY_STATUS*>(e->pPrivateDriverData);if(p->Magic!=PI5_DISPLAY_QUERY_MAGIC||p->Version!=PI5_DISPLAY_QUERY_VERSION)return STATUS_INVALID_PARAMETER;
    static_cast<Adapter*>(adapter)->display->PostNative().Snapshot(p);return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY FullCommit(HANDLE adapter,const DXGKARG_COMMITVIDPN *request){
    auto a=static_cast<Adapter*>(adapter);if(request->AffectedVidPnSourceId>=MAX_VIEWS)return STATUS_INVALID_PARAMETER;
    LockGpu(a);NTSTATUS s=a->display->CommitVidPn(request);
    if(s==STATUS_SUCCESS){
        LockPrimary(a);auto &primary=request->AffectedVidPnSourceId?a->secondaryPrimary:a->primary;
        auto &displayed=request->AffectedVidPnSourceId?a->secondaryDisplayed:a->displayed;
        primary={};primary.offset=MAXULONGLONG;displayed=primary;UnlockPrimary(a);
        const auto&mode=a->display->GetCurrentMode(a->display->PostSource())->DispInfo;
        if(a->mmioFlips||a->siliconRevision==0){a->gpu.linearWidth=mode.Width;a->gpu.linearHeight=mode.Height;a->gpu.linearPitch=mode.Pitch;}
    }
    UnlockGpu(a);return s;
}
static NTSTATUS APIENTRY FullControl(HANDLE adapter,DXGK_INTERRUPT_TYPE type,BOOLEAN enable){auto a=static_cast<Adapter*>(adapter);LockGpu(a);NTSTATUS s=a->display->ControlInterrupt(type,enable);UnlockGpu(a);return s;}
static NTSTATUS APIENTRY FullScanline(HANDLE adapter,DXGKARG_GETSCANLINE*p){auto a=static_cast<Adapter*>(adapter);if(p->VidPnTargetId>=MAX_VIEWS||!a->display->Native(p->VidPnTargetId).Active())return STATUS_INVALID_PARAMETER;ULONG line=a->display->Native(p->VidPnTargetId).Scanline();p->InVerticalBlank=line>=a->display->Native(p->VidPnTargetId).HeightPixels();p->ScanLine=p->InVerticalBlank?0:line;return STATUS_SUCCESS;}
static NTSTATUS APIENTRY ReleasePost(PVOID adapter,D3DDDI_VIDEO_PRESENT_TARGET_ID target,DXGK_DISPLAY_INFORMATION*info){auto a=static_cast<Adapter*>(adapter);if(target>=MAX_CHILDREN||!info)return STATUS_INVALID_PARAMETER;*info=a->display->PostNative().PostDisplay();info->TargetId=a->display->m_PostTarget;a->releasePost=1;return Stop(a);}
static VOID APIENTRY SystemDisplayWrite(PVOID adapter,PVOID source,UINT width,UINT height,UINT stride,UINT x,UINT y){
    auto a=static_cast<Adapter*>(adapter);BddDdiSystemDisplayWrite(a->display,source,width,height,stride,x,y);
#if DBG
    // Preserve the final scheduler state visibly even if the storage stack
    // cannot save a bugcheck dump. No allocation, callbacks, waits, or FP here.
    const auto&mode=a->display->GetCurrentMode(a->display->PostSource())->DispInfo;if(mode.Width<600||mode.Height<240||mode.Pitch<mode.Width*4)return;
    ULONG rows[7][5]={{0x46,static_cast<ULONG>(a->submitted),static_cast<ULONG>(a->completed),static_cast<ULONG>(a->fault),static_cast<ULONG>(a->queueHighWater)},{0x53},{0x50},{0x57},
        {0x58,static_cast<ULONG>(a->workerStage),static_cast<ULONG>(a->gpu.ioActive),static_cast<ULONG>(a->gpu.ioSequence),static_cast<ULONG>(workerTrace)},
        {0x59,static_cast<ULONG>(a->recoveryStage),static_cast<ULONG>(a->busy),static_cast<ULONG>(a->recovering),static_cast<ULONG>(a->stopping)},
        {0x5a,static_cast<ULONG>(a->powerState),static_cast<ULONG>(a->powerAction),a->head,a->tail}};
    for(UINT row=1;row<4;++row)for(UINT col=1;col<5;++col)rows[row][col]=static_cast<ULONG>(a->diagnostics[(row-1)*4+col-1]);
    crashWidth=mode.Width;crashHeight=mode.Height;crashPitch=mode.Pitch;
    for(ULONG frame=0;frame<a->display->PostNative().FramebufferCount();++frame){
        crashPixels[frame]=static_cast<volatile ULONG*>(a->display->PostNative().FramebufferAt(frame));
        DrawHexRows(crashPixels[frame],mode.Width,mode.Height,mode.Pitch,256,rows,7);
    }
    DrawCrashArguments();
#endif
}
template<class F,F Function>struct DisplayForward;
template<class R,class First,class...Args,R(APIENTRY*Function)(First,Args...)>struct DisplayForward<R(APIENTRY*)(First,Args...),Function>{static R APIENTRY Call(First h,Args...args){return Function(static_cast<Adapter*>(h)->display,args...);}};
// Serialize pageable descriptor/mode readers against the hotplug worker's
// EDID refresh and output initialization. Interrupt/DDIs at elevated IRQL
// retain the unlocked forwarding path.
template<class F,F Function>struct DisplayLocked;
template<class R,class First,class...Args,R(APIENTRY*Function)(First,Args...)>struct DisplayLocked<R(APIENTRY*)(First,Args...),Function>{static R APIENTRY Call(First h,Args...args){auto a=static_cast<Adapter*>(h);LockGpu(a);R result=Function(a->display,args...);UnlockGpu(a);return result;}};
static void DisplayFunctions(DRIVER_INITIALIZATION_DATA&d){
#define DISPLAY_DDI(field,fn) d.field=DisplayForward<decltype(&fn),&fn>::Call
    DISPLAY_DDI(DxgkDdiDispatchIoRequest,BddDdiDispatchIoRequest);DISPLAY_DDI(DxgkDdiQueryChildRelations,BddDdiQueryChildRelations);DISPLAY_DDI(DxgkDdiQueryChildStatus,BddDdiQueryChildStatus);DISPLAY_DDI(DxgkDdiQueryDeviceDescriptor,BddDdiQueryDeviceDescriptor);
    DISPLAY_DDI(DxgkDdiSetPointerPosition,BddDdiSetPointerPosition);DISPLAY_DDI(DxgkDdiSetPointerShape,BddDdiSetPointerShape);DISPLAY_DDI(DxgkDdiIsSupportedVidPn,BddDdiIsSupportedVidPn);DISPLAY_DDI(DxgkDdiRecommendFunctionalVidPn,BddDdiRecommendFunctionalVidPn);
    DISPLAY_DDI(DxgkDdiEnumVidPnCofuncModality,BddDdiEnumVidPnCofuncModality);DISPLAY_DDI(DxgkDdiSetVidPnSourceVisibility,BddDdiSetVidPnSourceVisibility);DISPLAY_DDI(DxgkDdiCommitVidPn,BddDdiCommitVidPn);DISPLAY_DDI(DxgkDdiUpdateActiveVidPnPresentPath,BddDdiUpdateActiveVidPnPresentPath);
    DISPLAY_DDI(DxgkDdiRecommendMonitorModes,BddDdiRecommendMonitorModes);DISPLAY_DDI(DxgkDdiQueryVidPnHWCapability,BddDdiQueryVidPnHWCapability);DISPLAY_DDI(DxgkDdiSystemDisplayEnable,BddDdiSystemDisplayEnable);DISPLAY_DDI(DxgkDdiSystemDisplayWrite,BddDdiSystemDisplayWrite);DISPLAY_DDI(DxgkDdiResetDevice,BddDdiResetDevice);
#undef DISPLAY_DDI
#define DISPLAY_LOCKED(field,fn) d.field=DisplayLocked<decltype(&fn),&fn>::Call
    DISPLAY_LOCKED(DxgkDdiQueryChildStatus,BddDdiQueryChildStatus);DISPLAY_LOCKED(DxgkDdiQueryDeviceDescriptor,BddDdiQueryDeviceDescriptor);
    DISPLAY_LOCKED(DxgkDdiEnumVidPnCofuncModality,BddDdiEnumVidPnCofuncModality);DISPLAY_LOCKED(DxgkDdiRecommendMonitorModes,BddDdiRecommendMonitorModes);
    DISPLAY_LOCKED(DxgkDdiSetVidPnSourceVisibility,BddDdiSetVidPnSourceVisibility);
#undef DISPLAY_LOCKED
    d.DxgkDdiPresent=FullPresent;d.DxgkDdiGetStandardAllocationDriverData=StandardAllocation;d.DxgkDdiSetVidPnSourceAddress=SourceAddress;d.DxgkDdiEscape=FullEscape;d.DxgkDdiControlInterrupt=FullControl;d.DxgkDdiGetScanLine=FullScanline;d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership=ReleasePost;
    d.DxgkDdiSystemDisplayWrite=SystemDisplayWrite;d.DxgkDdiCommitVidPn=FullCommit;
}
