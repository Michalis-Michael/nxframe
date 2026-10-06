/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: tests/test_v210_scalar_vs_avx2.cpp
 * Description: NxFrame regression and validation tests.
 */

#include "input/simd_v210_avx2.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

static int v210RowBytes(int width)
{
    // v210 stores 48 pixels in 128 bytes. Rows are padded to the next 48-pixel block.
    return ((width + 47) / 48) * 128;
}

static bool comparePlane(const char* name,
                         const std::vector<uint16_t>& scalar,
                         const std::vector<uint16_t>& simd,
                         int width,
                         int height)
{
    if (scalar.size() != simd.size()) {
        std::cerr << "[test_v210] " << name << " size mismatch scalar=" << scalar.size()
                  << " simd=" << simd.size() << "\n";
        return false;
    }

    for (size_t i = 0; i < scalar.size(); ++i) {
        if (scalar[i] != simd[i]) {
            const int x = width > 0 ? static_cast<int>(i % static_cast<size_t>(width)) : 0;
            const int y = width > 0 ? static_cast<int>(i / static_cast<size_t>(width)) : 0;
            std::cerr << "[test_v210] " << name << " mismatch at index=" << i
                      << " x=" << x << " y=" << y
                      << " scalar=" << scalar[i]
                      << " simd=" << simd[i]
                      << " frame=" << width << "x" << height << "\n";
            return false;
        }
    }
    return true;
}

static bool runCase(int width, int height, uint32_t seed)
{
    const int rowBytes = v210RowBytes(width);
    std::vector<uint8_t> src(static_cast<size_t>(rowBytes) * static_cast<size_t>(height));

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> byteDist(0, 255);
    for (uint8_t& b : src) {
        b = static_cast<uint8_t>(byteDist(rng));
    }

    const size_t ySamples = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t cSamples = static_cast<size_t>(width / 2) * static_cast<size_t>(height);

    std::vector<uint16_t> yScalar(ySamples, 0xFFFF), uScalar(cSamples, 0xFFFF), vScalar(cSamples, 0xFFFF);
    std::vector<uint16_t> ySimd(ySamples, 0xFFFF), uSimd(cSamples, 0xFFFF), vSimd(cSamples, 0xFFFF);

    v210_to_yuv422p10le_scalar(src.data(), rowBytes, width, height,
                               yScalar.data(), uScalar.data(), vScalar.data());

    if (cpu_has_avx2()) {
        v210_to_yuv422p10le_avx2(src.data(), rowBytes, width, height,
                                 ySimd.data(), uSimd.data(), vSimd.data());
        if (!comparePlane("Y AVX2", yScalar, ySimd, width, height) ||
            !comparePlane("U AVX2", uScalar, uSimd, width / 2, height) ||
            !comparePlane("V AVX2", vScalar, vSimd, width / 2, height)) return false;
    }

    if (cpu_has_avx512_v210()) {
        std::fill(ySimd.begin(), ySimd.end(), 0xFFFF);
        std::fill(uSimd.begin(), uSimd.end(), 0xFFFF);
        std::fill(vSimd.begin(), vSimd.end(), 0xFFFF);
        v210_to_yuv422p10le_avx512(src.data(), rowBytes, width, height,
                                   ySimd.data(), uSimd.data(), vSimd.data());
        if (!comparePlane("Y AVX512", yScalar, ySimd, width, height) ||
            !comparePlane("U AVX512", uScalar, uSimd, width / 2, height) ||
            !comparePlane("V AVX512", vScalar, vSimd, width / 2, height)) return false;
    }

    return true;
}

} // namespace

int main()
{
    std::cout << "[test_v210] AVX2=" << (cpu_has_avx2() ? "yes" : "no")
              << " AVX512=" << (cpu_has_avx512_v210() ? "yes" : "no") << "\n";
    if (!cpu_has_avx2() && !cpu_has_avx512_v210()) {
        std::cout << "[test_v210] SIMD unavailable; scalar reference only.\n";
        return 0;
    }

    struct Case { int width; int height; };
    const std::vector<Case> cases = {
        {2, 1}, {6, 2}, {12, 3}, {48, 4}, {64, 5},
        {720, 8}, {1280, 16}, {1920, 16}, {1920, 1080}
    };

    uint32_t seed = 0x210A11u;
    for (const Case& c : cases) {
        if ((c.width % 2) != 0) {
            std::cerr << "[test_v210] internal test error: width must be even.\n";
            return 2;
        }
        if (!runCase(c.width, c.height, seed++)) {
            return 1;
        }
        std::cout << "[test_v210] OK " << c.width << "x" << c.height << "\n";
    }

    std::atomic<bool> ok{true};
    std::vector<std::thread> callers;
    for (unsigned i=0;i<4;++i) callers.emplace_back([&,i] {
        for (unsigned n=0;n<4;++n)
            if (!runCase(1920,1080,0x210000+i*16+n)) ok.store(false);
    });
    for (auto& caller:callers) caller.join();
    if (!ok.load()) return 1;
    std::cout << "[test_v210] scalar/SIMD outputs match for all available paths.\n";
    return 0;
}
