// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// WDK types must be included by the caller. These helpers have no kernel API
// dependencies so their ABI/bounds contracts can also be tested in user mode.
namespace pi5 {
inline NTSTATUS QueryPhysicalSegment(const DXGKARG_QUERYADAPTERINFO& q,
    PHYSICAL_ADDRESS physical, SIZE_T bytes, UINT privateBytes) {
    if (!q.pInputData || q.InputDataSize < sizeof(DXGK_QUERYSEGMENTIN4))
        return STATUS_INVALID_PARAMETER;
    if (static_cast<const DXGK_QUERYSEGMENTIN4*>(q.pInputData)->PhysicalAdapterIndex)
        return STATUS_INVALID_PARAMETER;
    if (!q.pOutputData || q.OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4))
        return STATUS_BUFFER_TOO_SMALL;
    auto out = static_cast<DXGK_QUERYSEGMENTOUT4*>(q.pOutputData);
    // In the count query all other fields are undefined, including the pointer.
    if (!out->NbSegment) { out->NbSegment = 1; return STATUS_SUCCESS; }
    const SIZE_T minimum = FIELD_OFFSET(DXGK_SEGMENTDESCRIPTOR4, VprReserveSize) + sizeof(UINT);
    if (out->NbSegment != 1 || !out->pSegmentDescriptor ||
        out->SegmentDescriptorStride < minimum || !bytes)
        return STATUS_INVALID_PARAMETER;
    DXGK_SEGMENTDESCRIPTOR4 segment = {};
    segment.Flags.CpuVisible = 1;
    segment.Flags.PopulatedFromSystemMemory = 1;
    segment.CpuTranslatedAddress = physical;
    segment.Size = bytes;
    // BaseAddress is a segment-relative address, as in the WDDM 1.2 path.
    // No GPUVA, CPU host aperture, cache coherency or fake VRAM is advertised.
    const SIZE_T copyBytes = out->SegmentDescriptorStride < sizeof(segment) ?
        out->SegmentDescriptorStride : sizeof(segment);
    RtlCopyMemory(out->pSegmentDescriptor, &segment, copyBytes);
    out->PagingBufferSegmentId = 0;
    out->PagingBufferSize = 65536;
    out->PagingBufferPrivateDataSize = privateBytes;
    return STATUS_SUCCESS;
}

inline NTSTATUS GetPhysicalNodeMetadata(UINT ordinal, DXGKARG_GETNODEMETADATA* out) {
    // High word is physical adapter index; low word is the node ordinal.
    if (ordinal || !out) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(out, sizeof(*out));
    out->EngineType = DXGK_ENGINE_TYPE_3D;
    const WCHAR name[] = L"V3D 3D";
    RtlCopyMemory(out->FriendlyName, name, sizeof(name));
    return STATUS_SUCCESS;
}

inline void SetPhysicalAllocationFlags(DXGK_ALLOCATIONINFO& out, bool cpuVisible) {
    out.FlagsWddm2.Value = 0;
    out.FlagsWddm2.CpuVisible = cpuVisible;
    // Essential: without this, VidMm may supply discontiguous GPUVA pages and
    // reject the allocation lists used by our physical submission engine.
    out.FlagsWddm2.AccessedPhysically = 1;
    out.PhysicalAdapterIndex = 0;
}
} // namespace pi5
