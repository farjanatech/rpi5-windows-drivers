#Requires -Version 5.1
[CmdletBinding()]
param([switch]$Resume)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'

$KitVersion='1.0'
$AppName='RPi5 DWM UMD Auto Test v1.0'
$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
$SetupLog=Join-Path $WorkRoot 'setup.log'
$ResumeTask='Farjanatech RPi5 DWM UMD AutoTest Resume v1'
$BootstrapTask='Farjanatech RPi5 DWM UMD AutoTest Bootstrap v1'

function Test-Admin {
    $id=[Security.Principal.WindowsIdentity]::GetCurrent()
    $p=New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Relaunch-Admin {
    $arg='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'"'
    if($Resume){$arg+=' -Resume'}
    Start-Process powershell.exe -Verb RunAs -ArgumentList $arg
    exit
}
function Write-Log([string]$Text) {
    New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
    $line='[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'),$Text
    Write-Host $line
    Add-Content -LiteralPath $SetupLog -Value $line -Encoding UTF8
}
function Get-PnpValue([string]$Id,[string]$Key) {
    try{return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data}catch{return $null}
}
function Get-Rpi {
    $d=@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue|Where-Object InstanceId -match '^ACPI\\RPI1001(\\|$)'|Select-Object -First 1)
    if(-not $d.Count){return $null}
    $id=[string]$d[0].InstanceId
    return [pscustomobject]@{
        InstanceId=$id;Status=[string]$d[0].Status
        ProblemCode=(Get-PnpValue $id 'DEVPKEY_Device_ProblemCode')
        DriverVersion=(Get-PnpValue $id 'DEVPKEY_Device_DriverVersion')
        Inf=(Get-PnpValue $id 'DEVPKEY_Device_DriverInfPath')
    }
}
function Get-TestSigning {
    $bcd=(& bcdedit.exe /enum '{current}' 2>&1|Out-String)
    return [bool]($bcd -match '(?im)^\s*testsigning\s+Yes\s*$')
}
function Save-State($State) {
    $State | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $StatePath -Encoding UTF8
}
function Load-State {
    if(Test-Path $StatePath){return (Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json)}
    return $null
}
function Register-Resume {
    $ps="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $script=Join-Path $WorkRoot 'AUTO-INSTALL-TEST.ps1'
    $action=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$script+'" -Resume')
    $trigger=New-ScheduledTaskTrigger -AtStartup
    $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $ResumeTask -Action $action -Trigger $trigger -Principal $principal -Force | Out-Null
}
function Remove-Task([string]$Name) {
    try{Unregister-ScheduledTask -TaskName $Name -Confirm:$false -ErrorAction SilentlyContinue}catch{}
}
function Reboot-Resume([string]$Why) {
    Register-Resume
    Write-Log ($Why+' Rebooting in 10 seconds.')
    shutdown.exe /r /t 10 /c "$AppName preparation" /d p:0:0 | Out-Null
    exit
}

if(-not (Test-Admin)){Relaunch-Admin}

$source=Split-Path -Parent $PSCommandPath
if([IO.Path]::GetFullPath($source).TrimEnd('\') -ine [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\')){
    New-Item -ItemType Directory -Path $WorkRoot -Force | Out-Null
    robocopy.exe $source $WorkRoot /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if($LASTEXITCODE -ge 8){throw "Failed to copy auto-test kit (robocopy exit $LASTEXITCODE)"}
    Start-Process powershell.exe -Verb RunAs -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $WorkRoot 'AUTO-INSTALL-TEST.ps1')+'" -Resume')
    exit
}

