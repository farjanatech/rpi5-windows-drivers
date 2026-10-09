#Requires -Version 5.1
$ErrorActionPreference='Continue'
$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
if(!(Test-Path $StatePath)){exit 0}
& "$WorkRoot\FINALIZE-RESULT.ps1" -Force -Reason AutoTimeout
Start-Sleep -Seconds 5
& "$WorkRoot\ROLLBACK.ps1"
