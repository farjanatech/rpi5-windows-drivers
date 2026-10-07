/******************************Module*Header*******************************\
* Module Name: bdd.cxx
*
* Basic Display Driver functions implementation
*
*
* Copyright (c) 2010 Microsoft Corporation
\**************************************************************************/


#include "BDD.hxx"
#include <acpiioct.h>

static NTSTATUS EvalAcpiInteger(PDEVICE_OBJECT device,const CHAR name[4],ULONG *value)
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

NTSTATUS BddTrace(ULONG id, NTSTATUS status, ULONG a, ULONG b)
{
    struct ENTRY {ULONG Id,Status,A,B;};
    static ENTRY entries[128];
    static volatile LONG next=0;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return status;
    LONG n=InterlockedIncrement(&next)-1;
    if(n>=128)return status;
    entries[n]={id,(ULONG)status,a,b};
    HANDLE key; OBJECT_ATTRIBUTES attrs;
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\HARDWARE\\Pi5DisplayDiagnostics");
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"Trace");
    InitializeObjectAttributes(&attrs,&path,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    if(NT_SUCCESS(ZwCreateKey(&key,KEY_SET_VALUE,&attrs,0,NULL,REG_OPTION_VOLATILE,NULL))){
        (void)ZwSetValueKey(key,&name,0,REG_BINARY,entries,(n+1)*sizeof(ENTRY));ZwClose(key);
    }
    return status;
}

static void RecordStart(ULONG stage, NTSTATUS result, const DXGK_DISPLAY_INFORMATION *display)
{
    HANDLE key; OBJECT_ATTRIBUTES a;
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\HARDWARE\\Pi5DisplayDiagnostics");
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"Start");
    ULONG data[12]={stage,(ULONG)result,DBG};
    if(display){data[3]=display->Width;data[4]=display->Height;data[5]=display->Pitch;
        data[6]=display->ColorFormat;data[7]=display->PhysicAddress.LowPart;data[8]=display->PhysicAddress.HighPart;
        data[9]=display->TargetId;data[10]=display->AcpiId;}
    InitializeObjectAttributes(&a,&path,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    if(NT_SUCCESS(ZwCreateKey(&key,KEY_SET_VALUE,&a,0,NULL,REG_OPTION_VOLATILE,NULL))){
        (void)ZwSetValueKey(key,&name,0,REG_BINARY,data,sizeof(data));ZwClose(key);
    }
}

#pragma code_seg("PAGE")


BASIC_DISPLAY_DRIVER::BASIC_DISPLAY_DRIVER(_In_ DEVICE_OBJECT* pPhysicalDeviceObject) : m_pPhysicalDevice(pPhysicalDeviceObject),
                                                                                        m_MonitorPowerState(PowerDeviceD0),
                                                                                        m_AdapterPowerState(PowerDeviceD0)
{
    PAGED_CODE();
    *((UINT*)&m_Flags) = 0;
    m_Flags._LastFlag = TRUE;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_StartInfo, sizeof(m_StartInfo));
    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));


    for (UINT i=0;i<MAX_VIEWS;i++)
    {
        m_HardwareBlt[i].Initialize(this,i);
    }
}

BASIC_DISPLAY_DRIVER::~BASIC_DISPLAY_DRIVER()
{
    PAGED_CODE();


    CleanUp();
}


NTSTATUS BASIC_DISPLAY_DRIVER::StartDevice(_In_  DXGK_START_INFO*   pDxgkStartInfo,
                                           _In_  DXGKRNL_INTERFACE* pDxgkInterface,
                                           _Out_ ULONG*             pNumberOfViews,
                                           _Out_ ULONG*             pNumberOfChildren)
{
    PAGED_CODE();

    BDD_ASSERT(pDxgkStartInfo != NULL);
    BDD_ASSERT(pDxgkInterface != NULL);
    BDD_ASSERT(pNumberOfViews != NULL);
    BDD_ASSERT(pNumberOfChildren != NULL);

    RtlCopyMemory(&m_StartInfo, pDxgkStartInfo, sizeof(m_StartInfo));
    RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    m_CurrentModes[0].DispInfo.TargetId = D3DDDI_ID_UNINITIALIZED;

    RecordStart(1,STATUS_SUCCESS,NULL);
    // Get device information from OS.
    NTSTATUS Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%I64x",
                           Status);
        return Status;
    }

    RecordStart(2,Status,NULL);

    {
        ULONG revision=MAXULONG;
        NTSTATUS revisionStatus=EvalAcpiInteger(m_pPhysicalDevice,"_HRV",&revision);
        BddTrace(116,revisionStatus,revision,0);
        if(NT_SUCCESS(revisionStatus)){
            if(revision>1)return STATUS_NOT_SUPPORTED;
            m_SiliconRevision=revision;
        }
    }

    // Ignore return value, since it's not the end of the world if we failed to write these values to the registry
    RegisterHWInfo();

    // TODO: Uncomment the line below after updating the TODOs in the function CheckHardware
