#Requires -Version 5.1
[CmdletBinding()]
param([switch]$Force,[string]$Reason='RebootDetected')

$ErrorActionPreference='Continue'
$ProgressPreference='SilentlyContinue'

$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
$RecorderTask='Farjanatech RPi5 DWM UMD AutoTest Recorder v1'
$FinalizerTask='Farjanatech RPi5 DWM UMD AutoTest Finalizer v1'
$TimeoutTask='Farjanatech RPi5 DWM UMD AutoTest Timeout v1'
$ReproTask='Farjanatech RPi5 DWM UMD AutoTest Reproduce v1'
if(!(Test-Path $StatePath)){exit 0}
try{$state=Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json}catch{exit 0}
$SessionRoot=[string]$state.SessionRoot
if(-not $SessionRoot -or !(Test-Path $SessionRoot)){exit 0}
if(Test-Path (Join-Path $SessionRoot 'FINALIZED.flag')){exit 0}

$mutex=New-Object Threading.Mutex($false,'Global\FarjanatechRPi5DwmUmdAutoFinalizeV1')
if(-not $mutex.WaitOne(0,$false)){exit 0}

function Save-State($State){$State|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $StatePath -Encoding UTF8}
function Get-BootUtc {try{return (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime.ToUniversalTime()}catch{return (Get-Date).ToUniversalTime()}}
function Get-PnpValue([string]$Id,[string]$Key){try{return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data}catch{return $null}}
function Save-Command([string]$Path,[string]$Exe,[string[]]$Args){try{(& $Exe @Args 2>&1)|Out-File -LiteralPath $Path -Encoding UTF8 -Width 4096}catch{$_.Exception.ToString()|Set-Content -LiteralPath $Path -Encoding UTF8}}
function Parse-Wer([string]$Path){
    try{
        $lines=Get-Content -LiteralPath $Path -ErrorAction Stop
        $out=[ordered]@{Path=$Path;Application='';FaultModule='';ExceptionCode='';ExceptionOffset='';ReportIdentifier=''}
        foreach($line in $lines){
            if($line -match '^ReportIdentifier=(.+)$'){$out.ReportIdentifier=$Matches[1]}
            if($line -match '^Sig\[0\]\.Value=(.+)$'){$out.Application=$Matches[1]}
            if($line -match '^Sig\[3\]\.Value=(.+)$'){$out.FaultModule=$Matches[1]}
            if($line -match '^Sig\[6\]\.Value=(.+)$'){$out.ExceptionCode=$Matches[1]}
            if($line -match '^Sig\[7\]\.Value=(.+)$'){$out.ExceptionOffset=$Matches[1]}
        }
        return [pscustomobject]$out
    }catch{return $null}
}

$testBoot=[datetime]::Parse([string]$state.TestBootUtc).ToUniversalTime()
$currentBoot=Get-BootUtc
$isNewBoot=[math]::Abs(($currentBoot-$testBoot).TotalSeconds) -gt 5
if(-not $Force -and -not $isNewBoot){
    try{$mutex.ReleaseMutex()}catch{};try{$mutex.Dispose()}catch{}
    exit 0
}
if($isNewBoot -and -not $Force){Start-Sleep -Seconds 20}

try{Stop-ScheduledTask -TaskName $RecorderTask -ErrorAction SilentlyContinue}catch{}
Start-Sleep -Seconds 2

$collect=Join-Path $SessionRoot 'PostTest-Final'
New-Item -ItemType Directory -Path $collect -Force | Out-Null
$start=[datetime]::Parse([string]$state.TestStartUtc).ToLocalTime().AddMinutes(-2)
$end=Get-Date

try{
    Get-WinEvent -FilterHashtable @{LogName='Application';StartTime=$start;EndTime=$end} -ErrorAction Stop |
        Where-Object {$_.ProviderName -match 'Desktop Window Manager|Dwminit|Application Error|Windows Error Reporting' -or $_.Message -match 'dwm\.exe|8898008d|Pi5D3D|Pi5Graphics'} |
        Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message |
        Export-Csv -LiteralPath (Join-Path $collect 'Application-DWM-Events.csv') -NoTypeInformation -Encoding UTF8
}catch{}
try{
    Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$start;EndTime=$end} -ErrorAction Stop |
        Where-Object {$_.Id -in 41,1074,4101,14,141,117,116,219,225,411,10110,10111 -or $_.ProviderName -match 'Display|Dxg|Kernel-PnP|WHEA|Graphics|Kernel-Power' -or $_.Message -match 'RPI1001|Pi5Graphics|display|GPU'} |
        Select-Object TimeCreated,Id,LevelDisplayName,ProviderName,Message |
        Export-Csv -LiteralPath (Join-Path $collect 'System-Graphics-Events.csv') -NoTypeInformation -Encoding UTF8
}catch{}
foreach($pattern in @('Microsoft-Windows-DxgKrnl/*','Microsoft-Windows-Dwm-Core/*','Microsoft-Windows-Win32k/*')){
    try{
        foreach($log in (Get-WinEvent -ListLog $pattern -ErrorAction SilentlyContinue|Where-Object {$_.IsEnabled})){
            $safe=($log.LogName -replace '[\\/:*?"<>|]','_')
            try{
                Get-WinEvent -FilterHashtable @{LogName=$log.LogName;StartTime=$start;EndTime=$end} -ErrorAction Stop |
                    Select-Object -First 3000 TimeCreated,Id,LevelDisplayName,ProviderName,Message |
                    Export-Csv -LiteralPath (Join-Path $collect ($safe+'.csv')) -NoTypeInformation -Encoding UTF8
            }catch{}
        }
    }catch{}
}

