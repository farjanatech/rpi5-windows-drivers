# Experimental WDDM 2.0 physical-address driver

This candidate targets the missing Task Manager GPU performance graph. Windows
requires WDDM 2.0 or newer for its VidSch/VidMm GPU performance reporting.
It is a hardware-test candidate, not a validated production driver.

Build with `build.ps1 -Driver pi5-graphics -ExperimentalWddm20`. Only this explicit
option enables the new DDIs and changes the staged INF to **1.0.0.11**. Normal
builds retain the **WDDM 1.2 / 1.0.0.10** path and source INF.

The candidate registers the WDDM 2.0 interface, reports one physical-address
V3D 3D node, enumerates the existing CPU-visible reserved memory through
QUERYSEGMENT4, marks allocations AccessedPhysically and provides process object
lifetime callbacks. Two-stage segment enumeration never reads undefined fields
in the count query and respects the OS-supplied descriptor stride. Submission,
patching, real completion fences, preemption, global timeout recovery, UMD,
shader encoding and C1/D0 display paths remain the v10 implementation.

GPUVA, IOMMU, hardware scheduling, virtual display modes, overlays, video
engines, and precise GPU timestamp/history-buffer instrumentation are not
advertised. In particular, CPU timers are not reported as GPU clock samples.
Task Manager's eventual utilization measures scheduler occupancy, which includes
this driver's serialized software preparation and presentation work; it is not
a V3D shader-busy hardware counter.

## Validation gates

CI builds both configurations with warnings as errors, exercises the actual
segment/metadata/allocation helpers with malformed and guard-buffer inputs, and
runs the existing C1/D0 encoder and shader regressions. It also builds a separate
ARM64 hardware probe. CI does not run the probe on a software adapter.

Before hardware activation, retain the exact signed, working v10 package and
export its Driver Store entry. Establish a timed recovery action restoring v10
and record event-log bookmarks. Do not change firmware, boot security, TDR
timeouts or provider drivers for this experiment.

After activation, all of these must pass before retaining the candidate:

1. ACPI\RPI1001\0 uses v11 without a PnP problem; dxdiag reports WDDM 2.0 and
   Pi5D3D rather than Microsoft Basic Render Driver.
2. `hardware-3d-smoke.exe` passes on the explicit Pi5 V3D adapter. It verifies
   120 shader-drawn triangles, background clears and synchronized pixel readbacks.
3. GPU Engine/GPU Adapter Memory counters enumerate the Pi adapter, change
   under work, and Task Manager shows its GPU graph.
4. Render fences continue progressing without new DWM crashes, display resets,
   scheduler faults or black screens during the user's previous reproducer.

A failed gate means restore v10; do not interpret a version string alone as
success. Multi-process, memory-pressure/paging, sleep/resume, display changes
and timeout recovery still require broader hardware qualification before merge.

## References

- [Microsoft: GPUs in Task Manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)
- [Physical and virtual engines in WDDM 2.0](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)
- [GPU segments and physical allocation requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments)
- [QUERYSEGMENTOUT4 two-stage contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_querysegmentout4)
- [Optional precise GPU timing](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/graphics-kernel-performance-improvements)