//    Status = CheckHardware();
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // This sample driver only uses the frame buffer of the POST device. DxgkCbAcquirePostDisplayOwnership
    // gives you the frame buffer address and ensures that no one else is drawing to it. Be sure to give it back!
    Status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle, &(m_CurrentModes[0].DispInfo));
    RecordStart(3,Status,&m_CurrentModes[0].DispInfo);
    if (!NT_SUCCESS(Status) || m_CurrentModes[0].DispInfo.Width == 0)
    {
        // The most likely cause of failure is that the driver is simply not running on a POST device, or we are running
        // after a pre-WDDM 1.2 driver. Since we can't draw anything, we should fail to start.
        return STATUS_UNSUCCESSFUL;
    }
    if(m_CurrentModes[0].DispInfo.Width>4096 || m_CurrentModes[0].DispInfo.Height>2160 ||
       !m_CurrentModes[0].DispInfo.Height || m_CurrentModes[0].DispInfo.Pitch < m_CurrentModes[0].DispInfo.Width*4 ||
       (m_CurrentModes[0].DispInfo.Pitch&3) || m_CurrentModes[0].DispInfo.Pitch>16384 ||
       ULONGLONG(m_CurrentModes[0].DispInfo.Pitch)*m_CurrentModes[0].DispInfo.Height>32u*1024*1024 ||
       (m_CurrentModes[0].DispInfo.ColorFormat != D3DDDIFMT_A8R8G8B8 &&
        m_CurrentModes[0].DispInfo.ColorFormat != D3DDDIFMT_X8R8G8B8)){
        RecordStart(4,STATUS_NOT_SUPPORTED,&m_CurrentModes[0].DispInfo);return STATUS_NOT_SUPPORTED;
    }
    Status=PI5_DISPLAY_HW::FindPostPort(&m_DeviceInfo,&m_CurrentModes[0].DispInfo,&m_PostTarget,m_SiliconRevision);
    if(Status!=STATUS_SUCCESS || m_PostTarget>=MAX_CHILDREN){RecordStart(7,Status,&m_CurrentModes[0].DispInfo);return Status==STATUS_SUCCESS?STATUS_NOT_SUPPORTED:Status;}
    auto& boot=m_CurrentModes[0];
    Status=PostNative().Start(&m_DeviceInfo,&m_DxgkInterface,&boot.DispInfo,m_PostTarget,TRUE);
    if(Status!=STATUS_SUCCESS){RecordStart(7,Status,&boot.DispInfo);return Status;}
    boot.FrameBuffer.Ptr=PostNative().Framebuffer();
    boot.DispInfo.PhysicAddress=PostNative().Address();
    boot.DispInfo.Pitch=PostNative().Stride();
    boot.Flags.DoNotMapOrUnmap=TRUE;
    for(ULONG source=0;source<MAX_VIEWS;++source)m_CurrentModes[source].DispInfo.TargetId=source^m_PostTarget;
    if(m_SecondHeadEnabled&&PostNative().Connected(1-m_PostTarget)){
        Status=StartOutput(1-m_PostTarget);
        if(Status!=STATUS_SUCCESS){RecordStart(8,Status,&m_CurrentModes[SourceForTarget(1-m_PostTarget)].DispInfo);return Status;}
    }
    m_Native.Pair(&m_Secondary);m_Secondary.Pair(&m_Native);
    for(ULONG port=0;port<MAX_CHILDREN;++port)
        InterlockedExchange(&m_Connected[port],Native(port).Active()&&PostNative().Connected(port));
    RecordStart(5,STATUS_SUCCESS,&boot.DispInfo);
    m_Flags.DriverStarted = TRUE;
    *pNumberOfViews = MAX_VIEWS;
    *pNumberOfChildren = MAX_CHILDREN;


   return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::StartOutput(ULONG target)
{
    PAGED_CODE();
    if(!m_SecondHeadEnabled||target>=MAX_CHILDREN||target==m_PostTarget)return STATUS_NOT_SUPPORTED;
    if(Native(target).Active())return STATUS_SUCCESS;
    auto& mode=m_CurrentModes[SourceForTarget(target)];
    NTSTATUS status=PostNative().SecondaryMode(&mode.DispInfo);
    if(status==STATUS_SUCCESS)status=Native(target).Start(&m_DeviceInfo,&m_DxgkInterface,&mode.DispInfo,target,FALSE);
    if(status!=STATUS_SUCCESS)return status;
    mode.FrameBuffer.Ptr=Native(target).Framebuffer();
    mode.DispInfo.PhysicAddress=Native(target).Address();
    mode.DispInfo.Pitch=Native(target).Stride();
    mode.Flags.DoNotMapOrUnmap=TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::StopDevice(VOID)
{
    PAGED_CODE();

    NTSTATUS status=CleanUp();
    if(status!=STATUS_SUCCESS)return status;

    m_Flags.DriverStarted = FALSE;

    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::CleanUp()
{
    PAGED_CODE();

    NTSTATUS status=Native(1-m_PostTarget).Stop();
    if(status!=STATUS_SUCCESS)return status;
    status=PostNative().Stop();
    if(status!=STATUS_SUCCESS)return status;

    for (UINT Source = 0; Source < MAX_VIEWS; ++Source)
    {
        if (m_CurrentModes[Source].FrameBuffer.Ptr)
        {
            if(!m_CurrentModes[Source].Flags.DoNotMapOrUnmap)
                UnmapFrameBuffer(m_CurrentModes[Source].FrameBuffer.Ptr, m_CurrentModes[Source].DispInfo.Height * m_CurrentModes[Source].DispInfo.Pitch);
            m_CurrentModes[Source].FrameBuffer.Ptr = NULL;
            m_CurrentModes[Source].Flags.FrameBufferIsActive = FALSE;
        }
    }
    return STATUS_SUCCESS;
}


NTSTATUS BASIC_DISPLAY_DRIVER::DispatchIoRequest(_In_  ULONG                 VidPnSourceId,
                                                 _In_  VIDEO_REQUEST_PACKET* pVideoRequestPacket)
{
    PAGED_CODE();

    BDD_ASSERT(pVideoRequestPacket != NULL);
    BDD_ASSERT(VidPnSourceId < MAX_VIEWS);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS BASIC_DISPLAY_DRIVER::SetPowerState(_In_  ULONG              HardwareUid,
                                             _In_  DEVICE_POWER_STATE DevicePowerState,
                                             _In_  POWER_ACTION       ActionType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ActionType);

    BDD_ASSERT((HardwareUid < MAX_CHILDREN) || (HardwareUid == DISPLAY_ADAPTER_HW_ID));

    if (HardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        if (DevicePowerState == PowerDeviceD0)
        {

            // When returning from D3 the device visibility defined to be off for all targets
            if (m_AdapterPowerState == PowerDeviceD3)
            {
                DXGKARG_SETVIDPNSOURCEVISIBILITY Visibility;
                Visibility.VidPnSourceId = D3DDDI_ID_ALL;
                Visibility.Visible = FALSE;
                SetVidPnSourceVisibility(&Visibility);
            }
        }

        // Store new adapter power state
        m_AdapterPowerState = DevicePowerState;

        // There is nothing to do to specifically power up/down the display adapter
        return STATUS_SUCCESS;
    }
    else
    {
        // TODO: This is where the specified monitor should be powered up/down
        NOTHING;
        return STATUS_SUCCESS;
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR* pChildRelations,
                                                   _In_                             ULONG                  ChildRelationsSize)
{
    PAGED_CODE();

    BDD_ASSERT(pChildRelations != NULL);

    // The last DXGK_CHILD_DESCRIPTOR in the array of pChildRelations must remain zeroed out, so we subtract this from the count
    ULONG ChildRelationsCount = (ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR)) - 1;
    BDD_ASSERT(ChildRelationsCount <= MAX_CHILDREN);

    for (UINT ChildIndex = 0; ChildIndex < ChildRelationsCount; ++ChildIndex)
    {
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = m_AutoHotplug?HpdAwarenessInterruptible:HpdAwarenessPolled;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = m_CurrentModes[0].Flags.IsInternal ? D3DKMDT_VOT_INTERNAL : D3DKMDT_VOT_HDMI;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        // TODO: Replace 0 with the actual ACPI ID of the child device, if available
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }

    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS* pChildStatus,
                                                _In_    BOOLEAN            NonDestructiveOnly)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    BDD_ASSERT(pChildStatus != NULL);
    BDD_ASSERT(pChildStatus->ChildUid < MAX_CHILDREN);

    switch (pChildStatus->Type)
    {
        case StatusConnection:
        {
            if(pChildStatus->ChildUid>=MAX_CHILDREN)return STATUS_INVALID_PARAMETER;
            pChildStatus->HotPlug.Connected = IsDriverActive()&&(m_AutoHotplug?
                InterlockedCompareExchange(&m_Connected[pChildStatus->ChildUid],0,0)!=0:
                Native(pChildStatus->ChildUid).Active()&&PostNative().Connected(pChildStatus->ChildUid));
            return STATUS_SUCCESS;
        }

        case StatusRotation:
        {
            // D3DKMDT_MOA_NONE was reported, so this should never be called
            BDD_LOG_ERROR0("Child status being queried for StatusRotation even though D3DKMDT_MOA_NONE was reported");
            return STATUS_INVALID_PARAMETER;
        }

        default:
        {
            BDD_LOG_WARNING1("Unknown pChildStatus->Type (0x%I64x) requested.", pChildStatus->Type);
            return STATUS_NOT_SUPPORTED;
        }
    }
}

// EDID retrieval
NTSTATUS BASIC_DISPLAY_DRIVER::QueryDeviceDescriptor(_In_    ULONG                   ChildUid,
                                                     _Inout_ DXGK_DEVICE_DESCRIPTOR* pDeviceDescriptor)
{
    PAGED_CODE();

    if(!pDeviceDescriptor||ChildUid>=MAX_CHILDREN)return STATUS_INVALID_PARAMETER;
    return Native(ChildUid).Descriptor(pDeviceDescriptor->DescriptorOffset,pDeviceDescriptor->DescriptorLength,pDeviceDescriptor->DescriptorBuffer);
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO* pQueryAdapterInfo)
{
    PAGED_CODE();

    BDD_ASSERT(pQueryAdapterInfo != NULL);

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_DRIVERCAPS:
        {
            if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
            {
                BDD_LOG_ERROR2("pQueryAdapterInfo->OutputDataSize (0x%I64x) is smaller than sizeof(DXGK_DRIVERCAPS) (0x%I64x)", pQueryAdapterInfo->OutputDataSize, sizeof(DXGK_DRIVERCAPS));
                return STATUS_BUFFER_TOO_SMALL;
            }

            DXGK_DRIVERCAPS* pDriverCaps = (DXGK_DRIVERCAPS*)pQueryAdapterInfo->pOutputData;

            // Nearly all fields must be initialized to zero, so zero out to start and then change those that are non-zero.
            // Fields are zero since BDD is Display-Only and therefore does not support any of the render related fields.
            // It also doesn't support hardware interrupts, gamma ramps, etc.
            RtlZeroMemory(pDriverCaps, sizeof(DXGK_DRIVERCAPS));

            pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_2;
            pDriverCaps->HighestAcceptableAddress.QuadPart = -1;

            pDriverCaps->SupportNonVGA = TRUE;
            pDriverCaps->SupportSmoothRotation = FALSE;
            pDriverCaps->SchedulingCaps.VSyncPowerSaveAware = TRUE;

            return STATUS_SUCCESS;
        }

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
        case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        {
            DXGK_DISPLAY_DRIVERCAPS_EXTENSION* pDriverDisplayCaps;

            if (pQueryAdapterInfo->OutputDataSize < sizeof(*pDriverDisplayCaps))
            {
                BDD_LOG_ERROR2("pQueryAdapterInfo->OutputDataSize (0x%I64x) is smaller than sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION) (0x%I64x)",
                               pQueryAdapterInfo->OutputDataSize,
                               sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION));

                return STATUS_INVALID_PARAMETER;
            }

            pDriverDisplayCaps = (DXGK_DISPLAY_DRIVERCAPS_EXTENSION*)pQueryAdapterInfo->pOutputData;

            // Reset all caps values
            RtlZeroMemory(pDriverDisplayCaps, pQueryAdapterInfo->OutputDataSize);

            // We claim to support virtual display mode.
            pDriverDisplayCaps->VirtualModeSupport = 1;

            return STATUS_SUCCESS;
        }

#endif
        default:
        {
            // BDD does not need to support any other adapter information types
            BDD_LOG_WARNING1("Unknown QueryAdapterInfo Type (0x%I64x) requested", pQueryAdapterInfo->Type);
            return STATUS_NOT_SUPPORTED;
        }
    }
}