$werOut=Join-Path $collect 'WER-Metadata'
New-Item -ItemType Directory -Path $werOut -Force | Out-Null
$werRows=New-Object System.Collections.Generic.List[object]
foreach($root in @("$env:ProgramData\Microsoft\Windows\WER\ReportQueue","$env:ProgramData\Microsoft\Windows\WER\ReportArchive")){
    if(!(Test-Path $root)){continue}
    try{
        Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Where-Object {$_.Name -like 'AppCrash_dwm.exe*' -and $_.LastWriteTime -ge $start} |
            ForEach-Object {
                $src=Join-Path $_.FullName 'Report.wer'
                if(Test-Path $src){
                    $safe=$_.Name -replace '[^A-Za-z0-9_.-]','_'
                    $dst=Join-Path $werOut ($safe+'-Report.wer')
                    Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction SilentlyContinue
                    $row=Parse-Wer $dst;if($null -ne $row){$werRows.Add($row)}
                }
            }
    }catch{}
}
$liveWer=Join-Path $SessionRoot 'Live-WER'
if(Test-Path $liveWer){
    Get-ChildItem -LiteralPath $liveWer -Filter '*Report.wer' -File -ErrorAction SilentlyContinue | ForEach-Object {
        $dst=Join-Path $werOut $_.Name
        Copy-Item -LiteralPath $_.FullName -Destination $dst -Force -ErrorAction SilentlyContinue
        $row=Parse-Wer $dst
        if($null -ne $row -and -not ($werRows|Where-Object ReportIdentifier -eq $row.ReportIdentifier)){$werRows.Add($row)}
    }
}
$werRows|Export-Csv -LiteralPath (Join-Path $collect 'DWM-WER-SUMMARY.csv') -NoTypeInformation -Encoding UTF8

$umdOut=Join-Path $collect 'Pi5GraphicsDiagnostics'
New-Item -ItemType Directory -Path $umdOut -Force | Out-Null
$diagRoot=Join-Path $env:ProgramData 'Pi5GraphicsDiagnostics'
$testStartUtc=[datetime]::Parse([string]$state.TestStartUtc).ToUniversalTime()
if(Test-Path $diagRoot){
    try{
        Get-ChildItem -LiteralPath $diagRoot -File -ErrorAction SilentlyContinue |
            Where-Object {$_.LastWriteTimeUtc -ge $testStartUtc.AddMinutes(-2)} |
            ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $umdOut $_.Name) -Force -ErrorAction SilentlyContinue}
    }catch{}
}

try{
    $dev=@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue|Where-Object InstanceId -match '^ACPI\\RPI1001(\\|$)'|Select-Object -First 1)
    if($dev.Count){
        $id=[string]$dev[0].InstanceId
        [pscustomobject]@{Status=$dev[0].Status;ProblemCode=(Get-PnpValue $id 'DEVPKEY_Device_ProblemCode');Service=(Get-PnpValue $id 'DEVPKEY_Device_Service');DriverVersion=(Get-PnpValue $id 'DEVPKEY_Device_DriverVersion');Inf=(Get-PnpValue $id 'DEVPKEY_Device_DriverInfPath');InstanceId=$id} |
            Format-List *|Out-File (Join-Path $collect 'RPI1001-FINAL.txt') -Encoding UTF8 -Width 4096
        (& pnputil.exe /enum-devices /instanceid "$id" /drivers 2>&1)|Out-File (Join-Path $collect 'RPI1001-DRIVERS-FINAL.txt') -Encoding UTF8 -Width 4096
    }
}catch{}
try{Get-CimInstance Win32_VideoController|Select-Object Name,Status,PNPDeviceID,DriverVersion,AdapterRAM,VideoProcessor,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate|Format-List *|Out-File (Join-Path $collect 'VIDEO-FINAL.txt') -Encoding UTF8 -Width 4096}catch{}
Save-Command (Join-Path $collect 'BCDEDIT-FINAL.txt') 'bcdedit.exe' @('/enum','{current}')
try{reg.exe query 'HKLM\HARDWARE\Pi5DisplayDiagnostics' /s 2>&1|Out-File (Join-Path $collect 'Pi5DisplayDiagnostics-FINAL.txt') -Encoding UTF8 -Width 4096}catch{}
try{reg.exe query 'HKLM\HARDWARE\Pi5RenderDiagnostics' /s 2>&1|Out-File (Join-Path $collect 'Pi5RenderDiagnostics-FINAL.txt') -Encoding UTF8 -Width 4096}catch{}

