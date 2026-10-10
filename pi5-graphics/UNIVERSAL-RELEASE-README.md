# Raspberry Pi 5 Universal Graphics Driver — C1 / D0

Version: **1.0.0.0**

This is the stable universal graphics release derived from Damian's
`add-complete-graphics-release` branch and the C1 validation work in
`farjanatech/rpi5-windows-drivers`.

## Automatic hardware routing

There is one package for both silicon families.

- **BCM2712 C0/C1** — ACPI `_HRV = 0`
  - HVS 0x2453 layout.
  - C1 raster words: CTL0 `0x4900C007`, CTL2 `0x4000FFF0`.
  - Displayable primary stays linear.
  - Final presentation uses the C1-proven CPU copy into the inactive native
    framebuffer followed by the normal HVS flip.
  - V3D revision below 10 uses the original pre-7.1.10 shader-state record.

- **BCM2712 D0** — ACPI `_HRV = 1`
  - Damian's original HVS 0x2454/direct-MMIO scanout path is retained.
  - Damian's dual-head path remains enabled.
  - V3D revision 10 or newer uses Damian's original
    `GL_SHADER_STATE_RECORD_DRAW_INDEX` layout byte-for-byte.

The V3D packet decision is made from HUB IDENT3, not guessed from the HVS revision.

## Validation status

**C1 hardware: physically validated.** The 0.1.0.179 code base produced a clean
1920x1080 Windows desktop with Pi5Graphics active, Device Manager Status OK,
ProblemCode 0, no HVS HANDLE_ERROR/BAD_UPM/BAD_AXI faults, and matching
source/destination presentation hashes. Version 1.0.0.0 keeps that functional
graphics code unchanged.

**D0 hardware: not physically tested in this project because no D0 board is
available.** D0 confidence comes from preserving Damian's direct-scanout path
and from CI regression tests that fail the release build if the D0 shader
record bytes or C1/D0 routing invariants drift.

The installer adds a one-time first-boot health watchdog. If 1.0.0.0 does not
start with Status OK / ProblemCode 0, it removes only this release and reboots
so Windows can fall back to the older graphics package.

## Supported profile

- Windows 11 ARM64 with the Damian Edition provider/ACPI stack.
- Hardware ID: `ACPI\RPI1001`.
- C1 stable profile: one active display head.
- D0: Damian direct-scanout and dual-head behavior retained.
- UEFI is not modified.
- Wi-Fi is outside this graphics release.

## Install

1. Extract the ZIP.
2. Run **INSTALL-UNIVERSAL-GRAPHICS.cmd** as administrator.
3. If TESTSIGNING or normal-memory boot configuration needs a reboot, do that
   and run the installer again.
4. After the installer selects DriverVersion 1.0.0.0, reboot once.
5. The first-boot watchdog keeps the release if Device Manager health is good,
   otherwise it rolls back only 1.0.0.0.

This package is project/test signed, not Microsoft WHQL-signed. Secure Boot may
need to be disabled to enable TESTSIGNING.

## Rollback

Run **UNINSTALL-UNIVERSAL-GRAPHICS.cmd**, then reboot. Older Pi5Graphics
packages are deliberately left installed so Windows can select one as fallback.

Do not delete older Pi5Graphics packages until the first successful boot of
this release.