NTSTATUS BASIC_DISPLAY_DRIVER::CheckHardware()
{
    PAGED_CODE();

    NTSTATUS Status;
    ULONG VendorID;
    ULONG DeviceID;

// TODO: If developing a driver for PCI based hardware, then use the second method to retrieve Vendor/Device IDs.
// If developing for non-PCI based hardware (i.e. ACPI based hardware), use the first method to retrieve the IDs.
#if 1 // ACPI-based device

    // Get the Vendor & Device IDs on non-PCI system
    ACPI_EVAL_INPUT_BUFFER_COMPLEX AcpiInputBuffer = {0};
    AcpiInputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    AcpiInputBuffer.MethodNameAsUlong = ACPI_METHOD_HARDWARE_ID;
    AcpiInputBuffer.Size = 0;
    AcpiInputBuffer.ArgumentCount = 0;

    BYTE OutputBuffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 0x10];
    RtlZeroMemory(OutputBuffer, sizeof(OutputBuffer));
    ACPI_EVAL_OUTPUT_BUFFER* pAcpiOutputBuffer = reinterpret_cast<ACPI_EVAL_OUTPUT_BUFFER*>(&OutputBuffer);

    Status = m_DxgkInterface.DxgkCbEvalAcpiMethod(m_DxgkInterface.DeviceHandle,
                                                  DISPLAY_ADAPTER_HW_ID,
                                                  &AcpiInputBuffer,
                                                  sizeof(AcpiInputBuffer),
                                                  pAcpiOutputBuffer,
                                                  sizeof(OutputBuffer));
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR1("DxgkCbReadDeviceSpace failed to get hardware IDs with status 0x%I64x", Status);
        return Status;
    }

    VendorID = ((ULONG*)(pAcpiOutputBuffer->Argument[0].Data))[0];
    DeviceID = ((ULONG*)(pAcpiOutputBuffer->Argument[0].Data))[1];