try {
    Write-Log 'Starting/resuming automatic DWM UMD diagnostic test setup.'
    $state=Load-State
    if($null -eq $state){
        $rpi=Get-Rpi
        if($null -eq $rpi){throw 'ACPI\RPI1001 is not present. Keep Damian Edition RC1 UEFI installed.'}
        $consoleUser=$null
        try{$consoleUser=[string](Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).UserName}catch{}
        if(-not $consoleUser){$consoleUser=[Security.Principal.WindowsIdentity]::GetCurrent().Name}
        $state=[ordered]@{
            Version=$KitVersion
            Stage='Preparing'
            Created=(Get-Date).ToString('o')
            LaunchUser=$consoleUser
            BaselineInf=[string]$rpi.Inf
            BaselineVersion=[string]$rpi.DriverVersion
            TestInf=''
            TestCertThumbprint=''
            TestSigningWasEnabled=(Get-TestSigning)
            SessionRoot=''
            TestBootUtc=''
            TestStartUtc=''
            ResultZip=''
        }
        Save-State $state
        Write-Log ("Baseline: INF={0}; Version={1}; User={2}" -f $state.BaselineInf,$state.BaselineVersion,$state.LaunchUser)
    }

    $bcd=(& bcdedit.exe /enum '{current}' 2>&1|Out-String)
    if($bcd -match '(?im)^\s*truncatememory\s+'){
        Write-Log 'Removing leftover truncatememory before this test.'
        & bcdedit.exe /deletevalue '{current}' truncatememory 2>&1 | Add-Content -LiteralPath $SetupLog -Encoding UTF8
        if($LASTEXITCODE){throw 'Could not remove truncatememory.'}
        $state.Stage='CleanMemoryPendingReboot';Save-State $state
        Reboot-Resume 'Normal Windows memory configuration restored.'
    }

    if(-not (Get-TestSigning)){
        Write-Log 'TESTSIGNING is disabled. Enabling it for the test-signed diagnostic package.'
        & bcdedit.exe /set testsigning on 2>&1 | Add-Content -LiteralPath $SetupLog -Encoding UTF8
        if($LASTEXITCODE){throw 'Could not enable TESTSIGNING. Secure Boot must be disabled.'}
        $state.Stage='TestSigningPendingReboot';Save-State $state
        Reboot-Resume 'TESTSIGNING enabled.'
    }

    Remove-Task $ResumeTask

    $driverDir=Join-Path $WorkRoot 'Payload\Graphics'
    $cert=Join-Path $driverDir 'Farjanatech-RPi5-DWM-UMD-Diagnostic-Test.cer'
    $inf=Join-Path $driverDir 'pi5graphics.inf'
    $cat=Join-Path $driverDir 'pi5graphics.cat'
    foreach($p in @($cert,$inf,$cat,(Join-Path $driverDir 'Pi5D3D.dll'),(Join-Path $driverDir 'Pi5Graphics.sys'))){
        if(!(Test-Path $p)){throw "Diagnostic payload is incomplete: $p"}
    }

    $rootCert=Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\Root
    $pubCert=Import-Certificate -FilePath $cert -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
    $thumb=''
    if($rootCert){$thumb=[string]$rootCert.Thumbprint}
    if(-not $thumb -and $pubCert){$thumb=[string]$pubCert.Thumbprint}
    $state.TestCertThumbprint=$thumb;Save-State $state

    $sig=Get-AuthenticodeSignature -FilePath $cat
    Write-Log ("Catalog signature: {0}; signer={1}" -f $sig.Status,$(if($sig.SignerCertificate){$sig.SignerCertificate.Subject}else{'<none>'}))
    if($sig.Status -ne 'Valid'){throw "Catalog signature is not trusted: $($sig.Status)"}

    $rpi=Get-Rpi
    if($null -eq $rpi){throw 'RPI1001 disappeared before installation.'}
    $instance=[string]$rpi.InstanceId

    Write-Log 'Staging diagnostic Graphics package.'
    $stage=@(& pnputil.exe /add-driver "$inf" 2>&1)
    $exit=$LASTEXITCODE
    $stage | Set-Content -LiteralPath (Join-Path $WorkRoot 'pnputil-stage.txt') -Encoding UTF8
    if($exit -ne 0 -and $exit -ne 259){throw "PnPUtil staging failed with exit $exit"}

    if(-not ('Farjanatech.NewDev' -as [type])){
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Farjanatech {
    public static class NewDev {
        [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool UpdateDriverForPlugAndPlayDevices(
            IntPtr hwndParent, string hardwareId, string fullInfPath,
            uint installFlags, out bool rebootRequired);
    }
}
'@
    }

    $rebootRequired=$false
    Write-Log 'Force-binding the diagnostic INF to ACPI\RPI1001.'
    $ok=[Farjanatech.NewDev]::UpdateDriverForPlugAndPlayDevices(
        [IntPtr]::Zero,'ACPI\RPI1001',[IO.Path]::GetFullPath($inf),0x00000001,[ref]$rebootRequired)
    if(-not $ok){
        $win32=[Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "UpdateDriverForPlugAndPlayDevices failed with Win32 error $win32"
    }

    Start-Sleep -Seconds 3
    $active=Get-Rpi
    if($null -eq $active){throw 'RPI1001 unavailable after force-bind.'}
    if([string]$active.Inf -eq [string]$state.BaselineInf){
        Write-Log 'Driver INF did not change immediately; restarting the RPI1001 devnode once.'
        & pnputil.exe /restart-device "$instance" 2>&1 | Add-Content -LiteralPath $SetupLog -Encoding UTF8
        Start-Sleep -Seconds 5
        $active=Get-Rpi
    }
    if($null -eq $active -or -not $active.Inf){throw 'Could not determine active diagnostic INF.'}
    if([string]$active.Inf -eq [string]$state.BaselineInf){
        throw 'Diagnostic package did not become the selected INF. No test reboot will be attempted.'
    }

    $state.TestInf=[string]$active.Inf
    $state.Stage='DiagnosticInstalledPendingReboot'
    Save-State $state
    Write-Log ("Diagnostic INF selected: {0}; baseline INF remains {1}" -f $state.TestInf,$state.BaselineInf)

    $ps="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $bootstrap=Join-Path $WorkRoot 'TEST-BOOTSTRAP.ps1'
    $action=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$bootstrap+'"')
    $trigger=New-ScheduledTaskTrigger -AtStartup
    $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $BootstrapTask -Action $action -Trigger $trigger -Principal $principal -Force | Out-Null

    Write-Log 'Diagnostic package installed. Automatic test boot begins after reboot.'
    shutdown.exe /r /t 10 /c "$AppName diagnostic test boot" /d p:0:0 | Out-Null
}
catch {
    Write-Log ('SETUP ERROR: '+$_.Exception.ToString())
    Write-Host ''
    Write-Host 'Automatic setup failed. Run EMERGENCY-ROLLBACK.cmd from this kit if the diagnostic INF may have been installed.' -ForegroundColor Yellow
    throw
}
