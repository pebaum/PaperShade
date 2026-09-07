#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace paper {

enum class Preset : std::uint32_t {
    Natural, Classic, Average, Paper, HighContrast, Inverted,
    InkThreshold, InkDither, Ink4, Ink16, Ps1Gray, Ps1Color, Count
};

inline constexpr std::array<const wchar_t*, 12> PresetNames{
    L"Natural grayscale (Rec. 709)", L"Classic grayscale (Rec. 601)",
    L"Equal-channel grayscale", L"Soft paper grayscale",
    L"High-contrast grayscale", L"Inverted grayscale",
    L"E-ink - crisp black and white", L"E-ink - dithered black and white",
    L"E-ink - 4 shades", L"E-ink - 16 shades",
    L"PS1 - grayscale RGB555", L"PS1 - original color RGB555"
};

struct Settings {
    Preset preset = Preset::Natural;
    std::uint32_t fps = 15;
    std::uint32_t pixelSize = 1;
    bool enabled = false;
    bool hideCaptureIndicator = true;
};

inline constexpr bool ValidPreset(std::uint32_t value) {
    return value < static_cast<std::uint32_t>(Preset::Count);
}

inline constexpr bool ValidFps(std::uint32_t value) {
    return value == 10 || value == 15 || value == 30 || value == 60;
}

inline constexpr bool UsesCapture(Preset preset) {
    return preset >= Preset::InkThreshold && preset < Preset::Count;
}

// Layout is shared with the HLSL constant buffer (three 16-byte registers).
struct alignas(16) ShaderConstants {
    float red = 0.2126f;
    float green = 0.7152f;
    float blue = 0.0722f;
    float gain = 1.0f;
    float offset = 0.0f;
    std::int32_t quantizer = 0;
    std::int32_t levels = 256;
    std::int32_t pixelSize = 1;
    std::int32_t color = 0;
    std::int32_t padding[3]{};
};
static_assert(sizeof(ShaderConstants) == 48);

inline ShaderConstants Constants(Preset preset, std::uint32_t pixelSize = 1) {
    ShaderConstants result;
    result.pixelSize = static_cast<std::int32_t>(std::clamp(pixelSize, 1u, 4u));
    switch (preset) {
    case Preset::Classic:
        result.red = 0.299f; result.green = 0.587f; result.blue = 0.114f; break;
    case Preset::Average:
        result.red = result.green = result.blue = 1.0f / 3.0f; break;
    case Preset::Paper:
        result.gain = 0.88f; result.offset = 0.06f; break;
    case Preset::HighContrast:
        result.gain = 1.5f; result.offset = -0.25f; break;
    case Preset::Inverted:
        result.gain = -1.0f; result.offset = 1.0f; break;
    case Preset::InkThreshold:
        result.quantizer = 1; result.levels = 2; break;
    case Preset::InkDither:
        result.quantizer = 2; result.levels = 2; break;
    case Preset::Ink4:
        result.quantizer = 2; result.levels = 4; break;
    case Preset::Ink16:
        result.quantizer = 2; result.levels = 16; break;
    case Preset::Ps1Gray:
        result.quantizer = 3; result.levels = 32; break;
    case Preset::Ps1Color:
        result.quantizer = 3; result.levels = 32; result.color = 1; break;
    default: break;
    }
    return result;
}

inline constexpr std::array<int, 16> Ps1Matrix{
    -4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2
};
inline constexpr std::array<int, 16> BayerMatrix{
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5
};

inline constexpr int Ps1FiveBit(int input, int x, int y) {
    const auto index = static_cast<unsigned>((y & 3) * 4 + (x & 3));
    return std::clamp(input + Ps1Matrix[index], 0, 255) >> 3;
}

inline constexpr std::uint8_t ExpandFiveBit(int value) {
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

inline std::array<float, 25> ColorMatrix(Preset preset) {
    const auto c = Constants(preset);
    return {
        c.red * c.gain, c.red * c.gain, c.red * c.gain, 0, 0,
        c.green * c.gain, c.green * c.gain, c.green * c.gain, 0, 0,
        c.blue * c.gain, c.blue * c.gain, c.blue * c.gain, 0, 0,
        0, 0, 0, 1, 0,
        c.offset, c.offset, c.offset, 0, 1
    };
}

inline std::array<std::uint8_t, 3> ReferencePixel(
    std::array<std::uint8_t, 3> input, int x, int y, const ShaderConstants& c) {
    std::array<float, 3> rgb{};
    const float gray = std::clamp(
        (input[0] / 255.0f * c.red + input[1] / 255.0f * c.green +
         input[2] / 255.0f * c.blue) * c.gain + c.offset, 0.0f, 1.0f);
    for (int channel = 0; channel < 3; ++channel) {
        rgb[channel] = c.color ? input[channel] / 255.0f : gray;
    }
    x /= c.pixelSize;
    y /= c.pixelSize;
    std::array<std::uint8_t, 3> result{};
    for (int channel = 0; channel < 3; ++channel) {
        float value = rgb[channel];
        if (c.quantizer == 3) {
            const int byte = static_cast<int>(std::floor(value * 255.0f + 0.5f));
            result[channel] = ExpandFiveBit(Ps1FiveBit(byte, x, y));
            continue;
        }
        if (c.quantizer == 1 || c.quantizer == 2) {
            float threshold = 0.0f;
            if (c.quantizer == 2) {
                threshold = (BayerMatrix[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f - 0.5f;
            }
            const float steps = static_cast<float>(c.levels - 1);
            value = std::clamp(std::floor(value * steps + 0.5f + threshold) / steps, 0.0f, 1.0f);
        }
        result[channel] = static_cast<std::uint8_t>(std::floor(value * 255.0f + 0.5f));
    }
    return result;
}

}
