#Requires -Version 5.1
[CmdletBinding()]
param([switch]$Emergency)

$ErrorActionPreference='Continue'
$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
$Log=Join-Path $WorkRoot 'rollback.log'
$VerifyTask='Farjanatech RPi5 DWM UMD AutoTest VerifyRollback v1'
$Tasks=@(
'Farjanatech RPi5 DWM UMD AutoTest Resume v1',
'Farjanatech RPi5 DWM UMD AutoTest Bootstrap v1',
'Farjanatech RPi5 DWM UMD AutoTest Recorder v1',
'Farjanatech RPi5 DWM UMD AutoTest Finalizer v1',
'Farjanatech RPi5 DWM UMD AutoTest Timeout v1',
'Farjanatech RPi5 DWM UMD AutoTest Reproduce v1',
'Farjanatech RPi5 DWM UMD AutoTest PostBootRollback v1'
)

function Test-Admin {
    $id=[Security.Principal.WindowsIdentity]::GetCurrent()
    $p=New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if(-not (Test-Admin)){
    $arg='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'"'
    if($Emergency){$arg+=' -Emergency'}
    Start-Process powershell.exe -Verb RunAs -ArgumentList $arg
    exit
}
function Write-Log([string]$Text){
    New-Item -ItemType Directory -Path $WorkRoot -Force|Out-Null
    $line='[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'),$Text
    Add-Content -LiteralPath $Log -Value $line -Encoding UTF8
}
function Save-State($State){try{$State|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $StatePath -Encoding UTF8}catch{}}
function Get-PnpValue([string]$Id,[string]$Key){try{return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data}catch{return $null}}

if(!(Test-Path $StatePath)){Write-Log 'No kit state exists; nothing can be rolled back automatically.';exit 0}
try{$state=Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json}catch{Write-Log 'Kit state is unreadable.';exit 1}

$testInf=[string]$state.TestInf
$baselineInf=[string]$state.BaselineInf
Write-Log ("Rollback starting. TestInf={0}; BaselineInf={1}; Emergency={2}" -f $testInf,$baselineInf,$Emergency)

if($testInf -and $testInf -ine $baselineInf){
    $out=@(& pnputil.exe /delete-driver $testInf /uninstall /force 2>&1)
    $out|Add-Content -LiteralPath $Log -Encoding UTF8
    Write-Log ("Delete-driver exit code: "+$LASTEXITCODE)
}else{
    Write-Log 'Test INF is empty or equals baseline INF; refusing to delete the baseline package.'
}

try{
    $dev=@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue|Where-Object InstanceId -match '^ACPI\\RPI1001(\\|$)'|Select-Object -First 1)
    if($dev.Count){
        $id=[string]$dev[0].InstanceId
        & pnputil.exe /scan-devices 2>&1|Add-Content -LiteralPath $Log -Encoding UTF8
        & pnputil.exe /restart-device "$id" 2>&1|Add-Content -LiteralPath $Log -Encoding UTF8
        Start-Sleep -Seconds 3
        $active=(Get-PnpValue $id 'DEVPKEY_Device_DriverInfPath')
        Write-Log ("Active INF after uninstall/restart attempt: "+$active)
    }
}catch{}

$thumb=[string]$state.TestCertThumbprint
if($thumb){
    foreach($store in @('Cert:\LocalMachine\TrustedPublisher','Cert:\LocalMachine\Root')){
        try{
            Get-ChildItem $store -ErrorAction SilentlyContinue|Where-Object Thumbprint -eq $thumb|Remove-Item -Force -ErrorAction SilentlyContinue
            Write-Log ("Removed diagnostic certificate from "+$store)
        }catch{}
    }
}

if(-not [bool]$state.TestSigningWasEnabled){
    Write-Log 'Restoring original TESTSIGNING=off state.'
    & bcdedit.exe /set testsigning off 2>&1|Add-Content -LiteralPath $Log -Encoding UTF8
}

foreach($name in $Tasks){
    try{Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue}catch{}
    try{Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue}catch{}
}

$state.Stage='RollbackRequested'
Save-State $state

$ps="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$verify=Join-Path $WorkRoot 'VERIFY-ROLLBACK.ps1'
if(Test-Path $verify){
    try{
        $action=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$verify+'"')
        $trigger=New-ScheduledTaskTrigger -AtStartup
        $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
        Register-ScheduledTask -TaskName $VerifyTask -Action $action -Trigger $trigger -Principal $principal -Force|Out-Null
    }catch{}
}

Write-Log 'Rollback requested. Rebooting in 15 seconds.'
shutdown.exe /r /t 15 /c 'RPi5 DWM UMD automatic diagnostic rollback' /d p:0:0|Out-Null
