#include "PaperShadeCore.h"
#include "Filters.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        Require(PSPresetCount() == paper::PresetNames.size(), "Preset counts differ across the C bridge.");
        Require(sizeof(PSFilterParameters) == 48 && sizeof(PSPixel) == 4, "Cross-platform GPU ABI changed.");
        PSFilterParameters parameters{};
        Require(!PSGetFilterParameters(-1, 1, &parameters), "Invalid preset accepted.");
        Require(!PSGetFilterParameters(12, 1, &parameters), "Out-of-range preset accepted.");
        Require(!PSGetFilterParameters(0, 0, &parameters), "Zero pixel size accepted.");
        Require(!PSGetFilterParameters(0, 5, &parameters), "Oversized dither accepted.");
        Require(!PSGetFilterParameters(0, 1, nullptr), "Null output accepted.");
        Require(!PSValidFrameCap(0) && PSValidFrameCap(15), "Cross-platform frame caps differ.");
        for (int index = 0; index < 16; ++index) {
            int value = 0;
            Require(PSDitherValue(3, index, &value) && value == paper::Ps1Matrix[index], "PS1 table differs.");
            Require(PSDitherValue(2, index, &value) && value == paper::BayerMatrix[index], "Bayer table differs.");
        }
        for (int preset = 0; preset < static_cast<int>(PSPresetCount()); ++preset) {
            for (int scale = 1; scale <= 4; ++scale) {
                Require(PSGetFilterParameters(preset, scale, &parameters) == 1, "Valid parameters rejected.");
                const auto expectedParameters = paper::Constants(static_cast<paper::Preset>(preset), scale);
                for (int value = 0; value < 256; ++value) {
                    const PSPixel input{static_cast<uint8_t>(value),
                        static_cast<uint8_t>((value * 13) & 255),
                        static_cast<uint8_t>((value * 53) & 255), 255};
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            PSPixel result{};
                            Require(PSReferencePixel(input, x, y, &parameters, &result) == 1, "Reference pixel rejected.");
                            const auto expected = paper::ReferencePixel(
                                {input.red, input.green, input.blue}, x, y, expectedParameters);
                            Require(result.red == expected[0] && result.green == expected[1] &&
                                result.blue == expected[2] && result.alpha == 255, "C bridge changed filter math.");
                        }
                    }
                }
            }
        }
        PSPixel result{};
        Require(!PSReferencePixel({}, -1, 0, &parameters, &result), "Negative framebuffer position accepted.");
        parameters.pixelSize = 0;
        Require(!PSReferencePixel({}, 0, 0, &parameters, &result), "Invalid parameters accepted.");
        Require(PSGetFilterParameters(0, 1, &parameters), "Unable to restore valid parameters.");
        parameters.red = std::numeric_limits<float>::quiet_NaN();
        Require(!PSReferencePixel({}, 0, 0, &parameters, &result), "Non-finite weight accepted.");
        parameters.red = std::numeric_limits<float>::max();
        Require(!PSReferencePixel({}, 0, 0, &parameters, &result), "Overflowing weight accepted.");
        std::cout << "Shared C/C++ filter ABI and 196608 cross-platform reference pixels passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
