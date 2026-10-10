#pragma once

#include "pi5-display-abi.h"
#include "pi5-display-modes.h"
#include "pi5-native-mode.h"

class PI5_DISPLAY_HW {
public:
    PI5_DISPLAY_HW() {RtlZeroMemory(this,sizeof(*this));}
    static NTSTATUS FindPostPort(const DXGK_DEVICE_INFO *device,const DXGK_DISPLAY_INFORMATION *display,ULONG *port,ULONG revision);
    NTSTATUS Start(const DXGK_DEVICE_INFO *device,const DXGKRNL_INTERFACE *dxgk,
                   const DXGK_DISPLAY_INFORMATION *display,ULONG port,BOOLEAN ownsPost,ULONG revision);
    bool Active() const {return Owned!=0;}
    bool Connected(ULONG port) const {return port<2&&Reg[6+port]&&(Read(6+port,0x1c8)&1)!=0;}
    NTSTATUS SecondaryMode(DXGK_DISPLAY_INFORMATION *display) const;
    NTSTATUS RefreshMonitor(BOOLEAN requireEdid=TRUE);
    void Pair(PI5_DISPLAY_HW *peer){Peer=peer;}
    NTSTATUS Stop();
    BOOLEAN Interrupt();
    VOID RefreshDpc();
    NTSTATUS Control(DXGK_INTERRUPT_TYPE type,BOOLEAN enable);
    VOID Snapshot(PI5_DISPLAY_STATUS *out);
    ULONG Scanline() const;
    NTSTATUS WaitForRefresh(ULONG count);
    VOID Signal(D3DKMDT_VIDEO_SIGNAL_INFO *signal) const;
    const pi5display::Monitor &MonitorInfo() const {return Monitor;}
    VOID ModeSignal(ULONG index,D3DKMDT_VIDEO_SIGNAL_INFO *signal) const;
    LONG FindMode(const D3DKMDT_VIDEO_SIGNAL_INFO &signal) const;
    NTSTATUS SetMode(ULONG index,DXGK_DISPLAY_INFORMATION *display);
    NTSTATUS Descriptor(ULONG offset,ULONG bytes,PVOID buffer) const;
    PVOID Framebuffer() const {return FramebufferAt(static_cast<ULONG>(Front));}
    PVOID FramebufferAt(ULONG index) const {return index<2&&DirectPixels[index]?DirectPixels[index]:Buffer?const_cast<ULONG*>(Buffer+1024+SIZE_T(index)*FrameBytes/4):nullptr;}
    ULONG Stride() const {return Pitch;}
    ULONG HeightPixels() const {return Height;}
    ULONG FramebufferCount() const {return FullDisplay?2u:1u;}
    PVOID ScanoutMemory() const {return Buffer?const_cast<ULONG*>(Buffer+1024):nullptr;}
    ULONG ScanoutBytes() const {return FrameBytes*FramebufferCount();}
    PHYSICAL_ADDRESS Address() const {PHYSICAL_ADDRESS a=Dma;if(DirectPixels[Front])a.QuadPart=DirectAddresses[Front];else a.QuadPart+=4096+SIZE_T(Front)*FrameBytes;return a;}
    NTSTATUS AttachDirectSegment(PVOID memory,PHYSICAL_ADDRESS physical,ULONG bytes);
    // Called with the display interrupt synchronized. Only register writes;
    // all rendering to this linear allocation has already completed.
    NTSTATUS QueueDirectScanout(ULONGLONG offset);
    NTSTATUS PrepareScanout(BOOLEAN flip,ULONG *offset);
    using FLIP_COMPLETION=VOID(*)(PVOID,ULONG);
    NTSTATUS CommitScanout(ULONG offset,LONGLONG address,BOOLEAN flip,BOOLEAN noWait,ULONG interval,
                          FLIP_COMPLETION completion=nullptr,PVOID context=nullptr,ULONG fence=0);
    NTSTATUS DrainFlips() {return Owned&&FullDisplay?WaitForFlip():STATUS_SUCCESS;}
    // Caller holds the display interrupt synchronization lock.
    VOID PublishPendingFlip();
    VOID ClearFramebuffers();
    VOID Visibility(BOOLEAN visible);
#if DBG
    VOID SnapshotHdmi(PI5_HDMI_STATUS *out);
    VOID SnapshotNative(PI5_NATIVE_STATUS *out);
    VOID SnapshotFrames(PI5_FRAME_STATUS *out);
    NTSTATUS CheckFlips();
#endif
    const DXGK_DISPLAY_INFORMATION &PostDisplay() const {return OriginalDisplay;}
    NTSTATUS Presented(ULONG width,ULONG height);
    void FullWddm() {FullDisplay=TRUE;}
    void BlackedOut(ULONG reason) {BlackoutReason=reason;InterlockedIncrement(&Blackouts);}
    ULONG BlackoutCount() const {return static_cast<ULONG>(Blackouts);}
    ULONG LastBlackoutReason() const {return BlackoutReason;}
private:
    static BOOLEAN SynchronizeMask(PVOID context);
    static BOOLEAN SynchronizeFlip(PVOID context);
    static BOOLEAN SynchronizePublish(PVOID context);
    static BOOLEAN SynchronizeCancelCompletion(PVOID context);
    static BOOLEAN SynchronizeStop(PVOID context);
    static BOOLEAN SynchronizeMode(PVOID context);
    NTSTATUS OpenMonitor();
    NTSTATUS ProgramMode(const pi5display::Timing &timing);
    ULONG ModeRead(ULONG field) const;
    VOID ModeWrite(ULONG field,ULONG value);
    VOID SaveNative(pi5display::NativeState *state) const;
    NTSTATUS DisableNative();
    NTSTATUS ApplyNative(const pi5display::Timing &timing,const pi5display::NativeState &state);
    BOOLEAN WaitBits(ULONG unit,ULONG offset,ULONG mask,ULONG value,ULONG milliseconds);
    VOID ModeLists();
    VOID WriteCurrentLists();
    NTSTATUS WaitTiming(const pi5display::Timing &timing);
    PI5_DISPLAY_HW *Peer;
    pi5display::Monitor Monitor;
    pi5display::Timing OriginalTiming,CurrentTiming;
    HANDLE MailboxHandle;
    pi5display::NativeState OriginalNative;
    BOOLEAN TimingChanged;
    volatile LONG ModeChanging;
    ULONG PostList[0x800],PostListWords;
    NTSTATUS WaitForFlip();
    VOID RecordFrame();
    NTSTATUS Clock(ULONG code,ULONG id);
    BOOLEAN WaitHead(ULONG head);
    ULONG Read(ULONG unit,ULONG offset) const;
    VOID Write(ULONG unit,ULONG offset,ULONG value);
    PUCHAR Reg[12];
    const DXGKRNL_INTERFACE *Dxgk;
    HANDLE ClockHandle;
    ULONG Port,SiliconRevision,UpmDescriptor,Width,Height,Pitch,OldHead,OwnHead,SavedList[20],ClockMask,Fault;
    BOOLEAN PostOwner;
    ULONG HTotal,VTotal,PixelRate,HsmRate,BvbRate,NativeMaxClock;
    PDMA_ADAPTER Adapter;
    volatile ULONG *Buffer,*PostBuffer;
    PHYSICAL_ADDRESS Dma;
    PUCHAR DirectSegment;
    ULONGLONG DirectBase,DirectAddresses[2];
    ULONG DirectBytes;
    PVOID DirectPixels[2];
    DXGK_DISPLAY_INFORMATION OriginalDisplay;
    ULONG BufferBytes,FrameBytes,GuardFailures,Presents,SourceWidth,SourceHeight;
    volatile LONG Owned,Enabled,Irqs,Notified,ControlCalls,IsrCalls;
    volatile LONG RefreshWaiters;
    KEVENT RefreshWake;
    ULONG SavedL2Mask;
    BOOLEAN L2MaskOwned;
    BOOLEAN FullDisplay;
    BOOLEAN Visible;
    volatile LONG64 LogicalAddress;
    volatile LONG Front,Pending,PendingArmed;
    LONGLONG PendingAddress;
    FLIP_COMPLETION PendingCompletion;
    PVOID CompletionContext;
    ULONG CompletionFence;
#if DBG
    volatile LONG FrameGeneration;
    volatile LONG64 FrameSequence;
    PI5_FRAME_SAMPLE FrameSamples[512];
#endif
    volatile LONG Blackouts;
    ULONG BlackoutReason;
};
