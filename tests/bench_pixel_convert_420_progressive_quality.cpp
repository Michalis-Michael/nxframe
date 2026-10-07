extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

enum class Filter {
    Box2,
    Binomial4,
    Sharp4,
    Binomial6
};

const char* filterName(Filter f)
{
    switch (f) {
        case Filter::Box2: return "box2";
        case Filter::Binomial4: return "binomial4";
        case Filter::Sharp4: return "sharp4";
        case Filter::Binomial6: return "binomial6";
    }
    return "unknown";
}

inline int clampInt(int v, int lo, int hi)
{
    return std::max(lo, std::min(hi, v));
}

inline uint8_t q10To8(int value)
{
    value = clampInt(value, 0, 1023);
    return static_cast<uint8_t>(std::min(255, (value + 2) >> 2));
}

inline int roundedDivSigned(int64_t value, int divisor)
{
    if (value >= 0) return static_cast<int>((value + divisor / 2) / divisor);
    return static_cast<int>((value - divisor / 2) / divisor);
}

uint16_t sampleClamped(const std::vector<uint16_t>& plane,
                       int strideSamples,
                       int height,
                       int x,
                       int y)
{
    y = clampInt(y, 0, height - 1);
    return plane[static_cast<size_t>(y) * strideSamples + x];
}

int filtered10(const std::vector<uint16_t>& src,
               int strideSamples,
               int height,
               int x,
               int outY,
               Filter filter)
{
    const int y = outY * 2;
    int64_t sum = 0;
    int divisor = 1;

    switch (filter) {
        case Filter::Box2:
            sum = static_cast<int64_t>(sampleClamped(src, strideSamples, height, x, y)) +
                  sampleClamped(src, strideSamples, height, x, y + 1);
            divisor = 2;
            break;

        case Filter::Binomial4:
            sum = static_cast<int64_t>(sampleClamped(src, strideSamples, height, x, y - 1)) +
                  3LL * sampleClamped(src, strideSamples, height, x, y) +
                  3LL * sampleClamped(src, strideSamples, height, x, y + 1) +
                  sampleClamped(src, strideSamples, height, x, y + 2);
            divisor = 8;
            break;

        case Filter::Sharp4:
            sum = -static_cast<int64_t>(sampleClamped(src, strideSamples, height, x, y - 1)) +
                  9LL * sampleClamped(src, strideSamples, height, x, y) +
                  9LL * sampleClamped(src, strideSamples, height, x, y + 1) -
                  sampleClamped(src, strideSamples, height, x, y + 2);
            divisor = 16;
            break;

        case Filter::Binomial6:
            sum = static_cast<int64_t>(sampleClamped(src, strideSamples, height, x, y - 2)) +
                  5LL * sampleClamped(src, strideSamples, height, x, y - 1) +
                  10LL * sampleClamped(src, strideSamples, height, x, y) +
                  10LL * sampleClamped(src, strideSamples, height, x, y + 1) +
                  5LL * sampleClamped(src, strideSamples, height, x, y + 2) +
                  sampleClamped(src, strideSamples, height, x, y + 3);
            divisor = 32;
            break;
    }

    return clampInt(roundedDivSigned(sum, divisor), 0, 1023);
}

void convertCandidate(const std::vector<uint16_t>& src,
                      int chromaWidth,
                      int height,
                      Filter filter,
                      std::vector<uint8_t>& dst)
{
    const int outHeight = height / 2;
    dst.resize(static_cast<size_t>(chromaWidth) * outHeight);
    for (int y = 0; y < outHeight; ++y) {
        uint8_t* row = dst.data() + static_cast<size_t>(y) * chromaWidth;
        for (int x = 0; x < chromaWidth; ++x) {
            row[x] = q10To8(filtered10(src, chromaWidth, height, x, y, filter));
        }
    }
}

struct Metrics {
    double mse = 0.0;
    double psnr = std::numeric_limits<double>::infinity();
    double meanAbs = 0.0;
    int maxAbs = 0;
    double exactPct = 0.0;
    double bias = 0.0;
};

