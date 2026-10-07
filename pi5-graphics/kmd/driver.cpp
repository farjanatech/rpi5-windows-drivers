#include "precomp.h"
#include "gpu.h"
#include <acpiioct.h>

static NTSTATUS Pi5EvalAcpiInteger(PDEVICE_OBJECT device,const CHAR name[4],ULONG *value)
{
    if(!device||!value)return STATUS_INVALID_PARAMETER;
    ACPI_EVAL_INPUT_BUFFER input={};
    ACPI_EVAL_OUTPUT_BUFFER output={};
    KEVENT event;IO_STATUS_BLOCK iosb={};
    input.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName,name,4);
    KeInitializeEvent(&event,NotificationEvent,FALSE);
    PIRP irp=IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD,device,
        &input,sizeof(input),&output,sizeof(output),FALSE,&event,&iosb);
    if(!irp)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=IoCallDriver(device,irp);
    if(status==STATUS_PENDING){
        KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,nullptr);
        status=iosb.Status;
    }
    if(!NT_SUCCESS(status))return status;
    if(output.Signature!=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE||output.Count!=1||
       output.Argument[0].Type!=ACPI_METHOD_ARGUMENT_INTEGER||
       output.Argument[0].DataLength!=sizeof(ULONG))return STATUS_ACPI_INVALID_DATA;
    *value=output.Argument[0].Argument;
    return STATUS_SUCCESS;
}

#ifdef PI5_FULL_DISPLAY
#include "../display/bdd.hxx"
#if DBG
#include "debug-crash.inc"
#endif
#endif
struct Adapter;
struct Device {Adapter *adapter;HANDLE runtime;};
struct Context {Device *device;FAST_MUTEX validationLock;pi5::ProgramValidationCache validation;};
struct Allocation {Pi5AllocationInfo info;Adapter *adapter;ULONGLONG identity;};
struct OpenAllocation {Allocation *allocation;Device *device;BOOLEAN readOnly;};
struct Reference {Pi5AllocationInfo info;ULONGLONG offset,identity;};
struct Dma {
    ULONG magic,kind,bytes,count;
    PUCHAR data;
    Device *device;
    Reference refs[PI5_MAX_REFERENCES];
};
struct Submission {Dma dma;ULONG fence,start,end,flags;};
struct Adapter {
#ifdef PI5_FULL_DISPLAY
    BASIC_DISPLAY_DRIVER *display;
    KDPC displayWake;
    KEVENT hotplugStop;
    PETHREAD hotplugThread;
    KMUTEX primaryLock;
    Reference primary;
    Reference displayed;
    Reference secondaryPrimary,secondaryDisplayed;
    ULONG primaryFlags,primaryCalls,scanoutCopies;
    volatile LONG primaryPending,releasePost;
    BOOLEAN asyncFlips,mmioFlips;
#endif
    DXGKRNL_INTERFACE dxgk;
    GpuPath gpu;
    PUCHAR memory;
    PHYSICAL_ADDRESS physical;
    ULONG memoryBytes;
    volatile LONG64 nextAllocation;
    KSPIN_LOCK lock;
    KMUTEX gpuLock;
    KEVENT wake,idle;
    PETHREAD thread;
    Submission queue[64];
    ULONG head,tail;
    volatile LONG completed,submitted,preempt,preemptPending,stopping,busy,fault,controlSubmissions;
    volatile LONG recovering,recoveries;
#if DBG
    volatile LONG failNextDraw;
    volatile LONG workerStage,recoveryStage,powerState,powerAction;
#endif
    volatile LONG diagnostics[12],queueHighWater;
};
constexpr ULONG DmaMagic=0x4d443550;
#if DBG
static PETHREAD traceWorker;
static volatile LONG workerTrace;
struct WorkerStage {static void Set(Adapter*a,LONG value){InterlockedExchange(&a->workerStage,value);}};
struct PerfCounter {volatile LONG64 calls,ticks,bytes,maximum;};
static PerfCounter perfCounters[Pi5PerfCount];
void Pi5PerfRecord(Pi5PerfMetric metric,LONGLONG ticks,ULONGLONG bytes){
    auto&c=perfCounters[metric];InterlockedIncrement64(&c.calls);InterlockedAdd64(&c.ticks,ticks);InterlockedAdd64(&c.bytes,static_cast<LONG64>(bytes));
    LONG64 old=InterlockedCompareExchange64(&c.maximum,0,0);
    while(ticks>old){LONG64 found=InterlockedCompareExchange64(&c.maximum,ticks,old);if(found==old)break;old=found;}
}
#ifdef PI5_FULL_DISPLAY
static void PerfSnapshot(PI5_PERF_STATUS*p){
    LARGE_INTEGER frequency;KeQueryPerformanceCounter(&frequency);p->Frequency=frequency.QuadPart;
    for(unsigned i=0;i<Pi5PerfCount;++i){auto&c=perfCounters[i];auto&out=p->Counters[i];
        out.Calls=InterlockedCompareExchange64(&c.calls,0,0);out.Ticks=InterlockedCompareExchange64(&c.ticks,0,0);
        out.Bytes=InterlockedCompareExchange64(&c.bytes,0,0);out.MaxTicks=InterlockedCompareExchange64(&c.maximum,0,0);}
}
#endif
#endif
#if !DBG
struct WorkerStage {static void Set(Adapter*,LONG){}};
#endif
NTSTATUS Pi5Trace(ULONG id,NTSTATUS status,ULONG a,ULONG b){
    Pi5PerfScope timing(Pi5PerfTrace);
#if DBG
    struct TraceScope {bool worker;TraceScope(ULONG id):worker(PsGetCurrentThread()==traceWorker){if(worker)InterlockedExchange(&workerTrace,id);}~TraceScope(){if(worker)InterlockedExchange(&workerTrace,0);}} trace(id);
#endif
    struct Entry {ULONG id,status,a,b;};static Entry entries[512];static volatile LONG next=0;
    DbgPrintEx(DPFLTR_IHVVIDEO_ID,DPFLTR_INFO_LEVEL,"Pi5Render id=%lu status=%08lx a=%lu b=%lu\n",id,status,a,b);
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return status;ULONG index=static_cast<ULONG>(InterlockedIncrement(&next)-1);
    entries[index%512]={id,static_cast<ULONG>(status),a,b};
    // Preserve the recent ring and publish failures immediately. Rewriting the
    // whole volatile registry value for every draw/fence stalls the producer.
    static volatile LONG64 nextFlush=0;
    if(NT_SUCCESS(status)){
        LONG64 now=static_cast<LONG64>(KeQueryInterruptTime()),deadline=InterlockedCompareExchange64(&nextFlush,0,0);
        if(now<deadline||InterlockedCompareExchange64(&nextFlush,now+1000000,deadline)!=deadline)return status;
    }
    HANDLE key;OBJECT_ATTRIBUTES attrs;
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\HARDWARE\\Pi5RenderDiagnostics"),name=RTL_CONSTANT_STRING(L"Trace");
    InitializeObjectAttributes(&attrs,&path,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,nullptr,nullptr);
    if(NT_SUCCESS(ZwCreateKey(&key,KEY_SET_VALUE,&attrs,0,nullptr,REG_OPTION_VOLATILE,nullptr))){ZwSetValueKey(key,&name,0,REG_BINARY,entries,(index<512?index+1:512)*sizeof(Entry));ZwClose(key);}return status;
}
static bool Range(Adapter *a,ULONGLONG offset,SIZE_T bytes){return offset<=a->memoryBytes&&bytes<=a->memoryBytes-offset;}
// Serialize the worker's provider I/O and cache ownership with allocation
// destruction. KMUTEX preserves special kernel APC delivery for synchronous I/O.
static void LockGpu(Adapter*a){(void)KeWaitForSingleObject(&a->gpuLock,Executive,KernelMode,FALSE,nullptr);}
static void UnlockGpu(Adapter*a){(void)KeReleaseMutex(&a->gpuLock,FALSE);}
#ifdef PI5_FULL_DISPLAY
static BOOLEAN ConsumeFlipTrial(PCWSTR valueName,BOOLEAN defaultValue=FALSE){
#if DBG
    // DEBUG overrides are one-use and volatile. A reload or reboot returns
    // to the hardware-verified MMIO/opaque defaults; async copying stays off.
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\HARDWARE\\Pi5GraphicsTrial"),name;RtlInitUnicodeString(&name,valueName);
    OBJECT_ATTRIBUTES attrs;InitializeObjectAttributes(&attrs,&path,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,nullptr,nullptr);
    HANDLE key;NTSTATUS status=ZwOpenKey(&key,KEY_QUERY_VALUE|KEY_SET_VALUE,&attrs);if(status!=STATUS_SUCCESS)return defaultValue;
    alignas(8) UCHAR buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+sizeof(ULONG)]={};ULONG bytes=0,value=0;
    auto info=reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
    status=ZwQueryValueKey(key,&name,KeyValuePartialInformation,buffer,sizeof(buffer),&bytes);
    BOOLEAN enabled=defaultValue;
    if(status==STATUS_SUCCESS&&info->Type==REG_DWORD&&info->DataLength==sizeof(value)){
        RtlCopyMemory(&value,info->Data,sizeof(value));
        if(value<=1&&ZwDeleteValueKey(key,&name)==STATUS_SUCCESS)enabled=value!=0;
    }
    ZwClose(key);return enabled;