#else // PCI-based device

    // Get the Vendor & Device IDs on PCI system
    PCI_COMMON_HEADER Header = {0};
    ULONG BytesRead;

    Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   &Header,
                                                   0,
                                                   sizeof(Header),
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR1("DxgkCbReadDeviceSpace failed with status 0x%I64x", Status);
        return Status;
    }

    VendorID = Header.VendorID;
    DeviceID = Header.DeviceID;

#endif

    // TODO: Replace 0x1414 with your Vendor ID
    if (VendorID == 0x1414)
    {
        switch (DeviceID)
        {
            // TODO: Replace the case statements below with the Device IDs supported by this driver
            case 0x0000:
            case 0xFFFF: return STATUS_SUCCESS;
        }
    }

    return STATUS_GRAPHICS_DRIVER_MISMATCH;
}

// Even though Sample Basic Display Driver does not support hardware cursors, and reports such
// in QueryAdapterInfo. This function can still be called to set the pointer to not visible
NTSTATUS BASIC_DISPLAY_DRIVER::SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION* pSetPointerPosition)
{
    PAGED_CODE();

    BDD_ASSERT(pSetPointerPosition != NULL);
    BDD_ASSERT(pSetPointerPosition->VidPnSourceId < MAX_VIEWS);

    if (!(pSetPointerPosition->Flags.Visible))
    {
        return STATUS_SUCCESS;
    }
    else
    {
        BDD_LOG_ASSERTION0("SetPointerPosition should never be called to set the pointer to visible since BDD doesn't support hardware cursors.");
        return STATUS_UNSUCCESSFUL;
    }
}