Metrics compare(const std::vector<uint8_t>& ref, const std::vector<uint8_t>& test)
{
    Metrics m;
    if (ref.size() != test.size() || ref.empty()) return m;

    long double sq = 0.0;
    long double absSum = 0.0;
    long double signedSum = 0.0;
    size_t exact = 0;
    int maxAbs = 0;

    for (size_t i = 0; i < ref.size(); ++i) {
        const int d = static_cast<int>(test[i]) - static_cast<int>(ref[i]);
        const int a = std::abs(d);
        sq += static_cast<long double>(d) * d;
        absSum += a;
        signedSum += d;
        maxAbs = std::max(maxAbs, a);
        if (d == 0) ++exact;
    }

    m.mse = static_cast<double>(sq / ref.size());
    if (m.mse > 0.0) {
        m.psnr = 10.0 * std::log10((255.0 * 255.0) / m.mse);
    }
    m.meanAbs = static_cast<double>(absSum / ref.size());
    m.maxAbs = maxAbs;
    m.exactPct = 100.0 * static_cast<double>(exact) / ref.size();
    m.bias = static_cast<double>(signedSum / ref.size());
    return m;
}

void fillPattern(const std::string& name,
                 std::vector<uint16_t>& u,
                 std::vector<uint16_t>& v,
                 int chromaWidth,
                 int height)
{
    auto setBoth = [&](int x, int y, int uVal, int vVal) {
        const size_t i = static_cast<size_t>(y) * chromaWidth + x;
        u[i] = static_cast<uint16_t>(clampInt(uVal, 0, 1023));
        v[i] = static_cast<uint16_t>(clampInt(vVal, 0, 1023));
    };

    if (name == "vertical_ramp") {
        for (int y = 0; y < height; ++y) {
            const int uv = (1023 * y) / std::max(1, height - 1);
            for (int x = 0; x < chromaWidth; ++x) {
                setBoth(x, y, uv, 1023 - uv);
            }
        }
        return;
    }

    if (name == "hard_edge") {
        for (int y = 0; y < height; ++y) {
            const int uVal = y < height / 2 ? 128 : 896;
            const int vVal = y < height / 2 ? 896 : 128;
            for (int x = 0; x < chromaWidth; ++x) setBoth(x, y, uVal, vVal);
        }
        return;
    }

    if (name == "line_alternating") {
        for (int y = 0; y < height; ++y) {
            const int uVal = (y & 1) ? 896 : 128;
            const int vVal = (y & 1) ? 128 : 896;
            for (int x = 0; x < chromaWidth; ++x) setBoth(x, y, uVal, vVal);
        }
        return;
    }

    if (name == "two_line_alternating") {
        for (int y = 0; y < height; ++y) {
            const int uVal = ((y / 2) & 1) ? 896 : 128;
            const int vVal = ((y / 2) & 1) ? 128 : 896;
            for (int x = 0; x < chromaWidth; ++x) setBoth(x, y, uVal, vVal);
        }
        return;
    }

    if (name == "impulse") {
        std::fill(u.begin(), u.end(), static_cast<uint16_t>(512));
        std::fill(v.begin(), v.end(), static_cast<uint16_t>(512));
        const int line = height / 2;
        for (int x = 0; x < chromaWidth; ++x) {
            u[static_cast<size_t>(line) * chromaWidth + x] = 1023;
            v[static_cast<size_t>(line) * chromaWidth + x] = 0;
        }
        return;
    }

    // Smooth, deterministic pseudo-natural chroma field. The low-frequency
    // horizontal/vertical terms avoid turning this into a pure random-noise test.
    std::mt19937 rng(0x4E584652u);
    std::uniform_int_distribution<int> noise(-48, 48);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < chromaWidth; ++x) {
            const double fy = std::sin(static_cast<double>(y) * 0.037);
            const double fx = std::sin(static_cast<double>(x) * 0.021);
            const int baseU = static_cast<int>(512.0 + 220.0 * fy + 120.0 * fx);
            const int baseV = static_cast<int>(512.0 - 180.0 * fy + 100.0 * fx);
            setBoth(x, y, baseU + noise(rng), baseV + noise(rng));
        }
    }
}

