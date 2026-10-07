#include "bdd.hxx"
#include "pi5-fclk.h"

static const ULONGLONG Addresses[]={0x107c580000ull,0x107c410000ull,0x107c411000ull,0x107c502000ull,
    0x107c701d00ull,0x107c702000ull,0x107c701400ull,0x107c706400ull,
    0x107c720000ull,0x107c701000ull,0x107c703800ull,0x107c700100ull};
static const ULONG Sizes[]={0x1a000,0x100,0x100,0x30,0x300,0x80,0x300,0x300,0x100,0x200,0x200,0x80};
static const ULONG Clocks[]={4,16,13,14};
// GOP does not identify a connector. Match its framebuffer against the live
// firmware raster lists instead of assuming HDMI0 owns the POST display.
NTSTATUS PI5_DISPLAY_HW::FindPostPort(const DXGK_DEVICE_INFO *device,const DXGK_DISPLAY_INFORMATION *display,ULONG *port,ULONG revision) {
    PAGED_CODE();
    if(!device||!display||!port||!device->TranslatedResourceList)return STATUS_INVALID_PARAMETER;
    if(display->PhysicAddress.HighPart)return STATUS_NOT_SUPPORTED;
    auto list=device->TranslatedResourceList;
    for(ULONG f=0;f<list->Count;++f){
        auto partial=&list->List[f].PartialResourceList;
        for(ULONG i=0;i<partial->Count;++i){
            auto resource=&partial->PartialDescriptors[i];
            if(resource->Type!=CmResourceTypeMemory || ULONGLONG(resource->u.Memory.Start.QuadPart)!=Addresses[0] || resource->u.Memory.Length!=Sizes[0])continue;
            auto hvs=static_cast<PUCHAR>(MmMapIoSpaceEx(resource->u.Memory.Start,Sizes[0],PAGE_READONLY|PAGE_NOCACHE));
            if(!hvs)return STATUS_INSUFFICIENT_RESOURCES;
            auto read=[hvs](ULONG offset){return READ_REGISTER_ULONG(reinterpret_cast<PULONG>(hvs+offset));};
            NTSTATUS status=STATUS_DEVICE_CONFIGURATION_ERROR;
            ULONG version=read(0);
            BddTrace(116,STATUS_SUCCESS,revision,display->PhysicAddress.LowPart);
            BddTrace(125,STATUS_SUCCESS,version,version&0xff);
            const BOOLEAN c1=revision==0;
            const BOOLEAN d0=revision==1;
            const BOOLEAN recognized=(d0&&version==0x2454) || (c1&&((version&0xff)==0x53));
            if(recognized)for(ULONG candidate=0;candidate<2;++candidate){
                // BCM2712 C0/C1 and D0 place the per-display register blocks
                // at different offsets. These values come from Raspberry Pi's
                // upstream vc4 SCALER6/SCALER6D register definitions.
                ULONG headOffset=c1?(0x3c+candidate*0x20):(0x110+candidate*0x40);
                ULONG activeOffset=c1?(0x48+candidate*0x20):(0x11c+candidate*0x40);
                ULONG head=read(headOffset)&0xfff;
                ULONG active=read(activeOffset)&0xfff;
                BddTrace(117+candidate*4,STATUS_SUCCESS,head,active);
                if(head<0x800){
                    ULONG base=0x4000+head*4;
                    BddTrace(118+candidate*4,STATUS_SUCCESS,read(base),read(base+12));
                    BddTrace(119+candidate*4,STATUS_SUCCESS,read(base+20),read(base+24));
                    BddTrace(120+candidate*4,STATUS_SUCCESS,read(base+28),display->Pitch);
                }
                // D0 remains Damian's original strict path.
                if(d0){
                    if(head>=0x800 || active!=head)continue;
                    ULONG base=0x4000+head*4;
                    if(read(base)!=0x600cc007 || (read(base+20)&15)!=0 || read(base+24)!=display->PhysicAddress.LowPart ||
                       read(base+28)!=display->Pitch || read(base+12)!=((display->Height-1)<<16 | (display->Width-1)))continue;
                    *port=candidate;status=STATUS_SUCCESS;break;
                }
                if(c1){
                    if(head>=0x800 || active!=head)continue;
                    ULONG base=0x4000+head*4;
                    // C1 hardware capture 2026-10-07 plus Raspberry Pi's
                    // VC4_GEN_6_C definitions: fixed alpha is in CTL2, not
                    // D0's CTL0 alpha-mask bits.
                    if(read(base)!=0x6000c007 || read(base+4)!=0 ||
                       read(base+8)!=0x4000fff0 ||
                       (read(base+20)&15)!=0 ||
                       read(base+24)!=display->PhysicAddress.LowPart ||
                       read(base+28)!=display->Pitch ||
                       read(base+12)!=((display->Height-1)<<16 | (display->Width-1)))continue;
                    *port=candidate;status=STATUS_SUCCESS;break;
                }
            }
            MmUnmapIoSpace(hvs,Sizes[0]);return BddTrace(115,status,status==STATUS_SUCCESS?*port:MAXULONG,display->PhysicAddress.LowPart);
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
static ULONG HvsPhysicalOffset(ULONG logical,ULONG port,ULONG revision)
{
    // Callers use Damian's D0 register names as the canonical layout.
    // Offsets 0x100..0x120 mean "current display"; larger 0x140/0x180
    // references already identify a particular HVS channel.
    if(logical>=0x100&&logical<=0x120)logical+=port*0x40;
    if(revision!=0)return logical;

    if(logical>=0x100&&logical<=0x1a0){
        ULONG channel=(logical-0x100)/0x40;
        ULONG field=(logical-0x100)%0x40;
        ULONG mapped;
        switch(field){
        case 0x00:mapped=0x00;break; // CTRL0
        case 0x04:mapped=0x04;break; // CTRL1
        case 0x08:mapped=0x08;break; // BGND0 -> BGND
        case 0x0c:mapped=0x08;break; // D0 BGND1 has no C1 peer
        case 0x10:mapped=0x0c;break; // LPTRS
        case 0x14:mapped=0x10;break; // COB
        case 0x18:mapped=0x14;break; // STATUS
        case 0x1c:mapped=0x18;break; // DL
        case 0x20:mapped=0x1c;break; // RUN
        default:return logical;
        }
        return 0x30+channel*0x20+mapped;
    }
    return logical;
}
ULONG PI5_DISPLAY_HW::Read(ULONG unit,ULONG offset) const {
    if(unit==0)offset=HvsPhysicalOffset(offset,Port,SiliconRevision);
    return READ_REGISTER_ULONG(reinterpret_cast<PULONG>(Reg[unit]+offset));
}
VOID PI5_DISPLAY_HW::Write(ULONG unit,ULONG offset,ULONG value) {
    bool permitted=(unit==0&&(offset==0x100||offset==0x110||(offset>=0x4000+OwnHead*4&&offset<0x4000+(OwnHead+20)*4)))||
        (unit==1&&(offset==0x24||offset==0x28))||(unit==3&&(offset==0x10||offset==0x14));
    for(const auto&r:pi5display::NativeRegisters){
        ULONG region=r.Region==6?6+Port:r.Region,off=r.Offset+(r.Region==8?Port*4:0);
        if(unit==region&&offset==off)permitted=true;
    }
    NT_ASSERT(permitted);if(!permitted)return;
    if(unit==0)offset=HvsPhysicalOffset(offset,Port,SiliconRevision);
    WRITE_REGISTER_ULONG(reinterpret_cast<PULONG>(Reg[unit]+offset),value);
}
NTSTATUS PI5_DISPLAY_HW::SecondaryMode(DXGK_DISPLAY_INFORMATION *display) const {
    if(!display||!Reg[2]||!Connected(1-Port))return STATUS_DEVICE_NOT_CONNECTED;
    ULONG width=(Read(2,0x10)&65535)*2,height=Read(2,0x18)&65535;
    if(!(Read(2,0)&1)||Read(2,4)!=3||width<640||width>4096||height<480||height>2160||(width&15))return STATUS_NOT_SUPPORTED;
    RtlZeroMemory(display,sizeof(*display));display->Width=width;display->Height=height;display->Pitch=width*4;
    display->ColorFormat=D3DDDIFMT_A8R8G8B8;display->TargetId=1-Port;return STATUS_SUCCESS;
}
NTSTATUS PI5_DISPLAY_HW::Clock(ULONG code,ULONG id) {
    PI5_FCLK_REQUEST in={PI5_FCLK_VERSION,id,0,0};
    PI5_FCLK_STATUS out={}; IO_STATUS_BLOCK io={};
    NTSTATUS s=ZwDeviceIoControlFile(ClockHandle,NULL,NULL,NULL,&io,code,
        &in,sizeof(in),&out,sizeof(out));
    if(s==STATUS_SUCCESS && (io.Information!=sizeof(out) || out.Version!=PI5_FCLK_VERSION ||
        out.Id!=id || (out.Flags&PI5_FCLK_FLAG_UNCERTAIN)))return STATUS_DEVICE_PROTOCOL_ERROR;
    if(s==STATUS_SUCCESS&&code==IOCTL_PI5_FCLK_ACQUIRE){
        // BCM2712 reports zero GET_RATE for fixed display clocks; the equal
        // minimum/maximum bounds identify their fixed frequency instead.
        ULONG rate=out.RateHz?out.RateHz:out.MinHz==out.MaxHz?out.MinHz:0;
        if(id==13)HsmRate=rate;if(id==14)BvbRate=rate;
    }
    return s;
}
BOOLEAN PI5_DISPLAY_HW::WaitHead(ULONG head) {
    // Require two further frame starts after the requested list became active.
    ULONG frame=0,seen=0;
    LARGE_INTEGER pause;pause.QuadPart=-10000;
    for(ULONG i=0;i<200;++i){
        ULONG current=(Read(0,0x118)>>16)&63;
        if((Read(0,0x11c)&0xfff)==head){
            if(!seen){seen=1;frame=current;}
            else if(((current-frame)&63)>=2)return TRUE;
        }else seen=0;
        KeDelayExecutionThread(KernelMode,FALSE,&pause);
    }
    return FALSE;
}
NTSTATUS PI5_DISPLAY_HW::Start(const DXGK_DEVICE_INFO *device,const DXGKRNL_INTERFACE *dxgk,
                              const DXGK_DISPLAY_INFORMATION *display,ULONG port,BOOLEAN ownsPost,ULONG revision) {
    PAGED_CODE();
    if(port>1||revision>1||(!FullDisplay&&port))return STATUS_INVALID_PARAMETER;
    Port=port;SiliconRevision=revision;PostOwner=ownsPost;Dxgk=dxgk;Width=display->Width;Height=display->Height;Pitch=display->Pitch;OwnHead=0xf80+Port*0x20;
    // BCM2712 prefetch storage is shared across HVS outputs. Reserve one half
    // of its 1024 256-byte words per output, with a distinct hardware handle.
    // Two prefetched raster lines fit even at our maximum 4096-pixel width.
    // The pointer word carries these fields alongside the upper address bits.
    UpmDescriptor=Port?((512u<<16)|(1u<<10)):0;
    Front=0;Pending=-1;PendingAddress=0;PendingArmed=0;
    Visible=TRUE;
    DirectSegment=nullptr;DirectBase=0;DirectBytes=0;
    RtlZeroMemory(DirectPixels,sizeof(DirectPixels));RtlZeroMemory(DirectAddresses,sizeof(DirectAddresses));
    RefreshWaiters=0;KeInitializeEvent(&RefreshWake,SynchronizationEvent,FALSE);
    OriginalDisplay=*display;
    NTSTATUS s=STATUS_DEVICE_CONFIGURATION_ERROR;
    PCM_RESOURCE_LIST list=device->TranslatedResourceList;
    if(!list)return s;
    ULONG interruptIndex=0;
    for(ULONG f=0;f<list->Count;++f){
        PCM_PARTIAL_RESOURCE_LIST p=&list->List[f].PartialResourceList;
        for(ULONG i=0;i<p->Count;++i){
            PCM_PARTIAL_RESOURCE_DESCRIPTOR r=&p->PartialDescriptors[i];
            if(r->Type==CmResourceTypeInterrupt){BddTrace(40+interruptIndex++,STATUS_SUCCESS,r->u.Interrupt.Vector,(r->u.Interrupt.Level<<16)|(r->ShareDisposition<<8)|r->Flags);continue;}
            if(r->Type!=CmResourceTypeMemory)continue;
            for(ULONG j=0;j<RTL_NUMBER_OF(Addresses);++j){
                ULONGLONG address=Addresses[j];
                if(Port){if(j==1)address=Addresses[2];else if(j==2)address=Addresses[1];else if(j==4||j==5||j==9||j==10)address+=0x5000;else if(j==11)address+=0x80;}
                if((ULONGLONG)r->u.Memory.Start.QuadPart!=address)continue;
                if(Reg[j] || r->u.Memory.Length!=Sizes[j])goto Fail;
                Reg[j]=static_cast<PUCHAR>(MmMapIoSpaceEx(r->u.Memory.Start,Sizes[j],
                    (j==2||(j==6u+(!Port))?PAGE_READONLY:PAGE_READWRITE)|PAGE_NOCACHE));
                if(!Reg[j]){s=STATUS_INSUFFICIENT_RESOURCES;goto Fail;}
            }
        }
    }
    for(ULONG j=0;j<RTL_NUMBER_OF(Addresses);++j)if(!Reg[j])goto Fail;
    // Read-only diagnostics for the unhandled interrupt storm. The display L2
    // controller and second pixel valve are already in our mapped resources.
    BddTrace(34,STATUS_SUCCESS,Read(3,0),Read(3,0x0c));
    BddTrace(35,STATUS_SUCCESS,Read(2,0x24),Read(2,0x28));
    // We use PV0's direct VFP interrupt, not any HVS/MOP event routed through
    // display L2. Firmware leaves those sources unmasked; a pending EOF/error
    // then holds their separate GIC line high and repeatedly invokes our ISR.
    // Mask unused sources, as the stock L2 controller does at initialization,
    // and retain the exact incoming mask for the next display owner.
    if(PostOwner){SavedL2Mask=Read(3,0x0c);L2MaskOwned=TRUE;Write(3,0x10,MAXULONG);}
    BddTrace(36,STATUS_SUCCESS,Read(3,0),Read(3,0x0c));
    // Unimplemented mask bits need not read back as ones. Verify the actual
    // level interrupt output is quiet rather than testing reserved bits.
    if(Read(3,0)&~Read(3,0x0c))goto Fail;
    {
        OBJECT_ATTRIBUTES a;IO_STATUS_BLOCK io;UNICODE_STRING name=RTL_CONSTANT_STRING(PI5_FCLK_NAME);
        InitializeObjectAttributes(&a,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
        s=ZwCreateFile(&ClockHandle,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&a,&io,NULL,0,
            FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,NULL,0);
        if(s!=STATUS_SUCCESS)goto Fail;
        for(ULONG j=0;j<RTL_NUMBER_OF(Clocks);++j){
            s=Clock(IOCTL_PI5_FCLK_ACQUIRE,Clocks[j]);if(s!=STATUS_SUCCESS)goto Fail;
            ClockMask|=1u<<j;
        }
    }
    s=STATUS_DEVICE_CONFIGURATION_ERROR;
    {
        ULONG hA=Read(1,0x0c),hB=Read(1,0x10),vA=Read(1,0x14),vB=Read(1,0x18);
        ULONG ref=Read(4,0x1c),post=Read(4,0x28),div=Read(4,0x2c),rm=Read(5,0x18);
        BddTrace(30,STATUS_SUCCESS,ref,post);BddTrace(31,STATUS_SUCCESS,div,rm);
        if((ref&1023)!=54 || (post&31)!=9 || !(div&1024) || !(div&1023) || !(rm&0x80000000) ||
           (hB&65535)*2!=Width || (vB&65535)!=Height)goto Fail;
        HTotal=2*((hA>>16)+(hA&65535)+(hB>>16)+(hB&65535));
        VTotal=(vA>>16)+(vA&65535)+(vB>>16)+(vB&65535);
        ULONGLONG rate=((ULONGLONG)(rm&0x7fffffff)*54000000ull)/(1ull<<21)/((div&1023)*10u);
        if(!HTotal || !VTotal || rate<25000000 || rate>600000000)goto Fail;
        PixelRate=(ULONG)rate;
        BddTrace(32,STATUS_SUCCESS,HTotal,VTotal);BddTrace(33,STATUS_SUCCESS,PixelRate,Read(5,0x1c));
    }
    const BOOLEAN c1=SiliconRevision==0;
    const ULONG expectedVersion=c1?0x2453u:0x2454u;
    if(Read(0,0)!=expectedVersion || Read(0,4)!=4096 || Read(0,0x0c)<1024 ||
       (((Pitch+62)/32)*64+255)/256>512 || Read(0,0x100)!=(0x80000000u|((Width-1)<<16)|(Height-1)) ||
       !(Read(1,0)&1) || Read(1,4)!=3 || Read(1,0x24)!=0 ||
       (Read(1,0x18)&65535)!=Height || Read(0,0x180)!=0)goto Fail;
    OldHead=Read(0,0x110)&0xfff;
    if(OldHead>=0x800 || (Read(0,0x11c)&0xfff)!=OldHead || Read(0,0x4000)!=0x80000000)goto Fail;
    if(!PostOwner&&display->PhysicAddress.QuadPart)goto Fail;
    // Firmware may boot both connected HDMI outputs with independent raster
    // lists. C1 and D0 use the same GEN6 list format, except fixed alpha:
    // C1: CTL0 no alpha-mask bits, CTL2 fixed-alpha mode bit 30.
    // D0: fixed-alpha mask in CTL0, CTL2 mode bits clear.
    const ULONG postCtl0=c1?0x6000c007u:0x600cc007u;
    const ULONG postCtl2=c1?0x4000fff0u:0x0000fff0u;
    if(PostOwner||Read(0,0x4000+OldHead*4)==postCtl0){
       if(Read(0,0x4000+OldHead*4)!=postCtl0 || Read(0,0x4004+OldHead*4)!=0 ||
       Read(0,0x4008+OldHead*4)!=postCtl2 || Read(0,0x400c+OldHead*4)!=((Height-1)<<16 | (Width-1)) ||
       Read(0,0x401c+OldHead*4)!=Pitch)goto Fail;
       if(PostOwner&&((Read(0,0x4014+OldHead*4)&15)!=0 || Read(0,0x4018+OldHead*4)!=display->PhysicAddress.LowPart ||
          display->PhysicAddress.HighPart))goto Fail;
    }
    // Validate that the firmware list walk never enters our reserved tail.
    for(ULONG p=OldHead,n=0;;){
        if(p>=0x800 || ++n>64)goto Fail;
        ULONG w=Read(0,0x4000+p*4);
        if(w==0x80000000){PostListWords=p-OldHead+1;break;}
        if(w!=0x20000000&&!(p==OldHead&&w==postCtl0))goto Fail;
        p+=32;
    }
    for(ULONG i=0;i<10*FramebufferCount();++i){SavedList[i]=Read(0,0x4000+(OwnHead+i)*4);if(SavedList[i]!=0xb0b0b0b0)goto Fail;}
    for(ULONG i=0;i<PostListWords;++i)PostList[i]=Read(0,0x4000+(OldHead+i)*4);
    s=OpenMonitor();if(s!=STATUS_SUCCESS)goto Fail;
    SaveNative(&OriginalNative);
    // GOP permits row padding. Keep the exact POST layout for handback, while
    // the native scanout allocations use the packed pitch required by DWM.
    // A new UPM handle retires firmware's cached raster geometry.
    if(PostOwner){
        ULONG postPointer=Read(0,0x4014+OldHead*4);
        ULONG postBase=(postPointer>>16)&1023,postHandle=(postPointer>>10)&31;
        ULONG handle=postHandle;
        ULONG targetBase=Port?512u:0u;
        if(c1){
            // VC6 PTR0 stores UPM handle-1. The firmware C1 handoff uses
            // encoded handle 0 (hardware handle 1). Do not reuse that cached
            // prefetch context after replacing the framebuffer.
            handle=(postHandle+2)&31;
        }else{
            if(Pitch!=Width*4 || postBase!=targetBase)handle=(handle+2)&31;
            if(handle==(Port?0u:1u))handle=(handle+2)&31;
        }
        UpmDescriptor=(targetBase<<16)|(handle<<10);
        if(c1)BddTrace(219,STATUS_SUCCESS,postPointer,UpmDescriptor);
        Pitch=Width*4;
    }
    {
        DEVICE_DESCRIPTION desc={};ULONG maps=0;
        FrameBytes=(Pitch*Height+4095)&~4095u;
        for(ULONG i=0;i<Monitor.Count;++i)FrameBytes=max(FrameBytes,pi5display::FrameBytes(Monitor.Modes[i]));
        // A replacement monitor can advertise larger modes than the monitor
        // attached at startup. Reserve the supported maximum per fallback
        // surface so an EDID refresh never outgrows a live DMA allocation.
        if(FullDisplay)FrameBytes=32u*1024*1024;
        BufferBytes=ScanoutBytes()+8192;
        desc.Version=DEVICE_DESCRIPTION_VERSION3;desc.Master=TRUE;desc.ScatterGather=TRUE;
        desc.InterfaceType=Internal;desc.DmaAddressWidth=36;desc.MaximumLength=BufferBytes;
        Adapter=IoGetDmaAdapter(device->PhysicalDeviceObject,&desc,&maps);
        s=STATUS_INSUFFICIENT_RESOURCES;if(!Adapter)goto Fail;
        if(Adapter->DmaOperations->Size<FIELD_OFFSET(DMA_OPERATIONS,AllocateCommonBufferEx)+sizeof(Adapter->DmaOperations->AllocateCommonBufferEx) ||
            !Adapter->DmaOperations->AllocateCommonBufferEx){s=STATUS_NOT_SUPPORTED;goto Fail;}
        PHYSICAL_ADDRESS maximum;maximum.QuadPart=0x9ffffffffull;
        Buffer=static_cast<volatile ULONG*>(Adapter->DmaOperations->AllocateCommonBufferEx(Adapter,&maximum,BufferBytes,&Dma,FALSE,0));
        if(!Buffer)goto Fail;
        s=STATUS_DEVICE_CONFIGURATION_ERROR;
        if(((ULONG_PTR)Buffer&4095) || (Dma.QuadPart&4095) || (ULONGLONG)Dma.QuadPart+BufferBytes>0xa00000000ull)goto Fail;
        for(ULONG i=0;i<BufferBytes/4;++i)Buffer[i]=0x50494744;
        if(PostOwner){PostBuffer=static_cast<volatile ULONG*>(MmMapIoSpaceEx(display->PhysicAddress,OriginalDisplay.Pitch*Height,PAGE_READWRITE|PAGE_WRITECOMBINE));
            s=STATUS_INSUFFICIENT_RESOURCES;if(!PostBuffer)goto Fail;}
        for(ULONG frame=0;frame<FramebufferCount();++frame)
            for(ULONG y=0;y<Height;++y)
                for(ULONG x=0;x<Pitch/4;++x)
                    Buffer[1024+SIZE_T(frame)*FrameBytes/4+SIZE_T(y)*Pitch/4+x]=
                        PostBuffer?PostBuffer[SIZE_T(y)*OriginalDisplay.Pitch/4+x]:0;
        KeMemoryBarrier();
    }
    {
        for(ULONG frame=0;frame<FramebufferCount();++frame){
            PHYSICAL_ADDRESS address=Dma;address.QuadPart+=4096+SIZE_T(frame)*FrameBytes;
            ULONG own[10]={c1?0x4900c007u:0x490cc007u,0,c1?0x4000fff0u:0x0000fff0u,
                           ((Height-1)<<16)|(Width-1),0xc0c0c0c0u,
                           UpmDescriptor|(ULONG)address.HighPart,address.LowPart,Pitch,0x80000000,0x80000000};
            for(ULONG i=0;i<10;++i)Write(0,0x4000+(OwnHead+frame*10+i)*4,own[i]);
        }
        KeMemoryBarrier();InterlockedExchange(&Owned,1);Write(0,0x110,OwnHead);
        if(!WaitHead(OwnHead)){s=STATUS_IO_TIMEOUT;goto Fail;}
        if(c1){
            // Release-build C1 scanout telemetry. Keep this read-only: it is
            // specifically for comparing the Windows private list against the
            // user's known-good firmware C1 handoff.
            BddTrace(210,STATUS_SUCCESS,Read(0,0x110),Read(0,0x11c));
            BddTrace(211,STATUS_SUCCESS,Read(0,0x24),Read(0,0x28));
            BddTrace(212,STATUS_SUCCESS,Read(0,0x2c),Read(0,0x22c));
            BddTrace(213,STATUS_SUCCESS,Read(0,0x230),UpmDescriptor);
            BddTrace(220,STATUS_SUCCESS,Read(0,0x200),Read(0,0x204));
            BddTrace(221,STATUS_SUCCESS,Read(0,0x208),Read(0,0x0c));
            for(ULONG i=0;i<10;i+=2)
                BddTrace(214+i/2,STATUS_SUCCESS,
                    Read(0,0x4000+OwnHead*4+i*4),
                    Read(0,0x4000+OwnHead*4+(i+1)*4));
        }
    }
    BddTrace(20,STATUS_SUCCESS,OldHead,OwnHead);
    return STATUS_SUCCESS;
Fail:
#if DBG
    // Preserve the rejected firmware handoff before unmapping it. A failed
    // start has no live adapter escape through which to inspect these fields.
    if(Reg[0]&&OldHead<0xff0){
        BddTrace(101,s,Port,Read(0,0x11c));
        BddTrace(102,s,Read(0,0x150),Read(0,0x4000));
        BddTrace(103,s,display->PhysicAddress.LowPart,Pitch);
        for(ULONG i=0;i<10;i+=2)BddTrace(104+i/2,s,Read(0,0x4000+(OldHead+i)*4),Read(0,0x4000+(OldHead+i+1)*4));
        BddTrace(109,s,Read(0,0x4000+OwnHead*4),Read(0,0x4000+(OwnHead+19)*4));
        ULONG peer=Read(0,0x150)&0xfff;
        if(!Port&&peer<0xff0)for(ULONG i=0;i<10;i+=2)BddTrace(110+i/2,s,Read(0,0x4000+(peer+i)*4),Read(0,0x4000+(peer+i+1)*4));
    }
#endif
    BddTrace(20,s,OldHead,OwnHead);
    (void)Stop();return s;
}
static void TimingSignal(const pi5display::Timing &t,D3DKMDT_VIDEO_SIGNAL_INFO *signal){
    RtlZeroMemory(signal,sizeof(*signal));signal->VideoStandard=D3DKMDT_VSS_OTHER;
    signal->ActiveSize.cx=t.HDisplay;signal->ActiveSize.cy=t.VDisplay;
    signal->TotalSize.cx=t.HTotal;signal->TotalSize.cy=t.VTotal;signal->PixelRate=ULONGLONG(t.Clock)*1000;
    signal->HSyncFreq.Numerator=t.Clock*1000;signal->HSyncFreq.Denominator=t.HTotal;
    signal->VSyncFreq.Numerator=t.Clock*1000;signal->VSyncFreq.Denominator=ULONG(t.HTotal)*t.VTotal;
    signal->ScanLineOrdering=D3DDDI_VSSLO_PROGRESSIVE;
}
VOID PI5_DISPLAY_HW::ModeSignal(ULONG index,D3DKMDT_VIDEO_SIGNAL_INFO *signal) const {
    TimingSignal(Monitor.Modes[index],signal);
}
LONG PI5_DISPLAY_HW::FindMode(const D3DKMDT_VIDEO_SIGNAL_INFO &signal) const {
    for(ULONG i=0;i<Monitor.Count;++i){const auto&t=Monitor.Modes[i];
        if(signal.ActiveSize.cx==t.HDisplay&&signal.ActiveSize.cy==t.VDisplay&&
           signal.TotalSize.cx==t.HTotal&&signal.TotalSize.cy==t.VTotal&&
           signal.PixelRate==ULONGLONG(t.Clock)*1000&&signal.ScanLineOrdering==D3DDDI_VSSLO_PROGRESSIVE)return static_cast<LONG>(i);
    }
    return -1;
}
NTSTATUS PI5_DISPLAY_HW::Descriptor(ULONG offset,ULONG bytes,PVOID buffer) const {
    if(!Monitor.Bytes)return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    if(offset>=Monitor.Bytes)return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    if(!buffer||!bytes)return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(buffer,bytes);RtlCopyMemory(buffer,Monitor.Edid+offset,min(bytes,Monitor.Bytes-offset));return STATUS_SUCCESS;
}
NTSTATUS PI5_DISPLAY_HW::OpenMonitor(){
    RtlZeroMemory(&Monitor,sizeof(Monitor));
    auto&t=OriginalTiming;RtlZeroMemory(&t,sizeof(t));
    t.Display=static_cast<UCHAR>(Port?7:2);t.Clock=(PixelRate+500)/1000;
    t.HDisplay=static_cast<USHORT>(Width);t.VDisplay=static_cast<USHORT>(Height);
    t.HSyncStart=static_cast<USHORT>(Width+2*(Read(1,0x10)>>16));
    t.HSyncEnd=static_cast<USHORT>(t.HSyncStart+2*(Read(1,0x0c)&65535));t.HTotal=static_cast<USHORT>(HTotal);
    t.VSyncStart=static_cast<USHORT>(Height+(Read(1,0x18)>>16));
    t.VSyncEnd=static_cast<USHORT>(t.VSyncStart+(Read(1,0x14)&65535));t.VTotal=static_cast<USHORT>(VTotal);
    t.VRefresh=static_cast<USHORT>((ULONGLONG(PixelRate)+HTotal*VTotal/2)/(HTotal*VTotal));
    t.Flags=((Read(6+Port,0xec)&(1u<<14))?1u:0u)|((Read(6+Port,0xec)&(1u<<15))?2u:0u);
    CurrentTiming=t;
    if(!FullDisplay){Monitor.Preferred=0;Monitor.Add(t);return STATUS_SUCCESS;}
    NativeMaxClock=222000;
    if(HsmRate<120000000||BvbRate<75000000)NativeMaxClock=0;
    else{
        NativeMaxClock=min(NativeMaxClock,static_cast<ULONG>(ULONGLONG(HsmRate)*100/101/1000));
        if(BvbRate<150000000)NativeMaxClock=min(NativeMaxClock,148500u);
    }
    OBJECT_ATTRIBUTES a;IO_STATUS_BLOCK io;UNICODE_STRING name=RTL_CONSTANT_STRING(PI5_MAILBOX_NAME);
    InitializeObjectAttributes(&a,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,nullptr,nullptr);
    NTSTATUS s=ZwCreateFile(&MailboxHandle,GENERIC_READ|SYNCHRONIZE,&a,&io,nullptr,0,
        FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,nullptr,0);
    if(s!=STATUS_SUCCESS)return s;
    return RefreshMonitor(FALSE);
}
NTSTATUS PI5_DISPLAY_HW::RefreshMonitor(BOOLEAN requireEdid){
    PAGED_CODE();
    if(!MailboxHandle)return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&Monitor,sizeof(Monitor));
    auto t=OriginalTiming;IO_STATUS_BLOCK io={};NTSTATUS s=STATUS_SUCCESS;
    ULONG blocks=1;
    for(ULONG block=0;block<blocks;++block){
        PI5_MAILBOX_DISPLAY_QUERY q={PI5_MAILBOX_VERSION,Pi5MailboxDisplayEdid,Port,block};PI5_MAILBOX_DISPLAY_RESULT result={};
        s=ZwDeviceIoControlFile(MailboxHandle,nullptr,nullptr,nullptr,&io,IOCTL_PI5_MAILBOX_DISPLAY_QUERY,&q,sizeof(q),&result,sizeof(result));
        if(s!=STATUS_SUCCESS||io.Information!=sizeof(result)||result.Version!=q.Version||result.Operation!=q.Operation||result.Port!=Port||result.Block!=block||
           !pi5display::Checksum(result.Data.Edid)||(!block&&!pi5display::Header(result.Data.Edid)))break;
        RtlCopyMemory(Monitor.Edid+block*128,result.Data.Edid,128);Monitor.Bytes+=128;
        if(!block)blocks=min(ULONG(result.Data.Edid[126])+1,pi5display::MaxEdidBlocks);
    }
    if(requireEdid&&!Monitor.Bytes)return STATUS_DEVICE_NOT_READY;
    (void)Monitor.Parse();
    // Advertise only modes whose HDMI fields and analog drive range we can
    // program. Keep the preferred EDID mode when it survives this filter.
    ULONG count=0,preferred=pi5display::MaxModes;
    for(ULONG i=0;i<Monitor.Count;++i)if(pi5display::NativeSupported(Monitor.Modes[i])&&Monitor.Modes[i].Clock<=NativeMaxClock){
        if(i==Monitor.Preferred)preferred=count;
        Monitor.Modes[count++]=Monitor.Modes[i];
    }
    Monitor.Count=count;Monitor.Preferred=preferred;
    // Preserve the current signal as a safe mode if EDID is absent/truncated.
    // A running firmware mode need not appear in that monitor's EDID.
    if(!Monitor.Hdmi)t.Flags|=0x200;
    for(ULONG i=0;i<Monitor.Count;++i){const auto&m=Monitor.Modes[i];if(pi5display::Same(m,t)){t.VideoId=m.VideoId;t.Flags=m.Flags;break;}}
    if(!Owned)CurrentTiming=t;
    Monitor.Add(t);if(Monitor.Preferred>=Monitor.Count)Monitor.Preferred=0;
    if(!Monitor.Count)return STATUS_DEVICE_CONFIGURATION_ERROR;
    BddTrace(92,STATUS_SUCCESS,Port,Monitor.Count);return STATUS_SUCCESS;
}
BOOLEAN PI5_DISPLAY_HW::SynchronizeMode(PVOID context){
    auto self=static_cast<PI5_DISPLAY_HW*>(context);
    if(self->Pending>=0||(self->Peer&&self->Peer->Owned&&self->Peer->Pending>=0))return FALSE;
    self->ModeChanging=1;self->Write(1,0x24,0);self->Write(1,0x28,0x80);
    if(self->Peer&&self->Peer->Owned){auto peer=self->Peer;peer->ModeChanging=1;peer->Write(1,0x24,0);peer->Write(1,0x28,0x80);}
    return TRUE;
}
VOID PI5_DISPLAY_HW::ModeLists(){
    Front=0;Pending=-1;PendingArmed=0;PendingCompletion=nullptr;
    RtlZeroMemory(DirectPixels,sizeof(DirectPixels));RtlZeroMemory(DirectAddresses,sizeof(DirectAddresses));
    LogicalAddress=0;
    for(ULONG frame=0;frame<FramebufferCount();++frame){
        for(ULONG i=0;i<Pitch*Height/4;++i)Buffer[1024+SIZE_T(frame)*FrameBytes/4+i]=0;
    }
    WriteCurrentLists();
}
VOID PI5_DISPLAY_HW::WriteCurrentLists(){
    for(ULONG frame=0;frame<FramebufferCount();++frame){
        PHYSICAL_ADDRESS address=Dma;address.QuadPart+=4096+SIZE_T(frame)*FrameBytes;
        if(DirectPixels[frame])address.QuadPart=DirectAddresses[frame];
        const BOOLEAN c1=SiliconRevision==0;
        ULONG list[10]={c1?0x4900c007u:0x490cc007u,0,c1?0x4000fff0u:0x0000fff0u,
                        ((Height-1)<<16)|(Width-1),0xc0c0c0c0u,
                        UpmDescriptor|static_cast<ULONG>(address.HighPart),address.LowPart,Pitch,0x80000000,0x80000000};
        for(ULONG i=0;i<10;++i)Write(0,0x4000+(OwnHead+frame*10+i)*4,list[i]);
    }
}
NTSTATUS PI5_DISPLAY_HW::WaitTiming(const pi5display::Timing &timing){
    // Verify the signal and two hardware frame boundaries before completing
    // the Windows mode transaction.
    ULONG first=0,seen=0;LARGE_INTEGER pause;pause.QuadPart=-10000;
    ULONGLONG deadline=KeQueryInterruptTime()+2500000;
    do{
        ULONG frame=(Read(0,0x118)>>16)&63;
        ULONG hA=Read(1,0x0c),hB=Read(1,0x10),vA=Read(1,0x14),vB=Read(1,0x18);
        ULONG pixels=(Read(1,4)&0x20000000)?1u:2u;
        ULONG div=Read(4,0x2c)&1023,rm=Read(5,0x18)&0x7fffffff;
        ULONGLONG clock=div?(ULONGLONG(rm)*54000000ull)/(1ull<<21)/(div*10u):0;
        ULONGLONG requested=ULONGLONG(timing.Clock)*1000,difference=clock>requested?clock-requested:requested-clock;
        if((hB&65535)*pixels==timing.HDisplay&&(vB&65535)==timing.VDisplay&&
           (hB>>16)*pixels==ULONG(timing.HSyncStart-timing.HDisplay)&&(hA&65535)*pixels==ULONG(timing.HSyncEnd-timing.HSyncStart)&&
           (hA>>16)*pixels==ULONG(timing.HTotal-timing.HSyncEnd)&&(vB>>16)==ULONG(timing.VSyncStart-timing.VDisplay)&&
           (vA&65535)==ULONG(timing.VSyncEnd-timing.VSyncStart)&&(vA>>16)==ULONG(timing.VTotal-timing.VSyncEnd)&&
           difference<=max(requested/1000,1000ull)&&Read(0,0x100)==(0x80000000u|((ULONG(timing.HDisplay)-1)<<16)|(ULONG(timing.VDisplay)-1))){
            if(!seen){seen=1;first=frame;}else if(((frame-first)&63)>=2)return STATUS_SUCCESS;
        }else seen=0;
        KeDelayExecutionThread(KernelMode,FALSE,&pause);
    }while(KeQueryInterruptTime()<deadline);
    BddTrace(94,STATUS_IO_TIMEOUT,Read(4,0x2c),Read(5,0x18));
    BddTrace(95,STATUS_IO_TIMEOUT,Read(1,0x0c),Read(1,0x10));
    BddTrace(96,STATUS_IO_TIMEOUT,Read(1,0x14),Read(1,0x18));
    BddTrace(97,STATUS_IO_TIMEOUT,Read(0,0x100),Read(0,0x118));
    return STATUS_IO_TIMEOUT;
}
ULONG PI5_DISPLAY_HW::ModeRead(ULONG field) const {
    const auto&r=pi5display::NativeRegisters[field];
    return Read(r.Region==6?6+Port:r.Region,r.Offset+(r.Region==8?Port*4:0));
}
VOID PI5_DISPLAY_HW::ModeWrite(ULONG field,ULONG value){
    const auto&r=pi5display::NativeRegisters[field];
    Write(r.Region==6?6+Port:r.Region,r.Offset+(r.Region==8?Port*4:0),value);
}
VOID PI5_DISPLAY_HW::SaveNative(pi5display::NativeState *state) const {
    for(ULONG i=0;i<pi5display::NativeFieldCount;++i)state->Value[i]=ModeRead(i);
}
static VOID ModeDelay(ULONG milliseconds){
    LARGE_INTEGER pause;pause.QuadPart=-LONGLONG(milliseconds)*10000;
    KeDelayExecutionThread(KernelMode,FALSE,&pause);
}
BOOLEAN PI5_DISPLAY_HW::WaitBits(ULONG unit,ULONG offset,ULONG mask,ULONG value,ULONG milliseconds){
    ULONGLONG deadline=KeQueryInterruptTime()+ULONGLONG(milliseconds)*10000;
    do{if((Read(unit,offset)&mask)==value)return TRUE;ModeDelay(1);}while(KeQueryInterruptTime()<deadline);
    return (Read(unit,offset)&mask)==value;
}
NTSTATUS PI5_DISPLAY_HW::DisableNative(){
    using namespace pi5display;
    ModeWrite(PvVertical,ModeRead(PvVertical)&~1u);
    if(!WaitBits(1,4,1,0,20))return BddTrace(99,STATUS_IO_TIMEOUT,Port,10);
    // Drain the link FIFO before changing its clock. Never clear VID_CTL's
    // ENABLE on BCM2712: that operation can hang the complete system.
    ModeDelay(20);
    ModeWrite(PacketConfig,0x10000);
    ModeWrite(VidControl,ModeRead(VidControl)|0x00840000);
    ModeDelay(1);
    ModeWrite(PvControl,ModeRead(PvControl)&~1u);
    ModeWrite(PvControl,ModeRead(PvControl)|2u);
    Write(0,0x100,Read(0,0x100)|0x40000000);
    Write(0,0x100,Read(0,0x100)&~0x80000000u);
    return STATUS_SUCCESS;
}
NTSTATUS PI5_DISPLAY_HW::ApplyNative(const pi5display::Timing &timing,const pi5display::NativeState &state){
    using namespace pi5display;const auto*v=state.Value;
    // Both the normal transition and rollback use the same ordered sequence.
    // The snapshot includes every modified register, including AVI and CSC.
    ModeWrite(PhyReset,0);ModeWrite(PhyPower,0);ModeWrite(PostDivider,0x10);
    for(ULONG i=Misc0;i<=Misc8;++i)ModeWrite(i,v[i]);
    ModeWrite(RefClock,v[RefClock]);ModeWrite(PhyReset,v[PhyReset]);
    ModeWrite(RmOffset,v[RmOffset]);ModeWrite(VcoDivider,v[VcoDivider]);
    ModeWrite(PllConfig,v[PllConfig]);ModeWrite(PostDivider,v[PostDivider]);
    for(ULONG i=Lane0;i<=LaneClock;++i)ModeWrite(i,v[i]);
    ModeWrite(TmdsWord,v[TmdsWord]);ModeWrite(PhyPower,v[PhyPower]);ModeWrite(PllPower,v[PllPower]);
    ModeWrite(PllReset,v[PllReset]&~1u);ModeWrite(PllReset,v[PllReset]);
    ModeWrite(Scheduler,v[Scheduler]|0x8020);
    for(ULONG i=Ha;i<=Scrambler;++i)ModeWrite(i,v[i]);
    for(ULONG i=ClockStop;i<=CscChannel;++i)ModeWrite(i,v[i]);
    ModeWrite(PacketConfig,0x10000);
    for(ULONG i=Avi0;i<=Avi8;++i)ModeWrite(i,v[i]);
    ModeWrite(PvVertical,v[PvVertical]&~1u);
    for(ULONG i=PvEvenDelay;i<=PvPipe;++i)ModeWrite(i,v[i]);
    ModeWrite(PvControl,(v[PvControl]&~1u)|2u);
    ModeWrite(HvsControl1,v[HvsControl1]);
    Write(0,0x100,0x40000000);
    KeMemoryBarrier();Write(0,0x110,OwnHead+static_cast<ULONG>(Front)*10);
    Write(0,0x100,0x80000000u|((ULONG(timing.HDisplay)-1)<<16)|(ULONG(timing.VDisplay)-1));
    ModeWrite(PvControl,v[PvControl]|1u);
    ModeWrite(Fifo,1);
    ModeWrite(PvVertical,v[PvVertical]|1u);
    ModeWrite(VidControl,(v[VidControl]&~0x00040000u)|0x80000000u|(Visible?0u:0x00040000u));
    // Retain manual format and ignore stale VSYNC predictions after enabling
    // the stream, as in the native HDMI encoder sequence.
    ModeWrite(Scheduler,v[Scheduler]|0x8020);
    if(!WaitBits(6+Port,0xe8,2,(v[Scheduler]&1)?2u:0u,250))return BddTrace(99,STATUS_IO_TIMEOUT,Port,11);
    ModeWrite(PacketConfig,v[PacketConfig]);
    ULONG fifo=v[Fifo]&0xefff;
    ModeWrite(Fifo,fifo&~0x40u);ModeWrite(Fifo,fifo|0x40);
    ModeDelay(1);
    ModeWrite(Fifo,fifo&~0x40u);ModeWrite(Fifo,fifo|0x40);
    if(!WaitBits(6+Port,0x7c,0x4000,0x4000,50))return BddTrace(99,STATUS_IO_TIMEOUT,Port,12);
    NTSTATUS s=WaitTiming(timing);if(s!=STATUS_SUCCESS)return s;
    if(!WaitHead(OwnHead+static_cast<ULONG>(Front)*10))return BddTrace(99,STATUS_IO_TIMEOUT,Port,13);
    return STATUS_SUCCESS;
}
NTSTATUS PI5_DISPLAY_HW::ProgramMode(const pi5display::Timing &timing){
    using namespace pi5display;
    if(!Owned||!Supported(timing)||FrameBytes<pi5display::FrameBytes(timing))return STATUS_INVALID_PARAMETER;
    if(Same(timing,CurrentTiming)&&timing.Flags==CurrentTiming.Flags)return STATUS_SUCCESS;
    NativeState before={},desired={};SaveNative(&before);desired=before;
    if(Same(timing,OriginalTiming))desired=OriginalNative;
    else if(timing.Clock>NativeMaxClock||!NativeBuild(timing,desired))return STATUS_NOT_SUPPORTED;
    NTSTATUS s=DrainFlips();if(s!=STATUS_SUCCESS)return s;
    bool peerActive=Peer&&Peer->Owned;
    if(peerActive){s=Peer->DrainFlips();if(s!=STATUS_SUCCESS)return s;}
    BOOLEAN called=FALSE;s=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMode,this,0,&called);
    if(s!=STATUS_SUCCESS)return s;if(!called)return STATUS_DEVICE_BUSY;
    auto old=CurrentTiming;
    s=DisableNative();BddTrace(99,s,Port,0);
    if(s==STATUS_SUCCESS){
        // UPM handles describe a raster layout, not merely its reserved SRAM
        // address. Retire the previous handle when changing the row geometry.
        // Each output uses its own parity and its existing disjoint SRAM half.
        if(Pitch!=pi5display::Pitch(timing)){
            ULONG handle=(((UpmDescriptor>>10)&31)+2)&31;
            UpmDescriptor=(UpmDescriptor&~(31u<<10))|(handle<<10);
        }
        CurrentTiming=timing;Width=timing.HDisplay;Height=timing.VDisplay;Pitch=pi5display::Pitch(timing);
        HTotal=timing.HTotal;VTotal=timing.VTotal;PixelRate=timing.Clock*1000;
        ModeLists();s=ApplyNative(timing,desired);BddTrace(99,s,Port,1);
    }
    if(s!=STATUS_SUCCESS){
        NTSTATUS disabled=DisableNative();
        if(Pitch!=pi5display::Pitch(old)){
            ULONG handle=(((UpmDescriptor>>10)&31)+2)&31;
            UpmDescriptor=(UpmDescriptor&~(31u<<10))|(handle<<10);
        }
        CurrentTiming=old;Width=old.HDisplay;Height=old.VDisplay;Pitch=pi5display::Pitch(old);
        HTotal=old.HTotal;VTotal=old.VTotal;PixelRate=old.Clock*1000;
        ModeLists();NTSTATUS restored=ApplyNative(old,before);BddTrace(99,restored,Port,2);
        if(disabled!=STATUS_SUCCESS||restored!=STATUS_SUCCESS){Fault=1;s=STATUS_DEVICE_HARDWARE_ERROR;}
    }
    TimingChanged=!Same(CurrentTiming,OriginalTiming)||CurrentTiming.Flags!=OriginalTiming.Flags;
    InterlockedExchange(&ModeChanging,0);
    if(peerActive){InterlockedExchange(&Peer->ModeChanging,0);(void)Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMask,Peer,0,&called);}
    NTSTATUS mask=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMask,this,0,&called);
    BddTrace(93,s,Port,(Width<<16)|Height);return s==STATUS_SUCCESS?mask:s;
}
NTSTATUS PI5_DISPLAY_HW::SetMode(ULONG index,DXGK_DISPLAY_INFORMATION *display){
    PAGED_CODE();if(index>=Monitor.Count||!display)return STATUS_INVALID_PARAMETER;
    NTSTATUS s=ProgramMode(Monitor.Modes[index]);if(s!=STATUS_SUCCESS)return s;
    TimingChanged=!pi5display::Same(CurrentTiming,OriginalTiming)||CurrentTiming.Flags!=OriginalTiming.Flags;
    display->Width=Width;display->Height=Height;display->Pitch=Pitch;display->PhysicAddress=Address();return STATUS_SUCCESS;
}

