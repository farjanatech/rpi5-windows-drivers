#Requires -Version 5.1
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'

$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
$Log=Join-Path $WorkRoot 'test-bootstrap.log'
$BootstrapTask='Farjanatech RPi5 DWM UMD AutoTest Bootstrap v1'
$RecorderTask='Farjanatech RPi5 DWM UMD AutoTest Recorder v1'
$FinalizerTask='Farjanatech RPi5 DWM UMD AutoTest Finalizer v1'
$TimeoutTask='Farjanatech RPi5 DWM UMD AutoTest Timeout v1'
$ReproTask='Farjanatech RPi5 DWM UMD AutoTest Reproduce v1'
$PostBootRollbackTask='Farjanatech RPi5 DWM UMD AutoTest PostBootRollback v1'

function Write-Log([string]$Text){
    $line='[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'),$Text
    Add-Content -LiteralPath $Log -Value $line -Encoding UTF8
}
function Get-PnpValue([string]$Id,[string]$Key){
    try{return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data}catch{return $null}
}
function Get-Rpi {
    $d=@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue|Where-Object InstanceId -match '^ACPI\\RPI1001(\\|$)'|Select-Object -First 1)
    if(-not $d.Count){return $null}
    $id=[string]$d[0].InstanceId
    return [pscustomobject]@{InstanceId=$id;Status=[string]$d[0].Status;ProblemCode=(Get-PnpValue $id 'DEVPKEY_Device_ProblemCode');DriverVersion=(Get-PnpValue $id 'DEVPKEY_Device_DriverVersion');Inf=(Get-PnpValue $id 'DEVPKEY_Device_DriverInfPath')}
}
function Save-State($State){$State|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $StatePath -Encoding UTF8}
function Remove-Task([string]$Name){try{Unregister-ScheduledTask -TaskName $Name -Confirm:$false -ErrorAction SilentlyContinue}catch{}}

if(!(Test-Path $StatePath)){exit 2}
$state=Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json
Start-Sleep -Seconds 12
$rpi=Get-Rpi
if($null -eq $rpi){
    Write-Log 'RPI1001 is missing on diagnostic boot.'
    & "$WorkRoot\ROLLBACK.ps1" -Emergency
    exit 3
}
Write-Log ("Diagnostic boot: Status={0}; ProblemCode={1}; Version={2}; INF={3}" -f $rpi.Status,$rpi.ProblemCode,$rpi.DriverVersion,$rpi.Inf)
if([string]$rpi.Inf -ine [string]$state.TestInf){
    Write-Log ("TEST INVALID: expected INF {0}, observed {1}" -f $state.TestInf,$rpi.Inf)
    & "$WorkRoot\ROLLBACK.ps1" -Emergency
    exit 4
}

Remove-Task $BootstrapTask

$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$sessionRoot=Join-Path $WorkRoot ('Sessions\'+$stamp+'-'+([guid]::NewGuid().ToString('N').Substring(0,8)))
New-Item -ItemType Directory -Path $sessionRoot -Force | Out-Null
$bootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
$startUtc=(Get-Date).ToUniversalTime().ToString('o')
$state.SessionRoot=$sessionRoot
$state.TestBootUtc=$bootUtc
$state.TestStartUtc=$startUtc
$state.Stage='TestArmed'
Save-State $state

$state|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $sessionRoot '00-KIT-STATE.json') -Encoding UTF8
try{Get-CimInstance Win32_OperatingSystem|Select-Object Caption,Version,BuildNumber,LastBootUpTime,TotalVisibleMemorySize,FreePhysicalMemory|Format-List *|Out-File (Join-Path $sessionRoot '00-OS.txt') -Encoding UTF8 -Width 4096}catch{}
try{Get-CimInstance Win32_VideoController|Select-Object Name,Status,PNPDeviceID,DriverVersion,AdapterRAM,VideoProcessor,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate|Format-List *|Out-File (Join-Path $sessionRoot '00-VIDEO.txt') -Encoding UTF8 -Width 4096}catch{}
try{(& bcdedit.exe /enum '{current}' 2>&1)|Out-File (Join-Path $sessionRoot '00-BCDEDIT.txt') -Encoding UTF8 -Width 4096}catch{}
try{(& pnputil.exe /enum-devices /instanceid "$($rpi.InstanceId)" /drivers 2>&1)|Out-File (Join-Path $sessionRoot '00-RPI1001-DRIVERS.txt') -Encoding UTF8 -Width 4096}catch{}

