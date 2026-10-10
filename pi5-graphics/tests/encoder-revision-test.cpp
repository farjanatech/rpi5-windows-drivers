#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../native/encode.cpp"

static int test_revision(uint32_t revision, uint8_t e0, uint8_t e1, uint8_t e2)
{
    uint8_t storage[256 * 1024] = {};
    pi5::Draw d = {};
    d.width = 64;
    d.height = 64;
    d.pitch = 64 * 4;
    d.target = 0x00200000;
    d.tile = 0x00300000;
    d.coordinateCode = 0x00400000;
    d.vertexCode = 0x00500000;
    d.pixelCode = 0x00600000;
    d.coordinateUniforms = 0x00700000;
    d.vertexUniforms = 0x00710000;
    d.pixelUniforms = 0x00720000;
    d.vertexAddress = 0x00800000;
    d.vertexStride = 24;
    d.vertexCount = 3;
    d.vertexScalars = 6;
    d.varyingScalars = 2;
    d.v3dRevision = revision;
    d.loadTarget = false;

    pi5::EncodedCommands out = {};
    const char *error = nullptr;
    if (!pi5::EncodeDrawBatch(&d, 1, 0x00100000, storage, sizeof(storage), out, error)) {
        std::printf("EncodeDrawBatch failed for rev %u: %s\n", revision, error ? error : "<none>");
        return 1;
    }

    const uint32_t expectedPixel = d.pixelCode | 1u;
    const uint32_t expectedVertex = d.vertexCode | 1u;
    const uint32_t expectedCoord = d.coordinateCode | 1u;

    for (uint32_t off = 0; off + 32 <= out.bytes; ++off) {
        uint32_t p = 0, pu = 0, v = 0, vu = 0, c = 0, cu = 0;
        std::memcpy(&p,  storage + off + 8,  4);
        std::memcpy(&pu, storage + off + 12, 4);
        std::memcpy(&v,  storage + off + 16, 4);
        std::memcpy(&vu, storage + off + 20, 4);
        std::memcpy(&c,  storage + off + 24, 4);
        std::memcpy(&cu, storage + off + 28, 4);
        if (p == expectedPixel && pu == d.pixelUniforms &&
            v == expectedVertex && vu == d.vertexUniforms &&
            c == expectedCoord && cu == d.coordinateUniforms) {
            if (storage[off] != e0 || storage[off + 1] != e1 || storage[off + 2] != e2) {
                std::printf("FAIL rev %u: header %02X %02X %02X, expected %02X %02X %02X\n",
                            revision, storage[off], storage[off + 1], storage[off + 2], e0, e1, e2);
                return 2;
            }
            std::printf("PASS rev %u: shader record header %02X %02X %02X\n",
                        revision, storage[off], storage[off + 1], storage[off + 2]);
            return 0;
        }
    }

    std::printf("FAIL rev %u: shader-state record not found\n", revision);
    return 3;
}



static int test_final_thread_section()
{
    uint8_t storage[256 * 1024] = {};
    pi5::Draw d = {};
    d.width=64; d.height=64; d.pitch=256; d.target=0x00200000; d.tile=0x00300000;
    d.coordinateCode=0x00400000; d.vertexCode=0x00500000; d.pixelCode=0x00600000;
    d.coordinateUniforms=0x00700000; d.vertexUniforms=0x00710000; d.pixelUniforms=0x00720000;
    d.vertexAddress=0x00800000; d.vertexStride=24; d.vertexCount=3; d.vertexScalars=6; d.varyingScalars=2;
    d.v3dRevision=6; d.fourThreadMask=0b101; d.finalThreadMask=0b010;
    pi5::EncodedCommands out={}; const char *error=nullptr;
    if(!pi5::EncodeDrawBatch(&d,1,0x00100000,storage,sizeof(storage),out,error)){
        std::printf("FAIL final-section encode: %s\n",error?error:"<none>");return 1;
    }
    const uint32_t ep=d.pixelCode|1u,ev=d.vertexCode|2u,ec=d.coordinateCode|1u;
    for(uint32_t off=0;off+32<=out.bytes;++off){
        uint32_t p=0,pu=0,v=0,vu=0,c=0,cu=0;
        std::memcpy(&p,storage+off+8,4);std::memcpy(&pu,storage+off+12,4);
        std::memcpy(&v,storage+off+16,4);std::memcpy(&vu,storage+off+20,4);
        std::memcpy(&c,storage+off+24,4);std::memcpy(&cu,storage+off+28,4);
        if(p==ep&&pu==d.pixelUniforms&&v==ev&&vu==d.vertexUniforms&&c==ec&&cu==d.coordinateUniforms){
            std::puts("PASS final-section shader-state bit");return 0;
        }
    }
    std::puts("FAIL final-section shader-state record not found");return 2;
}

