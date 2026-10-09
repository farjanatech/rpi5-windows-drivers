#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]

checks = {
    root / "pi5-graphics/kmd/driver.cpp": [
        'Pi5EvalAcpiInteger(pdo,"_HRV",&silicon)',
        'a->mmioFlips=(NT_SUCCESS(siliconStatus)&&silicon==0)?FALSE:ConsumeFlipTrial(L"MmioFlips",TRUE);',
        'a->display->m_SecondHeadEnabled=a->mmioFlips&&ConsumeFlipTrial(L"DualHead",TRUE);',
        'if(a->mmioFlips||a->siliconRevision==0)',
    ],
    root / "pi5-graphics/kmd/display.inc": [
        'if(a->mmioFlips){NTSTATUS status=DirectScanout(a,r,0,false,a->display->PostSource());',
        'const BOOLEAN c1=a->siliconRevision==0;',
        'CopyGraphicsMemory(dst,src,bytes);KeMemoryBarrier();',
    ],
    root / "pi5-graphics/display/pi5-display-hw.cxx": [
        'const BOOLEAN c1=SiliconRevision==0;',
        'c1?0x4900c007u:0x490cc007u',
        'c1?0x4000fff0u:0x0000fff0u',
        'const ULONG expectedVersion=c1?0x2453u:0x2454u;',
    ],
    root / "pi5-graphics/kmd/gpu.cpp": [
        'v3dRevision=(status.HubIdent[3]>>8)&255u;',
        'if(status.TechVersion!=71)return STATUS_NOT_SUPPORTED;',
        'draw.v3dRevision=v3dRevision;',
    ],
    root / "pi5-graphics/native/encode.cpp": [
        'if(item.v3dRevision>=10)',
        'w.Bits(13,1,1);',
        'w.Bits(21,1,1);',
        'w.Bits(15,1,item.varyingScalars!=0);',
        'w.Bits(9,1,1);',
        'w.Bits(18,1,1);',
        'w.Bits(12,1,item.varyingScalars!=0);',
    ],
}

failed = False
for path, needles in checks.items():
    text = path.read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            print(f"FAIL: {path.relative_to(root)} missing invariant: {needle}")
            failed = True

if failed:
    sys.exit(1)

print("Universal C1/D0 source invariants: PASS")
print("  C1 _HRV=0 -> proven linear-primary/copy presentation path")
print("  D0 _HRV=1 -> Damian direct-MMIO path")
print("  HVS C1/D0 raster words preserved")
print("  V3D shader-state record selected by HUB_IDENT3 revision")