#else
    UNREFERENCED_PARAMETER(valueName);
    return defaultValue;
#endif
}
// A scanout can issue synchronous provider I/O. Preserve PASSIVE_LEVEL and
// special kernel APC delivery while protecting its allocation and buffer.
static void LockPrimary(Adapter*a){(void)KeWaitForSingleObject(&a->primaryLock,Executive,KernelMode,FALSE,nullptr);}
static void UnlockPrimary(Adapter*a){(void)KeReleaseMutex(&a->primaryLock,FALSE);}
static NTSTATUS Scanout(Adapter *a,const Reference&r,ULONG interval=0,bool noWait=false,ULONG fence=0,bool complete=false);
static NTSTATUS RefreshIfPrimary(Adapter *a,const Reference&r);
static NTSTATUS PresentWork(Adapter *a,const Submission&s);
static NTSTATUS PresentReferences(Device*d,Dma*out,const DXGK_ALLOCATIONLIST *list,UINT count,bool resident);
static VOID DisplayWake(KDPC*,PVOID context,PVOID,PVOID){auto a=static_cast<Adapter*>(context);if(!a->stopping)KeSetEvent(&a->wake,IO_NO_INCREMENT,FALSE);}
static NTSTATUS RefreshPrimary(Adapter *a){
    LockPrimary(a);NTSTATUS status=a->primary.offset==MAXULONGLONG?STATUS_SUCCESS:Scanout(a,a->primary);UnlockPrimary(a);return status;
}
static VOID HotplugWorker(PVOID context){
    auto a=static_cast<Adapter*>(context);auto display=a->display;
    bool observed[MAX_CHILDREN]={};ULONG stable[MAX_CHILDREN]={};
    for(ULONG port=0;port<MAX_CHILDREN;++port)observed[port]=display->m_Connected[port]!=0;
    LARGE_INTEGER timeout;timeout.QuadPart=-2500000;
    // HPD is readable without touching the link. Debounce it in a separate
    // passive worker; EDID/provider I/O must never run in the display ISR or
    // delay the render worker's scheduler fence notifications.
    while(KeWaitForSingleObject(&a->hotplugStop,Executive,KernelMode,FALSE,&timeout)==STATUS_TIMEOUT){
        if(a->stopping)break;
        if(a->fault||a->recovering)continue;
        for(ULONG port=0;port<MAX_CHILDREN;++port){
            bool connected=display->PostNative().Connected(port);
            if(port!=display->m_PostTarget&&!display->m_SecondHeadEnabled)continue;
            if(observed[port]!=connected){observed[port]=connected;stable[port]=1;continue;}
            if(stable[port]<2){++stable[port];continue;}
            if(connected==(InterlockedCompareExchange(&display->m_Connected[port],0,0)!=0))continue;
            LockGpu(a);NTSTATUS status=STATUS_SUCCESS;
            if(a->stopping||connected!=display->PostNative().Connected(port)){UnlockGpu(a);continue;}
            if(connected){
                bool starting=!display->Native(port).Active();
                if(starting)status=display->StartOutput(port);
                if(status==STATUS_SUCCESS&&starting&&a->mmioFlips)
                    status=display->Native(port).AttachDirectSegment(a->memory,a->physical,a->memoryBytes);
                if(status==STATUS_SUCCESS&&starting)
                    status=display->Native(port).Control(DXGK_INTERRUPT_CRTC_VSYNC,display->m_VsyncEnabled);
                if(status==STATUS_SUCCESS)status=display->Native(port).RefreshMonitor();
            }
            if(status==STATUS_SUCCESS&&connected==display->PostNative().Connected(port))
                InterlockedExchange(&display->m_Connected[port],connected);
            else if(status==STATUS_SUCCESS)status=STATUS_DEVICE_NOT_CONNECTED;
            UnlockGpu(a);
            if(status!=STATUS_SUCCESS){Pi5Trace(151,status,port,connected);stable[port]=0;continue;}
            // Do not hold the GPU mutex across an OS notification: the OS
            // can immediately request a descriptor or a new VidPn topology.
            DXGK_CHILD_STATUS child={};child.Type=StatusConnection;child.ChildUid=port;child.HotPlug.Connected=connected;
            status=a->dxgk.DxgkCbIndicateChildStatus(a->dxgk.DeviceHandle,&child);
            Pi5Trace(152,status,port,connected);
            if(status!=STATUS_SUCCESS)InterlockedExchange(&display->m_Connected[port],!connected);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS StartHotplug(Adapter *a){
    KeInitializeEvent(&a->hotplugStop,NotificationEvent,FALSE);
    OBJECT_ATTRIBUTES attrs;InitializeObjectAttributes(&attrs,nullptr,OBJ_KERNEL_HANDLE,nullptr,nullptr);HANDLE thread;
    NTSTATUS status=PsCreateSystemThread(&thread,THREAD_ALL_ACCESS,&attrs,nullptr,nullptr,HotplugWorker,a);
    if(status!=STATUS_SUCCESS)return status;
    status=ObReferenceObjectByHandle(thread,SYNCHRONIZE,*PsThreadType,KernelMode,reinterpret_cast<PVOID*>(&a->hotplugThread),nullptr);
    if(status!=STATUS_SUCCESS){KeSetEvent(&a->hotplugStop,IO_NO_INCREMENT,FALSE);ZwWaitForSingleObject(thread,FALSE,nullptr);}
    ZwClose(thread);return status;
}
#endif
struct Notice {Adapter *adapter;DXGKARGCB_NOTIFY_INTERRUPT_DATA data;};
static BOOLEAN Notify(PVOID data){auto n=static_cast<Notice*>(data);
#ifdef PI5_FULL_DISPLAY
    if(n->data.InterruptType==DXGK_INTERRUPT_DMA_COMPLETED)for(UINT i=0;i<MAX_VIEWS;++i)n->adapter->display->Native(i).PublishPendingFlip();
#endif
    n->adapter->dxgk.DxgkCbNotifyInterrupt(n->adapter->dxgk.DeviceHandle,&n->data);
    if(n->data.InterruptType==DXGK_INTERRUPT_DMA_COMPLETED)InterlockedExchange(&n->adapter->completed,static_cast<LONG>(n->data.DmaCompleted.SubmissionFenceId));
    return n->adapter->dxgk.DxgkCbQueueDpc(n->adapter->dxgk.DeviceHandle);}
#ifdef PI5_FULL_DISPLAY
static VOID CompleteFlipDma(PVOID context,ULONG fence){
    // Invoked by the display ISR after the new head latched, before VSync.
    Notice n={};n.adapter=static_cast<Adapter*>(context);n.data.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;
    n.data.DmaCompleted.SubmissionFenceId=fence;(void)Notify(&n);
}
#endif
static void Announce(Adapter *a,ULONG fence,NTSTATUS status,bool preempt=false){
    if(!preempt&&status!=STATUS_SUCCESS){
        // DMA_FAULTED is reserved for the OS. Leave this fence outstanding
        // and stop the queue so the scheduler can perform normal TDR.
        InterlockedCompareExchange(&a->fault,status,0);Pi5Trace(88,status,fence);return;
    }
    Notice n={};n.adapter=a;
    if(preempt){n.data.InterruptType=DXGK_INTERRUPT_DMA_PREEMPTED;n.data.DmaPreempted.PreemptionFenceId=fence;n.data.DmaPreempted.LastCompletedFenceId=static_cast<ULONG>(a->completed);}
    else if(status==STATUS_SUCCESS){n.data.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;n.data.DmaCompleted.SubmissionFenceId=fence;}
    BOOLEAN called=FALSE;NTSTATUS s=a->dxgk.DxgkCbSynchronizeExecution(a->dxgk.DeviceHandle,Notify,&n,0,&called);
    if(s!=STATUS_SUCCESS)InterlockedExchange(&a->fault,s);
}
static NTSTATUS Paging(Adapter *a,const DXGKARG_BUILDPAGINGBUFFER &p){
    Pi5PerfScope timing(Pi5PerfPaging);
    if(p.Operation==DXGK_OPERATION_DISCARD_CONTENT){
        // BuildPaging snapshots the allocation identity into this opaque
        // field. Discard retires cached aliases without writing their dirty
        // contents back into pages which VidMm is about to reuse.
        ULONGLONG identity=reinterpret_cast<ULONG_PTR>(p.DiscardContent.hAllocation);
        if(identity)a->gpu.ForgetShadow(identity);
        return Pi5Trace(52,STATUS_SUCCESS,static_cast<ULONG>(identity),p.DiscardContent.Flags.Value);
    }
    if(p.Operation==DXGK_OPERATION_FILL){
        if(p.Fill.Destination.SegmentId!=1||!Range(a,p.Fill.Destination.SegmentAddress.QuadPart,p.Fill.FillSize)||(p.Fill.FillSize&3))return STATUS_INVALID_PARAMETER;
        NTSTATUS status=a->gpu.SyncShadows(p.Fill.Destination.SegmentAddress.QuadPart,p.Fill.FillSize,true);if(status!=STATUS_SUCCESS)return status;
        a->gpu.textureCache.WriteSource(p.Fill.Destination.SegmentAddress.QuadPart,p.Fill.FillSize);
        auto out=reinterpret_cast<ULONG*>(a->memory+p.Fill.Destination.SegmentAddress.QuadPart);for(SIZE_T i=0;i<p.Fill.FillSize/4;++i)out[i]=p.Fill.FillPattern;return STATUS_SUCCESS;
    }
    if(p.Operation!=DXGK_OPERATION_TRANSFER||p.Transfer.Source.SegmentId>1||p.Transfer.Destination.SegmentId>1)return STATUS_NOT_SUPPORTED;
    PUCHAR src=nullptr,dst=nullptr;PMDL mapped[2]={};PVOID addresses[2]={};NTSTATUS result=STATUS_SUCCESS;
    for(unsigned i=0;i<2;++i){UINT segment=i?p.Transfer.Destination.SegmentId:p.Transfer.Source.SegmentId;
        LONGLONG offset=i?p.Transfer.Destination.SegmentAddress.QuadPart:p.Transfer.Source.SegmentAddress.QuadPart;PUCHAR cpu;
        if(segment){
            // TransferOffset is allocation-relative and applies only to segment
            // locations. MdlOffset below already selects the MDL's first page.
            if(!Range(a,offset,p.Transfer.TransferOffset)){result=STATUS_INVALID_PARAMETER;break;}
            ULONGLONG address=static_cast<ULONGLONG>(offset)+p.Transfer.TransferOffset;
            if(!Range(a,address,p.Transfer.TransferSize)){result=STATUS_INVALID_PARAMETER;break;}
            result=a->gpu.SyncShadows(address,p.Transfer.TransferSize,i!=0);if(result!=STATUS_SUCCESS)break;
            cpu=a->memory+address;
        }
        else{PMDL mdl=i?p.Transfer.Destination.pMdl:p.Transfer.Source.pMdl;SIZE_T byteOffset=SIZE_T(p.Transfer.MdlOffset)*PAGE_SIZE;
            if(!mdl||byteOffset>MmGetMdlByteCount(mdl)||p.Transfer.TransferSize>MmGetMdlByteCount(mdl)-byteOffset){result=STATUS_INVALID_PARAMETER;break;}
            bool hadMapping=(mdl->MdlFlags&(MDL_MAPPED_TO_SYSTEM_VA|MDL_SOURCE_IS_NONPAGED_POOL))!=0;cpu=static_cast<PUCHAR>(MmGetSystemAddressForMdlSafe(mdl,NormalPagePriority|MdlMappingNoExecute));
            if(!cpu){result=STATUS_INSUFFICIENT_RESOURCES;break;}if(!hadMapping){mapped[i]=mdl;addresses[i]=cpu;}cpu+=byteOffset;
        }if(i)dst=cpu;else src=cpu;
    }
    if(result==STATUS_SUCCESS){
        if(p.Transfer.Destination.SegmentId==1)a->gpu.textureCache.WriteSource(static_cast<ULONGLONG>(dst-a->memory),p.Transfer.TransferSize);
        CopyGraphicsMemory(dst,src,p.Transfer.TransferSize);
    }
    for(unsigned i=0;i<2;++i)if(mapped[i])MmUnmapLockedPages(addresses[i],mapped[i]);KeMemoryBarrier();return result;
}
#if DBG
#include "paging-check.inc"
#endif
static NTSTATUS Execute(Adapter *a,const Submission &s){
    // Context-switch and null-rendering packets retire in queue order, after
    // prior GPU work. They carry a real scheduler fence but no GPU payload.
    if(s.dma.kind==4){KeMemoryBarrier();return Pi5Trace(74,STATUS_SUCCESS,s.flags,static_cast<ULONG>(InterlockedIncrement(&a->controlSubmissions)));}
#ifdef PI5_FULL_DISPLAY
    if(s.dma.kind==3)return PresentWork(a,s);
#endif
    if(s.dma.kind==1){
#if DBG
        if(InterlockedExchange(&a->failNextDraw,0))return STATUS_INVALID_PARAMETER;
#endif
        if(s.start||s.end!=s.dma.bytes||s.dma.count<1||s.dma.count>PI5_MAX_REFERENCES)return STATUS_INVALID_PARAMETER;
        Pi5AllocationInfo infos[PI5_MAX_REFERENCES]={};void *cpu[PI5_MAX_REFERENCES]={};uint64_t identities[PI5_MAX_REFERENCES]={};for(ULONG i=0;i<s.dma.count;++i){identities[i]=s.dma.refs[i].identity;infos[i]=s.dma.refs[i].info;if(!Range(a,s.dma.refs[i].offset,infos[i].Bytes))return STATUS_INVALID_PARAMETER;cpu[i]=a->memory+s.dma.refs[i].offset;}
        NTSTATUS status=a->gpu.Execute(s.dma.data,s.dma.bytes,infos,cpu,s.dma.count,identities);
#ifdef PI5_FULL_DISPLAY
        if(status==STATUS_SUCCESS)status=RefreshIfPrimary(a,s.dma.refs[0]);
#endif
        return status;
    }
    if(s.dma.kind!=2||s.start%sizeof(DXGKARG_BUILDPAGINGBUFFER)||s.end%sizeof(DXGKARG_BUILDPAGINGBUFFER))return STATUS_INVALID_PARAMETER;
    for(ULONG at=s.start;at<s.end;at+=sizeof(DXGKARG_BUILDPAGINGBUFFER)){DXGKARG_BUILDPAGINGBUFFER p;RtlCopyMemory(&p,s.dma.data+at,sizeof(p));NTSTATUS result=Paging(a,p);if(result!=STATUS_SUCCESS)return result;}return STATUS_SUCCESS;
}
static VOID Worker(PVOID context){
    auto a=static_cast<Adapter*>(context);
#if DBG
    traceWorker=PsGetCurrentThread();
#endif
    // This thread services the GPU queue and retires scheduler fences. At the
    // default priority, a completed GPU interrupt can leave it ready but
    // unscheduled behind the clients waiting for those fences. It sleeps on
    // queue, GPU and refresh events, so give it the device-worker priority.
    KeSetPriorityThread(KeGetCurrentThread(),LOW_REALTIME_PRIORITY);
    for(;;){WorkerStage::Set(a,1);KeWaitForSingleObject(&a->wake,Executive,KernelMode,FALSE,nullptr);
        for(;;){WorkerStage::Set(a,2);Submission work={};bool have=false,preempt=false;ULONG preemptFence=0;KIRQL irql;KeAcquireSpinLock(&a->lock,&irql);
            if(!a->fault&&!a->recovering){
                if(a->preemptPending){a->head=a->tail;preemptFence=static_cast<ULONG>(a->preempt);a->preemptPending=0;preempt=true;}
                else if(a->head!=a->tail){work=a->queue[a->head];a->head=(a->head+1)%RTL_NUMBER_OF(a->queue);a->busy=1;KeClearEvent(&a->idle);have=true;}
            }
            KeReleaseSpinLock(&a->lock,irql);
            if(preempt){
                WorkerStage::Set(a,10);
#ifdef PI5_FULL_DISPLAY
                LockGpu(a);NTSTATUS drained=a->display->DrainFlips();UnlockGpu(a);
                if(drained!=STATUS_SUCCESS){InterlockedExchange(&a->fault,drained);Pi5Trace(81,drained);continue;}
#endif
                Announce(a,preemptFence,STATUS_SUCCESS,true);continue;}
            if(!have){
#ifdef PI5_FULL_DISPLAY
                if(!a->fault&&!a->recovering&&InterlockedExchange(&a->primaryPending,0)){WorkerStage::Set(a,9);LockGpu(a);NTSTATUS status=RefreshPrimary(a);UnlockGpu(a);if(status!=STATUS_SUCCESS)Pi5Trace(130,status);}
#endif
                WorkerStage::Set(a,8);KeSetEvent(&a->idle,IO_NO_INCREMENT,FALSE);break;}
            WorkerStage::Set(a,3);LockGpu(a);WorkerStage::Set(a,4);NTSTATUS result=a->fault?a->fault:Execute(a,work);WorkerStage::Set(a,5);UnlockGpu(a);a->diagnostics[8]=result;a->diagnostics[9]=work.dma.kind;a->diagnostics[10]=work.fence;a->diagnostics[11]=work.flags;WorkerStage::Set(a,6);Pi5Trace(70,result,work.dma.kind,work.fence);
            // A synchronous interval-one flip was already retired in its ISR.
            WorkerStage::Set(a,7);if(static_cast<ULONG>(InterlockedCompareExchange(&a->completed,0,0))!=work.fence)Announce(a,work.fence,result);
            InterlockedExchange(&a->busy,0);
        }if(a->stopping)break;
    }
#if DBG
    traceWorker=nullptr;
#endif
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS APIENTRY Add(DEVICE_OBJECT*pdo,PVOID *out){auto a=static_cast<Adapter*>(Allocate(sizeof(Adapter)));if(!a)return STATUS_INSUFFICIENT_RESOURCES;KeInitializeSpinLock(&a->lock);KeInitializeMutex(&a->gpuLock,0);KeInitializeEvent(&a->wake,SynchronizationEvent,FALSE);KeInitializeEvent(&a->idle,NotificationEvent,TRUE);
#ifdef PI5_FULL_DISPLAY
    PVOID display=nullptr;NTSTATUS status=BddDdiAddDevice(pdo,&display);if(status!=STATUS_SUCCESS){Free(a);return status;}a->display=static_cast<BASIC_DISPLAY_DRIVER*>(display);a->display->m_Native.FullWddm();KeInitializeMutex(&a->primaryLock,0);a->primary.offset=MAXULONGLONG;KeInitializeDpc(&a->displayWake,DisplayWake,a);
#endif
#ifdef PI5_FULL_DISPLAY
    ULONG silicon=MAXULONG;NTSTATUS siliconStatus=Pi5EvalAcpiInteger(pdo,"_HRV",&silicon);
    a->gpu.opaqueLoads=ConsumeFlipTrial(L"OpaqueLoads",TRUE);
    // C1 primary takeover itself is stable, but direct HVS scanout from the
    // GPU segment produced visible corruption on real C1 hardware with
    // Windows 22621. Use the existing copy-to-native-scanout path on C1.
    // D0 keeps Damian's original MMIO flip path unchanged.
    a->mmioFlips=(NT_SUCCESS(siliconStatus)&&silicon==0)?FALSE:ConsumeFlipTrial(L"MmioFlips",TRUE);
    a->asyncFlips=ConsumeFlipTrial(L"AsyncFlips");
    Pi5Trace(85,siliconStatus,a->asyncFlips|(a->mmioFlips?2u:0u),silicon);
    a->display->m_Secondary.FullWddm();a->display->m_SecondHeadEnabled=a->mmioFlips&&ConsumeFlipTrial(L"DualHead",TRUE);a->display->m_AutoHotplug=TRUE;
#if DBG
    a->gpu.profileCounters=ConsumeFlipTrial(L"ProfileCounters");
#endif
#endif
    *out=a;return Pi5Trace(1,STATUS_SUCCESS);}
static NTSTATUS APIENTRY Stop(PVOID context);
static NTSTATUS APIENTRY Start(PVOID context,DXGK_START_INFO*startInfo,DXGKRNL_INTERFACE *dxgk,ULONG *views,ULONG *children){
    auto a=static_cast<Adapter*>(context);a->dxgk=*dxgk;*views=*children=0;
    UNREFERENCED_PARAMETER(startInfo);
    a->head=a->tail=0;a->stopping=a->busy=a->fault=a->preemptPending=0;a->completed=a->submitted=a->preempt=a->controlSubmissions=0;KeClearEvent(&a->wake);KeSetEvent(&a->idle,IO_NO_INCREMENT,FALSE);
    a->recovering=a->recoveries=0;
#if DBG
    a->failNextDraw=0;
#endif
    RtlZeroMemory(const_cast<LONG*>(a->diagnostics),sizeof(a->diagnostics));a->queueHighWater=0;
    PHYSICAL_ADDRESS low={},high,boundary={};high.QuadPart=MAXLONGLONG;
#ifdef PI5_FULL_DISPLAY
    if(a->mmioFlips)high.QuadPart=0x9ffffffffull;
#endif
    for(ULONG bytes=256*1024*1024;bytes>=64*1024*1024;bytes/=2){a->memory=static_cast<PUCHAR>(MmAllocateContiguousMemorySpecifyCache(bytes,low,high,boundary,MmWriteCombined));if(a->memory){a->memoryBytes=bytes;break;}}
    if(!a->memory)return Pi5Trace(2,STATUS_INSUFFICIENT_RESOURCES);for(SIZE_T i=0;i<a->memoryBytes/4;++i)reinterpret_cast<volatile ULONG*>(a->memory)[i]=0;a->physical=MmGetPhysicalAddress(a->memory);
    NTSTATUS s=STATUS_SUCCESS;
#if DBG
    s=CheckPaging(a);if(s!=STATUS_SUCCESS){(void)Stop(a);return s;}
#endif
    s=a->gpu.Start(a->memory,a->memoryBytes);if(s!=STATUS_SUCCESS){(void)Stop(a);return Pi5Trace(3,s);}
    OBJECT_ATTRIBUTES attrs;InitializeObjectAttributes(&attrs,nullptr,OBJ_KERNEL_HANDLE,nullptr,nullptr);HANDLE thread;
    s=PsCreateSystemThread(&thread,THREAD_ALL_ACCESS,&attrs,nullptr,nullptr,Worker,a);if(s!=STATUS_SUCCESS){(void)Stop(a);return Pi5Trace(4,s);}
    s=ObReferenceObjectByHandle(thread,SYNCHRONIZE,*PsThreadType,KernelMode,reinterpret_cast<PVOID*>(&a->thread),nullptr);
    if(s!=STATUS_SUCCESS){InterlockedExchange(&a->stopping,1);KeSetEvent(&a->wake,IO_NO_INCREMENT,FALSE);ZwWaitForSingleObject(thread,FALSE,nullptr);ZwClose(thread);(void)Stop(a);return Pi5Trace(4,s);}ZwClose(thread);
#ifdef PI5_FULL_DISPLAY
    a->primary={};a->primary.offset=MAXULONGLONG;a->secondaryPrimary={};a->secondaryPrimary.offset=MAXULONGLONG;a->primaryPending=a->releasePost=0;s=a->display->StartDevice(startInfo,dxgk,views,children);if(s!=STATUS_SUCCESS){(void)Stop(a);return Pi5Trace(129,s);}
    {auto&native=a->display->PostNative();s=a->gpu.AttachScanout(native.ScanoutMemory(),native.ScanoutBytes());if(s!=STATUS_SUCCESS){(void)Stop(a);return Pi5Trace(78,s);}}
#if DBG
    // Exercise each active physical output before enabling OS notifications.
    for(ULONG port=0;port<MAX_CHILDREN;++port)if(a->display->Native(port).Active()){
        auto&native=a->display->Native(port);
        for(ULONG interval=1;interval<=4;++interval){s=native.WaitForRefresh(interval);Pi5Trace(79,s,interval,port);if(s!=STATUS_SUCCESS){(void)Stop(a);return s;}}
        s=native.CheckFlips();Pi5Trace(80,s,port);if(s!=STATUS_SUCCESS){(void)Stop(a);return s;}
    }
#endif
    a->gpu.linearWidth=a->gpu.linearHeight=a->gpu.linearPitch=0;
    if(a->mmioFlips){
        for(ULONG port=0;port<MAX_CHILDREN;++port)if(a->display->Native(port).Active()){
            s=a->display->Native(port).AttachDirectSegment(a->memory,a->physical,a->memoryBytes);
            if(s!=STATUS_SUCCESS){(void)Stop(a);return Pi5Trace(86,s,port);}
        }
        const auto&mode=a->display->GetCurrentMode(a->display->PostSource())->DispInfo;
        a->gpu.linearWidth=mode.Width;a->gpu.linearHeight=mode.Height;a->gpu.linearPitch=mode.Pitch;
    }
    s=StartHotplug(a);if(s!=STATUS_SUCCESS){(void)Stop(a);return Pi5Trace(150,s);}
#endif
    return Pi5Trace(5,s,a->memoryBytes,a->physical.HighPart);
}
static NTSTATUS APIENTRY Stop(PVOID context){
    auto a=static_cast<Adapter*>(context);InterlockedExchange(&a->stopping,1);
#ifdef PI5_FULL_DISPLAY
    if(a->hotplugThread){KeSetEvent(&a->hotplugStop,IO_NO_INCREMENT,FALSE);KeWaitForSingleObject(a->hotplugThread,Executive,KernelMode,FALSE,nullptr);ObDereferenceObject(a->hotplugThread);a->hotplugThread=nullptr;}
#if DBG
    ClearCrashDisplay();
#endif
    KeRemoveQueueDpc(&a->displayWake);KeFlushQueuedDpcs();
#endif
    if(a->thread){KeSetEvent(&a->wake,IO_NO_INCREMENT,FALSE);KeWaitForSingleObject(a->thread,Executive,KernelMode,FALSE,nullptr);ObDereferenceObject(a->thread);a->thread=nullptr;}
    NTSTATUS s=a->gpu.Stop();if(s!=STATUS_SUCCESS)return Pi5Trace(6,s);
#ifdef PI5_FULL_DISPLAY
    if(a->display){if(a->releasePost&&a->display->IsDriverActive())a->display->PostNative().ClearFramebuffers();
        s=a->display->StopDevice();if(s!=STATUS_SUCCESS)return Pi5Trace(131,s);}
#endif
    if(a->memory){MmFreeContiguousMemory(a->memory);a->memory=nullptr;}return Pi5Trace(6,STATUS_SUCCESS);
}
static NTSTATUS APIENTRY Remove(PVOID context){NTSTATUS s=Stop(context);if(s==STATUS_SUCCESS){
#ifdef PI5_FULL_DISPLAY
    auto a=static_cast<Adapter*>(context);if(a->display)BddDdiRemoveDevice(a->display);
#endif
    Free(context);}return s;}
static VOID APIENTRY Unload(){
#if DBG && defined(PI5_FULL_DISPLAY)
    UnregisterCrash();
#endif
}
static BOOLEAN APIENTRY Interrupt(PVOID context,ULONG){UNREFERENCED_PARAMETER(context);
#ifdef PI5_FULL_DISPLAY
    auto display=static_cast<Adapter*>(context)->display;BOOLEAN first=display->m_Native.Interrupt(),second=display->m_Secondary.Interrupt();return first||second;
#else
    return FALSE;
#endif
}
static VOID APIENTRY Dpc(PVOID context){auto a=static_cast<Adapter*>(context);
#ifdef PI5_FULL_DISPLAY
    a->display->m_Native.RefreshDpc();
    a->display->m_Secondary.RefreshDpc();
#endif
    a->dxgk.DxgkCbNotifyDpc(a->dxgk.DeviceHandle);}
static VOID APIENTRY ResetDevice(PVOID){}
static NTSTATUS APIENTRY Power(PVOID context,ULONG,DEVICE_POWER_STATE state,POWER_ACTION action){
#if DBG
    auto a=static_cast<Adapter*>(context);a->powerState=state;a->powerAction=action;
#else
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(action);
#endif
    return state==PowerDeviceD0?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
}
template<class F,ULONG Id> struct Unsupported;
template<class... A,ULONG Id>struct Unsupported<NTSTATUS(APIENTRY*)(A...),Id>{static NTSTATUS APIENTRY Call(A...){return Pi5Trace(Id,STATUS_NOT_SUPPORTED);}};
static NTSTATUS APIENTRY Query(HANDLE h,const DXGKARG_QUERYADAPTERINFO *q){
    auto a=static_cast<Adapter*>(h);Pi5Trace(20,STATUS_SUCCESS,q->Type,q->OutputDataSize);
    if(q->Type==DXGKQAITYPE_UMDRIVERPRIVATE){if(q->OutputDataSize!=sizeof(Pi5AdapterInfo))return STATUS_INVALID_PARAMETER;Pi5AdapterInfo info={PI5_UMD_MAGIC,PI5_UMD_ABI,71,0};
#ifdef PI5_FULL_DISPLAY
        info.Caps=PI5_ADAPTER_NATIVE_DISPLAY;
#endif
        *static_cast<Pi5AdapterInfo*>(q->pOutputData)=info;return STATUS_SUCCESS;}
    if(q->Type==DXGKQAITYPE_DRIVERCAPS){if(q->OutputDataSize<sizeof(DXGK_DRIVERCAPS))return STATUS_BUFFER_TOO_SMALL;auto c=static_cast<DXGK_DRIVERCAPS*>(q->pOutputData);RtlZeroMemory(c,sizeof(*c));c->HighestAcceptableAddress.QuadPart=MAXLONGLONG;c->WDDMVersion=DXGKDDI_WDDMv1_2;c->SupportNonVGA=TRUE;c->SchedulingCaps.MultiEngineAware=1;c->GpuEngineTopology.NbAsymetricProcessingNodes=1;c->PreemptionCaps.GraphicsPreemptionGranularity=D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;c->PreemptionCaps.ComputePreemptionGranularity=D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
#ifdef PI5_FULL_DISPLAY
        c->SchedulingCaps.VSyncPowerSaveAware=1;c->PresentationCaps.DriverSupportsCddDwmInterop=1;c->PresentationCaps.NoScreenToScreenBlt=1;c->PresentationCaps.NoOverlapScreenBlt=1;c->PresentationCaps.AlignmentShift=6;c->PresentationCaps.MaxTextureWidthShift=2;c->PresentationCaps.MaxTextureHeightShift=2;
        c->FlipCaps.FlipOnVSyncMmIo=a->mmioFlips;c->FlipCaps.FlipOnVSyncWithNoWait=a->asyncFlips||a->mmioFlips;
        c->MaxQueuedFlipOnVSync=a->asyncFlips||a->mmioFlips?1:0;
#endif
        return STATUS_SUCCESS;}
    if(q->Type==DXGKQAITYPE_QUERYSEGMENT3){if(q->OutputDataSize<sizeof(DXGK_QUERYSEGMENTOUT3))return STATUS_BUFFER_TOO_SMALL;auto o=static_cast<DXGK_QUERYSEGMENTOUT3*>(q->pOutputData);o->NbSegment=1;o->PagingBufferSegmentId=0;o->PagingBufferSize=65536;o->PagingBufferPrivateDataSize=sizeof(Dma);
        if(o->pSegmentDescriptor){auto s=o->pSegmentDescriptor;RtlZeroMemory(s,sizeof(*s));s->CpuTranslatedAddress=a->physical;s->Size=a->memoryBytes;s->Flags.CpuVisible=1;s->Flags.PopulatedFromSystemMemory=1;}return STATUS_SUCCESS;}
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS APIENTRY CreateDevice(HANDLE h,DXGKARG_CREATEDEVICE *a){auto d=static_cast<Device*>(Allocate(sizeof(Device)));if(!d)return STATUS_INSUFFICIENT_RESOURCES;d->adapter=static_cast<Adapter*>(h);d->runtime=a->hDevice;a->hDevice=d;return Pi5Trace(30,STATUS_SUCCESS);}
static NTSTATUS APIENTRY DestroyDevice(HANDLE h){Free(h);return Pi5Trace(31,STATUS_SUCCESS);}
static NTSTATUS APIENTRY CreateContext(HANDLE h,DXGKARG_CREATECONTEXT *a){if(a->NodeOrdinal||a->EngineAffinity!=1)return STATUS_INVALID_PARAMETER;auto c=static_cast<Context*>(Allocate(sizeof(Context)));if(!c)return STATUS_INSUFFICIENT_RESOURCES;c->device=static_cast<Device*>(h);ExInitializeFastMutex(&c->validationLock);a->hContext=c;a->ContextInfo={};a->ContextInfo.DmaBufferSize=PI5_UMD_COMMAND_BYTES;a->ContextInfo.DmaBufferPrivateDataSize=sizeof(Dma);a->ContextInfo.AllocationListSize=a->Flags.GdiContext?256:32;a->ContextInfo.PatchLocationListSize=32;return Pi5Trace(32,STATUS_SUCCESS,a->Flags.Value);}
static NTSTATUS APIENTRY DestroyContext(HANDLE h){Free(h);return Pi5Trace(33,STATUS_SUCCESS);}
static NTSTATUS APIENTRY CreateAllocation(HANDLE h,DXGKARG_CREATEALLOCATION *a){
    if(a->NumAllocations!=1||!a->pAllocationInfo)return STATUS_INVALID_PARAMETER;auto out=a->pAllocationInfo;Pi5AllocationInfo info;
    if(out->PrivateDriverDataSize!=sizeof(info)||!out->pPrivateDriverData)return Pi5Trace(40,STATUS_INVALID_PARAMETER,out->PrivateDriverDataSize);
    RtlCopyMemory(&info,out->pPrivateDriverData,sizeof(info));if(!pi5::ValidateAllocation(info))return Pi5Trace(40,STATUS_INVALID_PARAMETER,info.Width,info.Height);
    auto allocation=static_cast<Allocation*>(Allocate(sizeof(Allocation)));if(!allocation)return STATUS_INSUFFICIENT_RESOURCES;allocation->info=info;allocation->adapter=static_cast<Adapter*>(h);allocation->identity=static_cast<ULONGLONG>(InterlockedIncrement64(&allocation->adapter->nextAllocation));
    out->hAllocation=allocation;out->Alignment=PAGE_SIZE;out->Size=info.Bytes;out->PitchAlignedSize=0;out->HintedBank.Value=0;out->PreferredSegment.Value=0;out->PreferredSegment.SegmentId0=1;out->SupportedReadSegmentSet=out->SupportedWriteSegmentSet=1;out->EvictionSegmentSet=0;out->MaximumRenamingListLength=0;out->Flags.Value=0;out->Flags.CpuVisible=!Pi5CpuInvisible(info);out->Flags.Cached=0;out->pAllocationUsageHint=nullptr;out->AllocationPriority=D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    if(a->Flags.Resource&&!a->hResource){a->hResource=Allocate(sizeof(ULONG));if(!a->hResource){Free(allocation);return STATUS_INSUFFICIENT_RESOURCES;}}
    return Pi5Trace(40,STATUS_SUCCESS,info.Bytes,info.BindFlags);
}
static NTSTATUS APIENTRY DestroyAllocation(HANDLE h,const DXGKARG_DESTROYALLOCATION *a){
    auto adapter=static_cast<Adapter*>(h);LockGpu(adapter);
#ifdef PI5_FULL_DISPLAY
    NTSTATUS drained=adapter->display->DrainFlips();if(drained!=STATUS_SUCCESS){UnlockGpu(adapter);return drained;}
#endif
    for(UINT i=0;i<a->NumAllocations;++i){
    auto allocation=static_cast<Allocation*>(a->pAllocationList[i]);adapter->gpu.ForgetShadow(allocation->identity);
#ifdef PI5_FULL_DISPLAY
    LockPrimary(adapter);
    if(adapter->primary.identity==allocation->identity){adapter->primary={};adapter->primary.offset=MAXULONGLONG;InterlockedExchange(&adapter->primaryPending,0);}
    if(adapter->secondaryPrimary.identity==allocation->identity){adapter->secondaryPrimary={};adapter->secondaryPrimary.offset=MAXULONGLONG;}
    UnlockPrimary(adapter);
#endif
    Free(a->pAllocationList[i]);}UnlockGpu(adapter);if(a->Flags.DestroyResource)Free(a->hResource);return Pi5Trace(41,STATUS_SUCCESS,a->NumAllocations);}
static NTSTATUS APIENTRY Open(HANDLE h,const DXGKARG_OPENALLOCATION *a){
    auto d=static_cast<Device*>(h);for(UINT i=0;i<a->NumAllocations;++i){auto &out=a->pOpenAllocation[i];DXGKARGCB_GETHANDLEDATA get={};get.hObject=out.hAllocation;get.Type=DXGK_HANDLE_ALLOCATION;
        auto allocation=static_cast<Allocation*>(d->adapter->dxgk.DxgkCbGetHandleData(&get));if(!allocation||allocation->adapter!=d->adapter)return STATUS_INVALID_HANDLE;
        auto o=static_cast<OpenAllocation*>(Allocate(sizeof(OpenAllocation)));if(!o){for(UINT j=0;j<i;++j){Free(a->pOpenAllocation[j].hDeviceSpecificAllocation);a->pOpenAllocation[j].hDeviceSpecificAllocation=nullptr;}return STATUS_INSUFFICIENT_RESOURCES;}
        o->allocation=allocation;o->device=d;o->readOnly=a->Flags.ReadOnly;out.hDeviceSpecificAllocation=o;
    }return Pi5Trace(42,STATUS_SUCCESS,a->NumAllocations);
}
static NTSTATUS APIENTRY Close(HANDLE,const DXGKARG_CLOSEALLOCATION *a){for(UINT i=0;i<a->NumAllocations;++i)Free(a->pOpenHandleList[i]);return Pi5Trace(43,STATUS_SUCCESS,a->NumAllocations);}
static NTSTATUS APIENTRY Describe(HANDLE,DXGKARG_DESCRIBEALLOCATION *a){auto r=static_cast<Allocation*>(a->hAllocation);a->Width=r->info.Width;a->Height=r->info.Height;a->Format=r->info.Format==28?D3DDDIFMT_A8B8G8R8:r->info.Format==88?D3DDDIFMT_X8R8G8B8:D3DDDIFMT_A8R8G8B8;a->MultisampleMethod={1,0};a->RefreshRate={r->info.RefreshNumerator,r->info.RefreshDenominator};a->PrivateDriverFormatAttribute=0;return STATUS_SUCCESS;}
static NTSTATUS References(Device *d,Dma *out,const DXGK_ALLOCATIONLIST *list,UINT count,bool resident){
    if(count!=out->count)return STATUS_INVALID_PARAMETER;
    for(UINT i=0;i<count;++i){auto o=static_cast<OpenAllocation*>(list[i].hDeviceSpecificAllocation);
        if(!o||o->device!=d||!o->allocation||o->allocation->adapter!=d->adapter||(i==0&&o->readOnly)||!!list[i].WriteOperation!=(i==0)||list[i].SegmentId>1||(resident&&list[i].SegmentId!=1)||
           (list[i].SegmentId==1&&!Range(d->adapter,list[i].PhysicalAddress.QuadPart,o->allocation->info.Bytes)))return STATUS_INVALID_PARAMETER;
        out->refs[i].info=o->allocation->info;out->refs[i].offset=list[i].SegmentId==1?list[i].PhysicalAddress.QuadPart:MAXULONGLONG;out->refs[i].identity=o->allocation->identity;
    }
    for(UINT i=1;i<count;++i)if(out->refs[0].offset!=MAXULONGLONG&&out->refs[i].offset!=MAXULONGLONG&&out->refs[0].offset<out->refs[i].offset+out->refs[i].info.Bytes&&out->refs[i].offset<out->refs[0].offset+out->refs[0].info.Bytes)return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Render(HANDLE h,DXGKARG_RENDER *a){
    auto context=static_cast<Context*>(h);if(!a->pDmaBufferPrivateData||a->DmaBufferPrivateDataSize<sizeof(Dma)||a->CommandLength>PI5_UMD_COMMAND_BYTES||a->CommandLength>a->DmaSize||a->AllocationListSize<1||a->AllocationListSize>PI5_MAX_REFERENCES||a->PatchLocationListOutSize<a->AllocationListSize||a->MultipassOffset)return Pi5Trace(60,STATUS_INVALID_PARAMETER);
    // Capture the user command once; all later validation and execution use this kernel copy.
    __try{RtlCopyMemory(a->pDmaBuffer,a->pCommand,a->CommandLength);}__except(EXCEPTION_EXECUTE_HANDLER){return STATUS_INVALID_USER_BUFFER;}
    auto out=static_cast<Dma*>(a->pDmaBufferPrivateData);RtlZeroMemory(out,sizeof(*out));out->magic=DmaMagic;out->kind=1;out->bytes=a->CommandLength;out->data=static_cast<PUCHAR>(a->pDmaBuffer);out->count=a->AllocationListSize;out->device=context->device;
    NTSTATUS s=References(context->device,out,a->pAllocationList,a->AllocationListSize,false);if(s!=STATUS_SUCCESS)return Pi5Trace(61,s);
    Pi5AllocationInfo infos[PI5_MAX_REFERENCES]={};for(UINT i=0;i<out->count;++i)infos[i]=out->refs[i].info;
    // Cache only validation of this captured command's programs. Allocation
    // ranges and packet structure are still checked on every submission.
    ExAcquireFastMutex(&context->validationLock);
    bool valid=pi5::ValidateCommand(out->data,out->bytes,infos,out->count,&context->validation);
    ExReleaseFastMutex(&context->validationLock);
    if(!valid)return Pi5Trace(62,STATUS_INVALID_PARAMETER);
    for(UINT i=0;i<out->count;++i){auto &p=a->pPatchLocationListOut[i];RtlZeroMemory(&p,sizeof(p));p.AllocationIndex=i;p.PatchOffset=16+i*4;}
    a->pPatchLocationListOut+=out->count;a->pDmaBuffer=static_cast<PUCHAR>(a->pDmaBuffer)+a->CommandLength;return Pi5Trace(63,STATUS_SUCCESS,a->CommandLength);
}
static NTSTATUS PatchCommand(const DXGKARG_PATCH *a){
    if(a->DmaBufferSubmissionStartOffset==a->DmaBufferSubmissionEndOffset&&a->DmaBufferSubmissionEndOffset<=a->DmaBufferSize)return STATUS_SUCCESS;
    if(a->DmaBufferPrivateDataSize<sizeof(Dma)||!a->pDmaBufferPrivateData)return STATUS_INVALID_PARAMETER;auto d=static_cast<Dma*>(a->pDmaBufferPrivateData);if(d->magic!=DmaMagic)return STATUS_INVALID_PARAMETER;
    if(a->Flags.Paging)return STATUS_SUCCESS;auto context=static_cast<Context*>(a->hContext);if(!context||d->device!=context->device)return STATUS_INVALID_PARAMETER;
#ifdef PI5_FULL_DISPLAY
    if(d->kind==3)return Pi5Trace(136,PresentReferences(d->device,d,a->pAllocationList,a->AllocationListSize,true));
#endif
    return Pi5Trace(64,References(d->device,d,a->pAllocationList,a->AllocationListSize,true));
}
static NTSTATUS APIENTRY Patch(HANDLE h,const DXGKARG_PATCH*p){auto a=static_cast<Adapter*>(h);NTSTATUS result=PatchCommand(p);a->diagnostics[4]=result;a->diagnostics[5]=p->Flags.Value;a->diagnostics[6]=p->DmaBufferSubmissionStartOffset;a->diagnostics[7]=p->DmaBufferSubmissionEndOffset;KeMemoryBarrier();return result;}
static NTSTATUS APIENTRY BuildPaging(HANDLE h,DXGKARG_BUILDPAGINGBUFFER *a){
    if(a->Operation!=DXGK_OPERATION_FILL&&a->Operation!=DXGK_OPERATION_TRANSFER&&a->Operation!=DXGK_OPERATION_DISCARD_CONTENT)return Pi5Trace(50,STATUS_NOT_SUPPORTED,a->Operation);
    if(a->DmaSize<sizeof(*a))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if(a->DmaBufferPrivateDataSize<sizeof(Dma)||!a->pDmaBufferPrivateData)return STATUS_INVALID_PARAMETER;auto d=static_cast<Dma*>(a->pDmaBufferPrivateData);
    if(d->magic!=DmaMagic||d->kind!=2||static_cast<PUCHAR>(a->pDmaBuffer)!=d->data+d->bytes){RtlZeroMemory(d,sizeof(*d));d->magic=DmaMagic;d->kind=2;d->data=static_cast<PUCHAR>(a->pDmaBuffer);}
    DXGKARG_BUILDPAGINGBUFFER record=*a;
    if(a->Operation==DXGK_OPERATION_DISCARD_CONTENT&&a->DiscardContent.hAllocation){
        auto allocation=static_cast<Allocation*>(a->DiscardContent.hAllocation);
        if(allocation->adapter!=h)return STATUS_INVALID_PARAMETER;
        // Do not retain a driver allocation pointer in queued paging work.
        record.DiscardContent.hAllocation=reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(allocation->identity));
    }
    RtlCopyMemory(a->pDmaBuffer,&record,sizeof(record));d->bytes+=sizeof(*a);a->pDmaBuffer=static_cast<PUCHAR>(a->pDmaBuffer)+sizeof(*a);return Pi5Trace(51,STATUS_SUCCESS,a->Operation);
}
static NTSTATUS SubmitCommand(HANDLE h,const DXGKARG_SUBMITCOMMAND *s){
    auto a=static_cast<Adapter*>(h);if(a->stopping||s->NodeOrdinal||s->EngineOrdinal)return STATUS_DEVICE_NOT_READY;
    Dma command={};bool control=s->Flags.ContextSwitch||s->Flags.NullRendering||s->DmaBufferSubmissionStartOffset==s->DmaBufferSubmissionEndOffset;
    if(control){if(s->Flags.ContextSwitch&&s->DmaBufferSubmissionStartOffset!=s->DmaBufferSubmissionEndOffset)return STATUS_INVALID_PARAMETER;command.magic=DmaMagic;command.kind=4;}
    else{if(s->DmaBufferPrivateDataSize<sizeof(Dma)||!s->pDmaBufferPrivateData)return STATUS_INVALID_PARAMETER;
        command=*static_cast<const Dma*>(s->pDmaBufferPrivateData);if(command.magic!=DmaMagic||s->DmaBufferSubmissionStartOffset>s->DmaBufferSubmissionEndOffset||s->DmaBufferSubmissionEndOffset>command.bytes)return STATUS_INVALID_PARAMETER;}
    KIRQL irql;KeAcquireSpinLock(&a->lock,&irql);
    // An execution fault is recovered through TDR, never by returning an
    // error from SubmitCommand (which would itself force a system bugcheck).
    if(a->fault||a->recovering){a->submitted=static_cast<LONG>(s->SubmissionFenceId);KeReleaseSpinLock(&a->lock,irql);return STATUS_SUCCESS;}
    ULONG next=(a->tail+1)%RTL_NUMBER_OF(a->queue);if(next==a->head){KeReleaseSpinLock(&a->lock,irql);return STATUS_INSUFFICIENT_RESOURCES;}
    a->queue[a->tail]={command,s->SubmissionFenceId,s->DmaBufferSubmissionStartOffset,s->DmaBufferSubmissionEndOffset,s->Flags.Value};a->tail=next;a->submitted=static_cast<LONG>(s->SubmissionFenceId);ULONG depth=(a->tail+RTL_NUMBER_OF(a->queue)-a->head)%RTL_NUMBER_OF(a->queue);if(depth>static_cast<ULONG>(a->queueHighWater))a->queueHighWater=static_cast<LONG>(depth);KeClearEvent(&a->idle);KeReleaseSpinLock(&a->lock,irql);KeSetEvent(&a->wake,IO_NO_INCREMENT,FALSE);return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Submit(HANDLE h,const DXGKARG_SUBMITCOMMAND*s){auto a=static_cast<Adapter*>(h);NTSTATUS result=SubmitCommand(h,s);a->diagnostics[0]=result;a->diagnostics[1]=s->Flags.Value;a->diagnostics[2]=s->SubmissionFenceId;a->diagnostics[3]=s->DmaBufferPrivateDataSize;KeMemoryBarrier();return result;}
static NTSTATUS APIENTRY Preempt(HANDLE h,const DXGKARG_PREEMPTCOMMAND *p){if(p->NodeOrdinal||p->EngineOrdinal)return STATUS_INVALID_PARAMETER;auto a=static_cast<Adapter*>(h);KIRQL irql;KeAcquireSpinLock(&a->lock,&irql);a->preempt=static_cast<LONG>(p->PreemptionFenceId);a->preemptPending=1;KeReleaseSpinLock(&a->lock,irql);KeSetEvent(&a->wake,IO_NO_INCREMENT,FALSE);return STATUS_SUCCESS;}
struct FenceSnapshot {Adapter*adapter;ULONG value;};
static BOOLEAN ReadFence(PVOID data){auto snapshot=static_cast<FenceSnapshot*>(data);snapshot->value=static_cast<ULONG>(snapshot->adapter->completed);return TRUE;}
static NTSTATUS APIENTRY Fence(HANDLE h,DXGKARG_QUERYCURRENTFENCE *f){if(f->NodeOrdinal||f->EngineOrdinal)return STATUS_INVALID_PARAMETER;auto a=static_cast<Adapter*>(h);FenceSnapshot snapshot={a,0};BOOLEAN called=FALSE;NTSTATUS status=a->dxgk.DxgkCbSynchronizeExecution(a->dxgk.DeviceHandle,ReadFence,&snapshot,0,&called);if(status==STATUS_SUCCESS)f->CurrentFence=snapshot.value;return status;}
static NTSTATUS APIENTRY ResetTimeout(HANDLE h){
    PAGED_CODE();auto a=static_cast<Adapter*>(h);InterlockedExchange(&a->recovering,1);KeSetEvent(&a->wake,IO_NO_INCREMENT,FALSE);
#if DBG
    a->recoveryStage=1;
#endif
    LARGE_INTEGER timeout;timeout.QuadPart=-50000000;NTSTATUS s=KeWaitForSingleObject(&a->idle,Executive,KernelMode,FALSE,&timeout);
    if(s!=STATUS_SUCCESS)return Pi5Trace(89,STATUS_DEVICE_BUSY);
#if DBG
    a->recoveryStage=2;
#endif
    LockGpu(a);
#if DBG
    a->recoveryStage=3;
#endif
#ifdef PI5_FULL_DISPLAY
    s=a->display->DrainFlips();
#endif
    if(s==STATUS_SUCCESS){
#if DBG
        a->recoveryStage=4;
#endif
        s=a->gpu.Recover();}UnlockGpu(a);
    if(s!=STATUS_SUCCESS)return Pi5Trace(89,s);
    KIRQL irql;KeAcquireSpinLock(&a->lock,&irql);a->head=a->tail=0;a->preemptPending=0;KeReleaseSpinLock(&a->lock,irql);
#ifdef PI5_FULL_DISPLAY
    InterlockedExchange(&a->primaryPending,0);
#endif
    InterlockedExchange(&a->fault,0);InterlockedIncrement(&a->recoveries);
#if DBG
    a->recoveryStage=5;
#endif
    return Pi5Trace(89,STATUS_SUCCESS,a->submitted,a->completed);
}
static NTSTATUS APIENTRY RestartTimeout(HANDLE h){
    PAGED_CODE();auto a=static_cast<Adapter*>(h);if(a->fault)return STATUS_DEVICE_HARDWARE_ERROR;
    InterlockedExchange(&a->recovering,0);KeSetEvent(&a->wake,IO_NO_INCREMENT,FALSE);return Pi5Trace(90,STATUS_SUCCESS,a->recoveries);
}
static NTSTATUS APIENTRY DebugInfo(HANDLE h,const DXGKARG_COLLECTDBGINFO *i){auto a=static_cast<Adapter*>(h);ULONG data[]={DmaMagic,static_cast<ULONG>(a->submitted),static_cast<ULONG>(a->completed),static_cast<ULONG>(a->fault)};if(i->pBuffer&&i->BufferSize>=sizeof(data))RtlCopyMemory(i->pBuffer,data,sizeof(data));return STATUS_SUCCESS;}
static NTSTATUS APIENTRY SchedulerEscape(HANDLE h,const DXGKARG_ESCAPE*e){
    if(!e->pPrivateDriverData||e->PrivateDriverDataSize!=sizeof(Pi5SchedulerStatus))return STATUS_INVALID_PARAMETER;
    auto p=static_cast<Pi5SchedulerStatus*>(e->pPrivateDriverData);if(p->Magic!=PI5_SCHEDULER_QUERY_MAGIC||p->Version!=1)return STATUS_INVALID_PARAMETER;
    auto a=static_cast<Adapter*>(h);p->Submitted=static_cast<ULONG>(a->submitted);p->Completed=static_cast<ULONG>(a->completed);p->Fault=static_cast<ULONG>(a->fault);
    p->QueueHighWater=static_cast<ULONG>(a->queueHighWater);p->Head=a->head;p->Tail=a->tail;
    for(UINT i=0;i<RTL_NUMBER_OF(p->Diagnostics);++i)p->Diagnostics[i]=static_cast<ULONG>(a->diagnostics[i]);
    return STATUS_SUCCESS;
}
#ifdef PI5_FULL_DISPLAY
#include "display.inc"
#endif
extern "C" NTSTATUS DriverEntry(DRIVER_OBJECT *driver,UNICODE_STRING *registry){
    DRIVER_INITIALIZATION_DATA d={};d.Version=DXGKDDI_INTERFACE_VERSION_WIN8;
    d.DxgkDdiAddDevice=Add;d.DxgkDdiStartDevice=Start;d.DxgkDdiStopDevice=Stop;d.DxgkDdiRemoveDevice=Remove;d.DxgkDdiUnload=Unload;d.DxgkDdiResetDevice=ResetDevice;d.DxgkDdiInterruptRoutine=Interrupt;d.DxgkDdiDpcRoutine=Dpc;d.DxgkDdiSetPowerState=Power;d.DxgkDdiQueryAdapterInfo=Query;
    d.DxgkDdiCreateDevice=CreateDevice;d.DxgkDdiDestroyDevice=DestroyDevice;d.DxgkDdiCreateContext=CreateContext;d.DxgkDdiDestroyContext=DestroyContext;
    d.DxgkDdiCreateAllocation=CreateAllocation;d.DxgkDdiDestroyAllocation=DestroyAllocation;d.DxgkDdiOpenAllocation=Open;d.DxgkDdiCloseAllocation=Close;d.DxgkDdiDescribeAllocation=Describe;
    d.DxgkDdiRender=Render;d.DxgkDdiPatch=Patch;d.DxgkDdiBuildPagingBuffer=BuildPaging;d.DxgkDdiSubmitCommand=Submit;d.DxgkDdiPreemptCommand=Preempt;d.DxgkDdiQueryCurrentFence=Fence;d.DxgkDdiResetFromTimeout=ResetTimeout;d.DxgkDdiRestartFromTimeout=RestartTimeout;d.DxgkDdiCollectDbgInfo=DebugInfo;
#define STUB(field,id) d.field=Unsupported<decltype(d.field),id>::Call
    STUB(DxgkDdiDispatchIoRequest,100);STUB(DxgkDdiQueryChildRelations,101);STUB(DxgkDdiQueryChildStatus,102);STUB(DxgkDdiQueryDeviceDescriptor,103);STUB(DxgkDdiNotifyAcpiEvent,104);STUB(DxgkDdiQueryInterface,105);STUB(DxgkDdiPresent,106);STUB(DxgkDdiGetStandardAllocationDriverData,107);STUB(DxgkDdiControlInterrupt,109);
    d.DxgkDdiEscape=SchedulerEscape;
#ifdef PI5_FULL_DISPLAY
    DisplayFunctions(d);
#endif
    NTSTATUS status=DxgkInitialize(driver,registry,&d);
#if DBG && defined(PI5_FULL_DISPLAY)
    if(status==STATUS_SUCCESS){KeInitializeCallbackRecord(&crashRecord);crashRegistered=KeRegisterBugCheckReasonCallback(&crashRecord,CrashReason,KbCallbackTriageDumpData,crashComponent);Pi5Trace(84,crashRegistered?STATUS_SUCCESS:STATUS_UNSUCCESSFUL);}
#endif
    return Pi5Trace(0,status);
}
