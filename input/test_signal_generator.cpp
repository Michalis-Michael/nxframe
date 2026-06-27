/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * License / EULA notice:
 * This file is part of NxFrame. Use, redistribution, and modification are
 * governed by the project license and any written EULA or commercial license
 * agreement supplied with the project. If no separate written agreement is
 * supplied, the GPL-3.0-or-later terms apply.
 *
 * Description:
 * Deterministic test-pattern and tone generator for the NxFrame input
 * subsystem. The generated video matches the internal sender bus
 * (YUV422P10LE), which allows the test source to exercise the same downstream
 * encoder path as normalized DeckLink SDI input.
 */

#include "test_signal_generator.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TestSignalGenerator::TestSignalGenerator()
: width_(1920), height_(1080), fps_(50), sampleRate_(48000),
  phaseL_(0.0), phaseR_(0.0) {}

TestSignalGenerator::TestSignalGenerator(int w, int h, int fps, int sr)
: width_(w), height_(h), fps_(fps > 0 ? fps : 50),
  sampleRate_(sr > 0 ? sr : 48000),
  phaseL_(0.0), phaseR_(0.0) {}

void TestSignalGenerator::setVideoFormat(int w, int h, int fps) {
    width_ = w;
    height_ = h;
    fps_ = (fps > 0) ? fps : fps_;
}

void TestSignalGenerator::setAudioFormat(int sampleRate) {
    if (sampleRate > 0) {
        sampleRate_ = sampleRate;
    }
}

void TestSignalGenerator::generateFrame(uint8_t* buffer, int width, int height, int strideBytes) {
    if (!buffer || width <= 0 || height <= 0) return;

    // The test source deliberately uses the same internal bus as the DeckLink
    // capture path: planar 10-bit 4:2:2 stored in 16-bit little-endian words.
    // This keeps encoder-side zero-copy and format assumptions identical when
    // switching between real SDI and the synthetic source.
    const int min_y_stride_bytes = width * 2;

    // A positive external stride must be large enough for one luma row and must
    // preserve 16-bit alignment. Invalid stride values fall back to tight
    // packing instead of risking corrupted plane offsets.
    const bool valid_external_stride =
        (strideBytes >= min_y_stride_bytes) &&
        ((strideBytes & 1) == 0);

    const int y_stride_bytes =
        valid_external_stride ? strideBytes : min_y_stride_bytes;

    const int uv_stride_bytes =
        valid_external_stride ? (strideBytes / 2) : ((width / 2) * 2);

    uint8_t* y_base = buffer;
    uint8_t* u_base = y_base + static_cast<size_t>(y_stride_bytes) * static_cast<size_t>(height);
    uint8_t* v_base = u_base + static_cast<size_t>(uv_stride_bytes) * static_cast<size_t>(height);

    auto y_row = [&](int y) -> uint16_t* {
        return reinterpret_cast<uint16_t*>(y_base + static_cast<size_t>(y) * static_cast<size_t>(y_stride_bytes));
    };
    auto u_row = [&](int y) -> uint16_t* {
        return reinterpret_cast<uint16_t*>(u_base + static_cast<size_t>(y) * static_cast<size_t>(uv_stride_bytes));
    };
    auto v_row = [&](int y) -> uint16_t* {
        return reinterpret_cast<uint16_t*>(v_base + static_cast<size_t>(y) * static_cast<size_t>(uv_stride_bytes));
    };

    // SMPTE-like seven-bar pattern. The constants are nominal 8-bit YCbCr
    // values promoted to the 10-bit internal range by shifting left by two.
    static const struct { uint16_t y, u, v; } bars[] = {
        { uint16_t(180u << 2), uint16_t(128u << 2), uint16_t(128u << 2) },
        { uint16_t(168u << 2), uint16_t( 44u << 2), uint16_t(136u << 2) },
        { uint16_t(145u << 2), uint16_t(147u << 2), uint16_t( 44u << 2) },
        { uint16_t(133u << 2), uint16_t( 63u << 2), uint16_t( 52u << 2) },
        { uint16_t( 63u << 2), uint16_t(193u << 2), uint16_t(204u << 2) },
        { uint16_t( 51u << 2), uint16_t(109u << 2), uint16_t(212u << 2) },
        { uint16_t( 28u << 2), uint16_t(212u << 2), uint16_t(120u << 2) },
    };

    for (int y = 0; y < height; ++y) {
        uint16_t* Y = y_row(y);
        uint16_t* U = u_row(y);
        uint16_t* V = v_row(y);

        for (int x = 0; x < width; ++x) {
            int bar = (x * 7) / width;
            if (bar > 6) {
                bar = 6;
            }

            const uint16_t yy = bars[bar].y;
            const uint16_t uu = bars[bar].u;
            const uint16_t vv = bars[bar].v;

            Y[x] = yy;

            // YUV422 stores one chroma sample for each pair of horizontal luma
            // samples. Write U/V only on the first pixel of each pair.
            if ((x & 1) == 0) {
                const int cx = x >> 1;
                U[cx] = uu;
                V[cx] = vv;
            }
        }
    }
}

void TestSignalGenerator::generateAudioFrame(int16_t* buffer, int numChannels, int numSamples) {
    if (!buffer || numChannels <= 0 || numSamples <= 0) return;

    const double fL = 1000.0;
    const double fR =  800.0;
    const double incL = 2.0 * M_PI * fL / static_cast<double>(sampleRate_);
    const double incR = 2.0 * M_PI * fR / static_cast<double>(sampleRate_);

    for (int i = 0; i < numSamples; ++i) {
        const int16_t sL = static_cast<int16_t>(std::sin(phaseL_) * 30000.0);
        const int16_t sR = static_cast<int16_t>(std::sin(phaseR_) * 30000.0);

        buffer[i * numChannels + 0] = sL;
        if (numChannels >= 2) {
            buffer[i * numChannels + 1] = sR;
        }
        for (int ch = 2; ch < numChannels; ++ch) {
            buffer[i * numChannels + ch] = sL;
        }

        phaseL_ += incL;
        phaseR_ += incR;
        if (phaseL_ >= 2.0 * M_PI) {
            phaseL_ -= 2.0 * M_PI;
        }
        if (phaseR_ >= 2.0 * M_PI) {
            phaseR_ -= 2.0 * M_PI;
        }
    }
}
