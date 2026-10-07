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
