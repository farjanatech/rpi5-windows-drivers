#Requires -Version 5.1
$ErrorActionPreference='Continue'
$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
$TaskName='Farjanatech RPi5 DWM UMD AutoTest VerifyRollback v1'
if(!(Test-Path $StatePath)){exit 0}
Start-Sleep -Seconds 20
try{$state=Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json}catch{exit 0}
function Get-PnpValue([string]$Id,[string]$Key){
    try{return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data}catch{return $null}
}
$dev=@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue|Where-Object InstanceId -match '^ACPI\\RPI1001(\\|$)'|Select-Object -First 1)
$active=''
$status='RPI1001 missing'
if($dev.Count){
    $id=[string]$dev[0].InstanceId
    $active=[string](Get-PnpValue $id 'DEVPKEY_Device_DriverInfPath')
    $status='Status='+$dev[0].Status+' ProblemCode='+(Get-PnpValue $id 'DEVPKEY_Device_ProblemCode')+' Version='+(Get-PnpValue $id 'DEVPKEY_Device_DriverVersion')
}
$restored=$false
if($active -and $active -ine [string]$state.TestInf){$restored=$true}
if($restored){$state.Stage='RollbackVerified'}else{$state.Stage='RollbackNeedsAttention'}
try{$state|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $StatePath -Encoding UTF8}catch{}
$public=[Environment]::GetFolderPath('CommonDesktopDirectory')
if(-not $public){$public='C:\Users\Public\Desktop'}
$note=@(
'RPi5 DWM UMD AUTO TEST - ROLLBACK VERIFICATION',
'',
('Rollback verified: '+$restored),
('Active INF:       '+$active),
('Diagnostic INF:   '+[string]$state.TestInf),
('Baseline INF:     '+[string]$state.BaselineInf),
('Device:           '+$status),
'',
'Result ZIP:',
([string]$state.ResultZip),
'',
'Upload the result ZIP to ChatGPT.',
'If Rollback verified is False, run EMERGENCY-ROLLBACK.cmd from the original kit.'
)
$note|Set-Content -LiteralPath (Join-Path $public 'RPi5-DWM-UMD-AUTO-TEST-COMPLETE.txt') -Encoding UTF8
try{Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue}catch{}
