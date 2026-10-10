#Requires -Version 5.1
$ErrorActionPreference='Continue'
$ProgressPreference='SilentlyContinue'

$WorkRoot=Join-Path $env:ProgramData 'Farjanatech\RPi5-DWM-UMD-AutoTest-v1.0'
$StatePath=Join-Path $WorkRoot 'kit-state.json'
if(!(Test-Path $StatePath)){exit 0}
try{$state=Get-Content -LiteralPath $StatePath -Raw|ConvertFrom-Json}catch{exit 0}
$SessionRoot=[string]$state.SessionRoot
if(-not $SessionRoot -or !(Test-Path $SessionRoot)){exit 0}

$RuntimeCsv=Join-Path $SessionRoot '01-RUNTIME.csv'
$TraceChanges=Join-Path $SessionRoot '02-TRACE-CHANGES.csv'
$Heartbeat=Join-Path $SessionRoot '01-LAST-CHECKPOINT.json'
$DwmTransitions=Join-Path $SessionRoot '03-DWM-TRANSITIONS.txt'
$DbwinLog=Join-Path $SessionRoot '04-DBWIN-Pi5D3D.txt'
$LiveWer=Join-Path $SessionRoot 'Live-WER'
$RecorderLog=Join-Path $SessionRoot 'RECORDER.log'
New-Item -ItemType Directory -Path $LiveWer -Force | Out-Null

$mutex=New-Object Threading.Mutex($false,'Global\FarjanatechRPi5DwmUmdAutoRecorderV1')
if(-not $mutex.WaitOne(0,$false)){exit 0}

function Append-Utf8([string]$Path,[string]$Line){
    try{[IO.File]::AppendAllText($Path,$Line+[Environment]::NewLine,[Text.UTF8Encoding]::new($false))}catch{}
}
function Write-Log([string]$Text){Append-Utf8 $RecorderLog ('[{0}] {1}' -f (Get-Date -Format o),$Text)}
function Write-Atomic([string]$Path,[string]$Text){
    try{$tmp=$Path+'.tmp';[IO.File]::WriteAllText($tmp,$Text,[Text.UTF8Encoding]::new($false));Move-Item -LiteralPath $tmp -Destination $Path -Force}catch{}
}
function Get-PnpValue([string]$Id,[string]$Key){try{return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data}catch{return $null}}
function Get-RpiSnapshot {
    $d=@(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue|Where-Object InstanceId -match '^ACPI\\RPI1001(\\|$)'|Select-Object -First 1)
    if(-not $d.Count){return [ordered]@{Status='Missing';ProblemCode='';DriverVersion='';Inf=''}}
    $id=[string]$d[0].InstanceId
    return [ordered]@{Status=[string]$d[0].Status;ProblemCode=(Get-PnpValue $id 'DEVPKEY_Device_ProblemCode');DriverVersion=(Get-PnpValue $id 'DEVPKEY_Device_DriverVersion');Inf=(Get-PnpValue $id 'DEVPKEY_Device_DriverInfPath')}
}
function Get-RegBytes([string]$Path,[string]$Name){
    try{return [byte[]](Get-ItemPropertyValue -Path $Path -Name $Name -ErrorAction Stop)}catch{return $null}
}
function Get-Sha([byte[]]$Bytes){
    if($null -eq $Bytes){return ''}
    try{$sha=[Security.Cryptography.SHA256]::Create();return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant()}catch{return ''}
}
function Trace-Record([byte[]]$Bytes,[int]$Offset){
    if($null -eq $Bytes -or $Offset+15 -ge $Bytes.Length){return $null}
    return [pscustomobject]@{
        Id=[BitConverter]::ToUInt32($Bytes,$Offset)
        Status=[BitConverter]::ToUInt32($Bytes,$Offset+4)
        A=[BitConverter]::ToUInt32($Bytes,$Offset+8)
        B=[BitConverter]::ToUInt32($Bytes,$Offset+12)
    }
}
function Record-TraceChange([string]$Name,[byte[]]$Bytes,[byte[]]$Previous,[string]$Now){
    if($null -eq $Bytes){return}
    for($off=0;$off+15 -lt $Bytes.Length;$off+=16){
        $changed=$true
        if($null -ne $Previous -and $off+15 -lt $Previous.Length){
            $changed=$false
            for($j=0;$j -lt 16;$j++){if($Bytes[$off+$j] -ne $Previous[$off+$j]){$changed=$true;break}}
        }
        if($changed){
            $r=Trace-Record $Bytes $off
            if($null -ne $r){
                Append-Utf8 $TraceChanges ('{0},{1},{2},{3},0x{4:X8},0x{5:X8},0x{6:X8},{5},{6}' -f $Name,$Now,[int]($off/16),$r.Id,$r.Status,$r.A,$r.B)
            }
        }
    }
}
function Capture-LiveWer([datetime]$StartUtc){
    foreach($root in @("$env:ProgramData\Microsoft\Windows\WER\ReportQueue","$env:ProgramData\Microsoft\Windows\WER\ReportArchive")){
        if(!(Test-Path $root)){continue}
        try{
            Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
                Where-Object {$_.Name -like 'AppCrash_dwm.exe*' -and $_.LastWriteTimeUtc -ge $StartUtc.AddMinutes(-2)} |
                ForEach-Object {
                    $src=Join-Path $_.FullName 'Report.wer'
                    if(Test-Path $src){
                        $safe=$_.Name -replace '[^A-Za-z0-9_.-]','_'
                        $dst=Join-Path $LiveWer ($safe+'-Report.wer')
                        if(!(Test-Path $dst)){Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction SilentlyContinue}
                    }
                }
        }catch{}
    }
}

