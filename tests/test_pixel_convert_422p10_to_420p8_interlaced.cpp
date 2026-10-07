#include "video/pixel_convert.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace pc = nxframe::pixel_convert;

namespace {

uint8_t p10ToP8(unsigned v)
{
    const unsigned q = (v + 2u) >> 2;
    return static_cast<uint8_t>(q > 255u ? 255u : q);
}

void reference(const std::vector<uint16_t>& y,
               const std::vector<uint16_t>& u,
               const std::vector<uint16_t>& v,
               int width, int height,
               std::vector<uint8_t>& outY,
               std::vector<uint8_t>& outU,
               std::vector<uint8_t>& outV)
{
    const int cw = width / 2;
    const int ch = height / 2;
    outY.resize(static_cast<size_t>(width) * height);
    outU.resize(static_cast<size_t>(cw) * ch);
    outV.resize(static_cast<size_t>(cw) * ch);

    for (size_t i = 0; i < y.size(); ++i) outY[i] = p10ToP8(y[i]);

    auto filterPlane = [&](const std::vector<uint16_t>& src, std::vector<uint8_t>& dst) {
        for (int oy = 0; oy < ch; ++oy) {
            const int field = oy & 1;
            const int fieldPair = oy >> 1;
            const int y0 = fieldPair * 4 + field;
            const int y1 = y0 + 2;
            for (int x = 0; x < cw; ++x) {
                const unsigned a = src[static_cast<size_t>(y0) * cw + x];
                const unsigned b = src[static_cast<size_t>(y1) * cw + x];
                const unsigned weighted = field == 0 ? (3u * a + b) : (a + 3u * b);
                const unsigned filtered10 = (weighted + 2u) >> 2;
                dst[static_cast<size_t>(oy) * cw + x] = p10ToP8(filtered10);
            }
        }
    };

    filterPlane(u, outU);
    filterPlane(v, outV);
}

bool runOne(int width, int height, pc::Backend backend, const char* name)
{
    const int cw = width / 2;
    std::mt19937 rng(0x8420u + static_cast<unsigned>(width + height));
    std::uniform_int_distribution<int> dist(0, 1023);

    std::vector<uint16_t> y(static_cast<size_t>(width) * height);
    std::vector<uint16_t> u(static_cast<size_t>(cw) * height);
    std::vector<uint16_t> v(static_cast<size_t>(cw) * height);
    for (auto& s : y) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : u) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : v) s = static_cast<uint16_t>(dist(rng));

    // Force distinct field values and quantization edge cases. This catches
    // progressive line pairing, 50/50 same-field averaging, incorrect phase,
    // and 1023 -> 256 wraparound instead of saturation to 255.
    for (int x = 0; x < cw; ++x) {
        u[0 * cw + x] = 0;
        u[1 * cw + x] = 1023;
        u[2 * cw + x] = 200;
        u[3 * cw + x] = 800;
        v[0 * cw + x] = 1023;
        v[1 * cw + x] = 0;
        v[2 * cw + x] = 1023;
        v[3 * cw + x] = 1023;
    }
    y[0] = 1023;
    y[1] = 1022;

    std::vector<uint8_t> refY, refU, refV;
    reference(y, u, v, width, height, refY, refU, refV);

    std::vector<uint8_t> gotY(refY.size(), 0xA5u);
    std::vector<uint8_t> gotU(refU.size(), 0xA5u);
    std::vector<uint8_t> gotV(refV.size(), 0xA5u);

    pc::Yuv422p10View src{{reinterpret_cast<const uint8_t*>(y.data()),
                           reinterpret_cast<const uint8_t*>(u.data()),
                           reinterpret_cast<const uint8_t*>(v.data())},
                          {width * 2, cw * 2, cw * 2}, width, height};
    pc::Yuv420p8View dst{{gotY.data(), gotU.data(), gotV.data()},
                         {width, cw, cw}, width, height};

    pc::Backend used = pc::Backend::Auto;
    if (!pc::convert422p10To420p8Interlaced(src, dst, backend, &used)) return false;

    if (gotY != refY || gotU != refU || gotV != refV) {
        std::cerr << name << " mismatch for " << width << "x" << height << '\n';
        return false;
    }

    std::cout << name << " " << width << "x" << height << ": PASS\n";
    return true;
}

} // namespace

int main()
{
    if (!pc::cpuHasAvx2()) return 77;

    const int sizes[][2] = {{1280, 720}, {1920, 1080}, {1300, 724}};
    for (const auto& s : sizes) {
        if (!runOne(s[0], s[1], pc::Backend::AVX2, "AVX2")) return 1;
    }

    if (pc::cpuHasAvx512Bw()) {
        for (const auto& s : sizes) {
            if (!runOne(s[0], s[1], pc::Backend::AVX512, "AVX-512")) return 1;
        }
    }

    return 0;
}
