#Requires -Version 5.1
$ErrorActionPreference='Continue'
$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
if(!(Test-Path $StatePath)){exit 0}
Start-Sleep -Seconds 75
try{$state=Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json}catch{exit 0}
$session=[string]$state.SessionRoot
if($session -and !(Test-Path (Join-Path $session 'FINALIZED.flag'))){
    & "$WorkRoot\FINALIZE-RESULT.ps1" -Force -Reason PostBootRecovery
    Start-Sleep -Seconds 5
}
& "$WorkRoot\ROLLBACK.ps1"