// Basic Sample Display Driver does not support hardware cursors, and reports such
// in QueryAdapterInfo. Therefore this function should never be called.
NTSTATUS BASIC_DISPLAY_DRIVER::SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE* pSetPointerShape)
{
    PAGED_CODE();

    BDD_ASSERT(pSetPointerShape != NULL);
    BDD_LOG_ASSERTION0("SetPointerShape should never be called since BDD doesn't support hardware cursors.");

    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS BASIC_DISPLAY_DRIVER::PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY* pPresentDisplayOnly)
{
    PAGED_CODE();

    BDD_ASSERT(pPresentDisplayOnly != NULL);
    BDD_ASSERT(pPresentDisplayOnly->VidPnSourceId < MAX_VIEWS);

    if (pPresentDisplayOnly->BytesPerPixel < MIN_BYTES_PER_PIXEL_REPORTED)
    {
        // Only >=32bpp modes are reported, therefore this Present should never pass anything less than 4 bytes per pixel
        BDD_LOG_ERROR1("pPresentDisplayOnly->BytesPerPixel is 0x%I64x, which is lower than the allowed.", pPresentDisplayOnly->BytesPerPixel);
        return STATUS_INVALID_PARAMETER;
    }

    // If it is in monitor off state or source is not supposed to be visible, don't present anything to the screen
    if ((m_MonitorPowerState > PowerDeviceD0) ||
        (m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].Flags.SourceNotVisible))
    {
        return STATUS_SUCCESS;
    }

    // Present is only valid if the target is actively connected to this source
    if (m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].Flags.FrameBufferIsActive)
    {

        // If actual pixels are coming through, will need to completely zero out physical address next time in BlackOutScreen
        m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].ZeroedOutStart.QuadPart = 0;
        m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].ZeroedOutEnd.QuadPart = 0;


        D3DKMDT_VIDPN_PRESENT_PATH_ROTATION RotationNeededByFb = pPresentDisplayOnly->Flags.Rotate ?
                                                                 m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].Rotation :
                                                                 D3DKMDT_VPPR_IDENTITY;
            BYTE* pDst = (BYTE*)m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].FrameBuffer.Ptr;
            UINT DstBitPerPixel = BPPFromPixelFormat(m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].DispInfo.ColorFormat);
            if (m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].Scaling == D3DKMDT_VPPS_CENTERED)
            {
                UINT CenterShift = (m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].DispInfo.Height -
                    m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].SrcModeHeight)*m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].DispInfo.Pitch;
                CenterShift += (m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].DispInfo.Width -
                    m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].SrcModeWidth)*DstBitPerPixel/8;
                pDst += (int)CenterShift/2;
            }
            RECT full={0,0,(LONG)m_CurrentModes[0].SrcModeWidth,(LONG)m_CurrentModes[0].SrcModeHeight};
            BOOLEAN repaint=m_CurrentModes[0].Flags.FullscreenPresent;
            NTSTATUS Status = m_HardwareBlt[pPresentDisplayOnly->VidPnSourceId].ExecutePresentDisplayOnly(pDst,
                                                                    DstBitPerPixel,
                                                                    (BYTE*)pPresentDisplayOnly->pSource,
                                                                    pPresentDisplayOnly->BytesPerPixel,
                                                                    pPresentDisplayOnly->Pitch,
                                                                    repaint?0:pPresentDisplayOnly->NumMoves,
                                                                    repaint?NULL:pPresentDisplayOnly->pMoves,
                                                                    repaint?1:pPresentDisplayOnly->NumDirtyRects,
                                                                    repaint?&full:pPresentDisplayOnly->pDirtyRect,
                                                                    RotationNeededByFb);
            if(Status==STATUS_SUCCESS)Status=SourceNative(pPresentDisplayOnly->VidPnSourceId).Presented(m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].SrcModeWidth,m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].SrcModeHeight);
            if(Status==STATUS_SUCCESS)m_CurrentModes[0].Flags.FullscreenPresent=FALSE;
            static volatile LONG Presents=0;
            LONG Count=InterlockedIncrement(&Presents);
            if(Count==1 || !(Count & 1023) || !NT_SUCCESS(Status))
                RecordStart(6,Status,&m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].DispInfo);
            return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::StopDeviceAndReleasePostDisplayOwnership(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                        _Out_ DXGK_DISPLAY_INFORMATION*      pDisplayInfo)
{
    PAGED_CODE();

    BDD_ASSERT(TargetId < MAX_CHILDREN);


    D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = FindSourceForTarget(TargetId, TRUE);

    // In case BDD is the next driver to run, the monitor should not be off, since
    // this could cause the BIOS to hang when the EDID is retrieved on Start.
    if (m_MonitorPowerState > PowerDeviceD0)
    {
        SetPowerState(TargetId, PowerDeviceD0, PowerActionNone);
    }

    // The driver has to black out the display and ensure it is visible when releasing ownership
    BlackOutScreen(SourceId);

    *pDisplayInfo = PostNative().PostDisplay();
    pDisplayInfo->TargetId=m_PostTarget;

    return StopDevice();
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY* pVidPnHWCaps)
{
    PAGED_CODE();

    BDD_ASSERT(pVidPnHWCaps != NULL);
    BDD_ASSERT(pVidPnHWCaps->SourceId < MAX_VIEWS);
    BDD_ASSERT(pVidPnHWCaps->TargetId < MAX_CHILDREN);

    pVidPnHWCaps->VidPnHWCaps.DriverRotation             = 0; // BDD does rotation in software
    pVidPnHWCaps->VidPnHWCaps.DriverScaling              = 0; // BDD does not support scaling
    pVidPnHWCaps->VidPnHWCaps.DriverCloning              = 0; // BDD does not support clone
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert         = 1; // BDD does color conversions in software
    pVidPnHWCaps->VidPnHWCaps.DriverLinkedAdapaterOutput = 0; // BDD does not support linked adapters
    pVidPnHWCaps->VidPnHWCaps.DriverRemoteDisplay        = 0; // BDD does not support remote displays

    return STATUS_SUCCESS;
}

