# SPDX-License-Identifier: BSD-2-Clause-Patent
# Build the ARM64 drivers from a local checkout with MSVC and the WDK.
[CmdletBinding()]
param(
    [ValidateSet('all', 'source', 'rp1', 'nvme', 'rp1-service', 'rp1-clocks', 'rp1-gpio', 'rp1-uart', 'rp1-i2c', 'rp1-spi', 'rp1-dma', 'rp1-ethernet', 'bcm2712-platform', 'bcm2712-gpio', 'bcm2712-uart', 'cyw-bluetooth', 'pi5-board', 'pi5-graph', 'pi5-mailbox', 'pi5-fclk', 'pi5-pm', 'pi5-iommu', 'pi5-v3d', 'pi5-graphics', 'rp1-fan', 'pi5-nvram')]
    [string]$Driver = 'all',
    [string]$Output = (Join-Path $PSScriptRoot 'Build'),
    [string]$FirmwareRoot = '',
    [string]$WdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10",
    [string]$SdkVersion = '10.0.26100.0',
    [string]$WdkVersion = '10.0.26100.0',
    [string]$KmdfVersion = '1.33',
    [string]$KernelKit = '',
    [string]$KmdfKit = '',
    [string]$Inf2Cat = '',
    [string]$NvmeDriver = '',
    [string]$NvmeInf = '',
    [string]$NvmeHardwareId = '',
    [switch]$NvmeAnyDevice,
    [string]$Python = 'python',
    [string]$CertificateThumbprint = '',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Analyze,
    [switch]$ExperimentalWddm20
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Output = [IO.Path]::GetFullPath($Output)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio C++ build tools, including ARM64 tools.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
if (!$vs) { throw 'Visual Studio ARM64 C++ tools were not found.' }
$init = 'call "' + $vs + '\VC\Auxiliary\Build\vcvarsall.bat" amd64_arm64 ' + $SdkVersion
$kernelInclude = Join-Path $WdkRoot "Include\$WdkVersion\km"
$kernelLib = Join-Path $WdkRoot "Lib\$WdkVersion\km\arm64"
if ($KernelKit) {
    $kernelInclude = Join-Path $KernelKit 'include'
    $kernelLib = Join-Path $KernelKit 'lib'
}
$kmdfInclude = Join-Path $WdkRoot "Include\wdf\kmdf\$KmdfVersion"
$kmdfLib = Join-Path $WdkRoot "Lib\wdf\kmdf\arm64\$KmdfVersion"
if ($KmdfKit) {
    $kmdfInclude = Join-Path $KmdfKit 'include'
    $kmdfLib = Join-Path $KmdfKit 'lib'
}
if (!$Inf2Cat) {
    foreach ($candidate in @("$WdkRoot\bin\$WdkVersion\x86\Inf2Cat.exe", "$WdkRoot\bin\x86\Inf2Cat.exe")) {
        if (Test-Path $candidate) { $Inf2Cat = $candidate; break }
    }
}
$sign = Join-Path $WdkRoot "bin\$SdkVersion\x64\signtool.exe"
function Require-File([string]$Path) {
    if (!$Path -or !(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing build prerequisite: $Path" }
}
function Set-PackageIdentity([string]$Directory) {
    # Windows deduplicates packages by INF content. Reusing an unchanged INF can
    # silently select an older binary even when DevCon forces an update.
    $identity = @("; Build configuration: $Configuration")
    foreach ($file in Get-ChildItem $Directory -File | Where-Object Extension -in '.sys','.dll' | Sort-Object Name) {
        $identity += "; $($file.Name) SHA256: $((Get-FileHash $file.FullName -Algorithm SHA256).Hash)"
    }
    foreach ($inf in Get-ChildItem $Directory -Filter *.inf) {
        $contents = [IO.File]::ReadAllText($inf.FullName).TrimEnd() + "`r`n" + ($identity -join "`r`n") + "`r`n"
        [IO.File]::WriteAllText($inf.FullName, $contents, [Text.Encoding]::Unicode)
    }
}
Require-File (Join-Path $kernelInclude 'ntddk.h')
Require-File (Join-Path $kernelLib 'ntoskrnl.lib')
Require-File (Join-Path $kernelLib 'BufferOverflowFastFailK.lib')
Require-File $Inf2Cat
if ($CertificateThumbprint) {
    if ($CertificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$') { throw 'CertificateThumbprint must be a SHA-1 certificate thumbprint.' }
    Require-File $sign
}
$rp1Drivers = @('rp1-service','rp1-clocks','rp1-gpio','rp1-uart','rp1-i2c','rp1-spi','rp1-dma','rp1-ethernet','rp1-fan')
$names = if ($Driver -in 'all','source') { $rp1Drivers + @('bcm2712-platform','bcm2712-gpio','bcm2712-uart','cyw-bluetooth','pi5-board','pi5-graph','pi5-mailbox','pi5-fclk','pi5-pm','pi5-iommu','pi5-v3d','pi5-graphics','pi5-nvram') } elseif ($Driver -eq 'rp1') { $rp1Drivers } else { @($Driver) }
if ($Driver -eq 'all') { $names = @('nvme') + $names }
if ('pi5-nvram' -in $names) {
    if (!$FirmwareRoot) { throw 'Supply -FirmwareRoot with the path to the matching rpi5-uefi checkout to build the NVRAM driver.' }
    $FirmwareRoot = [IO.Path]::GetFullPath($FirmwareRoot)
    $firmware = Join-Path $FirmwareRoot 'edk2-platforms\Platform\RaspberryPi'
    foreach ($relative in @('Library\NvramFileLib\NvramFileLib.c', 'Include\Library\NvramFileLib.h')) {
        if (!(Test-Path -LiteralPath (Join-Path $firmware $relative) -PathType Leaf)) {
            throw 'The NVRAM driver needs the matching rpi5-uefi sources. Supply -FirmwareRoot with the path to an rpi5-uefi checkout containing NvramFileLib.'
        }
    }
}
if ('nvme' -in $names) {
    if (!$NvmeDriver -or !$NvmeInf) { throw 'The NVMe build requires -NvmeDriver and -NvmeInf from matching original ARM64 Windows media. Use -Driver source to build only the source drivers.' }
    if (!$NvmeAnyDevice -and !$NvmeHardwareId) { throw 'The NVMe build requires -NvmeHardwareId or -NvmeAnyDevice.' }
    Require-File $NvmeDriver
    Require-File $NvmeInf
    $NvmeDriver = (Resolve-Path -LiteralPath $NvmeDriver).ProviderPath
    $NvmeInf = (Resolve-Path -LiteralPath $NvmeInf).ProviderPath
}

foreach ($name in $names) {
    if ($name -eq 'nvme') {
        $work = Join-Path $Output ('.work\nvme-' + [Guid]::NewGuid().ToString('N'))
        [string[]]$nvmeBinding = if ($NvmeAnyDevice) { @('--any-device') } else { @('--hardware-id', $NvmeHardwareId) }
        & $Python (Join-Path $PSScriptRoot 'nvme\build.py') $NvmeDriver $NvmeInf $work @nvmeBinding
        if ($LASTEXITCODE) { throw 'NVMe package generation failed' }
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$work\stornvme.sys"
            if ($LASTEXITCODE) { throw 'NVMe driver signing failed' }
        }
        Set-PackageIdentity $work
        & $Inf2Cat "/driver:$work" /os:10_GE_ARM64
        if ($LASTEXITCODE) { throw 'NVMe catalog generation failed' }
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$work\pi5-stornvme.cat"
            if ($LASTEXITCODE) { throw 'NVMe catalog signing failed' }
        }
        $package = Join-Path $Output 'nvme'
        New-Item $package -ItemType Directory -Force | Out-Null
        Copy-Item "$work\stornvme.sys","$work\pi5-stornvme.inf","$work\pi5-stornvme.cat" $package -Force
        Write-Host "Built nvme -> $package"
        continue
    }
    $isNdis = $name -eq 'rp1-ethernet'
    if (!$isNdis) {
        if ($KmdfVersion -ne '1.33') { throw 'These INF packages require KMDF 1.33.' }
        Require-File (Join-Path $kmdfInclude 'wdf.h')
        Require-File (Join-Path $kmdfLib 'wdfdriverentry.lib')
        Require-File (Join-Path $kmdfLib 'wdfldr.lib')
    }
    if ($name -eq 'pi5-graphics') {
        Require-File (Join-Path $kernelLib 'displib.lib')
        $work = Join-Path $Output ('.work\pi5-graphics-' + [Guid]::NewGuid().ToString('N'))
        $graphicsArguments = @{
            Work = $work; Init = $init; KernelInclude = $kernelInclude; KernelLib = $kernelLib
            KmdfInclude = $kmdfInclude; KmdfLib = $kmdfLib
            Configuration = $Configuration; Analyze = $Analyze
            ExperimentalWddm20 = $ExperimentalWddm20
        }
        & (Join-Path $PSScriptRoot 'pi5-graphics\build.ps1') @graphicsArguments
        $staging = Join-Path $work 'package'
        if ($CertificateThumbprint) {
            foreach ($file in Get-ChildItem $staging -File | Where-Object Extension -in '.sys','.dll') {
                & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint $file.FullName
                if ($LASTEXITCODE) { throw "Graphics signing failed: $file" }
            }
        }
        Set-PackageIdentity $staging
        & $Inf2Cat "/driver:$staging" /os:10_GE_ARM64
        if ($LASTEXITCODE) { throw 'Graphics catalog generation failed' }
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$staging\pi5graphics.cat"
            if ($LASTEXITCODE) { throw 'Graphics catalog signing failed' }
        }
        $package = Join-Path $Output $name
        New-Item $package -ItemType Directory -Force | Out-Null
        Copy-Item "$staging\*" $package -Force
        Write-Host "Built $name -> $package"
        continue
    }
    $binary = switch ($name) {
        'rp1-service' { 'Pi5Rp1' }
        'rp1-clocks' { 'Pi5Rp1Clock' }
        'rp1-gpio' { 'Pi5Gpio' }
        'rp1-uart' { 'Pi5Uart' }
        'rp1-i2c' { 'Pi5I2c' }
        'rp1-spi' { 'Pi5Spi' }
        'rp1-dma' { 'Pi5Dma' }
        'rp1-ethernet' { 'Pi5Ethernet' }
        'bcm2712-platform' { 'Pi5Platform' }
        'bcm2712-gpio' { 'Pi5BcmGpio' }
        'bcm2712-uart' { 'Pi5BcmUart' }
        'cyw-bluetooth' { 'Pi5Bluetooth' }
        'pi5-board' { 'Pi5Board' }
        'pi5-graph' { 'Pi5Graph' }
        'pi5-mailbox' { 'Pi5Mailbox' }
        'pi5-fclk' { 'Pi5Fclk' }
        'pi5-pm' { 'Pi5Pm' }
        'pi5-iommu' { 'Pi5Iommu' }
        'pi5-v3d' { 'Pi5V3d' }
        'rp1-fan' { 'Pi5Fan' }
        'pi5-nvram' { 'Pi5Nvram' }
    }
    $source = Join-Path $PSScriptRoot $name
    $package = Join-Path $Output $name
    # A separate work directory prevents stale objects and catalogs from entering a package.
    $work = Join-Path $Output ('.work\' + $name + '-' + [Guid]::NewGuid().ToString('N'))
    New-Item $work -ItemType Directory -Force | Out-Null
    Get-ChildItem $source -File | Where-Object { $_.Extension -in '.c','.cpp','.h','.inf' } | Copy-Item -Destination $work
    if ($name -in 'rp1-service','rp1-clocks','rp1-gpio','rp1-uart','rp1-i2c','rp1-spi','rp1-dma','rp1-ethernet','pi5-graph','pi5-mailbox','pi5-fclk','pi5-pm','pi5-iommu','pi5-v3d') {
        Copy-Item (Join-Path $PSScriptRoot 'common\*.h') $work
    }
    if ($name -eq 'pi5-nvram') {
        New-Item (Join-Path $work 'Library') -ItemType Directory | Out-Null
        Copy-Item "$firmware\Library\NvramFileLib\NvramFileLib.c" $work
        Copy-Item "$firmware\Include\Library\NvramFileLib.h" $work
        Copy-Item "$firmware\Include\Library\NvramFileLib.h" (Join-Path $work 'Library')
    }
    $sources = switch ($name) {
        'rp1-service' { 'driver.c' }
        'rp1-clocks' { 'driver.c rates.c' }
        'bcm2712-gpio' { 'driver.c hardware.c layout.c' }
        'cyw-bluetooth' { 'driver.c Fdo.c io.c pdo.c device.c' }
        'pi5-board' { 'driver.c' }
        'pi5-graph' { 'driver.c graph.c' }
        'pi5-mailbox' { 'driver.c mailbox.c' }
        'pi5-fclk' { 'driver.c' }
        'pi5-pm' { 'driver.c hardware.c' }
        'pi5-iommu' { 'driver.c hardware.c' }
        'pi5-v3d' { 'driver.c hardware.c' }
        'rp1-gpio' { 'driver.c hardware.c' }
        'rp1-uart' { 'driver.c hardware.c' }
        'rp1-ethernet' { 'miniport.c gem.c' }
        'pi5-nvram' { 'driver.c NvramFileLib.c' }
        'rp1-fan' { 'driver.c hardware.c temperature.c' }
        default { 'driver.c hardware.c' }
    }
    $objects = $sources.Replace('.c', '.obj')
    $defines = '/D_ARM64_ /DWINNT=1 /D_ARM64_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00'
    $includes = '/I"' + $kernelInclude + '" /I"' + $work + '"'
    $libraries = '/LIBPATH:"' + $kernelLib + '" ntoskrnl.lib hal.lib BufferOverflowFastFailK.lib'
    $entry = 'GsDriverEntry'
    if ($isNdis) {
        Require-File (Join-Path $kernelInclude 'ndis.h')
        Require-File (Join-Path $kernelLib 'ndis.lib')
        $defines += ' /DNDIS_MINIPORT_DRIVER /DNDIS630_MINIPORT /DNDIS_WDM=1'
        $libraries = 'ndis.lib ' + $libraries
    } else {
        $defines += ' /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33'
        $includes += ' /I"' + $kmdfInclude + '"'
        $libraries = '/LIBPATH:"' + $kmdfLib + '" wdfdriverentry.lib wdfldr.lib ' + $libraries
        $entry = 'FxDriverEntry'
    }
    if ($name -in 'rp1-gpio','bcm2712-gpio') { $libraries += ' msgpioclxstub.lib' }
    if ($name -in 'rp1-service','rp1-clocks','rp1-dma','pi5-board','pi5-graph','pi5-mailbox','pi5-fclk','pi5-pm','pi5-iommu','pi5-v3d') { $libraries += ' wdmsec.lib' }
    if ($name -eq 'pi5-mailbox') { $libraries += ' oprghdlr.lib' }
    if ($name -in 'rp1-uart','bcm2712-uart') {
        $includes += ' /I"' + (Join-Path $kernelInclude 'sercx\2.0') + '"'
        $libraries += ' /LIBPATH:"' + (Join-Path $kernelLib 'sercx\2.0') + '" sercxstubs.lib'
    }
    if ($name -in 'rp1-i2c','rp1-spi') {
        $includes += ' /I"' + (Join-Path $kernelInclude 'spb\1.1') + '"'
        $libraries += ' /LIBPATH:"' + (Join-Path $kernelLib 'spb\1.1') + '" spbcxstubs.lib'
    }
    if ($name -eq 'cyw-bluetooth') {
        $defines += ' /DRESHUB_USE_HELPER_ROUTINES'
        $libraries += ' ntstrsafe.lib'
        Copy-Item "$source\LICENSE.txt","$source\FIRMWARE-LICENSE.txt" $work
        $blob = [IO.File]::ReadAllBytes("$source\BCM4345C0.hcd")
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add('/* Binary firmware; see FIRMWARE-LICENSE.txt. Generated by build.ps1. */')
        $lines.Add('static const UCHAR BcmFirmware[] = {')
        for ($offset = 0; $offset -lt $blob.Length; $offset += 16) {
            $last = [Math]::Min($offset + 15, $blob.Length - 1)
            $lines.Add((($blob[$offset..$last] | ForEach-Object { '0x{0:x2}' -f $_ }) -join ',') + ',')
        }
        $lines.Add('};')
        [IO.File]::WriteAllLines("$work\firmware.h", $lines)
        $wpp = Join-Path $WdkRoot "bin\$WdkVersion\x64\tracewpp.exe"
        Require-File $wpp
        Push-Location $work
        try {
            & $wpp "-cfgdir:$WdkRoot\bin\$WdkVersion\WppConfig\Rev1" -km '-func:DoTrace(LEVEL,FLAG,(MSG,...))' "-I$kernelInclude" "-I$work" "-odir:$work" driver.c Fdo.c io.c pdo.c device.c
            if ($LASTEXITCODE) { throw 'Bluetooth WPP generation failed' }
        } finally { Pop-Location }
    }
    $optimization = if ($Configuration -eq 'Debug') { '/Od /DDBG=1' } else { '/O2 /DDBG=0' }
    $compile = 'cl /nologo /TC /W4 /WX ' + $optimization + ' /kernel /Zi /external:anglebrackets /external:W0 ' + $defines + ' ' + $includes + ' /c ' + $sources
    if ($Analyze) { $compile += ' /analyze /analyze:external-' }
    Push-Location $work
    try {
        & cmd.exe /d /s /c ($init + ' && ' + $compile)
        if ($LASTEXITCODE) { throw "$name compilation failed" }
        & cmd.exe /d /s /c ($init + ' && link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:' + $entry + ' /MACHINE:ARM64 /DEBUG /INCREMENTAL:NO /OUT:' + $binary + '.sys ' + $objects + ' ' + $libraries)
        if ($LASTEXITCODE) { throw "$name linking failed" }
        $staging = Join-Path $work 'package'
        New-Item $staging -ItemType Directory | Out-Null
        if ($name -eq 'cyw-bluetooth') { Copy-Item LICENSE.txt,FIRMWARE-LICENSE.txt $staging }
        Copy-Item "$binary.sys", ($binary.ToLowerInvariant() + '.inf') $staging
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$staging\$binary.sys"
            if ($LASTEXITCODE) { throw "$name driver signing failed" }
        }
        Set-PackageIdentity $staging
        & $Inf2Cat "/driver:$staging" /os:10_GE_ARM64
        if ($LASTEXITCODE) { throw "$name catalog generation failed" }
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$staging\$($binary.ToLowerInvariant()).cat"
            if ($LASTEXITCODE) { throw "$name catalog signing failed" }
        }
        New-Item $package -ItemType Directory -Force | Out-Null
        Copy-Item "$staging\*" $package -Force
        Copy-Item "$binary.pdb" $package -Force
        if ($name -in 'rp1-gpio','rp1-uart','rp1-i2c','rp1-spi') {
            $toolSource = $name.Substring(4) + '.cpp'
            $toolName = $binary + 'Tool.exe'
            & cmd.exe /d /s /c ($init + ' && cl /nologo /EHsc /std:c++20 /W4 /WX /O2 /external:anglebrackets /external:W0 ' + $toolSource + ' /Fe:' + $toolName + ' /link windowsapp.lib cfgmgr32.lib')
            if ($LASTEXITCODE) { throw "$name desktop tool build failed" }
            Copy-Item $toolName $package -Force
        }
        if ($name -eq 'pi5-board') {
            & cmd.exe /d /s /c ($init + ' && cl /nologo /W4 /WX /O2 boardctl.c /Fe:Pi5BoardTool.exe')
            if ($LASTEXITCODE) { throw 'Board desktop tool build failed' }
            Copy-Item Pi5BoardTool.exe $package -Force
        }
        if ($name -eq 'pi5-graph') {
            & cmd.exe /d /s /c ($init + ' && cl /nologo /W4 /WX /O2 graphctl.c /Fe:Pi5GraphTool.exe')
            if ($LASTEXITCODE) { throw 'Graph desktop tool build failed' }
            Copy-Item Pi5GraphTool.exe $package -Force
            Copy-Item "$work\pi5-graph.h" $package -Force
        }
        Write-Host "Built $name -> $package"
    } finally {
        Pop-Location
    }
}

if ($CertificateThumbprint) {
    Export-Certificate -Cert "Cert:\CurrentUser\My\$CertificateThumbprint" -FilePath (Join-Path $Output 'RPi5-test.cer') | Out-Null
}
