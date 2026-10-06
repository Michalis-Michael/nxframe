#pragma once

#include <cstdint>

namespace nxframe {
namespace pixel_convert {

enum class Backend {
    Auto,
    AVX2,
    AVX512
};

struct Yuv422p10View {
    const uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
};

struct Yuv422p8View {
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
};

struct Yuv420p8View {
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
};

struct Yuv420p10View {
    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int stride[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
};

bool cpuHasAvx2();
bool cpuHasAvx512Bw();
const char* backendName(Backend backend);

// Convert planar 10-bit 4:2:2 (10-bit samples in uint16_t low bits) to planar
// 8-bit 4:2:2. No spatial resampling is performed. Conversion uses rounded
// 10->8 quantization with saturation: min(255, (sample + 2) >> 2).
//
// Auto selects AVX-512BW when available, otherwise AVX2. There is deliberately
// no scalar production fallback. Returns false when dimensions/strides are
// invalid or the requested SIMD backend is unavailable.
bool convert422p10To422p8(const Yuv422p10View& src,
                          const Yuv422p8View& dst,
                          Backend requested = Backend::Auto,
                          Backend* used = nullptr);

// Backend entry points. Exposed for validation/benchmarking.
bool convert422p10To422p8Avx2(const Yuv422p10View& src,
                              const Yuv422p8View& dst);
bool convert422p10To422p8Avx512(const Yuv422p10View& src,
                                const Yuv422p8View& dst);

// Progressive planar 10-bit 4:2:2 -> planar 8-bit 4:2:0. Luma uses rounded
// 10->8 quantization. Chroma uses the selected 4-tap binomial vertical
// low-pass filter [1 3 3 1] / 8 at the 2:1 decimation phase, preserving
// 10-bit precision until the final rounded 8-bit quantization.
//
// Auto selects AVX-512BW when available, otherwise AVX2. There is deliberately
// no scalar or swscale production fallback. Returns false when the views are
// invalid or the requested SIMD backend is unavailable.
bool convert422p10To420p8Progressive(const Yuv422p10View& src,
                                     const Yuv420p8View& dst,
                                     Backend requested = Backend::Auto,
                                     Backend* used = nullptr);

bool convert422p10To420p8ProgressiveAvx2(const Yuv422p10View& src,
                                         const Yuv420p8View& dst);
bool convert422p10To420p8ProgressiveAvx512(const Yuv422p10View& src,
                                           const Yuv420p8View& dst);

// Progressive planar 10-bit 4:2:2 -> planar 10-bit 4:2:0. Luma samples are
// copied unchanged. Each output chroma row is the rounded average of the
// corresponding pair of source rows: (row0 + row1 + 1) >> 1. This matches
// the validated progressive 8-bit 4:2:0 sampling phase without quantization.
//
// Auto selects AVX-512BW when available, otherwise AVX2. There is deliberately
// no scalar or swscale production fallback.
bool convert422p10To420p10Progressive(const Yuv422p10View& src,
                                      const Yuv420p10View& dst,
                                      Backend requested = Backend::Auto,
                                      Backend* used = nullptr);

bool convert422p10To420p10ProgressiveAvx2(const Yuv422p10View& src,
                                          const Yuv420p10View& dst);
bool convert422p10To420p10ProgressiveAvx512(const Yuv422p10View& src,
                                            const Yuv420p10View& dst);

// Interlaced planar 10-bit 4:2:2 -> planar 10-bit 4:2:0. Luma samples are
// copied unchanged. Chroma is vertically decimated independently within each
// field so opposite temporal fields are never blended. The two fields use
// opposite quarter-line phases for interlaced 4:2:0 chroma siting:
//   top:    dst 0 = (3*src 0 + src 2 + 2) / 4
//   bottom: dst 1 = (src 1 + 3*src 3 + 2) / 4
// and the pattern repeats every four source raster lines.
//
// Auto selects AVX-512BW when available, otherwise AVX2. There is deliberately
// no scalar or swscale production fallback.
bool convert422p10To420p10Interlaced(const Yuv422p10View& src,
                                     const Yuv420p10View& dst,
                                     Backend requested = Backend::Auto,
                                     Backend* used = nullptr);

bool convert422p10To420p10InterlacedAvx2(const Yuv422p10View& src,
                                         const Yuv420p10View& dst);
bool convert422p10To420p10InterlacedAvx512(const Yuv422p10View& src,
                                           const Yuv420p10View& dst);

} // namespace pixel_convert
} // namespace nxframe
