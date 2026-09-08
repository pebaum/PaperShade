#include "GpuRenderer.h"
#include <dxgi.h>
#include <array>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        const bool warp = argc == 2 && std::string(argv[1]) == "--warp";
        constexpr UINT width = 1024;
        constexpr UINT height = 16;
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        winrt::check_hresult(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
            device.put(), nullptr, context.put()));
        const auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
        DXGI_ADAPTER_DESC adapterDesc{};
        winrt::check_hresult(adapter->GetDesc(&adapterDesc));
        std::wcout << L"Shader test adapter: " << adapterDesc.Description << L"\n";
        paper::GpuRenderer renderer(device.get());
        std::vector<std::array<std::uint8_t, 4>> pixels(width * height);
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const auto value = static_cast<std::uint8_t>(x / 4);
                pixels[y * width + x] = {value, static_cast<std::uint8_t>((value * 13) & 255),
                    static_cast<std::uint8_t>((value * 53) & 255), 255};
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0};
        winrt::com_ptr<ID3D11Texture2D> input;
        winrt::check_hresult(device->CreateTexture2D(&desc, &data, input.put()));
        winrt::com_ptr<ID3D11ShaderResourceView> source;
        winrt::check_hresult(device->CreateShaderResourceView(input.get(), nullptr, source.put()));
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        winrt::com_ptr<ID3D11Texture2D> output;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, output.put()));
        winrt::com_ptr<ID3D11RenderTargetView> destination;
        winrt::check_hresult(device->CreateRenderTargetView(output.get(), nullptr, destination.put()));
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
        std::uint64_t checked = 0;
        for (unsigned preset = 0; preset < static_cast<unsigned>(paper::Preset::Count); ++preset) {
            for (unsigned variant = 0; variant < 16; ++variant) {
                const unsigned size = variant % 4 + 1;
                constexpr std::array<unsigned, 4> temperatures{6500, 4500, 2700, 1200};
                const unsigned kelvin = temperatures[variant / 4];
                const auto constants = paper::Constants(static_cast<paper::Preset>(preset), size, kelvin);
                renderer.Draw(context.get(), source.get(), destination.get(), width, height, constants);
                context->CopyResource(staging.get(), output.get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
                bool failed = false;
                for (UINT y = 0; y < height && !failed; ++y) {
                    const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
                    for (UINT x = 0; x < width; ++x) {
                        const auto inputPixel = pixels[y * width + x];
                        const auto expected = paper::ReferencePixel(
                            {inputPixel[0], inputPixel[1], inputPixel[2]}, x, y, constants);
                        for (UINT channel = 0; channel < 3; ++channel) {
                            const int actual = row[x * 4 + 2 - channel];
                            const int tolerance = constants.quantizer == 3 && kelvin == paper::NeutralKelvin ? 0 : 1;
                            if (std::abs(actual - expected[channel]) > tolerance || row[x * 4 + 3] != 255) {
                                std::cerr << "Shader mismatch: preset=" << preset << " scale=" << size << " kelvin=" << kelvin
                                    << " x=" << x << " y=" << y << " channel=" << channel
                                    << " expected=" << static_cast<int>(expected[channel])
                                    << " actual=" << actual << '\n';
                                failed = true;
                                break;
                            }
                        }
                        ++checked;
                        if (failed) break;
                    }
                }
                context->Unmap(staging.get(), 0);
                if (failed) return 1;
            }
        }
        std::cout << "GPU shader matched CPU reference for " << checked
                  << " pixels across every preset, pattern scale, and Kelvin variant.\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