try{
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Rpi5MemoryStatusAuto {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Auto)]
    public class MEMORYSTATUSEX {
        public uint dwLength=(uint)Marshal.SizeOf(typeof(MEMORYSTATUSEX));
        public uint dwMemoryLoad; public ulong ullTotalPhys; public ulong ullAvailPhys;
        public ulong ullTotalPageFile; public ulong ullAvailPageFile;
        public ulong ullTotalVirtual; public ulong ullAvailVirtual; public ulong ullAvailExtendedVirtual;
    }
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GlobalMemoryStatusEx([In,Out] MEMORYSTATUSEX b);
    public static MEMORYSTATUSEX Get(){var x=new MEMORYSTATUSEX();if(!GlobalMemoryStatusEx(x))throw new System.ComponentModel.Win32Exception();return x;}
}
'@ -ErrorAction Stop
}catch{}

try{
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading;
public static class Rpi5DbwinAuto {
    static Thread worker; static volatile bool running; static string path;
    public static void Start(string output){if(running)return;path=output;running=true;worker=new Thread(Run);worker.IsBackground=true;worker.Start();}
    public static void Stop(){running=false;if(worker!=null&&worker.IsAlive)worker.Join(3000);}
    static void Run(){
        try{
            var world=new SecurityIdentifier(WellKnownSidType.WorldSid,null);
            var es=new EventWaitHandleSecurity();es.AddAccessRule(new EventWaitHandleAccessRule(world,EventWaitHandleRights.FullControl,AccessControlType.Allow));
            bool created;
            using(var bufferReady=new EventWaitHandle(false,EventResetMode.AutoReset,"DBWIN_BUFFER_READY",out created,es))
            using(var dataReady=new EventWaitHandle(false,EventResetMode.AutoReset,"DBWIN_DATA_READY",out created,es)){
                var ms=new MemoryMappedFileSecurity();ms.AddAccessRule(new AccessRule<MemoryMappedFileRights>(world,MemoryMappedFileRights.FullControl,AccessControlType.Allow));
                using(var map=MemoryMappedFile.CreateOrOpen("DBWIN_BUFFER",4096,MemoryMappedFileAccess.ReadWrite,MemoryMappedFileOptions.None,ms,HandleInheritability.None))
                using(var view=map.CreateViewAccessor(0,4096,MemoryMappedFileAccess.Read)){
                    bufferReady.Set();
                    while(running){
                        if(!dataReady.WaitOne(500))continue;
                        int pid=view.ReadInt32(0);byte[] bytes=new byte[4092];view.ReadArray(4,bytes,0,bytes.Length);
                        int n=Array.IndexOf<byte>(bytes,0);if(n<0)n=bytes.Length;
                        string msg=System.Text.Encoding.Default.GetString(bytes,0,n);string name="";
                        try{name=System.Diagnostics.Process.GetProcessById(pid).ProcessName;}catch{}
                        string line=DateTime.Now.ToString("o")+" PID="+pid+" PROC="+name+" "+msg.Replace("\r","\\r").Replace("\n","\\n");
                        lock(typeof(Rpi5DbwinAuto)){File.AppendAllText(path,line+Environment.NewLine);}
                        bufferReady.Set();
                    }
                }
            }
        }catch(Exception ex){try{File.AppendAllText(path,DateTime.Now.ToString("o")+" DBWIN_ERROR "+ex+Environment.NewLine);}catch{}}
    }
}
'@ -ReferencedAssemblies @('System.Core.dll','System.Security.dll') -ErrorAction Stop
[Rpi5DbwinAuto]::Start($DbwinLog)
}catch{Write-Log ('DBWIN setup failed: '+$_.Exception.Message)}

if(!(Test-Path $RuntimeCsv)){
    'Timestamp,DwmPid,DwmWorkingSetMB,DwmPrivateMB,DwmCpuSeconds,DwmResponding,AvailableMemoryMB,PageFileUsedMB,RpiStatus,RpiProblemCode,RpiDriverVersion,RpiInf,DisplayTraceBytes,DisplayTraceSha256,RenderTraceBytes,RenderTraceSha256' | Set-Content -LiteralPath $RuntimeCsv -Encoding ASCII
}
if(!(Test-Path $TraceChanges)){
    'TraceName,Timestamp,Index,ID,Status,A,B,ADecimal,BDecimal' | Set-Content -LiteralPath $TraceChanges -Encoding ASCII
}

