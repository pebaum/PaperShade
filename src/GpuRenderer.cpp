#include "GpuRenderer.h"
#include "ShaderSource.h"
#include <d3dcompiler.h>
#include <cstring>
#include <string>

namespace paper {
namespace {

winrt::com_ptr<ID3DBlob> Compile(const char* entry, const char* target) {
    winrt::com_ptr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(
        FilterShaderSource, sizeof(FilterShaderSource) - 1, "Filter.hlsl",
        nullptr, nullptr, entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS |
        D3DCOMPILE_IEEE_STRICTNESS, 0, code.put(), errors.put());
    if (FAILED(hr)) {
        const std::string detail = errors
            ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
            : "No shader compiler diagnostics were returned.";
        throw winrt::hresult_error(hr, winrt::to_hstring(detail));
    }
    return code;
}

}

GpuRenderer::GpuRenderer(ID3D11Device* device) {
    const auto vs = Compile("VertexMain", "vs_5_0");
    const auto ps = Compile("PixelMain", "ps_5_0");
    winrt::check_hresult(device->CreateVertexShader(
        vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertex_.put()));
    winrt::check_hresult(device->CreatePixelShader(
        ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixel_.put()));
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(ShaderConstants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    winrt::check_hresult(device->CreateBuffer(&buffer, nullptr, constants_.put()));
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    winrt::check_hresult(device->CreateRasterizerState(&rasterizer, rasterizer_.put()));
}

void GpuRenderer::Draw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
                       ID3D11RenderTargetView* destination, UINT width, UINT height,
                       const ShaderConstants& constants) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(context->Map(constants_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constants_.get(), 0);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(rasterizer_.get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.get(), nullptr, 0);
    context->PSSetShader(pixel_.get(), nullptr, 0);
    ID3D11Buffer* cb = constants_.get();
    context->PSSetConstantBuffers(0, 1, &cb);
    context->PSSetShaderResources(0, 1, &source);
    context->OMSetRenderTargets(1, &destination, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetDepthStencilState(nullptr, 0);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context->PSSetShaderResources(0, 1, &empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

}