bool swscaleConvert(int width,
                    int height,
                    const std::vector<uint16_t>& y,
                    const std::vector<uint16_t>& u,
                    const std::vector<uint16_t>& v,
                    std::vector<uint8_t>& outY,
                    std::vector<uint8_t>& outU,
                    std::vector<uint8_t>& outV)
{
    const int cw = width / 2;
    const int ch = height / 2;
    outY.resize(static_cast<size_t>(width) * height);
    outU.resize(static_cast<size_t>(cw) * ch);
    outV.resize(static_cast<size_t>(cw) * ch);

    SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_YUV422P10LE,
                                     width, height, AV_PIX_FMT_YUV420P,
                                     SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws) return false;

    const uint8_t* srcData[4] = {
        reinterpret_cast<const uint8_t*>(y.data()),
        reinterpret_cast<const uint8_t*>(u.data()),
        reinterpret_cast<const uint8_t*>(v.data()),
        nullptr
    };
    int srcStride[4] = {width * 2, cw * 2, cw * 2, 0};
    uint8_t* dstData[4] = {outY.data(), outU.data(), outV.data(), nullptr};
    int dstStride[4] = {width, cw, cw, 0};

    const int rows = sws_scale(sws, srcData, srcStride, 0, height, dstData, dstStride);
    sws_freeContext(sws);
    return rows == height;
}

void printMetrics(const char* filter,
                  const Metrics& u,
                  const Metrics& v)
{
    std::cout << std::left << std::setw(12) << filter
              << " U: PSNR=" << std::fixed << std::setprecision(2) << std::setw(7) << u.psnr
              << " MAE=" << std::setprecision(4) << std::setw(8) << u.meanAbs
              << " MAX=" << std::setw(3) << u.maxAbs
              << " EXACT=" << std::setprecision(2) << std::setw(7) << u.exactPct << "%"
              << " BIAS=" << std::setprecision(4) << std::setw(8) << u.bias
              << " | V: PSNR=" << std::setprecision(2) << std::setw(7) << v.psnr
              << " MAE=" << std::setprecision(4) << std::setw(8) << v.meanAbs
              << " MAX=" << std::setw(3) << v.maxAbs
              << " EXACT=" << std::setprecision(2) << std::setw(7) << v.exactPct << "%"
              << " BIAS=" << std::setprecision(4) << v.bias
              << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    const int width = argc > 1 ? std::stoi(argv[1]) : 1920;
    const int height = argc > 2 ? std::stoi(argv[2]) : 1080;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        std::cerr << "Usage: " << argv[0] << " [even_width] [even_height]\n";
        return 2;
    }

    const int cw = width / 2;
    std::vector<uint16_t> y(static_cast<size_t>(width) * height, static_cast<uint16_t>(512));
    std::vector<uint16_t> u(static_cast<size_t>(cw) * height);
    std::vector<uint16_t> v(u.size());
    std::vector<uint8_t> swY, swU, swV;
    std::vector<uint8_t> candU, candV;

    const std::vector<std::string> patterns = {
        "vertical_ramp",
        "hard_edge",
        "line_alternating",
        "two_line_alternating",
        "impulse",
        "smooth_field"
    };
    const std::vector<Filter> filters = {
        Filter::Box2,
        Filter::Binomial4,
        Filter::Sharp4,
        Filter::Binomial6
    };

    std::cout << "Progressive YUV422P10LE -> YUV420P8 filter comparison against swscale SWS_BICUBIC\n"
              << width << "x" << height << " (chroma " << cw << "x" << height << " -> "
              << cw << "x" << height / 2 << ")\n\n";

    for (const std::string& pattern : patterns) {
        fillPattern(pattern, u, v, cw, height);
        if (!swscaleConvert(width, height, y, u, v, swY, swU, swV)) {
            std::cerr << "swscale conversion failed for pattern " << pattern << '\n';
            return 1;
        }

        std::cout << "[" << pattern << "]\n";
        for (Filter filter : filters) {
            convertCandidate(u, cw, height, filter, candU);
            convertCandidate(v, cw, height, filter, candV);
            printMetrics(filterName(filter), compare(swU, candU), compare(swV, candV));
        }
        std::cout << '\n';
    }

    std::cout << "This tool ranks candidate spatial filters against the current swscale reference.\n"
              << "It does not change the production progressive 4:2:0 path.\n";
    return 0;
}
