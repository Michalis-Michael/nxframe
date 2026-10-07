#include "video/pixel_convert.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace pc = nxframe::pixel_convert;

namespace {

void reference(const std::vector<uint16_t>& y,
               const std::vector<uint16_t>& u,
               const std::vector<uint16_t>& v,
               int width, int height,
               std::vector<uint16_t>& outY,
               std::vector<uint16_t>& outU,
               std::vector<uint16_t>& outV)
{
    const int cw = width / 2;
    const int ch = height / 2;
    outY = y;
    outU.resize(static_cast<size_t>(cw) * ch);
    outV.resize(static_cast<size_t>(cw) * ch);

    auto filterPlane = [&](const std::vector<uint16_t>& src, std::vector<uint16_t>& dst) {
        for (int oy = 0; oy < ch; ++oy) {
            const int field = oy & 1;
            const int fieldPair = oy >> 1;
            const int y0 = fieldPair * 4 + field;
            const int y1 = y0 + 2;
            for (int x = 0; x < cw; ++x) {
                const unsigned a = src[static_cast<size_t>(y0) * cw + x];
                const unsigned b = src[static_cast<size_t>(y1) * cw + x];
                const unsigned weighted = field == 0 ? (3u * a + b) : (a + 3u * b);
                dst[static_cast<size_t>(oy) * cw + x] = static_cast<uint16_t>((weighted + 2u) >> 2);
            }
        }
    };

    filterPlane(u, outU);
    filterPlane(v, outV);
}

bool runOne(int width, int height, pc::Backend backend, const char* name)
{
    const int cw = width / 2;
    std::mt19937 rng(0x1420u + static_cast<unsigned>(width + height));
    std::uniform_int_distribution<int> dist(0, 1023);

    std::vector<uint16_t> y(static_cast<size_t>(width) * height);
    std::vector<uint16_t> u(static_cast<size_t>(cw) * height);
    std::vector<uint16_t> v(static_cast<size_t>(cw) * height);
    for (auto& s : y) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : u) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : v) s = static_cast<uint16_t>(dist(rng));

    // Make the first four chroma rows intentionally very different so both a
    // progressive 0+1 / 2+3 implementation and a same-field 50/50 average
    // fail against the interlaced quarter-line chroma phase reference.
    for (int x = 0; x < cw; ++x) {
        u[0 * cw + x] = 0;
        u[1 * cw + x] = 1023;
        u[2 * cw + x] = 200;
        u[3 * cw + x] = 800;
        v[0 * cw + x] = 1000;
        v[1 * cw + x] = 20;
        v[2 * cw + x] = 600;
        v[3 * cw + x] = 400;
    }

    std::vector<uint16_t> refY, refU, refV;
    reference(y, u, v, width, height, refY, refU, refV);

    std::vector<uint16_t> gotY(refY.size(), 0xFFFFu);
    std::vector<uint16_t> gotU(refU.size(), 0xFFFFu);
    std::vector<uint16_t> gotV(refV.size(), 0xFFFFu);

    pc::Yuv422p10View src{{reinterpret_cast<const uint8_t*>(y.data()),
                           reinterpret_cast<const uint8_t*>(u.data()),
                           reinterpret_cast<const uint8_t*>(v.data())},
                          {width * 2, cw * 2, cw * 2}, width, height};
    pc::Yuv420p10View dst{{reinterpret_cast<uint8_t*>(gotY.data()),
                           reinterpret_cast<uint8_t*>(gotU.data()),
                           reinterpret_cast<uint8_t*>(gotV.data())},
                          {width * 2, cw * 2, cw * 2}, width, height};

    pc::Backend used = pc::Backend::Auto;
    if (!pc::convert422p10To420p10Interlaced(src, dst, backend, &used)) return false;

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
