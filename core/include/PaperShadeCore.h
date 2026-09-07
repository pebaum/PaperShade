#ifndef PAPERSHADE_CORE_H
#define PAPERSHADE_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable 48-byte layout shared with HLSL and Metal constant buffers. */
typedef struct PSFilterParameters {
    float red;
    float green;
    float blue;
    float gain;
    float offset;
    int32_t quantizer;
    int32_t levels;
    int32_t pixelSize;
    int32_t color;
    int32_t padding0;
    int32_t padding1;
    int32_t padding2;
} PSFilterParameters;

typedef struct PSPixel {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} PSPixel;

/* Validation functions return 1 on success and 0 without modifying outputs on failure. */
uint32_t PSPresetCount(void);
int32_t PSValidFrameCap(uint32_t fps);
int32_t PSGetFilterParameters(int32_t preset, int32_t pixelSize, PSFilterParameters* output);
int32_t PSReferencePixel(PSPixel input, int32_t x, int32_t y,
    const PSFilterParameters* parameters, PSPixel* output);
int32_t PSDitherValue(int32_t quantizer, int32_t index, int32_t* output);

#ifdef __cplusplus
}
#endif

#endif
