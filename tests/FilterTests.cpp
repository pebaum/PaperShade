#include "Filters.h"
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestPs1() {
    constexpr int reference[4][4]{
        {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}
    };
    for (int input = 0; input < 256; ++input) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const int sum = input + reference[y][x];
                const int expected = sum < 0 ? 0 : sum > 255 ? 31 : sum / 8;
                Require(paper::Ps1FiveBit(input, x, y) == expected, "PS1 arithmetic differs from the reference.");
                Require(paper::Ps1FiveBit(input, x + 4, y + 8) == expected, "PS1 matrix is not periodic.");
            }
        }
    }
    Require(paper::Ps1FiveBit(8, 0, 0) == 0, "PS1 negative dither must happen before truncation.");
    Require(paper::Ps1FiveBit(5, 2, 1) == 1, "PS1 positive dither must cross a quantization boundary.");
    Require(paper::ExpandFiveBit(0) == 0 && paper::ExpandFiveBit(31) == 255, "RGB555 endpoint expansion is wrong.");
    Require(paper::ExpandFiveBit(3) == 24, "RGB555 display expansion must use bit replication, not rounded normalization.");
}

void TestMatrices() {
    for (unsigned index = 0; index < 6; ++index) {
        const auto preset = static_cast<paper::Preset>(index);
        const auto matrix = paper::ColorMatrix(preset);
        const auto constants = paper::Constants(preset);
        for (const auto input : {std::array<float, 3>{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {0, 0, 0}}) {
            const float expected = input[0] * constants.red * constants.gain +
                input[1] * constants.green * constants.gain +
                input[2] * constants.blue * constants.gain + constants.offset;
            for (int channel = 0; channel < 3; ++channel) {
                const float actual = input[0] * matrix[channel] + input[1] * matrix[5 + channel] +
                    input[2] * matrix[10 + channel] + matrix[20 + channel];
                Require(std::abs(actual - expected) < 0.00001f, "Magnification matrix orientation or offset is wrong.");
            }
        }
        Require(matrix[18] == 1 && matrix[24] == 1, "Matrix must preserve alpha and homogeneous coordinates.");
        Require(!paper::UsesCapture(preset), "Basic grayscale must not initialize screen capture.");
    }
    for (unsigned index = 6; index <= static_cast<unsigned>(paper::Preset::Ps1Color); ++index) {
        Require(paper::UsesCapture(static_cast<paper::Preset>(index)), "A nonlinear preset was routed to a linear matrix.");
    }
}

void TestPalettes() {
    for (const auto preset : {paper::Preset::InkThreshold, paper::Preset::InkDither,
             paper::Preset::Ink4, paper::Preset::Ink16, paper::Preset::Ps1Gray}) {
        const auto constants = paper::Constants(preset);
        std::set<int> shades;
        for (int value = 0; value < 256; ++value) {
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const auto byte = static_cast<std::uint8_t>(value);
                    const auto result = paper::ReferencePixel({byte, byte, byte}, x, y, constants);
                    Require(result[0] == result[1] && result[1] == result[2], "A grayscale preset introduced color.");
                    shades.insert(result[0]);
                }
            }
        }
        Require(shades.size() == static_cast<std::size_t>(constants.levels), "Unexpected e-ink/PS1 palette size.");
        Require(*shades.begin() == 0 && *shades.rbegin() == 255, "Palette does not preserve black and white.");
    }
    const auto native = paper::Constants(paper::Preset::Ps1Color);
    const auto scaled = paper::Constants(paper::Preset::Ps1Color, 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            Require(paper::ReferencePixel({127, 51, 8}, x, y, scaled) ==
                paper::ReferencePixel({127, 51, 8}, x / 4, y / 4, native), "Artistic pattern scaling moved the source pixels.");
        }
    }
}

}

int main() {
    try {
        TestPs1();
        TestMatrices();
        TestPalettes();
        Require(paper::PresetNames.size() == static_cast<std::size_t>(paper::Preset::Count), "Preset labels are incomplete.");
        Require(paper::ValidFps(10) && paper::ValidFps(15) && paper::ValidFps(30) && paper::ValidFps(60), "Supported frame cap rejected.");
        Require(!paper::ValidFps(0) && !paper::ValidFps(144), "Unsupported frame cap accepted.");
        Require(paper::ValidPreset(12) && !paper::ValidPreset(13) && !paper::ValidPreset(0xffffffff), "Invalid preset accepted.");
        Require(!paper::Settings{}.enabled, "First launch must not change the display without a user action.");
        Require(paper::Settings{}.hideCaptureIndicator, "Borderless capture must be preferred by default.");
        std::cout << "All filter, palette, settings-boundary, and 4096 exhaustive PS1 cases passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