static int test_vertex_scalars(uint32_t scalars, bool expected)
{
    uint8_t storage[256 * 1024] = {};
    pi5::Draw d = {};
    d.width = 64;
    d.height = 64;
    d.pitch = 64 * 4;
    d.target = 0x00200000;
    d.tile = 0x00300000;
    d.coordinateCode = 0x00400000;
    d.vertexCode = 0x00500000;
    d.pixelCode = 0x00600000;
    d.coordinateUniforms = 0x00700000;
    d.vertexUniforms = 0x00710000;
    d.pixelUniforms = 0x00720000;
    d.vertexAddress = 0x00800000;
    d.vertexStride = scalars * 4;
    d.vertexCount = 3;
    d.vertexScalars = scalars;
    d.varyingScalars = 0;
    d.v3dRevision = 6;

    pi5::EncodedCommands out = {};
    const char *error = nullptr;
    bool ok = pi5::EncodeDrawBatch(&d, 1, 0x00100000, storage, sizeof(storage), out, error);
    if (ok != expected) {
        std::printf("FAIL vertex scalars %u: ok=%u expected=%u error=%s\n",
                    scalars, ok ? 1u : 0u, expected ? 1u : 0u, error ? error : "<none>");
        return 1;
    }
    std::printf("PASS vertex scalars %u: %s\n", scalars, ok ? "accepted" : "rejected as expected");
    return 0;
}


static int test_varying_flag_chunks()
{
    uint8_t storage[256 * 1024] = {};
    pi5::Draw d = {};
    d.width = 64;
    d.height = 64;
    d.pitch = 64 * 4;
    d.target = 0x00200000;
    d.tile = 0x00300000;
    d.coordinateCode = 0x00400000;
    d.vertexCode = 0x00500000;
    d.pixelCode = 0x00600000;
    d.coordinateUniforms = 0x00700000;
    d.vertexUniforms = 0x00710000;
    d.pixelUniforms = 0x00720000;
    d.vertexAddress = 0x00800000;
    d.vertexStride = 24;
    d.vertexCount = 3;
    d.vertexScalars = 6;
    d.varyingScalars = 32;
    d.flatMask = (1u << 1) | (1u << 25);
    d.nonPerspectiveMask = 1u << 30;
    d.v3dRevision = 6;

    pi5::EncodedCommands out = {};
    const char *error = nullptr;
    if (!pi5::EncodeDrawBatch(&d, 1, 0x00100000, storage, sizeof(storage), out, error)) {
        std::printf("FAIL varying chunks: %s\n", error ? error : "<none>");
        return 1;
    }
    const uint32_t begin = out.binStart - 0x00100000;
    const uint32_t end = out.binEnd - 0x00100000;
    auto Has = [&](const uint8_t expected[5]) {
        for (uint32_t off = begin; off + 5 <= end; ++off)
            if (!std::memcmp(storage + off, expected, 5)) return true;
        return false;
    };
    const uint8_t flat0[5] = {98, 0, 0x02, 0, 0};
    const uint8_t flat1[5] = {98, 1, 0x02, 0, 0};
    const uint8_t noperspective1[5] = {100, 1, 0x40, 0, 0};
    if (!Has(flat0) || !Has(flat1) || !Has(noperspective1)) {
        std::puts("FAIL varying chunks: expected offset packets were not emitted");
        return 2;
    }

    d.varyingScalars = 33;
    if (pi5::EncodeDrawBatch(&d, 1, 0x00100000, storage, sizeof(storage), out, error)) {
        std::puts("FAIL varying chunks: 33 scalars unexpectedly accepted");
        return 3;
    }
    std::puts("PASS varying chunks: 32 accepted across two 24-scalar packets; 33 rejected");
    return 0;
}

int main()
{
    if (test_revision(6,  0x02, 0x12, 0x04)) return 1;
    if (test_revision(9,  0x02, 0x12, 0x04)) return 1;
    if (test_revision(10, 0x02, 0xA0, 0x20)) return 1;
    if (test_revision(11, 0x02, 0xA0, 0x20)) return 1;
    if (test_vertex_scalars(17, true)) return 1;
    if (test_vertex_scalars(32, true)) return 1;
    if (test_vertex_scalars(33, false)) return 1;
    if (test_varying_flag_chunks()) return 1;
    if (test_final_thread_section()) return 1;

    pi5::Draw defaults = {};
    if (defaults.v3dRevision != 10) {
        std::puts("FAIL: default v3dRevision changed from D0-safe value 10");
        return 1;
    }

    std::puts("Universal V3D revision encoder regression: PASS");
    return 0;
}
