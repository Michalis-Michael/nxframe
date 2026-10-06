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
            const int y0 = oy * 2;
            const int y1 = y0 + 1;
            for (int x = 0; x < cw; ++x) {
                const unsigned sum = static_cast<unsigned>(src[static_cast<size_t>(y0) * cw + x]) +
                                     static_cast<unsigned>(src[static_cast<size_t>(y1) * cw + x]);
                dst[static_cast<size_t>(oy) * cw + x] = static_cast<uint16_t>((sum + 1u) >> 1);
            }
        }
    };

    filterPlane(u, outU);
    filterPlane(v, outV);
}

bool runOne(int width, int height, pc::Backend backend, const char* name)
{
    const int cw = width / 2;
    std::mt19937 rng(0x4210u + static_cast<unsigned>(width + height));
    std::uniform_int_distribution<int> dist(0, 1023);

    std::vector<uint16_t> y(static_cast<size_t>(width) * height);
    std::vector<uint16_t> u(static_cast<size_t>(cw) * height);
    std::vector<uint16_t> v(static_cast<size_t>(cw) * height);
    for (auto& s : y) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : u) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : v) s = static_cast<uint16_t>(dist(rng));

    if (!u.empty()) { u[0] = 0; u[1] = 1023; }
    if (!v.empty()) { v[0] = 1023; v[1] = 0; }

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
    if (!pc::convert422p10To420p10Progressive(src, dst, backend, &used)) return false;

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

    const int sizes[][2] = {{1280, 720}, {1920, 1080}, {1300, 722}};
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
