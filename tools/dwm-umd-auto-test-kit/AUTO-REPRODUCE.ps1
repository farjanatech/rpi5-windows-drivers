#Requires -Version 5.1
$ErrorActionPreference='Continue'
$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
$Log=Join-Path $WorkRoot 'auto-reproduce.log'

function Write-Log([string]$Text){
    $line='[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'),$Text
    Add-Content -LiteralPath $Log -Value $line -Encoding UTF8
}
if(!(Test-Path $StatePath)){exit 0}
try{$state=Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json}catch{exit 0}
if([string]$state.Stage -ne 'TestArmed'){exit 0}

Start-Sleep -Seconds 20
Write-Log 'Launching Date & time Settings page automatically.'
try{Start-Process 'ms-settings:dateandtime'}catch{Write-Log ('First launch failed: '+$_.Exception.Message)}
try{
    if($state.SessionRoot){
        ('{0},AUTO_SETTINGS_LAUNCH' -f (Get-Date -Format o)) | Add-Content -LiteralPath (Join-Path ([string]$state.SessionRoot) 'USER-MARKERS.csv') -Encoding UTF8
    }
}catch{}

Start-Sleep -Seconds 20
Write-Log 'Launching Date & time Settings page a second time to exercise the DWM/DComp path.'
try{Start-Process 'ms-settings:dateandtime'}catch{Write-Log ('Second launch failed: '+$_.Exception.Message)}

$public=[Environment]::GetFolderPath('CommonDesktopDirectory')
if(-not $public){$public='C:\Users\Public\Desktop'}
@"
Automatic reproduction launched at $(Get-Date -Format o).

The kit opened Date & time Settings twice.
You may also right-click the desktop or File Explorer while the screen remains usable.

No other action is required. The test will auto-finalize and rollback.
"@ | Set-Content -LiteralPath (Join-Path $public 'RPi5-DWM-UMD-AUTO-REPRODUCER-RAN.txt') -Encoding UTF8
