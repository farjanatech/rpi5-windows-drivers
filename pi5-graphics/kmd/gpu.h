#pragma once
#include "../native/abi.h"
#include "pi5-v3d.h"
#include "../native/tile-cache.h"
#include "../native/encode.h"
#include "../native/validate.h"
#include "../display/pi5-display-abi.h"
struct GpuPath {
    HANDLE file;
    PUCHAR scratch,commands,textureData,blitCommand,codeStaging,uniformStaging;
    pi5::ProgramValidationCache *programCache;
    PI5_V3D_BUFFER code,uniform,cl,tile,state,target,source,texture,constants[3];
    PI5_V3D_BUFFER segment;
    PMDL segmentMdl;
    PI5_V3D_BUFFER scanout;
    PMDL scanoutMdl;
    PUCHAR segmentCpu;
    BOOLEAN session,opaqueLoads;
    pi5::Draw batchDraws[PI5_MAX_BATCH_DRAWS];
    ULONG batchCount,codeUsed,uniformUsed,constantUsed[3],viewCount;
    struct CodeEntry {ULONG offset,bytes;};
    CodeEntry batchCode[PI5_MAX_BATCH_DRAWS*3];
    ULONG batchCodeCount;
    uint32_t textureCursor,v3dRevision;
    pi5::TextureView views[PI5_MAX_BATCH_DRAWS*PI5_BINDINGS];
    void ResetBatch();
    bool FitsDraw(const Pi5DrawCommand &draw,const Pi5AllocationInfo *resources,void *const *cpu,const uint64_t *identities);
    NTSTATUS PrepareDraw(const Pi5DrawCommand &command,const Pi5AllocationInfo *resources,void *const *cpu,const uint64_t *identities);
    NTSTATUS SubmitBatch();

    pi5::TileCache textureCache;
    struct Shadow {uint64_t identity,source,age;Pi5AllocationInfo info;uint32_t arena,bytes,rows;bool dirty;};
    Shadow shadows[32];
    uint64_t shadowAge;
    uint32_t shadowCursor,activeShadow;
    bool sampledShadow;
    ULONG linearWidth,linearHeight,linearPitch;
    uint32_t FindShadow(uint64_t identity,uint64_t source);
    NTSTATUS AcquireShadow(uint64_t identity,uint64_t source,const Pi5AllocationInfo &info,uint32_t &index,bool preserve=true);
    NTSTATUS ResolveShadow(uint32_t index);
    NTSTATUS SyncShadows(uint64_t offset,uint64_t bytes,bool invalidate=false);
    void ForgetShadow(uint64_t identity);
    NTSTATUS Io(ULONG control,void *in,ULONG inBytes,void *out,ULONG outBytes);
    NTSTATUS Request(ULONG control);
    NTSTATUS Buffer(PI5_V3D_BUFFER *buffer,ULONG bytes,BOOLEAN writable);
    NTSTATUS Transfer(const PI5_V3D_BUFFER &buffer,void *data,ULONG bytes,BOOLEAN write,ULONG bufferOffset=0);
    NTSTATUS Region(PUCHAR destination,ULONG destinationPitch,const UCHAR *source,ULONG sourcePitch,ULONG width,ULONG height,ULONG pixelBytes);
    NTSTATUS BlitRegion(const Pi5RegionCommand &region,const Pi5AllocationInfo *resources,void *const *cpu,const uint64_t *identities);
    NTSTATUS Bindings(const Pi5DrawCommand &draw,const Pi5AllocationInfo *resources,void *const *cpu,const uint64_t *identities);
#if DBG
    volatile LONG ioActive,ioSequence;
    BOOLEAN profileCounters;
    NTSTATUS profileError;
    PI5_GPU_PROFILE_SAMPLE jobProfile,profileSamples[128];
    FAST_MUTEX jobLock;
    uint64_t jobSequence;
    uint64_t jobRenderTicks,jobTileTicks,jobCopyTicks;
    uint32_t jobSkippedLoads;
    PI5_JOB_SAMPLE jobSamples[512];
    void RecordJob(const Pi5CommandHeader *command,const Pi5AllocationInfo *resources,LONGLONG ticks);
    void SnapshotJobs(PI5_JOB_STATUS *status);
    void SnapshotProfile(PI5_GPU_PROFILE_STATUS *status);
    NTSTATUS CheckTiles();
#endif
    NTSTATUS Start(PUCHAR memory,ULONG bytes);
    NTSTATUS Recover();
    NTSTATUS AttachScanout(PVOID memory,ULONG bytes);
    NTSTATUS CopyScanout(ULONGLONG sourceOffset,ULONG pitch,ULONG height,ULONG destinationOffset=0);
    NTSTATUS CopySurface(ULONG sourceAddress,ULONG destinationAddress,ULONG width,ULONG height,ULONG sourcePitch,ULONG destinationPitch,ULONG sourceRows=0,ULONG destinationRows=0);
    NTSTATUS Stop();
    NTSTATUS Execute(const void *data,ULONG bytes,const Pi5AllocationInfo *resources,void *const *cpu,ULONG count,const uint64_t *identities);
};