VOID BASIC_DISPLAY_DRIVER::BlackOutScreen(D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId,ULONG Reason)
{
    PAGED_CODE();


    SourceNative(SourceId).BlackedOut(Reason);
#ifdef PI5_FULL_DISPLAY
    // Full WDDM primaries are live Windows allocations, not private DOD
    // buffers. Mode/visibility changes must not erase reusable desktop data.
    // Reason 0 remains the crash/system-display path, which draws directly.
    if(Reason==1){SourceNative(SourceId).Visibility(FALSE);return;}
    if(Reason==2)return;
#endif
    CURRENT_BDD_MODE *mode=&m_CurrentModes[SourceId];
    if(mode->Flags.FrameBufferIsActive && mode->FrameBuffer.Ptr){
        SourceNative(SourceId).ClearFramebuffers();
    }
    mode->ZeroedOutStart=mode->DispInfo.PhysicAddress;
    mode->ZeroedOutEnd.QuadPart=mode->ZeroedOutStart.QuadPart+mode->DispInfo.Height*mode->DispInfo.Pitch;

}

NTSTATUS BASIC_DISPLAY_DRIVER::WriteHWInfoStr(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue)
{
    PAGED_CODE();

    NTSTATUS Status;
    ANSI_STRING AnsiStrValue;
    UNICODE_STRING UnicodeStrValue;
    UNICODE_STRING UnicodeStrValueName;

    // ZwSetValueKey wants the ValueName as a UNICODE_STRING
    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    // REG_SZ is for WCHARs, there is no equivalent for CHARs
    // Use the ansi/unicode conversion functions to get from PSTR to PWSTR
    RtlInitAnsiString(&AnsiStrValue, pszValue);
    Status = RtlAnsiStringToUnicodeString(&UnicodeStrValue, &AnsiStrValue, TRUE);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR1("RtlAnsiStringToUnicodeString failed with Status: 0x%I64x", Status);
        return Status;
    }

    // Write the value to the registry
    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &UnicodeStrValueName,
                           0,
                           REG_SZ,
                           UnicodeStrValue.Buffer,
                           UnicodeStrValue.MaximumLength);

    // Free the earlier allocated unicode string
    RtlFreeUnicodeString(&UnicodeStrValue);

    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR1("ZwSetValueKey failed with Status: 0x%I64x", Status);
    }

    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RegisterHWInfo()
{
    PAGED_CODE();

    NTSTATUS Status;

    // TODO: Replace these strings with proper information
    PCSTR StrHWInfoChipType = "Replace with the chip name";
    PCSTR StrHWInfoDacType = "Replace with the DAC name or identifier (ID)";
    PCSTR StrHWInfoAdapterString = "Replace with the name of the adapter";
    PCSTR StrHWInfoBiosString = "Replace with information about the BIOS";

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR2("IoOpenDeviceRegistryKey failed for PDO: 0x%I64x, Status: 0x%I64x", m_pPhysicalDevice, Status);
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.ChipType", StrHWInfoChipType);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.DacType", StrHWInfoDacType);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.AdapterString", StrHWInfoAdapterString);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.BiosString", StrHWInfoBiosString);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // MemorySize is a ULONG, unlike the others which are all strings
    UNICODE_STRING ValueNameMemorySize;
    RtlInitUnicodeString(&ValueNameMemorySize, L"HardwareInformation.MemorySize");
    DWORD MemorySize = 0; // BDD has no access to video memory
    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &ValueNameMemorySize,
                           0,
                           REG_DWORD,
                           &MemorySize,
                           sizeof(MemorySize));
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR1("ZwSetValueKey for MemorySize failed with Status: 0x%I64x", Status);
        return Status;
    }

    return Status;
}

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()
D3DDDI_VIDEO_PRESENT_SOURCE_ID BASIC_DISPLAY_DRIVER::FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, BOOLEAN DefaultToZero)
{
    BDD_ASSERT_CHK(TargetId < MAX_CHILDREN);
    for(UINT source=0;source<MAX_VIEWS;++source)
        if(m_CurrentModes[source].DispInfo.TargetId==TargetId&&m_CurrentModes[source].FrameBuffer.Ptr)return source;
    return DefaultToZero ? 0 : D3DDDI_ID_UNINITIALIZED;
}

