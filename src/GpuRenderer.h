#pragma once

#include "Filters.h"
#include <d3d11.h>
#include <winrt/base.h>

namespace paper {

class GpuRenderer {
public:
    explicit GpuRenderer(ID3D11Device* device);
    void Draw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
              ID3D11RenderTargetView* destination, UINT width, UINT height,
              const ShaderConstants& constants);

private:
    winrt::com_ptr<ID3D11VertexShader> vertex_;
    winrt::com_ptr<ID3D11PixelShader> pixel_;
    winrt::com_ptr<ID3D11Buffer> constants_;
    winrt::com_ptr<ID3D11RasterizerState> rasterizer_;
};

}
