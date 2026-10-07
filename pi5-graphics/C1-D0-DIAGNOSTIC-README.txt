RPi5 Damian Edition C1/D0 Graphics Diagnostic Build
================================================

Purpose
-------
This package is NOT the final C1 compatibility driver.

It preserves Damian's existing D0 display acceptance rules exactly, but adds:
- firmware revision detection from ACPI RPI1001._HRV
- release-mode HVS candidate snapshots for both HDMI paths
- trace IDs 116-124 in HKLM\HARDWARE\Pi5DisplayDiagnostics

Revision contract:
  0 = BCM2712 C0/C1 generation
  1 = BCM2712 D0 generation
  other/missing = legacy/unknown firmware

Install
-------
1. Ensure Windows test signing is enabled:
     bcdedit /set testsigning on
   then reboot if you just changed it.
2. Run Install-Graphics-Diagnostic.ps1 as Administrator.
3. Reboot.
4. Run the RPi5 Display Handoff Diagnostics utility and upload its ZIP.

Expected on the current C1 machine
----------------------------------
The driver may still report Code 43. That is intentional for this diagnostic
build. It does not weaken the existing D0 safety checks. The new trace gives us
the exact live C1 HVS list so the final C1 parser can be implemented safely.

Do not use this package as a production graphics release yet.

Second C1 capture stage:
- accepts C1 HVS identity (SCALER6_VERSION low byte 0x53) for diagnostics;
- uses C1 per-display LPTRS/DL offsets (0x3c/0x48, stride 0x20);
- records HVS VERSION as trace ID 125;
- never takes ownership of a C1 display in this build;
- keeps Damian's D0 0x2454 path and validation unchanged.


Final read-only C1 handoff capture:
- IDs 200-203: C1 HVS sizing/control and display-0 registers.
- IDs 204-208: all 10 words of the active C1 boot raster list.
- ID 209: Damian private-list tail sentinels at 0xF80..0xF93.
This build still never takes ownership of a C1 display.


First write-enabled C1 milestone (0.1.0.166):
- C1 HVS 0x2453 and D0 HVS 0x2454 are selected by the existing RPI1001._HRV.
- C1 SCALER6 per-display registers are translated from Damian's canonical D0 offsets.
- C1 raster lists use CTL0 0x4900c007 and CTL2 0x4000fff0.
- D0 keeps Damian's 0x490cc007 / 0x0000fff0 list and D0 register offsets.
- C1 is forced to primary/single-head for this first takeover test.
- The reserved HVS tail at 0xf80 was verified unused on the user's C1 board before enabling writes.


C1 copy-scanout stabilization (0.1.0.166):
- Reads RPI1001._HRV in AddDevice before WDDM capabilities are queried.
- C1 (revision 0) does NOT advertise/use FlipOnVSyncMmIo.
- C1 uses Damian's existing CopyScanout -> native common-buffer -> HVS path.
- D0 (revision 1) keeps Damian's original direct MMIO flip path.
- This change targets the real-world symptom where 0.1.0.165 had PnP/render
  success but visible black/corrupted desktop scanout on C1.