$startUtc=[datetime]::Parse([string]$state.TestStartUtc).ToUniversalTime()
$deadline=(Get-Date).AddMinutes(20)
$previousDisplay=$null;$previousRender=$null;$previousStart=$null
$previousDisplayHash='';$previousRenderHash='';$previousStartHash=''
$lastDwmPid=-1;$lastRpi=(Get-Date).AddMinutes(-10);$rpi=Get-RpiSnapshot;$lastWer=(Get-Date).AddMinutes(-10)
Write-Log 'Recorder started.'

try{
    while((Get-Date) -lt $deadline){
        if(Test-Path (Join-Path $SessionRoot 'FINALIZED.flag')){break}
        $now=(Get-Date).ToString('o')
        $dwm=Get-Process dwm -ErrorAction SilentlyContinue|Select-Object -First 1
        $pid=0;$ws=0;$priv=0;$cpu=0;$responding=$false
        if($dwm){
            $pid=[int]$dwm.Id
            try{$ws=[math]::Round($dwm.WorkingSet64/1MB,1)}catch{}
            try{$priv=[math]::Round($dwm.PrivateMemorySize64/1MB,1)}catch{}
            try{$cpu=[math]::Round($dwm.CPU,3)}catch{}
            try{$responding=[bool]$dwm.Responding}catch{}
        }
        if($pid -ne $lastDwmPid){Append-Utf8 $DwmTransitions ('{0} DWM_PID {1} -> {2}' -f $now,$lastDwmPid,$pid);$lastDwmPid=$pid}

        $avail=-1;$pageUsed=-1
        try{$m=[Rpi5MemoryStatusAuto]::Get();$avail=[math]::Round($m.ullAvailPhys/1MB,1);$pageUsed=[math]::Round(($m.ullTotalPageFile-$m.ullAvailPageFile)/1MB,1)}catch{}

        if(((Get-Date)-$lastRpi).TotalSeconds -ge 5){$rpi=Get-RpiSnapshot;$lastRpi=Get-Date}

        $start=Get-RegBytes 'HKLM:\HARDWARE\Pi5DisplayDiagnostics' 'Start'
        $display=Get-RegBytes 'HKLM:\HARDWARE\Pi5DisplayDiagnostics' 'Trace'
        $render=Get-RegBytes 'HKLM:\HARDWARE\Pi5RenderDiagnostics' 'Trace'
        $hs=Get-Sha $start;$hd=Get-Sha $display;$hr=Get-Sha $render

        if($hs -and $hs -ne $previousStartHash){Record-TraceChange 'Display.Start' $start $previousStart $now;$previousStart=$start;$previousStartHash=$hs}
        if($hd -and $hd -ne $previousDisplayHash){Record-TraceChange 'Display.Trace' $display $previousDisplay $now;$previousDisplay=$display;$previousDisplayHash=$hd}
        if($hr -and $hr -ne $previousRenderHash){Record-TraceChange 'Render.Trace' $render $previousRender $now;$previousRender=$render;$previousRenderHash=$hr}

        if(((Get-Date)-$lastWer).TotalSeconds -ge 10){Capture-LiveWer $startUtc;$lastWer=Get-Date}

        Append-Utf8 $RuntimeCsv ('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15}' -f $now,$pid,$ws,$priv,$cpu,$responding,$avail,$pageUsed,$rpi.Status,$rpi.ProblemCode,$rpi.DriverVersion,$rpi.Inf,$(if($display){$display.Length}else{0}),$hd,$(if($render){$render.Length}else{0}),$hr)
        $checkpoint=[ordered]@{Timestamp=$now;DwmPid=$pid;DwmWorkingSetMB=$ws;DwmPrivateMB=$priv;DwmResponding=$responding;AvailableMemoryMB=$avail;PageFileUsedMB=$pageUsed;RpiStatus=$rpi.Status;RpiProblemCode=$rpi.ProblemCode;RpiDriverVersion=$rpi.DriverVersion;RpiInf=$rpi.Inf;DisplayTraceSha256=$hd;RenderTraceSha256=$hr}
        Write-Atomic $Heartbeat ($checkpoint|ConvertTo-Json -Compress)
        Start-Sleep -Seconds 1
    }
}catch{Write-Log ('RECORDER ERROR: '+$_.Exception.ToString())}
finally{
    try{[Rpi5DbwinAuto]::Stop()}catch{}
    Write-Log 'Recorder stopped.'
    try{$mutex.ReleaseMutex()}catch{}
    try{$mutex.Dispose()}catch{}
}
