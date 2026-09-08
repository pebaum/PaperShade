#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace paper {

enum class Preset : std::uint32_t {
    Natural, Classic, Average, Paper, HighContrast, Inverted,
    InkThreshold, InkDither, Ink4, Ink16, Ps1Gray, Ps1Color, Original, Count
};

inline constexpr std::array<const wchar_t*, 13> PresetNames{
    L"Natural grayscale (Rec. 709)", L"Classic grayscale (Rec. 601)",
    L"Equal-channel grayscale", L"Soft paper grayscale",
    L"High-contrast grayscale", L"Inverted grayscale",
    L"E-ink - crisp black and white", L"E-ink - dithered black and white",
    L"E-ink - 4 shades", L"E-ink - 16 shades",
    L"PS1 - grayscale RGB555", L"PS1 - original color RGB555",
    L"Original colors (warmth only)"
};

inline constexpr std::uint32_t NeutralKelvin = 6500;
inline constexpr std::uint32_t MinimumKelvin = 1000;
inline constexpr std::array<std::uint32_t, 7> KelvinPresets{6500, 5500, 4500, 3500, 2700, 2000, 1200};

struct Settings {
    Preset preset = Preset::Natural;
    std::uint32_t fps = 15;
    std::uint32_t pixelSize = 1;
    bool enabled = false;
    bool hideCaptureIndicator = true;
    std::uint32_t temperatureKelvin = NeutralKelvin;
};

inline constexpr bool ValidPreset(std::uint32_t value) {
    return value < static_cast<std::uint32_t>(Preset::Count);
}

inline constexpr bool ValidFps(std::uint32_t value) {
    return value == 10 || value == 15 || value == 30 || value == 60;
}

inline constexpr bool ValidKelvin(std::uint32_t value) {
    return value >= MinimumKelvin && value <= NeutralKelvin;
}

inline constexpr bool UsesCapture(Preset preset, std::uint32_t kelvin = NeutralKelvin) {
    return (preset >= Preset::InkThreshold && preset <= Preset::Ps1Color) ||
        (preset == Preset::HighContrast && kelvin != NeutralKelvin);
}

inline constexpr bool IsNeutralOriginal(Preset preset, std::uint32_t kelvin) {
    return preset == Preset::Original && kelvin == NeutralKelvin;
}

inline std::array<float, 3> KelvinGains(std::uint32_t kelvin) {
    if (!ValidKelvin(kelvin)) throw std::invalid_argument("Color temperature must be 1000-6500 K.");
    if (kelvin == NeutralKelvin) return {1.0f, 1.0f, 1.0f};
    const auto green = [](double t) {
        return std::clamp(99.4708025861 * std::log(t) - 161.1195681661, 0.0, 255.0);
    };
    const auto blue = [](double t) {
        return t <= 19.0 ? 0.0 :
            std::clamp(138.5177312231 * std::log(t - 10.0) - 305.0447927307, 0.0, 255.0);
    };
    // Approximate blackbody RGB, normalized to neutral 6500 K. Only attenuate channels;
    // this is a display effect, not a measurement of a monitor's physical white point.
    const double t = kelvin / 100.0;
    return {1.0f, static_cast<float>(green(t) / green(65.0)),
        static_cast<float>(blue(t) / blue(65.0))};
}

// Layout is shared with the HLSL and Metal constant buffers (four 16-byte registers).
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
    float warmthRed = 1.0f;
    float warmthGreen = 1.0f;
    float warmthBlue = 1.0f;
    float warmthPadding = 0.0f;
};
static_assert(sizeof(ShaderConstants) == 64);

inline ShaderConstants Constants(Preset preset, std::uint32_t pixelSize = 1,
    std::uint32_t kelvin = NeutralKelvin) {
    ShaderConstants result;
    result.pixelSize = static_cast<std::int32_t>(std::clamp(pixelSize, 1u, 4u));
    const auto warmth = KelvinGains(kelvin);
    result.warmthRed = warmth[0];
    result.warmthGreen = warmth[1];
    result.warmthBlue = warmth[2];
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
    case Preset::Original:
        result.color = 1; break;
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

inline std::array<float, 25> ColorMatrix(Preset preset, std::uint32_t kelvin = NeutralKelvin) {
    if (UsesCapture(preset, kelvin)) {
        throw std::invalid_argument("This style and temperature require per-pixel rendering.");
    }
    const auto c = Constants(preset, 1, kelvin);
    if (c.color) {
        return {
            c.warmthRed, 0, 0, 0, 0,
            0, c.warmthGreen, 0, 0, 0,
            0, 0, c.warmthBlue, 0, 0,
            0, 0, 0, 1, 0,
            0, 0, 0, 0, 1
        };
    }
    return {
        c.red * c.gain * c.warmthRed, c.red * c.gain * c.warmthGreen, c.red * c.gain * c.warmthBlue, 0, 0,
        c.green * c.gain * c.warmthRed, c.green * c.gain * c.warmthGreen, c.green * c.gain * c.warmthBlue, 0, 0,
        c.blue * c.gain * c.warmthRed, c.blue * c.gain * c.warmthGreen, c.blue * c.gain * c.warmthBlue, 0, 0,
        0, 0, 0, 1, 0,
        c.offset * c.warmthRed, c.offset * c.warmthGreen, c.offset * c.warmthBlue, 0, 1
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
    const std::array<float, 3> warmth{c.warmthRed, c.warmthGreen, c.warmthBlue};
    for (int channel = 0; channel < 3; ++channel) {
        float value = rgb[channel];
        if (c.quantizer == 3) {
            const int byte = static_cast<int>(std::floor(value * 255.0f + 0.5f));
            value = ExpandFiveBit(Ps1FiveBit(byte, x, y)) / 255.0f;
        }
        if (c.quantizer == 1 || c.quantizer == 2) {
            float threshold = 0.0f;
            if (c.quantizer == 2) {
                threshold = (BayerMatrix[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f - 0.5f;
            }
            const float steps = static_cast<float>(c.levels - 1);
            value = std::clamp(std::floor(value * steps + 0.5f + threshold) / steps, 0.0f, 1.0f);
        }
        result[channel] = static_cast<std::uint8_t>(
            std::floor(std::clamp(value * warmth[channel], 0.0f, 1.0f) * 255.0f + 0.5f));
    }
    return result;
}

}
