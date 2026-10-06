#include "video/pixel_convert.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

using nxframe::pixel_convert::Backend;
using nxframe::pixel_convert::Yuv422p10View;
using nxframe::pixel_convert::Yuv422p8View;

namespace {

uint8_t ref10to8(uint16_t v)
{
    const unsigned q = (static_cast<unsigned>(v) + 2u) >> 2;
    return static_cast<uint8_t>(q > 255u ? 255u : q);
}

bool runCase(int width, int height, Backend backend)
{
    const int cw = width / 2;
    std::vector<uint16_t> y(static_cast<size_t>(width) * height);
    std::vector<uint16_t> u(static_cast<size_t>(cw) * height);
    std::vector<uint16_t> v(static_cast<size_t>(cw) * height);
    std::vector<uint8_t> yo(y.size(), 0), uo(u.size(), 0), vo(v.size(), 0);

    std::mt19937 rng(0x42210u + static_cast<unsigned>(width + height));
    std::uniform_int_distribution<int> dist(0, 1023);
    for (auto& s : y) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : u) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : v) s = static_cast<uint16_t>(dist(rng));

    // Deterministic edge values catch rounding and 1023 -> 255 saturation.
    const uint16_t edges[] = {0,1,2,3,4,5,63,64,65,939,940,959,960,1021,1022,1023};
    for (size_t i = 0; i < sizeof(edges)/sizeof(edges[0]) && i < y.size(); ++i) y[i] = edges[i];
    for (size_t i = 0; i < sizeof(edges)/sizeof(edges[0]) && i < u.size(); ++i) u[i] = edges[i];
    for (size_t i = 0; i < sizeof(edges)/sizeof(edges[0]) && i < v.size(); ++i) v[i] = edges[i];

    Yuv422p10View src;
    src.data[0] = reinterpret_cast<const uint8_t*>(y.data());
    src.data[1] = reinterpret_cast<const uint8_t*>(u.data());
    src.data[2] = reinterpret_cast<const uint8_t*>(v.data());
    src.stride[0] = width * 2;
    src.stride[1] = cw * 2;
    src.stride[2] = cw * 2;
    src.width = width;
    src.height = height;

    Yuv422p8View dst;
    dst.data[0] = yo.data(); dst.data[1] = uo.data(); dst.data[2] = vo.data();
    dst.stride[0] = width; dst.stride[1] = cw; dst.stride[2] = cw;
    dst.width = width; dst.height = height;

    if (!nxframe::pixel_convert::convert422p10To422p8(src, dst, backend, nullptr)) return false;

    auto check = [](const std::vector<uint16_t>& in, const std::vector<uint8_t>& out, const char* name) {
        for (size_t i = 0; i < in.size(); ++i) {
            const uint8_t expected = ref10to8(in[i]);
            if (out[i] != expected) {
                std::cerr << name << " mismatch at " << i << ": in=" << in[i]
                          << " got=" << static_cast<int>(out[i])
                          << " expected=" << static_cast<int>(expected) << "\n";
                return false;
            }
        }
        return true;
    };
    return check(y, yo, "Y") && check(u, uo, "U") && check(v, vo, "V");
}

} // namespace

int main()
{
    const int widths[] = {1280, 1920, 1300};
    const int heights[] = {720, 1080, 8};

    bool tested = false;
    if (nxframe::pixel_convert::cpuHasAvx2()) {
        for (int i = 0; i < 3; ++i) {
            if (!runCase(widths[i], heights[i], Backend::AVX2)) return 1;
        }
        std::cout << "AVX2 correctness: PASS\n";
        tested = true;
    }
    if (nxframe::pixel_convert::cpuHasAvx512Bw()) {
        for (int i = 0; i < 3; ++i) {
            if (!runCase(widths[i], heights[i], Backend::AVX512)) return 1;
        }
        std::cout << "AVX-512 correctness: PASS\n";
        tested = true;
    }

    if (!tested) {
        std::cout << "SKIP: CPU has neither AVX2 nor required AVX-512BW support\n";
        return 77;
    }
    return 0;
}
