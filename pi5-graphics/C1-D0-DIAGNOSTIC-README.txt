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


C1 captured-context experiment (0.1.0.167):
- Keeps Damian Edition RC1 firmware unchanged.
- Keeps the C1 copy-scanout / primary-only path from 0.1.0.166.
- C1 generated HVS lists now use the user's hardware-captured firmware
  context word 0x80000048 instead of 0xc0c0c0c0.
- D0 generated lists remain 0xc0c0c0c0 and all D0 behavior is unchanged.
- The C1 context value is used in BOTH initial private-list creation and
  WriteCurrentLists(), so mode/list regeneration cannot silently revert it.
- Release trace telemetry:
    210 = LPTRS / active DL head
    211 = FETCHER_STATUS / FETCH_STATUS
    212 = HANDLE_ERROR / BAD_UPM
    213 = BAD_AXI / UPM descriptor
    214-218 = active private raster words 0..9


C1 UPM experiment (0.1.0.168):
- Restores generated Context to 0xc0c0c0c0.
- C1 primary no longer reuses the firmware UPM selection after replacing the framebuffer.
- C1 advances the encoded UPM selection by two positions for this diagnostic.
- D0 behavior is unchanged.
- New traces:
  219 firmware PTR0 / selected Windows UPM descriptor
  220 C1 UPM isolation registers 0 / 1
  221 C1 UPM isolation register 2 / UBM size

C1 fresh-UPM-range experiment (0.1.0.170):
- Triggered by a real 0.1.0.169 takeover that was driver-healthy but still visibly ARTIFACT.
- Keeps the fresh C1 UPM handle introduced in 0.1.0.168.
- Also gives Windows a non-overlapping UPM memory range instead of reusing firmware base 0.
- Mirrors upstream VC6 UPM sizing: raster fetch region, two buffered lines, 256-byte UBM words.
- The Windows base begins immediately after the firmware allocation and is checked against
  both the existing 512-word per-port partition and the live SCALER6_UBM_SIZE value.
- D0 behavior is unchanged; C1 remains primary-only and copy-scanout.
- New traces:
  227 = firmware UPM base / selected Windows UPM base
  228 = firmware UPM words / Windows UPM words
  229 = live UBM size / selected per-port partition limit
- Trace 219 still records firmware PTR0 / final Windows UPM descriptor.

C1 pre-OpenMonitor predicate diagnostic (0.1.0.171):
- Triggered because 0.1.0.170 returned Code 43 at Start stage 7 before trace 222.
- The 0.1.0.170 fresh-UPM-range code is retained but is NOT changed by this build.
- C1-only telemetry now isolates the exact validation group between trace 33 and trace 222.
- D0 keeps the prior validation path unchanged.
- New traces:
  230 = HVS VERSION / CXM size
  231 = live UBM size / required UPM words
  232 = HVS display mode / PV control
  233 = PV format / PV status
  234 = PV vertical register / HVS idle register
  235 = requested HVS head / active HVS head
  236 = display-list word 0 / private-list head
  237-240 = firmware POST raster words and framebuffer-high validation
  241 = firmware list-walk result or first failing pointer/word
  242 = private-tail sentinel result or first failing index/value
- A nonzero trace status identifies the first rejected predicate group.

C1 HVS physical-address diagnostic (0.1.0.172):
- Triggered because 0.1.0.171 reached Status OK / ProblemCode 0, used the expected
  fresh UPM base/handle/size, had HANDLE_ERROR=BAD_UPM=BAD_AXI=0, but remained visibly ARTIFACT.
- No scanout-address behavior is changed by this build.
- C1-only telemetry checks whether the HAL common-buffer DMA address actually equals the
  CPU physical address required by the HVS, and whether every scanout page is physically contiguous.
- D0 behavior is unchanged.
- New traces:
  243 = HVS-programmed logical/DMA scanout start low/high
  244 = CPU physical scanout start low/high
  245 = CPU physical scanout last-page low/high
  246 = physical-contiguity mismatch count / first mismatching page index
  247 = scanout page count / 1 if logical start equals physical start, else 0
C1 frozen-scanout producer isolation (0.1.0.173):
- Triggered because 0.1.0.172 proved the HAL DMA scanout address equals the real
  CPU physical address and all 16384 scanout pages are physically contiguous,
  while the driver remained healthy and the physical HDMI picture remained ARTIFACT.
- HVS list, address, UPM base/handle/size, copy-scanout allocation and C1 primary-only
  rules remain unchanged.
- C1 only: after Start() copies the known-good firmware framebuffer into the native
  scanout buffer, later Windows scanout presents are acknowledged without executing
  V3D CopyScanout() or flipping to another buffer.
- Expected result is a frozen boot/firmware image. This is intentional.
- Interpretation:
  CLEAN/FROZEN = HVS path is good; corruption is introduced by V3D->scanout updates.
  ARTIFACT/FROZEN = corruption remains HVS-side despite valid address/list/UPM state.
- Pi5Render trace 160 marks the first suppressed C1 scanout update.
- D0 behavior is unchanged.
C1 CPU-authored frozen pattern isolation (0.1.0.174):
- Triggered because 0.1.0.173 stayed healthy and produced a pure black, artifact-free
  physical output while later C1 scanout updates were suppressed.
- Keeps the 0.1.0.173 frozen C1 present path and all 0.1.0.172 HVS/UPM/address logic.
- Before HVS takeover, C1 CPU-writes both native scanout buffers with a deterministic
  black/white pattern: vertical stripes in the top half, 64x64 checkerboard in the
  bottom half, and a white border. This avoids color-channel ambiguity.
- D0 behavior is unchanged.
- New display traces:
  248 = pattern black/white sentinel values
  249 = pattern width/height
- Interpretation:
  PATTERN_CLEAN = HVS/list/address/UPM/fetch geometry is working for real pixels;
                  corruption is introduced later by V3D->scanout updates.
  PATTERN_ARTIFACT = corruption remains HVS-side even with CPU-authored pixels.
  BLACK = pattern was not visible; inspect visibility/takeover state before concluding.
