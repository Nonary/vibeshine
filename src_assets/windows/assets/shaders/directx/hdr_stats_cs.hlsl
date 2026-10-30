// Per-frame statistics for HDR10+ dynamic metadata (src/hdr_dynamic_metadata.h).
//
// PQ(maxRGB) of the captured frame, in the signal the converters encode, at the centre of
// each cell of a grid of at most 240x135 cells (video::hdr_dynamic::sample_grid), counted
// in 1024 equal PQ bins, with the samples' minimum and maximum. Each 16x16 group histograms
// its tile in groupshared memory and folds it into the frame's with one atomic per
// occupied bin.
//
// Ported from Foundation Sunshine's hdr_luminance_analysis_cs.hlsl
// (https://github.com/AlkaidLab/foundation-sunshine, commit 880f1ef).
// Copyright (c) Foundation Sunshine authors, GPL-3.0.
//
// Readout (video::hdr_dynamic::parse_readout): word 0 is the minimum as inverted float
// bits, so the zeroed buffer finds it with an atomic maximum; word 1 is the maximum as
// float bits; then the bins. The host zeroes the buffer before every dispatch.

#include "include/common.hlsl"

static const uint BINS = 1024;

// The transfer functions of the converters (include/convert_*_base.hlsl).
static const uint TRANSFER_SCRGB = 0;  // FP16 scRGB: Rec. 2100 PQ, after `scale` (TrueHDR peak expansion)
static const uint TRANSFER_SDR_TO_PQ = 1;  // UNORM sRGB: SDR white placed at `sdr_white_nits`
static const uint TRANSFER_ENCODED = 2;  // UNORM: the values already are the signal

Texture2D image : register(t0);

cbuffer stats_params_cbuffer : register(b0) {
    uint2 grid;  // sample cells across and down
    uint2 source_size;  // texels across and down
    uint transfer;
    float scale;
    float sdr_white_nits;
    float stats_params_padding;
};

RWStructuredBuffer<uint> stats : register(u0);

groupshared uint group_bins[BINS];
groupshared uint group_min;  // inverted float bits, like word 0
groupshared uint group_max;

float3 RemoveSRGBCurve(float3 x)
{
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

float3 to_pq(float3 rgb)
{
    if (transfer == TRANSFER_SCRGB) {
        return scRGBTo2100PQ(rgb * scale);
    }
    if (transfer == TRANSFER_SDR_TO_PQ) {
        return NitsToPQ(Rec709toRec2020(RemoveSRGBCurve(saturate(rgb))) * sdr_white_nits);
    }
    return rgb;
}

[numthreads(16, 16, 1)]
void main_cs(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    [unroll] for (uint clear = 0; clear < BINS / 256; clear++) {
        group_bins[index + clear * 256] = 0;
    }
    if (index == 0) {
        group_min = 0;
        group_max = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (all(id.xy < grid)) {
        float3 pq = to_pq(image.Load(int3((2 * id.xy + 1) * source_size / (2 * grid), 0)).rgb);
        // Without the sign bit, the bits of floats in 0..1 order as integers.
        uint code = asuint(saturate(max(pq.r, max(pq.g, pq.b)))) & 0x7fffffff;
        InterlockedAdd(group_bins[min(uint(asfloat(code) * BINS), BINS - 1)], 1);
        InterlockedMax(group_min, ~code);
        InterlockedMax(group_max, code);
    }
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint fold = 0; fold < BINS / 256; fold++) {
        uint bin = index + fold * 256;
        if (group_bins[bin] != 0) {
            InterlockedAdd(stats[2 + bin], group_bins[bin]);
        }
    }
    // A sample leaves group_min nonzero: the inverted bits of 0..1 are all above 0xc07fffff.
    if (index == 0 && group_min != 0) {
        InterlockedMax(stats[0], group_min);
        InterlockedMax(stats[1], group_max);
    }
}