foreach($p in @((Join-Path $WorkRoot 'setup.log'),(Join-Path $WorkRoot 'test-bootstrap.log'),(Join-Path $WorkRoot 'auto-reproduce.log'))){
    if(Test-Path $p){Copy-Item -LiteralPath $p -Destination $collect -Force -ErrorAction SilentlyContinue}
}

$summary=New-Object System.Collections.Generic.List[string]
$summary.Add('RPi5 DWM UMD AUTO TEST v1.0')
$summary.Add(('Finalize reason: '+$Reason))
$summary.Add(('New boot detected: '+$isNewBoot))
$summary.Add(('Diagnostic INF: '+[string]$state.TestInf))
$summary.Add(('Baseline INF:   '+[string]$state.BaselineInf))
$summary.Add(('Test start UTC: '+[string]$state.TestStartUtc))
$summary.Add(('Test boot UTC:  '+[string]$state.TestBootUtc))
$summary.Add('')
$hb=Join-Path $SessionRoot '01-LAST-CHECKPOINT.json'
if(Test-Path $hb){
    try{$h=Get-Content -LiteralPath $hb -Raw|ConvertFrom-Json;$summary.Add(('Last checkpoint: '+$h.Timestamp));$summary.Add(('Last DWM PID: '+$h.DwmPid));$summary.Add(('Last RPI1001: Status='+$h.RpiStatus+' ProblemCode='+$h.RpiProblemCode+' INF='+$h.RpiInf))}catch{}
}
$summary.Add(('DWM WER reports captured: '+$werRows.Count))
$noHw=@($werRows|Where-Object {$_.ExceptionCode -match '(?i)^0x?8898008d$'})
if($noHw.Count){$summary.Add(('DWM 0x8898008D reports: '+$noHw.Count))}
$firstFatal=@(Get-ChildItem -LiteralPath $umdOut -Filter 'umd-first-fatal-*.txt' -File -ErrorAction SilentlyContinue|Sort-Object LastWriteTime)
$umdEvents=@(Get-ChildItem -LiteralPath $umdOut -Filter 'umd-event-*.txt' -File -ErrorAction SilentlyContinue|Sort-Object LastWriteTime)
$summary.Add(('Pi5D3D first-fatal files: '+$firstFatal.Count))
$summary.Add(('Pi5D3D unsupported-DXGI event files: '+$umdEvents.Count))
if($firstFatal.Count){
    $summary.Add('')
    $summary.Add('FIRST Pi5D3D FATAL:')
    try{
        foreach($line in (Get-Content -LiteralPath $firstFatal[0].FullName -ErrorAction Stop|Select-Object -First 30)){$summary.Add('  '+$line)}
    }catch{}
}
if($umdEvents.Count){
    $summary.Add('')
    $summary.Add('FIRST unsupported DXGI event:')
    try{foreach($line in (Get-Content -LiteralPath $umdEvents[0].FullName -ErrorAction Stop|Select-Object -First 20)){$summary.Add('  '+$line)}}catch{}
}
$summary.Add('')
$summary.Add('Automatic rollback begins after this result is safely packaged.')
$summary|Set-Content -LiteralPath (Join-Path $SessionRoot 'SUMMARY.txt') -Encoding UTF8

'FINALIZED'|Set-Content -LiteralPath (Join-Path $SessionRoot 'FINALIZED.flag') -Encoding ASCII
$state.Stage='Finalized'
Save-State $state

$public=[Environment]::GetFolderPath('CommonDesktopDirectory')
if(-not $public){$public='C:\Users\Public\Desktop'}
New-Item -ItemType Directory -Path $public -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$out=Join-Path $public ('RPI5-DWM-UMD-AUTO-RESULT-'+$stamp+'.zip')
try{if(Test-Path $out){Remove-Item $out -Force};Compress-Archive -Path (Join-Path $SessionRoot '*') -DestinationPath $out -CompressionLevel Optimal -Force}catch{}
if(Test-Path $out){
    $state.ResultZip=$out
    Save-State $state
    @"
RPi5 DWM UMD automatic diagnostic result is ready:
$out

The kit is proceeding with automatic rollback to the previous graphics package.
Upload this ZIP to ChatGPT.
"@ | Set-Content -LiteralPath (Join-Path $public 'RPi5-DWM-UMD-AUTO-RESULT-READY.txt') -Encoding UTF8
}

foreach($name in @($RecorderTask,$FinalizerTask,$TimeoutTask,$ReproTask)){
    try{Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue}catch{}
}
try{$mutex.ReleaseMutex()}catch{}
try{$mutex.Dispose()}catch{}
