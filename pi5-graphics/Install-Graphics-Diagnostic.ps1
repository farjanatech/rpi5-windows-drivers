[CmdletBinding()]
param()
$ErrorActionPreference='Stop'

function Test-Admin {
    $id=[Security.Principal.WindowsIdentity]::GetCurrent()
    $p=New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if(-not (Test-Admin)){
    $arg='-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"'
    Start-Process powershell.exe -Verb RunAs -ArgumentList $arg
    exit
}

$here=Split-Path -Parent $PSCommandPath
$cert=Join-Path $here 'Farjanatech-RPi5-Damian-Edition-Test.cer'
$inf=Join-Path $here 'pi5graphics.inf'
if(!(Test-Path $cert)){throw "Missing certificate: $cert"}
if(!(Test-Path $inf)){throw "Missing INF: $inf"}

$bcd=(& bcdedit /enum '{current}' 2>&1 | Out-String)
if($bcd -notmatch '(?im)^\s*testsigning\s+Yes\s*$'){
    Write-Warning 'TESTSIGNING is not enabled. Run: bcdedit /set testsigning on ; reboot; then rerun this installer.'
}

Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

Write-Host 'Installing Pi5Graphics C1/D0 diagnostic package...' -ForegroundColor Cyan
& pnputil.exe /add-driver $inf /install
if($LASTEXITCODE){throw "pnputil failed with exit code $LASTEXITCODE"}

Write-Host ''
Write-Host 'Installation requested successfully.' -ForegroundColor Green
Write-Host 'Reboot Windows, then run the display-handoff diagnostic utility again.'
