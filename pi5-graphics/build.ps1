# SPDX-License-Identifier: BSD-2-Clause-Patent
# Invoked by the repository build.ps1; the caller catalogs and signs this package.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Work,
    [Parameter(Mandatory)][string]$Init,
    [Parameter(Mandatory)][string]$KernelInclude,
    [Parameter(Mandatory)][string]$KernelLib,
    [Parameter(Mandatory)][string]$KmdfInclude,
    [Parameter(Mandatory)][string]$KmdfLib,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$Analyze,
    [switch]$ExperimentalWddm20,
    [switch]$C1ScanoutProbe
)
$ErrorActionPreference = 'Stop'
if ($ExperimentalWddm20 -and $C1ScanoutProbe) { throw 'The C1 probe must run under the WDDM 1.2 control.' }
Set-StrictMode -Version Latest
New-Item $Work -ItemType Directory -Force | Out-Null
Copy-Item "$PSScriptRoot\kmd","$PSScriptRoot\native","$PSScriptRoot\display" $Work -Recurse
Copy-Item "$PSScriptRoot\..\common\*.h" $Work
$staging = Join-Path $Work 'package'
New-Item $staging -ItemType Directory | Out-Null

function Invoke-Compiler([string]$Command) {
    & cmd.exe /d /s /c ($Init + ' && ' + $Command)
    if ($LASTEXITCODE) { throw "Graphics build failed (exit $LASTEXITCODE): $Command" }
}

# Keep the checked graphics build optimized, as required by its display timing.
# DBG and the static debug CRT retain diagnostics without a redistributable DLL.
$kernelFlags = if ($Configuration -eq 'Debug') { '/O2 /DDBG=1' } else { '/O2 /DDBG=0' }
$runtime = if ($Configuration -eq 'Debug') { '/MTd' } else { '/MT' }
# Report the imported WDDM C++ code's existing SAL diagnostics without changing
# its runtime implementation for this build integration. Compiler warnings still
# fail /W4 /WX builds; static-analysis diagnostics in graphics are advisory.
$analysis = if ($Analyze) { ' /analyze /analyze:external- /analyze:WX-' } else { '' }
$warnings = '/nologo /W4 /WX /Zi /external:anglebrackets /external:W0'
$defines = '/D_ARM64_ /DWINNT=1 /DPI5_FULL_DISPLAY=1 /D_ARM64_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00'
if ($ExperimentalWddm20) { $defines += ' /DPI5_EXPERIMENTAL_WDDM20=1' }
if ($C1ScanoutProbe) { $defines += ' /DPI5_C1_SCANOUT_PROBE=1' }
$includes = '/I"' + $KernelInclude + '" /I"' + $Work + '"'
$libraries = '/LIBPATH:"' + $KernelLib + '" ntoskrnl.lib hal.lib BufferOverflowFastFailK.lib displib.lib'

Push-Location $Work
try {
    Invoke-Compiler ('cl ' + $warnings + ' /std:c++17 /O2 ' + $runtime + ' /EHsc /LD native\umd.cpp native\shader.cpp native\validate.cpp' + $analysis + ' /link /DEBUG /OUT:package\Pi5D3D.dll')
    $sources = 'kmd\driver.cpp kmd\gpu.cpp native\encode.cpp native\validate.cpp native\texture.cpp display\bdd.cxx display\bdd_ddi.cxx display\bdd_dmm.cxx display\bdd_util.cxx display\bltfuncs.cxx display\blthw.cxx display\memory.cxx display\pi5-display-hw.cxx'
    Invoke-Compiler ('cl ' + $warnings + ' /std:c++17 ' + $kernelFlags + ' /kernel /GR- ' + $defines + ' ' + $includes + ' /c ' + $sources + $analysis)
    $objects = 'driver.obj gpu.obj encode.obj validate.obj texture.obj bdd.obj bdd_ddi.obj bdd_dmm.obj bdd_util.obj bltfuncs.obj blthw.obj memory.obj pi5-display-hw.obj'
    Invoke-Compiler ('link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:GsDriverEntry /MACHINE:ARM64 /DEBUG /INCREMENTAL:NO /OUT:package\Pi5Graphics.sys ' + $objects + ' ' + $libraries)
    $wdfIncludes = '/DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 /I"' + $KmdfInclude + '"'
    $wdfLibraries = '/LIBPATH:"' + $KmdfLib + '" wdfdriverentry.lib wdfldr.lib'
    Invoke-Compiler ('cl ' + $warnings + ' ' + $kernelFlags + ' /kernel ' + $defines + ' ' + $includes + ' ' + $wdfIncludes + ' /c /Fopower-filter.obj kmd\power-filter.c' + $analysis)
    Invoke-Compiler ('link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry /MACHINE:ARM64 /DEBUG /INCREMENTAL:NO /OUT:package\Pi5GraphicsPower.sys power-filter.obj ' + $libraries + ' ' + $wdfLibraries)
    Copy-Item "$PSScriptRoot\pi5graphics.inf","$PSScriptRoot\display\LICENSE.txt","$PSScriptRoot\display\EDID-LICENSE.txt" $staging
    if ($ExperimentalWddm20 -or $C1ScanoutProbe) {
        $infPath = Join-Path $staging 'pi5graphics.inf'
        $inf = [IO.File]::ReadAllText($infPath)
        if ($inf -notmatch '(?m)^DriverVer=10/10/2026,1\.0\.0\.12\s*$') {
            throw 'Experimental WDDM 2.0 build requires the v12 base INF.'
        }
        $version = if ($C1ScanoutProbe) { '1.0.0.16' } else { '1.0.0.15' }
        $inf = $inf -replace 'DriverVer=10/10/2026,1\.0\.0\.12', ('DriverVer=10/10/2026,'+$version)
        [IO.File]::WriteAllText($infPath, $inf, [Text.Encoding]::Unicode)
    }
    # Import libraries and export files are build intermediates, not driver files.
    Remove-Item "$staging\*.lib","$staging\*.exp" -ErrorAction SilentlyContinue
} finally { Pop-Location }
