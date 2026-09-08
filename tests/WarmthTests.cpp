#include "Filters.h"
#include "PaperShadeCore.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestTemperatureCurve() {
    Require(paper::ValidKelvin(1000) && paper::ValidKelvin(6500), "Temperature endpoints rejected.");
    Require(!paper::ValidKelvin(999) && !paper::ValidKelvin(6501) && !paper::ValidKelvin(0), "Invalid temperature accepted.");
    Require(paper::KelvinGains(6500) == std::array<float, 3>{1, 1, 1}, "Neutral warmth must be an exact identity.");
    auto previous = paper::KelvinGains(1000);
    for (unsigned kelvin = 1001; kelvin <= 6500; ++kelvin) {
        const auto gains = paper::KelvinGains(kelvin);
        Require(gains[0] == 1 && gains[1] >= previous[1] && gains[2] >= previous[2], "Warming curve is not monotonic.");
        Require(gains[1] <= 1 && gains[2] <= gains[1] && gains[2] >= 0, "Warming must attenuate, not boost, channels.");
        previous = gains;
    }
    const auto evening = paper::ReferencePixel({255, 255, 255}, 0, 0, paper::Constants(paper::Preset::Original, 1, 3500));
    Require(evening == std::array<std::uint8_t, 3>{255, 193, 144}, "3500 K white differs from its independent golden.");
    const auto candle = paper::ReferencePixel({255, 255, 255}, 0, 0, paper::Constants(paper::Preset::Original, 1, 1200));
    Require(candle == std::array<std::uint8_t, 3>{255, 86, 0}, "1200 K white differs from its independent golden.");
    Require(paper::Settings{}.temperatureKelvin == 6500, "Existing profiles must remain neutral by default.");
}

void TestCompositorParity() {
    for (const auto preset : {paper::Preset::Natural, paper::Preset::Classic, paper::Preset::Average,
             paper::Preset::Paper, paper::Preset::Inverted, paper::Preset::Original}) {
        for (const auto kelvin : paper::KelvinPresets) {
            Require(!paper::UsesCapture(preset, kelvin), "A linear warming mode must remain compositor-only.");
            const auto matrix = paper::ColorMatrix(preset, kelvin);
            const auto constants = paper::Constants(preset, 1, kelvin);
            for (unsigned value = 0; value < 256; ++value) {
                const std::array<std::uint8_t, 3> input{static_cast<std::uint8_t>(value),
                    static_cast<std::uint8_t>((value * 13) & 255), static_cast<std::uint8_t>((value * 53) & 255)};
                const auto expected = paper::ReferencePixel(input, 0, 0, constants);
                for (unsigned channel = 0; channel < 3; ++channel) {
                    float output = matrix[20 + channel];
                    for (unsigned component = 0; component < 3; ++component) {
                        output += input[component] / 255.0f * matrix[component * 5 + channel];
                    }
                    const auto actual = static_cast<int>(std::floor(std::clamp(output, 0.0f, 1.0f) * 255 + 0.5f));
                    Require(std::abs(actual - expected[channel]) <= 1, "Warm compositor matrix differs from the GPU reference.");
                }
            }
        }
    }
    Require(!paper::UsesCapture(paper::Preset::HighContrast, 6500), "Neutral contrast should retain its fast path.");
    Require(paper::UsesCapture(paper::Preset::HighContrast, 3500), "Warm contrast needs clamp-before-warmth GPU processing.");
    Require(paper::ReferencePixel({255, 255, 255}, 0, 0, paper::Constants(paper::Preset::HighContrast, 1, 3500)) ==
        paper::ReferencePixel({255, 255, 255}, 0, 0, paper::Constants(paper::Preset::Original, 1, 3500)),
        "High-contrast whites must retain the configured warm white point.");
    Require(paper::IsNeutralOriginal(paper::Preset::Original, 6500), "Original neutral mode must do no processing.");
    Require(!paper::IsNeutralOriginal(paper::Preset::Original, 3500), "Warm original colors must still be processed.");
}

void TestSharedTemperatureAPI() {
    Require(PSValidTemperature(1000) && PSValidTemperature(6500) && !PSValidTemperature(6501), "C temperature validation differs.");
    PSFilterParameters parameters{};
    Require(PSGetWarmFilterParameters(12, 1, 3500, &parameters), "Valid warm original parameters rejected.");
    const auto sentinel = parameters;
    for (int kelvin : {-1, 0, 999, 6501, std::numeric_limits<int>::max()}) {
        Require(!PSGetWarmFilterParameters(12, 1, kelvin, &parameters), "Invalid C temperature accepted.");
        Require(std::memcmp(&parameters, &sentinel, sizeof(parameters)) == 0, "Rejected temperature changed output.");
    }
    PSPixel output{};
    Require(PSReferencePixel({255, 255, 255, 255}, 0, 0, &parameters, &output), "Warm C reference rejected.");
    Require(output.red == 255 && output.green == 193 && output.blue == 144, "Warm C reference differs.");
    parameters.warmthBlue = std::numeric_limits<float>::quiet_NaN();
    Require(!PSReferencePixel({}, 0, 0, &parameters, &output), "Non-finite warmth accepted.");
    Require(PSGetFilterParameters(12, 1, &parameters), "Original-color neutral parameters rejected.");
    Require(parameters.warmthRed == 1 && parameters.warmthGreen == 1 && parameters.warmthBlue == 1,
        "Legacy parameter factory must remain neutral.");
    for (unsigned value = 0; value < 256; ++value) {
        const std::array<std::uint8_t, 3> input{static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(255 - value), static_cast<std::uint8_t>((value * 37) & 255)};
        Require(paper::ReferencePixel(input, 0, 0, paper::Constants(paper::Preset::Original)) == input,
            "Original neutral colors were changed.");
    }
}

}

int main() {
    try {
        TestTemperatureCurve();
        TestCompositorParity();
        TestSharedTemperatureAPI();
        std::cout << "Kelvin curve, neutral identity, warm white goldens, compositor parity, and C API checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