VOID BASIC_DISPLAY_DRIVER::DpcRoutine(VOID)
{
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
}

BOOLEAN BASIC_DISPLAY_DRIVER::InterruptRoutine(_In_  ULONG MessageNumber)
{
    UNREFERENCED_PARAMETER(MessageNumber);

    BOOLEAN first=m_Native.Interrupt(),second=m_Secondary.Interrupt();return first||second;
}

VOID BASIC_DISPLAY_DRIVER::ResetDevice(VOID)
{
}

// Must be Non-Paged, as it sets up the display for a bugcheck
NTSTATUS BASIC_DISPLAY_DRIVER::SystemDisplayEnable(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   _In_  PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                                   _Out_ UINT* pWidth,
                                                   _Out_ UINT* pHeight,
                                                   _Out_ D3DDDIFORMAT* pColorFormat)
{
    UNREFERENCED_PARAMETER(Flags);

    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;

    BDD_ASSERT((TargetId < MAX_CHILDREN) || (TargetId == D3DDDI_ID_UNINITIALIZED));

    // Find the frame buffer for displaying the bugcheck, if it was successfully mapped
    if (TargetId == D3DDDI_ID_UNINITIALIZED)
    {
        for (UINT SourceIdx = 0; SourceIdx < MAX_VIEWS; ++SourceIdx)
        {
            if (m_CurrentModes[SourceIdx].FrameBuffer.Ptr != NULL)
            {
                m_SystemDisplaySourceId = SourceIdx;
                break;
            }
        }
    }
    else
    {
        m_SystemDisplaySourceId = FindSourceForTarget(TargetId, FALSE);
    }

    if (m_SystemDisplaySourceId == D3DDDI_ID_UNINITIALIZED)
    {
        {
            return STATUS_UNSUCCESSFUL;
        }
    }

    if ((m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE90) ||
        (m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE270))
    {
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }
    else
    {
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }

    *pColorFormat = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat;
#ifdef PI5_FULL_DISPLAY
    SourceNative(m_SystemDisplaySourceId).Visibility(TRUE);
#endif


    return STATUS_SUCCESS;
}