BOOLEAN PI5_DISPLAY_HW::SynchronizeMask(PVOID context) {
    PI5_DISPLAY_HW *self=static_cast<PI5_DISPLAY_HW*>(context);
    if(self->Reg[1]){self->Write(1,0x24,!self->ModeChanging&&(self->Enabled||self->RefreshWaiters||self->Pending>=0)?0x80:0);self->Write(1,0x28,0x80);}
    return TRUE;
}
NTSTATUS PI5_DISPLAY_HW::Control(DXGK_INTERRUPT_TYPE type,BOOLEAN enable) {
    PAGED_CODE();
    if(type!=(FullDisplay?DXGK_INTERRUPT_CRTC_VSYNC:DXGK_INTERRUPT_DISPLAYONLY_VSYNC))return STATUS_NOT_SUPPORTED;
    if(!Owned)return STATUS_DEVICE_NOT_READY;
    if(!enable){NTSTATUS drained=DrainFlips();if(drained!=STATUS_SUCCESS)return drained;}
    BOOLEAN result=FALSE;InterlockedIncrement(&ControlCalls);InterlockedExchange(&Enabled,!!enable);
    return Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMask,this,0,&result);
}
BOOLEAN PI5_DISPLAY_HW::Interrupt() {
    InterlockedIncrement(&IsrCalls);
    if(!Reg[1] || !Owned || !(Read(1,0x28)&0x80))return FALSE;
    Write(1,0x28,0x80);if(ModeChanging)return TRUE;InterlockedIncrement(&Irqs);
    BOOLEAN completed=FALSE;
    if(Pending>=0&&PendingArmed&&(Read(0,0x11c)&0xfff)==OwnHead+static_cast<ULONG>(Pending)*10){
        // The active-list register is also the stock vc4 driver's retirement
        // test. Only now may the old front buffer be overwritten.
        InterlockedExchange(&Front,Pending);InterlockedExchange64(&LogicalAddress,PendingAddress);
        RecordFrame();
        // A synchronous present finishes at this refresh, before its VSync
        // notification. Reporting it from the woken worker misses this DPC's
        // retirement and can add another refresh of latency.
        auto completion=PendingCompletion;PendingCompletion=nullptr;
        if(completion)completion(CompletionContext,CompletionFence);
        PendingArmed=0;InterlockedExchange(&Pending,-1);completed=TRUE;
    }
    if(Enabled){
        DXGKARGCB_NOTIFY_INTERRUPT_DATA data={};
        if(FullDisplay){data.InterruptType=DXGK_INTERRUPT_CRTC_VSYNC;data.CrtcVsync.VidPnTargetId=Port;data.CrtcVsync.PhysicalAddress.QuadPart=InterlockedCompareExchange64(&LogicalAddress,0,0);}
        else{data.InterruptType=DXGK_INTERRUPT_DISPLAYONLY_VSYNC;data.DisplayOnlyVsync.VidPnTargetId=Port;}
        Dxgk->DxgkCbNotifyInterrupt(Dxgk->DeviceHandle,&data);
        InterlockedIncrement(&Notified);
    }
    if(Enabled||RefreshWaiters||completed)Dxgk->DxgkCbQueueDpc(Dxgk->DeviceHandle);
    if(!Enabled&&!RefreshWaiters&&Pending<0)Write(1,0x24,0);
    return TRUE;
}
VOID PI5_DISPLAY_HW::RefreshDpc() {
    // KeSetEvent cannot run at the display interrupt's DIRQL.
    if(RefreshWaiters)KeSetEvent(&RefreshWake,IO_NO_INCREMENT,FALSE);
}
struct PI5_FLIP_REQUEST {PI5_DISPLAY_HW *Self;ULONG Offset;LONGLONG Address;BOOLEAN Flip,NoWait;NTSTATUS Status;
    PI5_DISPLAY_HW::FLIP_COMPLETION Completion;PVOID Context;ULONG Fence;};
