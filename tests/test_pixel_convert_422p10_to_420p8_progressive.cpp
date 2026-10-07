#include "video/pixel_convert.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace pc = nxframe::pixel_convert;

namespace {

uint8_t q10To8(unsigned v)
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

    for (size_t i = 0; i < outY.size(); ++i) outY[i] = q10To8(y[i]);

    auto filterPlane = [&](const std::vector<uint16_t>& src, std::vector<uint8_t>& dst) {
        for (int oy = 0; oy < ch; ++oy) {
            const int y0 = oy * 2;
            const int y1 = y0 + 1;
            for (int x = 0; x < cw; ++x) {
                const unsigned sum = static_cast<unsigned>(src[static_cast<size_t>(y0) * cw + x]) +
                                     static_cast<unsigned>(src[static_cast<size_t>(y1) * cw + x]);
                const unsigned filtered = (sum + 1u) >> 1;
                dst[static_cast<size_t>(oy) * cw + x] = q10To8(filtered);
            }
        }
    };

    filterPlane(u, outU);
    filterPlane(v, outV);
}

bool runOne(int width, int height, pc::Backend backend, const char* name)
{
    const int cw = width / 2;
    std::mt19937 rng(0x420u + static_cast<unsigned>(width + height));
    std::uniform_int_distribution<int> dist(0, 1023);

    std::vector<uint16_t> y(static_cast<size_t>(width) * height);
    std::vector<uint16_t> u(static_cast<size_t>(cw) * height);
    std::vector<uint16_t> v(static_cast<size_t>(cw) * height);
    for (auto& s : y) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : u) s = static_cast<uint16_t>(dist(rng));
    for (auto& s : v) s = static_cast<uint16_t>(dist(rng));

    // Force edge values into all planes to exercise 10->8 saturation.
    if (!y.empty()) { y[0] = 0; y[1] = 1023; }
    if (!u.empty()) { u[0] = 0; u[1] = 1023; }
    if (!v.empty()) { v[0] = 1023; v[1] = 0; }

    std::vector<uint8_t> refY, refU, refV;
    reference(y, u, v, width, height, refY, refU, refV);

    std::vector<uint8_t> gotY(refY.size(), 0xCD);
    std::vector<uint8_t> gotU(refU.size(), 0xCD);
    std::vector<uint8_t> gotV(refV.size(), 0xCD);

    pc::Yuv422p10View src;
    src.data[0] = reinterpret_cast<const uint8_t*>(y.data());
    src.data[1] = reinterpret_cast<const uint8_t*>(u.data());
    src.data[2] = reinterpret_cast<const uint8_t*>(v.data());
    src.stride[0] = width * static_cast<int>(sizeof(uint16_t));
    src.stride[1] = cw * static_cast<int>(sizeof(uint16_t));
    src.stride[2] = cw * static_cast<int>(sizeof(uint16_t));
    src.width = width;
    src.height = height;

    pc::Yuv420p8View dst;
    dst.data[0] = gotY.data();
    dst.data[1] = gotU.data();
    dst.data[2] = gotV.data();
    dst.stride[0] = width;
    dst.stride[1] = cw;
    dst.stride[2] = cw;
    dst.width = width;
    dst.height = height;

    pc::Backend used = pc::Backend::Auto;
    if (!pc::convert422p10To420p8Progressive(src, dst, backend, &used)) {
        std::cerr << name << " unavailable for " << width << "x" << height << '\n';
        return false;
    }

    if (gotY != refY || gotU != refU || gotV != refV) {
        auto report = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const char* plane) {
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i] != b[i]) {
                    std::cerr << plane << " mismatch at " << i << ": got="
                              << static_cast<int>(a[i]) << " ref=" << static_cast<int>(b[i]) << '\n';
                    return;
                }
            }
        };
        report(gotY, refY, "Y");
        report(gotU, refU, "U");
        report(gotV, refV, "V");
        return false;
    }

    std::cout << name << " " << width << "x" << height << ": PASS\n";
    return true;
}

} // namespace

int main()
{
    if (!pc::cpuHasAvx2()) {
        std::cerr << "AVX2 is required by NxFrame.\n";
        return 77;
    }

    const int sizes[][2] = {{1280, 720}, {1920, 1080}, {1300, 722}};
    for (const auto& s : sizes) {
        if (!runOne(s[0], s[1], pc::Backend::AVX2, "AVX2")) return 1;
    }

    if (pc::cpuHasAvx512Bw()) {
        for (const auto& s : sizes) {
            if (!runOne(s[0], s[1], pc::Backend::AVX512, "AVX-512")) return 1;
        }
    } else {
        std::cout << "AVX-512BW unavailable: skipped\n";
    }

    return 0;
}
