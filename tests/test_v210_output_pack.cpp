/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * File: tests/test_v210_output_pack.cpp
 * Description: Regression tests for planar YUV422P10LE -> v210 packing,
 * including scalar/SIMD bit-exactness and active widths not divisible by six.
 */

#include "output/v210_pack.h"
#include "output/v210_pack_simd.h"
#include "input/simd_v210_avx2.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace {

static uint32_t field0(uint32_t w) { return w & 0x3ffu; }
static uint32_t field1(uint32_t w) { return (w >> 10) & 0x3ffu; }
static uint32_t field2(uint32_t w) { return (w >> 20) & 0x3ffu; }
static int rowBytesForWidth(int width) { return ((width + 47) / 48) * 128; }

static void packScalarFrame(const uint16_t* yBase,
                            const uint16_t* uBase,
                            const uint16_t* vBase,
                            int yStride,
                            int uStride,
                            int vStride,
                            int width,
                            int height,
                            uint8_t* dst,
                            int rowBytes)
{
    for (int row = 0; row < height; ++row) {
        nxframe::packYuv422p10RowToV210(
            yBase + static_cast<size_t>(row) * static_cast<size_t>(yStride),
            uBase + static_cast<size_t>(row) * static_cast<size_t>(uStride),
            vBase + static_cast<size_t>(row) * static_cast<size_t>(vStride),
            width,
            reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(row) * static_cast<size_t>(rowBytes)));
    }
}

static bool test1280Tail()
{
    constexpr int width = 1280;
    std::vector<uint16_t> y(width);
    std::vector<uint16_t> u(width / 2);
    std::vector<uint16_t> v(width / 2);

    for (int i = 0; i < width; ++i) y[i] = static_cast<uint16_t>(i & 0x3ff);
    for (int i = 0; i < width / 2; ++i) {
        u[i] = static_cast<uint16_t>((200 + i) & 0x3ff);
        v[i] = static_cast<uint16_t>((600 + i) & 0x3ff);
    }

    const int groups = (width + 5) / 6;
    std::vector<uint32_t> packed(static_cast<size_t>(groups) * 4u, 0xdeadbeefu);
    nxframe::packYuv422p10RowToV210(y.data(), u.data(), v.data(), width, packed.data());

    const int x = 1278;
    const int c = x / 2;
    const size_t base = static_cast<size_t>(groups - 1) * 4u;

    if (field0(packed[base + 0]) != u[c] ||
        field1(packed[base + 0]) != y[x] ||
        field2(packed[base + 0]) != v[c]) {
        std::cerr << "[test_v210_output_pack] first tail word mismatch\n";
        return false;
    }
    if (field0(packed[base + 1]) != y[x + 1] ||
        field1(packed[base + 1]) != 0 ||
        field2(packed[base + 1]) != 0 ||
        packed[base + 2] != 0 ||
        packed[base + 3] != 0) {
        std::cerr << "[test_v210_output_pack] tail padding mismatch\n";
        return false;
    }

    return true;
}

static bool test1920FullGroup()
{
    constexpr int width = 1920;
    std::vector<uint16_t> y(width, 64);
    std::vector<uint16_t> u(width / 2, 512);
    std::vector<uint16_t> v(width / 2, 512);
    const int groups = (width + 5) / 6;
    std::vector<uint32_t> packed(static_cast<size_t>(groups) * 4u, 0);

    nxframe::packYuv422p10RowToV210(y.data(), u.data(), v.data(), width, packed.data());

    const size_t base = static_cast<size_t>(groups - 1) * 4u;
    return field0(packed[base + 0]) == 512 &&
           field1(packed[base + 0]) == 64 &&
           field2(packed[base + 0]) == 512 &&
           field0(packed[base + 3]) == 64 &&
           field1(packed[base + 3]) == 512 &&
           field2(packed[base + 3]) == 64;
}