VOID PI5_DISPLAY_HW::PublishPendingFlip() {
    if(!Owned||Pending<0||PendingArmed)return;
    PendingArmed=1;KeMemoryBarrier();Write(0,0x110,OwnHead+static_cast<ULONG>(Pending)*10);
    Write(1,0x24,0x80);
}
BOOLEAN PI5_DISPLAY_HW::SynchronizePublish(PVOID context) {
    static_cast<PI5_DISPLAY_HW*>(context)->PublishPendingFlip();return TRUE;
}
NTSTATUS PI5_DISPLAY_HW::AttachDirectSegment(PVOID memory,PHYSICAL_ADDRESS physical,ULONG bytes) {
    PAGED_CODE();
    // The platform has no HVS IOMMU. Confirm that the HAL common-buffer DMA
    // address is its CPU physical address before applying that same mapping
    // to the physically contiguous WDDM segment.
    if(!Owned||!FullDisplay||!memory||bytes<FrameBytes||(physical.QuadPart&4095)||
       static_cast<ULONGLONG>(physical.QuadPart)>=0xa00000000ull||
       bytes>0xa00000000ull-static_cast<ULONGLONG>(physical.QuadPart)||
       MmGetPhysicalAddress(ScanoutMemory()).QuadPart!=Dma.QuadPart+4096)return STATUS_NOT_SUPPORTED;
    DirectSegment=static_cast<PUCHAR>(memory);DirectBase=physical.QuadPart;DirectBytes=bytes;
    return STATUS_SUCCESS;
}
NTSTATUS PI5_DISPLAY_HW::QueueDirectScanout(ULONGLONG offset) {
    if(!Owned||!FullDisplay||!DirectSegment||Pending>=0||ModeChanging)return STATUS_INVALID_DEVICE_STATE;
    if((offset&4095)||offset>DirectBytes||((Pitch*Height+4095)&~4095u)>DirectBytes-offset)return STATUS_INVALID_PARAMETER;
    ULONG index=1u-static_cast<ULONG>(Front);ULONGLONG address=DirectBase+offset;
    // Rewrite only the inactive list, and publish it after both address words
    // and its CPU metadata are ready. The ISR retires it from HVS's active head.
    Write(0,0x4000+(OwnHead+index*10+5)*4,UpmDescriptor|static_cast<ULONG>(address>>32));
    Write(0,0x4000+(OwnHead+index*10+6)*4,static_cast<ULONG>(address));
    DirectPixels[index]=DirectSegment+offset;DirectAddresses[index]=address;
    ++Presents;SourceWidth=Width;SourceHeight=Height;
    PendingCompletion=nullptr;PendingAddress=static_cast<LONGLONG>(offset);
    InterlockedExchange(&Pending,static_cast<LONG>(index));PendingArmed=0;
    PublishPendingFlip();return STATUS_SUCCESS;
}
BOOLEAN PI5_DISPLAY_HW::SynchronizeFlip(PVOID context) {
    auto request=static_cast<PI5_FLIP_REQUEST*>(context);auto self=request->Self;
    request->Status=STATUS_INVALID_DEVICE_STATE;
    if(!self->Owned||!self->FullDisplay||self->Pending>=0)return TRUE;
    ULONG index=request->Offset/self->FrameBytes;
    if(request->Offset%self->FrameBytes||index>1||index!=(request->Flip?1u-static_cast<ULONG>(self->Front):static_cast<ULONG>(self->Front)))return TRUE;
    if(request->Flip){
        self->PendingCompletion=request->Completion;self->CompletionContext=request->Context;self->CompletionFence=request->Fence;
        self->PendingAddress=request->Address;InterlockedExchange(&self->Pending,static_cast<LONG>(index));
        self->PendingArmed=0;
        // A no-wait flip is published in the same interrupt-serialized callback
        // that reports DMA completion. VSync cannot report the new address
        // between programming the HVS list and announcing that completion.
        if(!request->NoWait)self->PublishPendingFlip();
    }else{
        InterlockedExchange64(&self->LogicalAddress,request->Address);self->RecordFrame();
    }
    request->Status=STATUS_SUCCESS;return TRUE;
}
NTSTATUS PI5_DISPLAY_HW::WaitForFlip() {
    PAGED_CODE();
    if(!Owned||!FullDisplay)return STATUS_INVALID_DEVICE_STATE;
    if(InterlockedCompareExchange(&Pending,0,0)<0)return STATUS_SUCCESS;
    ULONGLONG deadline=KeQueryInterruptTime()+2500000;
    InterlockedIncrement(&RefreshWaiters);BOOLEAN called=FALSE;
    NTSTATUS status=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMask,this,0,&called);
    while(status==STATUS_SUCCESS){
        KeClearEvent(&RefreshWake);
        if(InterlockedCompareExchange(&Pending,0,0)<0)break;
        ULONGLONG now=KeQueryInterruptTime();if(now>=deadline){status=STATUS_IO_TIMEOUT;break;}
        LARGE_INTEGER timeout;timeout.QuadPart=-static_cast<LONGLONG>(deadline-now);
        NTSTATUS waited=KeWaitForSingleObject(&RefreshWake,Executive,KernelMode,FALSE,&timeout);
        if(waited!=STATUS_SUCCESS&&waited!=STATUS_TIMEOUT){status=waited;break;}
    }
    InterlockedDecrement(&RefreshWaiters);
    NTSTATUS restored=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMask,this,0,&called);
    return status==STATUS_SUCCESS?restored:status;
}
NTSTATUS PI5_DISPLAY_HW::PrepareScanout(BOOLEAN flip,ULONG *offset) {
    PAGED_CODE();
    if(!offset||!FrameBytes)return STATUS_INVALID_PARAMETER;
    NTSTATUS status=WaitForFlip();if(status!=STATUS_SUCCESS)return status;
    *offset=(flip?1u-static_cast<ULONG>(Front):static_cast<ULONG>(Front))*FrameBytes;return STATUS_SUCCESS;
}
BOOLEAN PI5_DISPLAY_HW::SynchronizeCancelCompletion(PVOID context) {
    static_cast<PI5_DISPLAY_HW*>(context)->PendingCompletion=nullptr;return TRUE;
}
NTSTATUS PI5_DISPLAY_HW::CommitScanout(ULONG offset,LONGLONG address,BOOLEAN flip,BOOLEAN noWait,ULONG interval,
                                     FLIP_COMPLETION completion,PVOID context,ULONG fence) {
    PAGED_CODE();
    if(!FrameBytes||interval>4||(noWait&&interval>1)||(completion&&(!flip||noWait||interval!=1)))return STATUS_INVALID_PARAMETER;
    PI5_FLIP_REQUEST request={this,offset,address,flip,noWait,STATUS_UNSUCCESSFUL,completion,context,fence};BOOLEAN called=FALSE;
    NTSTATUS status=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeFlip,&request,0,&called);
    if(status!=STATUS_SUCCESS)return status;if(request.Status!=STATUS_SUCCESS)return request.Status;
    if(flip&&!noWait){status=WaitForFlip();if(status!=STATUS_SUCCESS){
        // The callback context belongs to the adapter, but a faulted fence
        // must not be completed later if the display eventually catches up.
        (void)Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeCancelCompletion,this,0,&called);
        return status;}if(interval>1)return WaitForRefresh(interval-1);}
    return STATUS_SUCCESS;
}
VOID PI5_DISPLAY_HW::ClearFramebuffers() {
    if(!Buffer)return;
    for(ULONG frame=0;frame<FramebufferCount();++frame){auto pixels=static_cast<volatile ULONG*>(FramebufferAt(frame));
        for(ULONG i=0;i<Pitch*Height/4;++i)pixels[i]=0;
    }KeMemoryBarrier();
}
VOID PI5_DISPLAY_HW::Visibility(BOOLEAN visible) {
    Visible=visible;
    if(Owned&&Reg[8]){
        // Keep sync and scanout running while hiding pixels. A primary belongs
        // to Windows; clearing it here destroys content DWM may later reuse.
        ULONG control=ModeRead(pi5display::VidControl)&~0x00040000u;
        ModeWrite(pi5display::VidControl,control|(visible?0u:0x00040000u));
    }
}
VOID PI5_DISPLAY_HW::RecordFrame() {
#if DBG
    // Writers run only in the ISR or its synchronize callback. Readers use a
    // generation check rather than copying pageable escape memory at DIRQL.
    InterlockedIncrement(&FrameGeneration);uint64_t sequence=static_cast<uint64_t>(FrameSequence)+1;
    FrameSamples[(sequence-1)%512]={sequence,static_cast<uint64_t>(KeQueryPerformanceCounter(nullptr).QuadPart)};
    InterlockedExchange64(&FrameSequence,static_cast<LONG64>(sequence));InterlockedIncrement(&FrameGeneration);
#endif
}
#if DBG
VOID PI5_DISPLAY_HW::SnapshotFrames(PI5_FRAME_STATUS *out) {
    LARGE_INTEGER frequency;KeQueryPerformanceCounter(&frequency);out->Frequency=frequency.QuadPart;out->Blackouts=BlackoutCount();out->Reserved=0;
    for(unsigned attempt=0;attempt<16;++attempt){LONG generation=InterlockedCompareExchange(&FrameGeneration,0,0);if(generation&1){KeStallExecutionProcessor(1);continue;}
        out->Sequence=static_cast<uint64_t>(InterlockedCompareExchange64(&FrameSequence,0,0));RtlCopyMemory(out->Samples,FrameSamples,sizeof(FrameSamples));KeMemoryBarrier();
        if(generation==InterlockedCompareExchange(&FrameGeneration,0,0))return;
    }
    out->Sequence=0;
}
NTSTATUS PI5_DISPLAY_HW::CheckFlips() {
    if(!FullDisplay)return STATUS_INVALID_DEVICE_STATE;
    for(ULONG i=0;i<6;++i){ULONG offset=0;NTSTATUS status=PrepareScanout(TRUE,&offset);if(status!=STATUS_SUCCESS)return status;
        status=CommitScanout(offset,0,TRUE,(i&1)!=0,1);if(status!=STATUS_SUCCESS)return status;
        if(i&1){BOOLEAN called=FALSE;status=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizePublish,this,0,&called);if(status!=STATUS_SUCCESS)return status;}
        status=WaitForFlip();if(status!=STATUS_SUCCESS)return status;
        if((Read(0,0x11c)&0xfff)!=OwnHead+static_cast<ULONG>(Front)*10||static_cast<ULONG>(Front)!=offset/FrameBytes)return STATUS_DATA_ERROR;
    }
    return Front==0?STATUS_SUCCESS:STATUS_DATA_ERROR;
}
#endif
BOOLEAN PI5_DISPLAY_HW::SynchronizeStop(PVOID context) {
    auto self=static_cast<PI5_DISPLAY_HW*>(context);self->Pending=-1;self->PendingArmed=0;self->PendingCompletion=nullptr;self->Enabled=0;self->RefreshWaiters=0;
    return SynchronizeMask(self);
}
NTSTATUS PI5_DISPLAY_HW::Stop() {
    PAGED_CODE();
    if(Owned){
        if(TimingChanged){NTSTATUS restored=ProgramMode(OriginalTiming);if(restored!=STATUS_SUCCESS)return restored;TimingChanged=FALSE;}
        NTSTATUS drained=DrainFlips();if(drained!=STATUS_SUCCESS)return drained;
        BOOLEAN result=FALSE;InterlockedExchange(&Enabled,0);
        NTSTATUS s=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeStop,this,0,&result);
        if(s!=STATUS_SUCCESS)return s;
        // Return a valid firmware allocation to the next display owner.
        if(Buffer && PostBuffer){
            auto pixels=static_cast<volatile ULONG*>(Framebuffer());
            for(ULONG y=0;y<Height;++y)
                for(ULONG x=0;x<Width;++x)
                    PostBuffer[SIZE_T(y)*OriginalDisplay.Pitch/4+x]=pixels[SIZE_T(y)*Pitch/4+x];
            KeMemoryBarrier();
        }
        for(ULONG i=0;i<PostListWords;++i)
            WRITE_REGISTER_ULONG(reinterpret_cast<PULONG>(Reg[0]+0x4000+(OldHead+i)*4),PostList[i]);
        KeMemoryBarrier();Write(0,0x110,OldHead);
        if(!WaitHead(OldHead)){Fault=1;BddTrace(21,STATUS_IO_TIMEOUT);return STATUS_IO_TIMEOUT;}
        ModeWrite(pi5display::VidControl,OriginalNative.Value[pi5display::VidControl]);
        InterlockedExchange(&Owned,0);
        Pending=-1;
        for(ULONG i=0;i<10*FramebufferCount();++i)Write(0,0x4000+(OwnHead+i)*4,SavedList[i]);
    }
    if(L2MaskOwned){
        Write(3,0x10,MAXULONG);Write(3,0x14,~SavedL2Mask);
        (void)Read(3,0x0c);L2MaskOwned=FALSE;
    }
    if(PostBuffer){MmUnmapIoSpace(const_cast<ULONG*>(PostBuffer),OriginalDisplay.Pitch*OriginalDisplay.Height);PostBuffer=NULL;}
    if(Buffer){Adapter->DmaOperations->FreeCommonBuffer(Adapter,BufferBytes,Dma,const_cast<ULONG*>(Buffer),FALSE);Buffer=NULL;BufferBytes=0;}
    DirectSegment=nullptr;DirectBytes=0;RtlZeroMemory(DirectPixels,sizeof(DirectPixels));
    if(Adapter){Adapter->DmaOperations->PutDmaAdapter(Adapter);Adapter=NULL;}
    if(MailboxHandle){
        ZwClose(MailboxHandle);MailboxHandle=nullptr;
    }
    if(ClockHandle){
        for(ULONG i=0;i<RTL_NUMBER_OF(Clocks);++i)if(ClockMask&(1u<<i)){
            NTSTATUS s=Clock(IOCTL_PI5_FCLK_RELEASE,Clocks[i]);if(s!=STATUS_SUCCESS)return s;
            ClockMask&=~(1u<<i);
        }
        ZwClose(ClockHandle);ClockHandle=NULL;
    }
    for(ULONG i=0;i<RTL_NUMBER_OF(Addresses);++i)if(Reg[i]){MmUnmapIoSpace(Reg[i],Sizes[i]);Reg[i]=NULL;}
    return STATUS_SUCCESS;
}
NTSTATUS PI5_DISPLAY_HW::WaitForRefresh(ULONG count) {
    PAGED_CODE();
    if(count>4||!Reg[0]||!Owned)return STATUS_INVALID_PARAMETER;
    if(!count)return STATUS_SUCCESS;
    ULONG first=(Read(0,0x118)>>16)&63;
    ULONGLONG deadline=KeQueryInterruptTime()+2500000;
    InterlockedIncrement(&RefreshWaiters);BOOLEAN called=FALSE;
    NTSTATUS status=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMask,this,0,&called);
    // The hardware frame counter continues while Windows disables VSync
    // notifications for power saving. Never replace a queued flip before its
    // requested refresh interval, including a flip to the same allocation.
    while(status==STATUS_SUCCESS){
        KeClearEvent(&RefreshWake);
        ULONG frame=(Read(0,0x118)>>16)&63;
        if(((frame-first)&63)>=count)break;
        ULONGLONG now=KeQueryInterruptTime();if(now>=deadline){status=STATUS_IO_TIMEOUT;break;}
        LARGE_INTEGER timeout;timeout.QuadPart=-static_cast<LONGLONG>(deadline-now);
        NTSTATUS waited=KeWaitForSingleObject(&RefreshWake,Executive,KernelMode,FALSE,&timeout);
        if(waited!=STATUS_SUCCESS&&waited!=STATUS_TIMEOUT){status=waited;break;}
    }
    InterlockedDecrement(&RefreshWaiters);
    NTSTATUS restored=Dxgk->DxgkCbSynchronizeExecution(Dxgk->DeviceHandle,SynchronizeMask,this,0,&called);
    return status==STATUS_SUCCESS?restored:status;
}
ULONG PI5_DISPLAY_HW::Scanline() const {
    if(!Reg[0] || !Width)return Height;
    ULONG state=Read(0,0x118);
    if(((state>>13)&3)!=2)return Height;
    ULONG line=state&0x1fff,cob=Read(0,0x114);
    ULONG top=(cob>>16)&~3u,base=(cob&65535)&~3u;
    if(top<base)return Height;
    ULONG fifo=(top-base+4)/Width;
    if(!fifo || line<=fifo)return Height;
    return line-fifo-1;
}
VOID PI5_DISPLAY_HW::Signal(D3DKMDT_VIDEO_SIGNAL_INFO *signal) const {
    RtlZeroMemory(signal,sizeof(*signal));
    signal->VideoStandard=D3DKMDT_VSS_OTHER;
    signal->ActiveSize.cx=Width;signal->ActiveSize.cy=Height;
    signal->TotalSize.cx=HTotal;signal->TotalSize.cy=VTotal;
    signal->PixelRate=PixelRate;
    signal->HSyncFreq.Numerator=PixelRate;signal->HSyncFreq.Denominator=HTotal;
    signal->VSyncFreq.Numerator=PixelRate;signal->VSyncFreq.Denominator=HTotal*VTotal;
    signal->ScanLineOrdering=D3DDDI_VSSLO_PROGRESSIVE;
}
VOID PI5_DISPLAY_HW::Snapshot(PI5_DISPLAY_STATUS *out) {
    RtlZeroMemory(out,sizeof(*out));out->Magic=PI5_DISPLAY_QUERY_MAGIC;out->Version=PI5_DISPLAY_QUERY_VERSION;out->Debug=DBG;
    out->Owned=Owned;out->Fault=Fault;out->ClockMask=ClockMask;out->Irqs=Irqs;out->Notified=Notified;
    out->Width=Width;out->Height=Height;out->Pitch=Pitch;out->OldHead=OldHead;out->OwnHead=OwnHead;
    out->ControlCalls=ControlCalls;out->LastEnable=Enabled;
    out->Reserved=PixelRate;
    out->ScanoutAddress=Buffer?(ULONGLONG)Address().QuadPart:0;out->AllocationBytes=BufferBytes;
    out->GuardFailures=GuardFailures;out->Presents=Presents;out->SourceWidth=SourceWidth;out->SourceHeight=SourceHeight;
    out->Reserved2=static_cast<ULONG>(InterlockedCompareExchange(&IsrCalls,0,0));
    if(Buffer){
        for(ULONG i=0;i<4;++i){ULONG x=(i&1)?Width*3/4:Width/4,y=(i&2)?Height*3/4:Height/4;
            out->Samples[i]=static_cast<volatile ULONG*>(Framebuffer())[y*(Pitch/4)+x];}
    }
    if(Reg[0]){out->Head=Read(0,0x110);out->Active=Read(0,0x11c);out->Frame=Read(0,0x118);
        for(ULONG i=0;i<10;++i)out->List[i]=Read(0,0x4000+(OwnHead+static_cast<ULONG>(Front)*10+i)*4);}
    if(Reg[1]){out->Scanline=Scanline();out->PvControl=Read(1,0);out->PvVideo=Read(1,4);
        out->PvIntEnable=Read(1,0x24);out->PvIntStatus=Read(1,0x28);}
}
NTSTATUS PI5_DISPLAY_HW::Presented(ULONG width,ULONG height) {
    if(Fault || !Owned || !Buffer)return STATUS_DEVICE_HARDWARE_ERROR;
    KeMemoryBarrier();++Presents;SourceWidth=width;SourceHeight=height;
    if(Presents==1 || !(Presents&63)){
        for(ULONG i=0;i<1024;++i)if(Buffer[i]!=0x50494744 || Buffer[BufferBytes/4-1024+i]!=0x50494744)++GuardFailures;
        if(GuardFailures){Fault=1;return STATUS_DATA_ERROR;}
    }
    return STATUS_SUCCESS;
}

