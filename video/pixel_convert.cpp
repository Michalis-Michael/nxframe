#include "pixel_convert.h"

namespace nxframe {
namespace pixel_convert {

namespace {

bool valid422Views(const Yuv422p10View& src, const Yuv422p8View& dst)
{
    if (src.width <= 0 || src.height <= 0 || (src.width & 1)) return false;
    if (dst.width != src.width || dst.height != src.height) return false;
    for (int p = 0; p < 3; ++p) {
        if (!src.data[p] || !dst.data[p]) return false;
        const int samples = (p == 0) ? src.width : src.width / 2;
        if (src.stride[p] < samples * static_cast<int>(sizeof(uint16_t))) return false;
        if (dst.stride[p] < samples) return false;
    }
    return true;
}

bool valid420ProgressiveViews(const Yuv422p10View& src, const Yuv420p8View& dst)
{
    if (src.width <= 0 || src.height <= 0 || (src.width & 1) || (src.height & 1)) return false;
    if (dst.width != src.width || dst.height != src.height) return false;
    for (int p = 0; p < 3; ++p) {
        if (!src.data[p] || !dst.data[p]) return false;
        const int srcSamples = (p == 0) ? src.width : src.width / 2;
        const int dstSamples = (p == 0) ? dst.width : dst.width / 2;
        if (src.stride[p] < srcSamples * static_cast<int>(sizeof(uint16_t))) return false;
        if (dst.stride[p] < dstSamples) return false;
    }
    return true;
}


bool valid420p10ProgressiveViews(const Yuv422p10View& src, const Yuv420p10View& dst)
{
    if (src.width <= 0 || src.height <= 0 || (src.width & 1) || (src.height & 1)) return false;
    if (dst.width != src.width || dst.height != src.height) return false;
    for (int p = 0; p < 3; ++p) {
        if (!src.data[p] || !dst.data[p]) return false;
        const int srcSamples = (p == 0) ? src.width : src.width / 2;
        const int dstSamples = (p == 0) ? dst.width : dst.width / 2;
        if (src.stride[p] < srcSamples * static_cast<int>(sizeof(uint16_t))) return false;
        if (dst.stride[p] < dstSamples * static_cast<int>(sizeof(uint16_t))) return false;
    }
    return true;
}

bool valid420p10InterlacedViews(const Yuv422p10View& src, const Yuv420p10View& dst)
{
    // Each field must contain an even number of raster lines so its chroma can
    // be decimated 2:1 independently. Standard 480i/576i/1080i heights satisfy
    // this naturally.
    if ((src.height & 3) != 0) return false;
    return valid420p10ProgressiveViews(src, dst);
}

} // namespace

bool cpuHasAvx2()
{
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

bool cpuHasAvx512Bw()
{
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx512f") &&
           __builtin_cpu_supports("avx512bw") &&
           __builtin_cpu_supports("avx512vl");
#else
    return false;
#endif
}

const char* backendName(Backend backend)
{
    switch (backend) {
        case Backend::AVX512: return "avx512";
        case Backend::AVX2: return "avx2";
        case Backend::Auto: return "auto";
    }
    return "unknown";
}

bool convert422p10To422p8(const Yuv422p10View& src,
                          const Yuv422p8View& dst,
                          Backend requested,
                          Backend* used)
{
    if (used) *used = Backend::Auto;
    if (!valid422Views(src, dst)) return false;

    if (requested == Backend::AVX512 || requested == Backend::Auto) {
        if (cpuHasAvx512Bw() && convert422p10To422p8Avx512(src, dst)) {
            if (used) *used = Backend::AVX512;
            return true;
        }
        if (requested == Backend::AVX512) return false;
    }

    if (requested == Backend::AVX2 || requested == Backend::Auto) {
        if (cpuHasAvx2() && convert422p10To422p8Avx2(src, dst)) {
            if (used) *used = Backend::AVX2;
            return true;
        }
    }

    return false;
}

bool convert422p10To420p8Progressive(const Yuv422p10View& src,
                                     const Yuv420p8View& dst,
                                     Backend requested,
                                     Backend* used)
{
    if (used) *used = Backend::Auto;
    if (!valid420ProgressiveViews(src, dst)) return false;

    if (requested == Backend::AVX512 || requested == Backend::Auto) {
        if (cpuHasAvx512Bw() && convert422p10To420p8ProgressiveAvx512(src, dst)) {
            if (used) *used = Backend::AVX512;
            return true;
        }
        if (requested == Backend::AVX512) return false;
    }

    if (requested == Backend::AVX2 || requested == Backend::Auto) {
        if (cpuHasAvx2() && convert422p10To420p8ProgressiveAvx2(src, dst)) {
            if (used) *used = Backend::AVX2;
            return true;
        }
    }

    return false;
}

bool convert422p10To420p10Progressive(const Yuv422p10View& src,
                                      const Yuv420p10View& dst,
                                      Backend requested,
                                      Backend* used)
{
    if (used) *used = Backend::Auto;
    if (!valid420p10ProgressiveViews(src, dst)) return false;

    if (requested == Backend::AVX512 || requested == Backend::Auto) {
        if (cpuHasAvx512Bw() && convert422p10To420p10ProgressiveAvx512(src, dst)) {
            if (used) *used = Backend::AVX512;
            return true;
        }
        if (requested == Backend::AVX512) return false;
    }

    if (requested == Backend::AVX2 || requested == Backend::Auto) {
        if (cpuHasAvx2() && convert422p10To420p10ProgressiveAvx2(src, dst)) {
            if (used) *used = Backend::AVX2;
            return true;
        }
    }

    return false;
}

bool convert422p10To420p10Interlaced(const Yuv422p10View& src,
                                     const Yuv420p10View& dst,
                                     Backend requested,
                                     Backend* used)
{
    if (used) *used = Backend::Auto;
    if (!valid420p10InterlacedViews(src, dst)) return false;

    if (requested == Backend::AVX512 || requested == Backend::Auto) {
        if (cpuHasAvx512Bw() && convert422p10To420p10InterlacedAvx512(src, dst)) {
            if (used) *used = Backend::AVX512;
            return true;
        }
        if (requested == Backend::AVX512) return false;
    }

    if (requested == Backend::AVX2 || requested == Backend::Auto) {
        if (cpuHasAvx2() && convert422p10To420p10InterlacedAvx2(src, dst)) {
            if (used) *used = Backend::AVX2;
            return true;
        }
    }

    return false;
}

} // namespace pixel_convert
} // namespace nxframe
