# SPDX-License-Identifier: BSD-2-Clause-Patent
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Installer,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'Assert-Wddm20Hardware.ps1')
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new trace directory to preserve earlier evidence.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$tracePath = Join-Path (Resolve-Path -LiteralPath $OutputDirectory).Path 'startup.etl'
$session = 'Pi5-Startup-' + [Guid]::NewGuid().ToString('N')
$metadata = [ordered]@{StartUtc=[DateTime]::UtcNow.ToString('o'); Installer=$installerPath; InstallerStartUtc=$null; InstallerEndUtc=$null; StopUtc=$null; Error=$null}
# DriverEvents, DxgKrnl_WDI, Diagnostics and AzureTriageLogging. In particular,
# startup rejection event 494 uses AzureTriageLogging on Windows 22621.
# Avoid Base/Present/Profiler per-frame traffic. Sequential storage preserves
# the beginning if the size limit is reached instead of overwriting startup.
& logman.exe create trace $session -p '{802EC45A-1E99-4B83-9920-87C98277BA9D}' 0x40420400 5 -o $tracePath -f bin -max 128 -ets
if ($LASTEXITCODE) { throw 'Unable to start graphics diagnostic trace.' }
try {
    $metadata.InstallerStartUtc = [DateTime]::UtcNow.ToString('o')
    & $installerPath
} catch {
    $metadata.Error = $_.Exception.Message
    throw
} finally {
    $metadata.InstallerEndUtc = [DateTime]::UtcNow.ToString('o')
    & logman.exe stop $session -ets
    $metadata.StopUtc = [DateTime]::UtcNow.ToString('o')
    $metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'capture.json') -Encoding UTF8
    # The caller must retain its independent recovery task and verify trace
    # timestamps/lost-event statistics before interpreting missing events.
}
