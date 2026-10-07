#include "pixel_convert.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nxframe {
namespace pixel_convert {

#if defined(__AVX2__)
namespace {

inline void convertPlaneAvx2(const uint8_t* srcBytes, int srcStride,
                             uint8_t* dstBytes, int dstStride,
                             int samplesPerRow, int rows)
{
    const __m256i add2 = _mm256_set1_epi16(2);
    const __m256i max255 = _mm256_set1_epi16(255);

    for (int y = 0; y < rows; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y) * srcStride);
        auto* dst = dstBytes + static_cast<size_t>(y) * dstStride;

        int x = 0;
        for (; x + 16 <= samplesPerRow; x += 16) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + x));
            v = _mm256_add_epi16(v, add2);
            v = _mm256_srli_epi16(v, 2);
            v = _mm256_min_epu16(v, max255);

            const __m128i lo = _mm256_castsi256_si128(v);
            const __m128i hi = _mm256_extracti128_si256(v, 1);
            const __m128i packed = _mm_packus_epi16(lo, hi);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), packed);
        }

        // Standard 1280/1920 4:2:2 planes have no tail, but keep a correct
        // remainder for other even widths without adding a scalar production
        // backend/dispatch path.
        for (; x < samplesPerRow; ++x) {
            const unsigned q = (static_cast<unsigned>(src[x]) + 2u) >> 2;
            dst[x] = static_cast<uint8_t>(q > 255u ? 255u : q);
        }
    }
}


inline void downsampleChromaBox2Avx2(const uint8_t* srcBytes, int srcStride,
                                      uint8_t* dstBytes, int dstStride,
                                      int samplesPerRow, int srcRows)
{
    const __m256i add1 = _mm256_set1_epi16(1);
    const __m256i add2 = _mm256_set1_epi16(2);
    const __m256i max255 = _mm256_set1_epi16(255);
    const int dstRows = srcRows / 2;

    for (int outY = 0; outY < dstRows; ++outY) {
        const int y0 = outY * 2;
        const int y1 = y0 + 1;

        const auto* r0 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y0) * srcStride);
        const auto* r1 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y1) * srcStride);
        auto* dst = dstBytes + static_cast<size_t>(outY) * dstStride;

        int x = 0;
        for (; x + 16 <= samplesPerRow; x += 16) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + x));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + x));

            // Progressive 4:2:2 -> 4:2:0: center each output chroma row
            // between the corresponding pair of source rows. Compared with the
            // old [1 3 3 1]/8 kernel this avoids unnecessary vertical chroma
            // softening while retaining the required 2:1 anti-alias average.
            // Inputs are <=1023, so a+b <=2046 and fits safely in 16-bit lanes.
            __m256i sum = _mm256_add_epi16(a, b);
            sum = _mm256_add_epi16(sum, add1);
            sum = _mm256_srli_epi16(sum, 1); // rounded 10-bit pair average

            sum = _mm256_add_epi16(sum, add2);
            sum = _mm256_srli_epi16(sum, 2); // rounded 10 -> 8
            sum = _mm256_min_epu16(sum, max255);

            const __m128i lo = _mm256_castsi256_si128(sum);
            const __m128i hi = _mm256_extracti128_si256(sum, 1);
            const __m128i packed = _mm_packus_epi16(lo, hi);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), packed);
        }

        for (; x < samplesPerRow; ++x) {
            const unsigned filtered = (static_cast<unsigned>(r0[x]) +
                                       static_cast<unsigned>(r1[x]) + 1u) >> 1;
            const unsigned q = (filtered + 2u) >> 2;
            dst[x] = static_cast<uint8_t>(q > 255u ? 255u : q);
        }
    }
}

inline void downsampleChroma8InterlacedPhaseAvx2(const uint8_t* srcBytes, int srcStride,
                                                 uint8_t* dstBytes, int dstStride,
                                                 int samplesPerRow, int srcRows)
{
    const __m256i round2 = _mm256_set1_epi16(2);
    const __m256i max255 = _mm256_set1_epi16(255);
    const int dstRows = srcRows / 2;

    for (int outY = 0; outY < dstRows; ++outY) {
        const int field = outY & 1;
        const int fieldPair = outY >> 1;
        const int y0 = fieldPair * 4 + field;
        const int y1 = y0 + 2;

        const auto* r0 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y0) * srcStride);
        const auto* r1 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y1) * srcStride);
        auto* dst = dstBytes + static_cast<size_t>(outY) * dstStride;

        int x = 0;
        for (; x + 16 <= samplesPerRow; x += 16) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + x));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + x));

            __m256i weighted;
            if (field == 0) {
                weighted = _mm256_add_epi16(_mm256_add_epi16(a, a),
                                             _mm256_add_epi16(a, b));
            } else {
                weighted = _mm256_add_epi16(_mm256_add_epi16(b, b),
                                             _mm256_add_epi16(a, b));
            }

            // First round the v2 quarter-phase filter back to 10-bit, then
            // apply NxFrame's validated rounded 10->8 quantization. Keeping
            // these as two explicit rounding stages matches the 10-bit path
            // followed by the 422p10->422p8 conversion exactly.
            weighted = _mm256_add_epi16(weighted, round2);
            weighted = _mm256_srli_epi16(weighted, 2);
            weighted = _mm256_add_epi16(weighted, round2);
            weighted = _mm256_srli_epi16(weighted, 2);
            weighted = _mm256_min_epu16(weighted, max255);

            const __m128i lo = _mm256_castsi256_si128(weighted);
            const __m128i hi = _mm256_extracti128_si256(weighted, 1);
            const __m128i packed = _mm_packus_epi16(lo, hi);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), packed);
        }

        for (; x < samplesPerRow; ++x) {
            const unsigned a = r0[x];
            const unsigned b = r1[x];
            const unsigned weighted = field == 0 ? (3u * a + b) : (a + 3u * b);
            const unsigned filtered10 = (weighted + 2u) >> 2;
            const unsigned q = (filtered10 + 2u) >> 2;
            dst[x] = static_cast<uint8_t>(q > 255u ? 255u : q);
        }
    }
}