$diagRoot=Join-Path $env:ProgramData 'Pi5GraphicsDiagnostics'
New-Item -ItemType Directory -Path $diagRoot -Force | Out-Null
@"
This marker separates the automatic DWM UMD diagnostic run.
Test start UTC: $startUtc
Expected diagnostic INF: $($state.TestInf)
Baseline INF: $($state.BaselineInf)
"@ | Set-Content -LiteralPath (Join-Path $diagRoot ('AUTO-TEST-START-'+$stamp+'.txt')) -Encoding UTF8

$ps="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 1) -MultipleInstances IgnoreNew

$recAction=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $WorkRoot 'RECORDER.ps1')+'"')
$startup=New-ScheduledTaskTrigger -AtStartup
Register-ScheduledTask -TaskName $RecorderTask -Action $recAction -Trigger $startup -Principal $principal -Settings $settings -Force | Out-Null

$finAction=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $WorkRoot 'FINALIZE-RESULT.ps1')+'" -Reason RebootDetected')
Register-ScheduledTask -TaskName $FinalizerTask -Action $finAction -Trigger $startup -Principal $principal -Settings $settings -Force | Out-Null

$postAction=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $WorkRoot 'POSTBOOT-ROLLBACK.ps1')+'"')
Register-ScheduledTask -TaskName $PostBootRollbackTask -Action $postAction -Trigger $startup -Principal $principal -Settings $settings -Force | Out-Null

$timeoutAt=(Get-Date).AddMinutes(10)
$timeoutTrigger=New-ScheduledTaskTrigger -Once -At $timeoutAt
$timeoutAction=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+(Join-Path $WorkRoot 'TIMEOUT-FINALIZE-ROLLBACK.ps1')+'"')
Register-ScheduledTask -TaskName $TimeoutTask -Action $timeoutAction -Trigger $timeoutTrigger -Principal $principal -Settings $settings -Force | Out-Null

$launchUser=[string]$state.LaunchUser
if($launchUser){
    $reproAction=New-ScheduledTaskAction -Execute $ps -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+(Join-Path $WorkRoot 'AUTO-REPRODUCE.ps1')+'"')
    $reproTrigger=New-ScheduledTaskTrigger -AtLogOn -User $launchUser
    $reproPrincipal=New-ScheduledTaskPrincipal -UserId $launchUser -LogonType Interactive -RunLevel Highest
    Register-ScheduledTask -TaskName $ReproTask -Action $reproAction -Trigger $reproTrigger -Principal $reproPrincipal -Force | Out-Null
}

Start-ScheduledTask -TaskName $RecorderTask
Write-Log 'Recorder started and automatic finalization/rollback tasks registered.'

$public=[Environment]::GetFolderPath('CommonDesktopDirectory')
if(-not $public){$public='C:\Users\Public\Desktop'}
New-Item -ItemType Directory -Path $public -Force | Out-Null
@"
RPi5 DWM UMD AUTO TEST v1.0 IS ACTIVE

The diagnostic Pi5D3D package is active.
The recorder is already running.

After you log in, the kit will automatically open:
  Settings -> Time & language -> Date & time

If the display turns black, you do not need to launch anything.
- If Windows still runs, the kit will auto-finalize and rollback within 10 minutes.
- If you reboot/power-cycle first, startup finalization will collect the evidence and then rollback automatically.

The result ZIP will remain on the Public Desktop after rollback.
"@ | Set-Content -LiteralPath (Join-Path $public 'RPi5-DWM-UMD-AUTO-TEST-ACTIVE.txt') -Encoding UTF8

# If the intended user is already logged on by the time startup work completes,
# start the one-shot reproducer immediately; otherwise its AtLogOn trigger will run it.
try{
    $current=[string](Get-CimInstance Win32_ComputerSystem).UserName
    if($current -and $launchUser -and $current -ieq $launchUser){
        Start-ScheduledTask -TaskName $ReproTask -ErrorAction SilentlyContinue
    }
}catch{}