// Must be Non-Paged, as it is called to display the bugcheck screen
VOID BASIC_DISPLAY_DRIVER::SystemDisplayWrite(_In_reads_bytes_(SourceHeight * SourceStride) VOID* pSource,
                                              _In_ UINT SourceWidth,
                                              _In_ UINT SourceHeight,
                                              _In_ UINT SourceStride,
                                              _In_ INT PositionX,
                                              _In_ INT PositionY)
{

    // Rect will be Offset by PositionX/Y in the src to reset it back to 0
    RECT Rect;
    Rect.left = PositionX;
    Rect.top = PositionY;
    Rect.right =  Rect.left + SourceWidth;
    Rect.bottom = Rect.top + SourceHeight;

    // Set up destination blt info
    BLT_INFO DstBltInfo;
    DstBltInfo.pBits = m_CurrentModes[m_SystemDisplaySourceId].FrameBuffer.Ptr;
    DstBltInfo.Pitch = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Pitch;
    DstBltInfo.BitsPerPel = BPPFromPixelFormat(m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat);
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = m_CurrentModes[m_SystemDisplaySourceId].Rotation;
    DstBltInfo.Width = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
    DstBltInfo.Height = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;

    // Set up source blt info
    BLT_INFO SrcBltInfo;
    SrcBltInfo.pBits = pSource;
    SrcBltInfo.Pitch = SourceStride;
    SrcBltInfo.BitsPerPel = 32;

    SrcBltInfo.Offset.x = -PositionX;
    SrcBltInfo.Offset.y = -PositionY;
    SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    SrcBltInfo.Width = SourceWidth;
    SrcBltInfo.Height = SourceHeight;

    // A pending HVS flip may still latch while the bugcheck path paints.
    // Keep both resident buffers identical without waiting at high IRQL.
    for(ULONG frame=0;frame<SourceNative(m_SystemDisplaySourceId).FramebufferCount();++frame){
    DstBltInfo.pBits=SourceNative(m_SystemDisplaySourceId).FramebufferAt(frame);
    BltBits(&DstBltInfo,
            &SrcBltInfo,
            1, // NumRects
            &Rect);
    }
}

#pragma code_seg(pop) // End Non-Paged Code
