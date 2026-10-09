#include <windows.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "../native/shader.h"

static uint32_t word(const uint8_t *p)
{
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static bool has_opcode(ID3DBlob *blob, uint32_t wanted)
{
    const auto *p = static_cast<const uint8_t *>(blob->GetBufferPointer());
    const size_t size = blob->GetBufferSize();
    if (size < 32 || std::memcmp(p, "DXBC", 4)) return false;
    uint32_t chunks = word(p + 28);
    if (32ull + uint64_t(chunks) * 4 > size) return false;
    for (uint32_t i = 0; i < chunks; ++i) {
        uint32_t off = word(p + 32 + i * 4);
        if (uint64_t(off) + 8 > size) continue;
        uint32_t fourcc = word(p + off), bytes = word(p + off + 4);
        if (fourcc != 0x52444853u && fourcc != 0x58454853u) continue; // SHDR/SHEX
        if (uint64_t(off) + 8 + bytes > size || (bytes & 3)) return false;
        const auto *tokens = reinterpret_cast<const uint32_t *>(p + off + 8);
        uint32_t words = bytes / 4;
        if (words < 2) return false;
        for (uint32_t at = 2; at < words;) {
            uint32_t token = tokens[at], op = token & 0x7ffu, length = (token >> 24) & 0x7fu;
            if (op == wanted) return true;
            if (!length || length > words - at) return false;
            at += length;
        }
    }
    return false;
}

static int compile_shader(const char *source, bool requireSqrt, uint32_t expectedVaryings)
{
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, "main", "ps_4_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &blob, &errors);
    if (FAILED(hr)) {
        std::printf("D3DCompile failed: %08lx %s\n", static_cast<unsigned long>(hr),
                    errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        if (errors) errors->Release();
        return 1;
    }
    if (errors) errors->Release();
    if (requireSqrt && !has_opcode(blob, 75)) {
        std::puts("FAIL: HLSL compiler did not emit DXBC SQRT opcode 75");
        blob->Release();
        return 2;
    }

    pi5::Shader shader;
    std::string error;
    bool ok = pi5::CompileShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                 pi5::ShaderStage::Pixel, shader, error);
    blob->Release();
    if (!ok) {
        std::printf("Pi5 shader compile failed: %s\n", error.c_str());
        return 3;
    }
    if (shader.varyingScalars != expectedVaryings) {
        std::printf("FAIL: varyingScalars=%u expected=%u\n", shader.varyingScalars, expectedVaryings);
        return 4;
    }
    return 0;
}

int main()
{
    const char *sqrtShader =
        "float4 main(float4 a : TEXCOORD0) : SV_Target { return sqrt(a); }";
    if (int r = compile_shader(sqrtShader, true, 4)) return r;

    const char *varyingShader =
        "struct I {"
        " float4 a0:TEXCOORD0; float4 a1:TEXCOORD1; float4 a2:TEXCOORD2; float4 a3:TEXCOORD3;"
        " float4 a4:TEXCOORD4; float4 a5:TEXCOORD5; float4 a6:TEXCOORD6; float4 a7:TEXCOORD7;"
        "};"
        "float4 main(I i) : SV_Target {"
        " return i.a0+i.a1+i.a2+i.a3+i.a4+i.a5+i.a6+i.a7;"
        "}";
    if (int r = compile_shader(varyingShader, false, 32)) return 10 + r;

    std::puts("Pi5 shader SQRT + 32-varying regression: PASS");
    return 0;
}
