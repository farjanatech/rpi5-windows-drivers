// SPDX-License-Identifier: BSD-2-Clause-Patent
// Run on the Pi, never in CI: explicit Pi5 hardware adapter, no WARP fallback.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
using Microsoft::WRL::ComPtr;

#define HR(x) do { HRESULT result = (x); if (FAILED(result)) { std::printf("FAIL line %d: %s = 0x%08lx\n", __LINE__, #x, static_cast<unsigned long>(result)); return 1; } } while (0)

int main() {
    ComPtr<IDXGIFactory1> factory;
    HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc = {};
    for (UINT i = 0;; ++i) {
        adapter.Reset();
        HRESULT result = factory->EnumAdapters1(i, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) { std::puts("FAIL: Pi5 V3D hardware adapter not found"); return 1; }
        HR(result); HR(adapter->GetDesc1(&desc));
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && std::wcsstr(desc.Description, L"Pi5") &&
            std::wcsstr(desc.Description, L"V3D")) break;
    }
    std::printf("Adapter: %ls; LUID: %08lx:%08lx\n", desc.Description,
        static_cast<unsigned long>(desc.AdapterLuid.HighPart), static_cast<unsigned long>(desc.AdapterLuid.LowPart));
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0, actual = {};
    HR(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        &requested, 1, D3D11_SDK_VERSION, &device, &actual, &context));
    if (actual != requested) return 1;
    const char vsSource[] = "float4 main(float4 p:POSITION):SV_Position{return p;}";
    const char psSource[] = "float4 main():SV_Target{return float4(1,0,0,1);}";
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    HR(D3DCompile(vsSource, sizeof(vsSource)-1, nullptr, nullptr, nullptr, "main", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsCode, &errors));
    errors.Reset();
    HR(D3DCompile(psSource, sizeof(psSource)-1, nullptr, nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psCode, &errors));
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    HR(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
    HR(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
    D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    HR(device->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout));
    const float vertices[3][4] = {{-.75f,-.75f,0,1},{0,.75f,0,1},{.75f,-.75f,0,1}};
    D3D11_BUFFER_DESC bufferDesc = {}; bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.Usage = D3D11_USAGE_IMMUTABLE; bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data = {}; data.pSysMem = vertices;
    ComPtr<ID3D11Buffer> vertexBuffer; HR(device->CreateBuffer(&bufferDesc, &data, &vertexBuffer));
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = textureDesc.Height = 128; textureDesc.MipLevels = textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT; textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, readback;
    HR(device->CreateTexture2D(&textureDesc, nullptr, &target));
    textureDesc.Usage = D3D11_USAGE_STAGING; textureDesc.BindFlags = 0; textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HR(device->CreateTexture2D(&textureDesc, nullptr, &readback));
    ComPtr<ID3D11RenderTargetView> rtv; HR(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    D3D11_RASTERIZER_DESC rasterDesc = {}; rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster; HR(device->CreateRasterizerState(&rasterDesc, &raster));
    D3D11_VIEWPORT viewport = {0,0,128,128,0,1};
    context->RSSetViewports(1, &viewport); context->RSSetState(raster.Get());
    ID3D11RenderTargetView* view = rtv.Get(); context->OMSetRenderTargets(1, &view, nullptr);
    ID3D11Buffer* vb = vertexBuffer.Get(); UINT stride = sizeof(vertices[0]), offset = 0;
    context->IASetVertexBuffers(0,1,&vb,&stride,&offset); context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    const float clear[4] = {0,0,1,1};
    for (UINT frame = 0; frame < 120; ++frame) {
        context->ClearRenderTargetView(rtv.Get(), clear); context->Draw(3,0);
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto p = static_cast<const unsigned char*>(mapped.pData);
        const auto center = p + 64 * mapped.RowPitch + 64 * 4;
        const auto corner = p + 4 * mapped.RowPitch + 4 * 4;
        bool correct = center[0] > 240 && center[1] < 16 && center[2] < 16 &&
            corner[0] < 16 && corner[1] < 16 && corner[2] > 240;
        if (!correct) std::printf("FAIL frame %u: center=%u,%u,%u corner=%u,%u,%u\n",frame,center[0],center[1],center[2],corner[0],corner[1],corner[2]);
        context->Unmap(readback.Get(),0);
        if (!correct) return 1;
    }
    HR(device->GetDeviceRemovedReason());
    std::puts("PASS: 120 hardware shader draws, clears and synchronized pixel readbacks on Pi5 V3D");
    return 0;
}
