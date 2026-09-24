/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small helpers for packing planar 10-bit 4:2:2 rows into v210.
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace nxframe {

inline uint32_t normalize10SampleForV210(uint16_t v)
{
    uint32_t sample = static_cast<uint32_t>(v);
    if (sample > 1023) {
        sample = (sample + 32) >> 6;
    }
    return std::min<uint32_t>(1023, sample);
}

inline void packYuv422p10RowToV210(const uint16_t* yRow,
                                   const uint16_t* uRow,
                                   const uint16_t* vRow,
                                   int width,
                                   uint32_t* out)
{
    const int chromaWidth = width / 2;

    auto ySample = [&](int x) -> uint32_t {
        return (x >= 0 && x < width)
            ? normalize10SampleForV210(yRow[x])
            : 0u;
    };
    auto uSample = [&](int x) -> uint32_t {
        return (x >= 0 && x < chromaWidth)
            ? normalize10SampleForV210(uRow[x])
            : 0u;
    };
    auto vSample = [&](int x) -> uint32_t {
        return (x >= 0 && x < chromaWidth)
            ? normalize10SampleForV210(vRow[x])
            : 0u;
    };

    for (int x = 0; x < width; x += 6) {
        const int c = x / 2;

        const uint32_t U0 = uSample(c + 0);
        const uint32_t Y0 = ySample(x + 0);
        const uint32_t V0 = vSample(c + 0);

        const uint32_t Y1 = ySample(x + 1);
        const uint32_t U1 = uSample(c + 1);
        const uint32_t Y2 = ySample(x + 2);

        const uint32_t V1 = vSample(c + 1);
        const uint32_t Y3 = ySample(x + 3);
        const uint32_t U2 = uSample(c + 2);

        const uint32_t Y4 = ySample(x + 4);
        const uint32_t V2 = vSample(c + 2);
        const uint32_t Y5 = ySample(x + 5);

        out[0] = U0 | (Y0 << 10) | (V0 << 20);
        out[1] = Y1 | (U1 << 10) | (Y2 << 20);
        out[2] = V1 | (Y3 << 10) | (U2 << 20);
        out[3] = Y4 | (V2 << 10) | (Y5 << 20);
        out += 4;
    }
}

} // namespace nxframe
