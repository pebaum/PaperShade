#include "PaperShadeCore.h"
#include "Filters.h"
#include <cstddef>
#include <cstring>
#include <type_traits>

static_assert(sizeof(PSFilterParameters) == sizeof(paper::ShaderConstants));
static_assert(offsetof(PSFilterParameters, gain) == offsetof(paper::ShaderConstants, gain));
static_assert(offsetof(PSFilterParameters, quantizer) == offsetof(paper::ShaderConstants, quantizer));
static_assert(offsetof(PSFilterParameters, color) == offsetof(paper::ShaderConstants, color));
static_assert(offsetof(PSFilterParameters, warmthRed) == offsetof(paper::ShaderConstants, warmthRed));
static_assert(std::is_trivially_copyable_v<PSFilterParameters>);
static_assert(std::is_trivially_copyable_v<paper::ShaderConstants>);

uint32_t PSPresetCount(void) {
    return static_cast<uint32_t>(paper::Preset::Count);
}

int32_t PSValidFrameCap(uint32_t fps) {
    return paper::ValidFps(fps) ? 1 : 0;
}

int32_t PSGetFilterParameters(int32_t preset, int32_t pixelSize, PSFilterParameters* output) {
    return PSGetWarmFilterParameters(preset, pixelSize, paper::NeutralKelvin, output);
}

int32_t PSValidTemperature(uint32_t kelvin) {
    return paper::ValidKelvin(kelvin) ? 1 : 0;
}

int32_t PSGetWarmFilterParameters(int32_t preset, int32_t pixelSize, int32_t kelvin,
    PSFilterParameters* output) {
    if (!output || preset < 0 || !paper::ValidPreset(static_cast<uint32_t>(preset)) ||
        pixelSize < 1 || pixelSize > 4 || kelvin < 0 ||
        !paper::ValidKelvin(static_cast<uint32_t>(kelvin))) return 0;
    const auto values = paper::Constants(static_cast<paper::Preset>(preset),
        static_cast<uint32_t>(pixelSize), static_cast<uint32_t>(kelvin));
    std::memcpy(output, &values, sizeof(values));
    return 1;
}

int32_t PSReferencePixel(PSPixel input, int32_t x, int32_t y,
    const PSFilterParameters* parameters, PSPixel* output) {
    if (!parameters || !output || x < 0 || y < 0 ||
        parameters->pixelSize < 1 || parameters->pixelSize > 4 ||
        parameters->quantizer < 0 || parameters->quantizer > 3 ||
        parameters->levels < 2 || parameters->levels > 256 ||
        (parameters->color != 0 && parameters->color != 1) ||
        !std::isfinite(parameters->red) || !std::isfinite(parameters->green) ||
        !std::isfinite(parameters->blue) || !std::isfinite(parameters->gain) ||
        !std::isfinite(parameters->offset) ||
        parameters->red < 0 || parameters->red > 1 ||
        parameters->green < 0 || parameters->green > 1 ||
        parameters->blue < 0 || parameters->blue > 1 ||
        std::abs(parameters->gain) > 16 || std::abs(parameters->offset) > 16 ||
        !std::isfinite(parameters->warmthRed) || !std::isfinite(parameters->warmthGreen) ||
        !std::isfinite(parameters->warmthBlue) ||
        parameters->warmthRed < 0 || parameters->warmthRed > 1 ||
        parameters->warmthGreen < 0 || parameters->warmthGreen > 1 ||
        parameters->warmthBlue < 0 || parameters->warmthBlue > 1) return 0;
    paper::ShaderConstants values;
    std::memcpy(&values, parameters, sizeof(values));
    const auto pixel = paper::ReferencePixel({input.red, input.green, input.blue}, x, y, values);
    *output = PSPixel{pixel[0], pixel[1], pixel[2], 255};
    return 1;
}

int32_t PSDitherValue(int32_t quantizer, int32_t index, int32_t* output) {
    if (!output || index < 0 || index >= 16 || (quantizer != 2 && quantizer != 3)) return 0;
    *output = quantizer == 3 ? paper::Ps1Matrix[static_cast<size_t>(index)] :
        paper::BayerMatrix[static_cast<size_t>(index)];
    return 1;
}
