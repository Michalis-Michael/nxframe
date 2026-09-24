/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * File: tests/test_v210_output_pack.cpp
 * Description: Regression tests for planar YUV422P10LE -> v210 row packing,
 * including active widths that are not divisible by six (for example 1280).
 */

#include "output/v210_pack.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

static uint32_t field0(uint32_t w) { return w & 0x3ffu; }
static uint32_t field1(uint32_t w) { return (w >> 10) & 0x3ffu; }
static uint32_t field2(uint32_t w) { return (w >> 20) & 0x3ffu; }

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

    // The final 1280-wide group contains exactly two active luma pixels and
    // one active chroma pair. All samples beyond the active row must be zero.
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

} // namespace

int main()
{
    if (!test1280Tail()) return 1;
    if (!test1920FullGroup()) {
        std::cerr << "[test_v210_output_pack] full-group regression failed\n";
        return 1;
    }

    std::cout << "[test_v210_output_pack] 1280 tail padding and 1920 full-group packing OK\n";
    return 0;
}
