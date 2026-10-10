#include <windows.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "../native/shader.h"
#include "../native/validate.h"

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
        "cbuffer C : register(b0) { float4 k[8]; };"
        "struct I {"
        " float4 a0:TEXCOORD0; float4 a1:TEXCOORD1; float4 a2:TEXCOORD2; float4 a3:TEXCOORD3;"
        " float4 a4:TEXCOORD4; float4 a5:TEXCOORD5; float4 a6:TEXCOORD6; float4 a7:TEXCOORD7;"
        "};"
        "struct O {"
        " float4 pos:SV_Position;"
        " float4 o0:TEXCOORD0; float4 o1:TEXCOORD1; float4 o2:TEXCOORD2; float4 o3:TEXCOORD3;"
        " float4 o4:TEXCOORD4; float4 o5:TEXCOORD5; float4 o6:TEXCOORD6; float4 o7:TEXCOORD7;"
        "};"
        "O main(I i) {"
        " O o;"
        " float4 t0=i.a0+k[0]; float4 t1=i.a1+k[1];"
        " float4 t2=i.a2+k[2]; float4 t3=i.a3+k[3];"
        " float4 t4=i.a4+k[4]; float4 t5=i.a5+k[5];"
        " float4 t6=i.a6+k[6]; float4 t7=i.a7+k[7];"
        " float4 sum=t0+t1+t2+t3+t4+t5+t6+t7;"
        " o.pos=float4(sum.xy,0.0,1.0);"
        " o.o0=t0+sum*0.001; o.o1=t1+sum*0.002;"
        " o.o2=t2+sum*0.003; o.o3=t3+sum*0.004;"
        " o.o4=t4+sum*0.005; o.o5=t5+sum*0.006;"
        " o.o6=t6+sum*0.007; o.o7=t7+sum*0.008;"
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
        std::printf("Pi5 high-pressure vertex compile failed: %s\n",error.c_str());
        return 2;
    }
    unsigned scalars=0;
    for(auto mask:shader.inputs)for(unsigned c=0;c<4;++c)scalars+=(mask>>c)&1u;
    if(scalars!=32||shader.varyingScalars!=32){
        std::printf("FAIL: vertex inputs=%u varyings=%u expected=32/32\n",scalars,shader.varyingScalars);
        return 3;
    }

    std::vector<uint32_t> uniforms;
    uniforms.reserve(shader.uniforms.size());
    for(const auto &u:shader.uniforms)
        uniforms.push_back(u.kind==pi5::UniformKind::Literal?u.value:0u);
    pi5::ProgramRules rules;
    rules.stage=pi5::ProgramStage::Vertex;
    rules.vertexScalars=32;
    rules.varyingScalars=shader.varyingScalars;
    rules.constants=shader.constantWords!=0;
    uint64_t registers=0;
    if(!pi5::ValidateProgram(shader.code.data(),static_cast<uint32_t>(shader.code.size()),
                             uniforms.data(),static_cast<uint32_t>(uniforms.size()),
                             rules,nullptr,&registers)){
        std::puts("FAIL: 64-register vertex program did not pass kernel validator");
        return 4;
    }
    if(!(registers&UINT64_C(0xffffffff00000000))){
        std::printf("FAIL: stress shader stayed in RF0-RF31 mask=%016llx\n",
                    static_cast<unsigned long long>(registers));
        return 5;
    }
    constexpr uint64_t Nop=UINT64_C(0x38003186bb03f000);
    constexpr uint64_t Switch=UINT64_C(0x38203186bb03f000);
    unsigned switches=0;
    for(auto word:shader.code)if(word==Switch)++switches;
    if(switches!=1||shader.code.size()<4||
       shader.code[shader.code.size()-4]!=Nop||
       shader.code[shader.code.size()-3]!=Switch||
       shader.code[shader.code.size()-2]!=Nop||
       shader.code[shader.code.size()-1]!=Nop){
        std::printf("FAIL: final-section 2-thread epilogue switches=%u words=%zu\n",switches,shader.code.size());
        return 6;
    }
    std::printf("PASS vertex-pressure shader: high-register mask=%016llx and single program-end THRSW\n",
                static_cast<unsigned long long>(registers));
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

    std::puts("Pi5 shader SQRT + EXP + LOG + 32-varying + 64-register vertex fallback regression: PASS");
    return 0;
}