static bool compareSimdForSize(int width, int height, uint32_t seed)
{
    const int yStride = width + 32;
    const int cStride = width / 2 + 16;
    const int rowBytes = rowBytesForWidth(width);

    std::vector<uint16_t> y(static_cast<size_t>(yStride) * static_cast<size_t>(height));
    std::vector<uint16_t> u(static_cast<size_t>(cStride) * static_cast<size_t>(height));
    std::vector<uint16_t> v(static_cast<size_t>(cStride) * static_cast<size_t>(height));

    std::mt19937 rng(seed);
    // Include both ordinary low-bit 10-bit samples and occasional values in
    // the high-bit-aligned range handled by normalize10SampleForV210().
    std::uniform_int_distribution<int> low(0, 1023);
    std::uniform_int_distribution<int> high(0, 65535);
    for (auto& sample : y) sample = static_cast<uint16_t>((rng() % 11u) ? low(rng) : high(rng));
    for (auto& sample : u) sample = static_cast<uint16_t>((rng() % 11u) ? low(rng) : high(rng));
    for (auto& sample : v) sample = static_cast<uint16_t>((rng() % 11u) ? low(rng) : high(rng));

    const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(height);
    std::vector<uint8_t> scalar(bytes, 0x5a);
    std::vector<uint8_t> simd(bytes, 0x5a);

    packScalarFrame(y.data(), u.data(), v.data(), yStride, cStride, cStride,
                    width, height, scalar.data(), rowBytes);

    if (cpu_has_avx2()) {
        nxframe::packYuv422p10ToV210Avx2(y.data(), u.data(), v.data(),
                                         yStride, cStride, cStride,
                                         width, height, simd.data(), rowBytes);
        if (scalar != simd) {
            std::cerr << "[test_v210_output_pack] AVX2 mismatch " << width << "x" << height << "\n";
            return false;
        }
    }

    if (cpu_has_avx512_v210()) {
        std::fill(simd.begin(), simd.end(), 0x5a);
        nxframe::packYuv422p10ToV210Avx512(y.data(), u.data(), v.data(),
                                           yStride, cStride, cStride,
                                           width, height, simd.data(), rowBytes);
        if (scalar != simd) {
            std::cerr << "[test_v210_output_pack] AVX-512 mismatch " << width << "x" << height << "\n";
            return false;
        }
    }

    return true;
}

template <typename Fn>
static double benchmarkUs(Fn&& fn, int iterations)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) fn();
    const auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    return elapsed / static_cast<double>(iterations);
}

static void benchmark1920x1080()
{
    constexpr int width = 1920;
    constexpr int height = 1080;
    const int yStride = width;
    const int cStride = width / 2;
    const int rowBytes = rowBytesForWidth(width);

    std::vector<uint16_t> y(static_cast<size_t>(width) * height, 64);
    std::vector<uint16_t> u(static_cast<size_t>(width / 2) * height, 512);
    std::vector<uint16_t> v(static_cast<size_t>(width / 2) * height, 512);
    std::vector<uint8_t> dst(static_cast<size_t>(rowBytes) * height, 0);

    constexpr int iterations = 50;
    const double scalarUs = benchmarkUs([&] {
        packScalarFrame(y.data(), u.data(), v.data(), yStride, cStride, cStride,
                        width, height, dst.data(), rowBytes);
    }, iterations);

    std::cout << "[test_v210_output_pack] 1920x1080 scalar avg_us=" << scalarUs;
    if (cpu_has_avx2()) {
        const double avx2Us = benchmarkUs([&] {
            nxframe::packYuv422p10ToV210Avx2(y.data(), u.data(), v.data(),
                                             yStride, cStride, cStride,
                                             width, height, dst.data(), rowBytes);
        }, iterations);
        std::cout << " avx2=" << avx2Us;
    }
    if (cpu_has_avx512_v210()) {
        const double avx512Us = benchmarkUs([&] {
            nxframe::packYuv422p10ToV210Avx512(y.data(), u.data(), v.data(),
                                               yStride, cStride, cStride,
                                               width, height, dst.data(), rowBytes);
        }, iterations);
        std::cout << " avx512=" << avx512Us;
    }
    std::cout << "\n";
}

} // namespace

int main()
{
    if (!test1280Tail()) return 1;
    if (!test1920FullGroup()) {
        std::cerr << "[test_v210_output_pack] full-group regression failed\n";
        return 1;
    }
    if (!compareSimdForSize(1920, 8, 0x210u)) return 1;
    if (!compareSimdForSize(1280, 8, 0x1280u)) return 1;
    if (!compareSimdForSize(720, 8, 0x720u)) return 1;

    std::cout << "[test_v210_output_pack] scalar/AVX2/AVX-512 packing is bit-exact\n";
    benchmark1920x1080();
    return 0;
}