inline void copyPlane10Avx2(const uint8_t* srcBytes, int srcStride,
                            uint8_t* dstBytes, int dstStride,
                            int samplesPerRow, int rows)
{
    for (int y = 0; y < rows; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y) * srcStride);
        auto* dst = reinterpret_cast<uint16_t*>(dstBytes + static_cast<size_t>(y) * dstStride);
        int x = 0;
        for (; x + 16 <= samplesPerRow; x += 16) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + x));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x), v);
        }
        for (; x < samplesPerRow; ++x) dst[x] = src[x];
    }
}

inline void downsampleChroma10Box2Avx2(const uint8_t* srcBytes, int srcStride,
                                       uint8_t* dstBytes, int dstStride,
                                       int samplesPerRow, int srcRows)
{
    const int dstRows = srcRows / 2;
    for (int outY = 0; outY < dstRows; ++outY) {
        const int y0 = outY * 2;
        const int y1 = y0 + 1;
        const auto* r0 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y0) * srcStride);
        const auto* r1 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y1) * srcStride);
        auto* dst = reinterpret_cast<uint16_t*>(dstBytes + static_cast<size_t>(outY) * dstStride);

        int x = 0;
        for (; x + 16 <= samplesPerRow; x += 16) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + x));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + x));
            const __m256i avg = _mm256_avg_epu16(a, b);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x), avg);
        }
        for (; x < samplesPerRow; ++x) {
            dst[x] = static_cast<uint16_t>((static_cast<unsigned>(r0[x]) +
                                            static_cast<unsigned>(r1[x]) + 1u) >> 1);
        }
    }
}

inline void downsampleChroma10InterlacedPhaseAvx2(const uint8_t* srcBytes, int srcStride,
                                                  uint8_t* dstBytes, int dstStride,
                                                  int samplesPerRow, int srcRows)
{
    const __m256i round2 = _mm256_set1_epi16(2);
    const int dstRows = srcRows / 2;
    for (int outY = 0; outY < dstRows; ++outY) {
        const int field = outY & 1;
        const int fieldPair = outY >> 1;
        const int y0 = fieldPair * 4 + field;
        const int y1 = y0 + 2;

        const auto* r0 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y0) * srcStride);
        const auto* r1 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y1) * srcStride);
        auto* dst = reinterpret_cast<uint16_t*>(dstBytes + static_cast<size_t>(outY) * dstStride);

        int x = 0;
        for (; x + 16 <= samplesPerRow; x += 16) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + x));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + x));

            // Interlaced 4:2:0 uses opposite quarter-line chroma phases for
            // the two fields.  Keep each field independent, but bias the
            // top field toward its first line and the bottom field toward its
            // second line: (3*a+b)/4 vs (a+3*b)/4.  10-bit inputs keep the
            // intermediate below 4093, safely inside unsigned 16-bit lanes.
            __m256i weighted;
            if (field == 0) {
                weighted = _mm256_add_epi16(_mm256_add_epi16(a, a),
                                             _mm256_add_epi16(a, b));
            } else {
                weighted = _mm256_add_epi16(_mm256_add_epi16(b, b),
                                             _mm256_add_epi16(a, b));
            }
            weighted = _mm256_add_epi16(weighted, round2);
            weighted = _mm256_srli_epi16(weighted, 2);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x), weighted);
        }
        for (; x < samplesPerRow; ++x) {
            const unsigned a = r0[x];
            const unsigned b = r1[x];
            const unsigned weighted = field == 0 ? (3u * a + b) : (a + 3u * b);
            dst[x] = static_cast<uint16_t>((weighted + 2u) >> 2);
        }
    }
}


} // namespace
#endif

bool convert422p10To422p8Avx2(const Yuv422p10View& src,
                              const Yuv422p8View& dst)
{
#if defined(__AVX2__)
    convertPlaneAvx2(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    convertPlaneAvx2(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    convertPlaneAvx2(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}


bool convert422p10To420p8ProgressiveAvx2(const Yuv422p10View& src,
                                         const Yuv420p8View& dst)
{
#if defined(__AVX2__)
    convertPlaneAvx2(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    downsampleChromaBox2Avx2(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    downsampleChromaBox2Avx2(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}


bool convert422p10To420p8InterlacedAvx2(const Yuv422p10View& src,
                                        const Yuv420p8View& dst)
{
#if defined(__AVX2__)
    convertPlaneAvx2(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    downsampleChroma8InterlacedPhaseAvx2(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    downsampleChroma8InterlacedPhaseAvx2(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}

bool convert422p10To420p10ProgressiveAvx2(const Yuv422p10View& src,
                                          const Yuv420p10View& dst)
{
#if defined(__AVX2__)
    copyPlane10Avx2(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    downsampleChroma10Box2Avx2(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    downsampleChroma10Box2Avx2(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}

bool convert422p10To420p10InterlacedAvx2(const Yuv422p10View& src,
                                         const Yuv420p10View& dst)
{
#if defined(__AVX2__)
    copyPlane10Avx2(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    downsampleChroma10InterlacedPhaseAvx2(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    downsampleChroma10InterlacedPhaseAvx2(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}

} // namespace pixel_convert
} // namespace nxframe
