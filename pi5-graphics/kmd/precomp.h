#pragma once
#include "ddi-version.h"
#define D3D_UMD_INTERFACE_VERSION 0x3004
#define NOMINMAX
extern "C" {
#include <ntifs.h>
#include <windef.h>
#include <ntstrsafe.h>
#include <d3dkmddi.h>
#include <dispmprt.h>
}
#include "../native/abi.h"
#include "../native/encode.h"
#include "../native/validate.h"
#include "../display/pi5-display-abi.h"
#if DBG
void Pi5PerfRecord(Pi5PerfMetric metric,LONGLONG ticks,ULONGLONG bytes);
class Pi5PerfScope {
    Pi5PerfMetric metric;LONGLONG start;ULONGLONG bytes;
public:
    explicit Pi5PerfScope(Pi5PerfMetric m,ULONGLONG b=0):metric(m),start(KeQueryPerformanceCounter(nullptr).QuadPart),bytes(b){}
    ~Pi5PerfScope(){if(metric<Pi5PerfCount)Pi5PerfRecord(metric,KeQueryPerformanceCounter(nullptr).QuadPart-start,bytes);}
};
#else
class Pi5PerfScope {public:explicit Pi5PerfScope(Pi5PerfMetric,ULONGLONG=0){}};
#endif
#define PI5_TAG '5gpP'
NTSTATUS Pi5Trace(ULONG id,NTSTATUS status,ULONG a=0,ULONG b=0);
inline void *Allocate(SIZE_T bytes){return ExAllocatePool2(POOL_FLAG_NON_PAGED,bytes,PI5_TAG);}
inline void Free(void *p){if(p)ExFreePoolWithTag(p,PI5_TAG);}
// CPU-visible graphics memory can be mapped noncached on ARM64. Keep accesses
// scalar and naturally aligned rather than using an optimized SIMD memcpy.
inline void CopyGraphicsMemory(void *destination,const void *source,SIZE_T bytes){
    auto dst=static_cast<volatile UCHAR*>(destination);auto src=static_cast<const volatile UCHAR*>(source);
    if(!((reinterpret_cast<ULONG_PTR>(destination)|reinterpret_cast<ULONG_PTR>(source)|bytes)&3)){
        auto d=reinterpret_cast<volatile ULONG*>(dst);auto s=reinterpret_cast<const volatile ULONG*>(src);
        for(SIZE_T i=0;i<bytes/4;++i)d[i]=s[i];
    }else for(SIZE_T i=0;i<bytes;++i)dst[i]=src[i];
}
