cbuffer Filter : register(b0)
{
    float3 weights;
    float gain;
    float offset;
    int quantizer;
    int levels;
    int pixelSize;
    int keepColor;
    int3 padding;
};

Texture2D<float4> desktop : register(t0);

float4 VertexMain(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

static const int ps1[16] = {
    -4, 0, -3, 1,
     2,-2,  3,-1,
    -3, 1, -4, 0,
     3,-1,  2,-2
};
static const int bayer[16] = {
     0, 8, 2,10,
    12, 4,14, 6,
     3,11, 1, 9,
    15, 7,13, 5
};

float4 PixelMain(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float3 source = desktop.Load(int3(pixel, 0)).rgb;
    float gray = saturate(dot(source, weights) * gain + offset);
    float3 color = keepColor != 0 ? source : gray.xxx;
    int2 grid = pixel / max(pixelSize, 1);
    int index = (grid.y & 3) * 4 + (grid.x & 3);

    if (quantizer == 3)
    {
        // PS1: signed dither in 8-bit space, saturate, then discard three bits.
        int3 bytes = int3(floor(saturate(color) * 255.0 + 0.5));
        int3 rgb5 = clamp(bytes + ps1[index], 0, 255) >> 3;
        int3 rgb8 = (rgb5 << 3) | (rgb5 >> 2);
        return float4(float3(rgb8) / 255.0, 1);
    }

    if (quantizer == 1 || quantizer == 2)
    {
        float threshold = quantizer == 2 ? (bayer[index] + 0.5) / 16.0 - 0.5 : 0.0;
        float steps = float(levels - 1);
        color = saturate(floor(color * steps + 0.5 + threshold) / steps);
    }
    return float4(color, 1);
}