#if DBG
VOID PI5_DISPLAY_HW::SnapshotNative(PI5_NATIVE_STATUS *out) {
    RtlZeroMemory(out,sizeof(*out));out->Magic=PI5_NATIVE_QUERY_MAGIC;out->Version=1;
    out->Count=pi5display::NativeFieldCount;
    static_assert(pi5display::NativeFieldCount<=96,"diagnostic snapshot capacity");
    static const ULONG formatOffsets[]={0x148,0x14c,0x150,0x158,0x15c,0x160,0x164,0x168,0x16c,0x170};
    for(ULONG p=0;p<2;++p){
        auto hw=p==Port?this:Peer;if(!hw||!hw->Owned)continue;
        out->Active[p]=1;
        for(ULONG i=0;i<out->Count;++i){out->Current[p][i]=hw->ModeRead(i);out->Original[p][i]=hw->OriginalNative.Value[i];}
        auto*v=out->Output[p];
        v[0]=hw->Read(1,0x2c);v[1]=hw->Read(8,0x60+p*4);v[2]=hw->Read(0,0x118);v[3]=hw->Read(0,0x11c);
        v[4]=hw->Read(5,0);v[5]=hw->Read(5,0x1c);v[6]=hw->Read(0,0x114);v[7]=hw->Read(3,0);
        for(ULONG i=0;i<RTL_NUMBER_OF(formatOffsets);++i)out->Format[p][i]=hw->Read(6+p,formatOffsets[i]);
        auto &scan=out->Scanout[p];hw->Snapshot(&scan);
        ULONG head=scan.Active&0xfff;
        if(head!=hw->OwnHead&&head!=hw->OwnHead+10)continue;
        for(ULONG i=0;i<10;++i)scan.List[i]=hw->Read(0,0x4000+(head+i)*4);
        ULONGLONG address=(ULONGLONG(scan.List[5]&255)<<32)|scan.List[6];
        scan.ScanoutAddress=address;
        ULONGLONG bytes=ULONGLONG(hw->Pitch)*hw->Height;
        const volatile UCHAR *pixels=nullptr;
        ULONGLONG base=hw->Dma.QuadPart;
        if(address>=base+4096&&address-base<=hw->BufferBytes&&bytes<=hw->BufferBytes-(address-base))
            pixels=reinterpret_cast<const volatile UCHAR*>(hw->Buffer)+(address-base);
        else if(hw->DirectSegment&&address>=hw->DirectBase&&address-hw->DirectBase<=hw->DirectBytes&&bytes<=hw->DirectBytes-(address-hw->DirectBase))
            pixels=hw->DirectSegment+(address-hw->DirectBase);
        if(!pixels||scan.List[7]!=hw->Pitch||scan.List[3]!=((hw->Height-1)<<16|(hw->Width-1)))continue;
        out->GridValid[p]=1;
        for(ULONG y=0;y<54;++y)for(ULONG x=0;x<96;++x)
            out->Grid[p][y*96+x]=*reinterpret_cast<const volatile ULONG*>(pixels+SIZE_T(y*hw->Height/54)*hw->Pitch+(x*hw->Width/96)*4);
    }
    if(Reg[0])for(ULONG i=0;i<8;++i)out->HvsGlobal[i]=Read(0,0x20+i*4);
}
VOID PI5_DISPLAY_HW::SnapshotHdmi(PI5_HDMI_STATUS *out) {
    RtlZeroMemory(out,sizeof(*out));out->Magic=PI5_HDMI_QUERY_MAGIC;out->Version=1;out->Debug=DBG;
    static const ULONG hdmiOffsets[]={0,4,8,12,0xec,0xf0,0xf4,0xf8,0x1c8,0x1e4};
    for(ULONG port=0;port<2;++port){
        if(Reg[6+port]){out->Hotplug[port]=Read(6+port,0x1c8);
            for(ULONG i=0;i<RTL_NUMBER_OF(hdmiOffsets);++i)out->Hdmi[port][i]=Read(6+port,hdmiOffsets[i]);}
        ULONG pv=port==Port?1u:2u;
        if(Reg[pv])for(ULONG i=0;i<12;++i)out->PixelValve[port][i]=Read(pv,i*4);
    }
    if(Reg[0])for(ULONG channel=0;channel<3;++channel)
        for(ULONG i=0;i<8;++i)out->Hvs[channel][i]=READ_REGISTER_ULONG(reinterpret_cast<PULONG>(Reg[0]+0x100+channel*0x40+i*4));
}
#endif
