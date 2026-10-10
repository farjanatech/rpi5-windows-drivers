// SPDX-License-Identifier: BSD-2-Clause-Patent
#define PI5_EXPERIMENTAL_WDDM20 1
#include "../kmd/ddi-version.h"
#define NOMINMAX
#include <ntddk.h>
#include <windef.h>
#include <d3dkmddi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "../kmd/wddm20.h"

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    DXGK_DRIVERCAPS driverCaps = {};
    pi5::SetPhysicalSchedulerCaps(driverCaps);
    // WDDM 2 uses the Windows 8+ preemption policy. Keep granularity honest:
    // this serialized engine can stop between DMA buffers, not mid-shader.
    CHECK(driverCaps.WDDMVersion == DXGKDDI_WDDMv2);
    CHECK(driverCaps.SchedulingCaps.MultiEngineAware && driverCaps.SchedulingCaps.PreemptionAware);
    CHECK(driverCaps.PreemptionCaps.GraphicsPreemptionGranularity == D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY);
    CHECK(driverCaps.PreemptionCaps.ComputePreemptionGranularity == D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY);
    CHECK(!driverCaps.SchedulingCaps.NoDmaPatching && !driverCaps.SchedulingCaps.CancelCommandAware);
    CHECK(!driverCaps.MemoryManagementCaps.Value && !driverCaps.SupportPerEngineTDR);

    DXGK_QUERYPHYSICALADAPTERCAPSIN adapterIn = {};
    alignas(DXGK_PHYSICALADAPTERCAPS) unsigned char adapterStorage[sizeof(DXGK_PHYSICALADAPTERCAPS) + 16];
    DXGKARG_QUERYADAPTERINFO adapterQuery = {};
    adapterQuery.pInputData = &adapterIn; adapterQuery.InputDataSize = sizeof(adapterIn);
    adapterQuery.pOutputData = adapterStorage;
    const UINT adapterMinimum = FIELD_OFFSET(DXGK_PHYSICALADAPTERCAPS, Flags) + sizeof(DXGK_PHYSICALADAPTERFLAGS);
    CHECK(adapterMinimum == 20); // ARM64/x64 WDDM 2.0 physical-adapter wire size
    HANDLE runtime = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0x1234));
    for (UINT outputSize : {adapterMinimum, static_cast<UINT>(sizeof(DXGK_PHYSICALADAPTERCAPS)), static_cast<UINT>(sizeof(adapterStorage))}) {
        std::memset(adapterStorage, 0xa5, sizeof(adapterStorage)); adapterQuery.OutputDataSize = outputSize;
        CHECK(pi5::QueryPhysicalAdapter(adapterQuery, runtime) == STATUS_SUCCESS);
        DXGK_PHYSICALADAPTERCAPS caps = {};
        std::memcpy(&caps, adapterStorage, adapterMinimum);
        CHECK(caps.NumExecutionNodes == 1 && caps.PagingNodeIndex == 0);
        CHECK(caps.DxgkPhysicalAdapterHandle == runtime && caps.Flags.Value == 0);
        SIZE_T written = outputSize < sizeof(caps) ? outputSize : sizeof(caps);
        for (SIZE_T i = written; i < sizeof(adapterStorage); ++i) CHECK(adapterStorage[i] == 0xa5);
    }
    std::memset(adapterStorage, 0x5a, sizeof(adapterStorage));
    adapterQuery.OutputDataSize = adapterMinimum - 1;
    CHECK(pi5::QueryPhysicalAdapter(adapterQuery, runtime) == STATUS_BUFFER_TOO_SMALL);
    for (auto value : adapterStorage) CHECK(value == 0x5a);
    adapterQuery.OutputDataSize = adapterMinimum; adapterIn.PhysicalAdapterIndex = 1;
    CHECK(pi5::QueryPhysicalAdapter(adapterQuery, runtime) == STATUS_INVALID_PARAMETER);
    adapterIn.PhysicalAdapterIndex = 0; adapterQuery.InputDataSize--;
    CHECK(pi5::QueryPhysicalAdapter(adapterQuery, runtime) == STATUS_INVALID_PARAMETER);
    adapterQuery.InputDataSize++; adapterQuery.pInputData = nullptr;
    CHECK(pi5::QueryPhysicalAdapter(adapterQuery, runtime) == STATUS_INVALID_PARAMETER);
    adapterQuery.pInputData = &adapterIn; adapterQuery.pOutputData = nullptr;
    CHECK(pi5::QueryPhysicalAdapter(adapterQuery, runtime) == STATUS_BUFFER_TOO_SMALL);
    adapterQuery.pOutputData = adapterStorage;
    CHECK(pi5::QueryPhysicalAdapter(adapterQuery, nullptr) == STATUS_INVALID_PARAMETER);

    constexpr SIZE_T bytes = 256 * 1024 * 1024;
    PHYSICAL_ADDRESS physical = {}; physical.QuadPart = 0x100000000ll;
    DXGK_QUERYSEGMENTIN4 in = {};
    DXGK_QUERYSEGMENTOUT4 out;
    std::memset(&out, 0xa5, sizeof(out)); out.NbSegment = 0;
    auto before = out;
    DXGKARG_QUERYADAPTERINFO q = {};
    q.pInputData = &in; q.InputDataSize = sizeof(in);
    q.pOutputData = &out; q.OutputDataSize = sizeof(out);
    CHECK(pi5::QueryPhysicalSegment(q, physical, bytes, 256) == STATUS_SUCCESS);
    CHECK(out.NbSegment == 1);
    before.NbSegment = 1;
    CHECK(std::memcmp(&out, &before, sizeof(out)) == 0); // poisoned pointer never read

    alignas(DXGK_SEGMENTDESCRIPTOR4) unsigned char storage[sizeof(DXGK_SEGMENTDESCRIPTOR4) + 32];
    const SIZE_T minimum = FIELD_OFFSET(DXGK_SEGMENTDESCRIPTOR4, VprReserveSize) + sizeof(UINT);
    const SIZE_T strides[] = {minimum, sizeof(DXGK_SEGMENTDESCRIPTOR4), sizeof(storage)};
    for (SIZE_T stride : strides) {
        std::memset(storage, 0xa5, sizeof(storage));
        out = {}; out.NbSegment = 1; out.pSegmentDescriptor = storage; out.SegmentDescriptorStride = stride;
        CHECK(pi5::QueryPhysicalSegment(q, physical, bytes, 256) == STATUS_SUCCESS);
        const auto& s = *reinterpret_cast<const DXGK_SEGMENTDESCRIPTOR4*>(storage);
        CHECK(s.Size == bytes && s.CpuTranslatedAddress.QuadPart == physical.QuadPart);
        CHECK(s.BaseAddress.QuadPart == 0 && s.Flags.CpuVisible && s.Flags.PopulatedFromSystemMemory);
        CHECK(!s.Flags.Aperture && !s.Flags.SupportsCpuHostAperture && !s.Flags.CacheCoherent);
        CHECK(out.PagingBufferSegmentId == 0 && out.PagingBufferSize == 65536 && out.PagingBufferPrivateDataSize == 256);
        const SIZE_T written = stride < sizeof(s) ? stride : sizeof(s);
        for (SIZE_T i = written; i < sizeof(storage); ++i) CHECK(storage[i] == 0xa5);
    }
    out.SegmentDescriptorStride = minimum - 1;
    std::memset(storage, 0x5a, sizeof(storage));
    CHECK(pi5::QueryPhysicalSegment(q, physical, bytes, 256) == STATUS_INVALID_PARAMETER);
    for (auto v : storage) CHECK(v == 0x5a);
    out.SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4);
    out.pSegmentDescriptor = nullptr;
    CHECK(pi5::QueryPhysicalSegment(q, physical, bytes, 256) == STATUS_INVALID_PARAMETER);
    out.NbSegment = 0; in.PhysicalAdapterIndex = 1;
    CHECK(pi5::QueryPhysicalSegment(q, physical, bytes, 256) == STATUS_INVALID_PARAMETER);
    in.PhysicalAdapterIndex = 0; q.OutputDataSize--;
    CHECK(pi5::QueryPhysicalSegment(q, physical, bytes, 256) == STATUS_BUFFER_TOO_SMALL);
    q.OutputDataSize++; q.pInputData = nullptr;
    CHECK(pi5::QueryPhysicalSegment(q, physical, bytes, 256) == STATUS_INVALID_PARAMETER);

    DXGKARG_GETNODEMETADATA node = {};
    CHECK(pi5::GetPhysicalNodeMetadata(0, &node) == STATUS_SUCCESS);
    CHECK(node.EngineType == DXGK_ENGINE_TYPE_3D && !node.GpuMmuSupported && !node.IoMmuSupported);
    CHECK(pi5::GetPhysicalNodeMetadata(1, &node) == STATUS_INVALID_PARAMETER);
    CHECK(pi5::GetPhysicalNodeMetadata(0x10000, &node) == STATUS_INVALID_PARAMETER);
    CHECK(pi5::GetPhysicalNodeMetadata(0, nullptr) == STATUS_INVALID_PARAMETER);
    for (bool visible : {false, true}) {
        DXGK_ALLOCATIONINFO allocation = {};
        allocation.FlagsWddm2.Value = ~0u;
        pi5::SetPhysicalAllocationFlags(allocation, visible);
        CHECK(allocation.FlagsWddm2.AccessedPhysically && allocation.FlagsWddm2.CpuVisible == visible);
        DXGK_ALLOCATIONINFOFLAGS_WDDM2_0 expected = {};
        expected.AccessedPhysically = 1; expected.CpuVisible = visible;
        CHECK(allocation.FlagsWddm2.Value == expected.Value && allocation.PhysicalAdapterIndex == 0);
    }
    std::puts("PASS: WDDM 2.0 segment bounds, engine metadata and physical-allocation contracts");
}
