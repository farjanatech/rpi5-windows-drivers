# VideoCore VII Direct3D and HDMI graphics

`Pi5Graphics.sys` supplies the Windows display and rendering driver for the
Pi 5's VideoCore VII GPU. `Pi5D3D.dll` translates Direct3D rendering commands
and shaders for the GPU. The package supports the native HDMI outputs,
monitor EDID, display mode changes and hotplug. `Pi5GraphicsPower.sys` keeps
the GPU and display providers powered in the required order.

It depends on [pi5-v3d](../pi5-v3d/README.md) 0.1.0.22 or newer,
[pi5-fclk](../pi5-fclk/README.md) 0.1.0.9 or newer,
[pi5-mailbox](../pi5-mailbox/README.md) 0.1.0.16 or newer, and matching
rpi5-uefi firmware exposing `ACPI\RPI1001` and the native display resources.
The V3D provider additionally requires the graph and reset-gate services.

The display framework retains its Microsoft Public License in
`display/LICENSE.txt`. EDID timing-table attribution is in
`display/EDID-LICENSE.txt`; both notices accompany the binary package.


## Damian Edition C1 / D0 compatibility

The `damian-edition` branch is being extended as a single auto-detecting
graphics package for both BCM2712 generations.

The matching Damian Edition UEFI exposes `RPI1001._HRV`:

- `0` = C0/C1 generation
- `1` = D0 generation
- missing/other = legacy or unsupported firmware

`Pi5Graphics.sys` reads that value directly from the ACPI PDO. The existing
Damian D0 handoff acceptance remains unchanged.

The first C1/D0 integration build additionally records both live HVS candidate
heads and raster-list words in `HKLM\HARDWARE\Pi5DisplayDiagnostics\Trace`
even in Release configuration. This is deliberate: C1 support will use a
separate validated handoff parser derived from real C1 state rather than
weakening the D0 checks.

Trace IDs added for this work:

- 116: silicon revision / POST framebuffer
- 117-120: HDMI/HVS candidate 0
- 121-124: HDMI/HVS candidate 1
