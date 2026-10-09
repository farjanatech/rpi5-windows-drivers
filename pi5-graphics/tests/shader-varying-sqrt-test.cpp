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

static int compile_shader(const char *source, uint32_t requiredOpcode, uint32_t expectedVaryings)
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
    if (requiredOpcode && !has_opcode(blob, requiredOpcode)) {
        std::printf("FAIL: HLSL compiler did not emit required DXBC opcode %u\n", requiredOpcode);
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


static int compile_vertex_pressure_shader()
{
    const char *source =
        "cbuffer C : register(b0) { float4 k; };"
        "struct I {"
        " float4 a0:TEXCOORD0; float4 a1:TEXCOORD1; float4 a2:TEXCOORD2; float4 a3:TEXCOORD3;"
        " float4 a4:TEXCOORD4; float4 a5:TEXCOORD5; float4 a6:TEXCOORD6; float4 a7:TEXCOORD7;"
        "};"
        "struct O { float4 pos:SV_Position; float4 t:TEXCOORD0; };"
        "O main(I i) {"
        " O o;"
        " float4 s=i.a0+i.a1+i.a2+i.a3+i.a4+i.a5+i.a6+i.a7+k;"
        " o.pos=float4(s.xy,0.0,1.0);"
        " o.t=s;"
        " return o;"
        "}";

    ID3DBlob *blob=nullptr,*errors=nullptr;
    HRESULT hr=D3DCompile(source,std::strlen(source),"vertex-pressure",nullptr,nullptr,"main","vs_4_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL0,0,&blob,&errors);
    if(FAILED(hr)){
        std::printf("D3DCompile vertex-pressure failed: %08lx %s\n",static_cast<unsigned long>(hr),
                    errors?static_cast<const char*>(errors->GetBufferPointer()):"");
        if(errors)errors->Release();
        return 1;
    }
    if(errors)errors->Release();

    pi5::Shader shader;
    std::string error;
    bool ok=pi5::CompileShader(blob->GetBufferPointer(),blob->GetBufferSize(),
                               pi5::ShaderStage::Vertex,shader,error);
    blob->Release();
    if(!ok){
        std::printf("Pi5 linked-style vertex compile failed: %s\n",error.c_str());
        return 2;
    }
    unsigned scalars=0;
    for(auto mask:shader.inputs)for(unsigned c=0;c<4;++c)scalars+=(mask>>c)&1u;
    if(scalars!=32){
        std::printf("FAIL: vertex input scalars=%u expected=32\n",scalars);
        return 3;
    }
    std::puts("PASS vertex-pressure shader: 32 inputs plus cbuffer compile without uniform register exhaustion");
    return 0;
}

int main()
{
    const char *sqrtShader =
        "float4 main(float4 a : TEXCOORD0) : SV_Target { return sqrt(a); }";
    if (int r = compile_shader(sqrtShader, 75, 4)) return r;

    const char *varyingShader =
        "struct I {"
        " float4 a0:TEXCOORD0; float4 a1:TEXCOORD1; float4 a2:TEXCOORD2; float4 a3:TEXCOORD3;"
        " float4 a4:TEXCOORD4; float4 a5:TEXCOORD5; float4 a6:TEXCOORD6; float4 a7:TEXCOORD7;"
        "};"
        "float4 main(I i) : SV_Target {"
        " return i.a0+i.a1+i.a2+i.a3+i.a4+i.a5+i.a6+i.a7;"
        "}";
    if (int r = compile_shader(varyingShader, 0, 32)) return 10 + r;

    const char *expLogShader =
        "float4 main(float4 a : TEXCOORD0) : SV_Target {"
        " float4 x = abs(a) + 2.0;"
        " return exp2(a * 0.25) + log2(x);"
        "}";
    if (int r = compile_shader(expLogShader, 25, 4)) return 20 + r;
    if (int r = compile_shader(expLogShader, 47, 4)) return 30 + r;
    if (int r = compile_vertex_pressure_shader()) return 40 + r;

    std::puts("Pi5 shader SQRT + EXP + LOG + 32-varying + lazy-vertex regression: PASS");
    return 0;
}
