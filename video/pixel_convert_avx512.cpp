#include "pixel_convert.h"

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
#include <immintrin.h>
#endif

namespace nxframe {
namespace pixel_convert {

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
namespace {

inline void convertPlaneAvx512(const uint8_t* srcBytes, int srcStride,
                               uint8_t* dstBytes, int dstStride,
                               int samplesPerRow, int rows)
{
    const __m512i add2 = _mm512_set1_epi16(2);
    const __m512i max255 = _mm512_set1_epi16(255);

    for (int y = 0; y < rows; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y) * srcStride);
        auto* dst = dstBytes + static_cast<size_t>(y) * dstStride;

        int x = 0;
        for (; x + 32 <= samplesPerRow; x += 32) {
            __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(src + x));
            v = _mm512_add_epi16(v, add2);
            v = _mm512_srli_epi16(v, 2);
            v = _mm512_min_epu16(v, max255);
            _mm512_mask_cvtepi16_storeu_epi8(dst + x, static_cast<__mmask32>(0xffffffffu), v);
        }

        // Use AVX2-width vector work for the remainder before the final tiny tail.
        const __m256i add2_256 = _mm256_set1_epi16(2);
        const __m256i max255_256 = _mm256_set1_epi16(255);
        for (; x + 16 <= samplesPerRow; x += 16) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + x));
            v = _mm256_add_epi16(v, add2_256);
            v = _mm256_srli_epi16(v, 2);
            v = _mm256_min_epu16(v, max255_256);
            const __m128i lo = _mm256_castsi256_si128(v);
            const __m128i hi = _mm256_extracti128_si256(v, 1);
            const __m128i packed = _mm_packus_epi16(lo, hi);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x), packed);
        }
        for (; x < samplesPerRow; ++x) {
            const unsigned q = (static_cast<unsigned>(src[x]) + 2u) >> 2;
            dst[x] = static_cast<uint8_t>(q > 255u ? 255u : q);
        }
    }
}


inline void downsampleChromaBox2Avx512(const uint8_t* srcBytes, int srcStride,
                                        uint8_t* dstBytes, int dstStride,
                                        int samplesPerRow, int srcRows)
{
    const __m512i add1 = _mm512_set1_epi16(1);
    const __m512i add2 = _mm512_set1_epi16(2);
    const __m512i max255 = _mm512_set1_epi16(255);
    const int dstRows = srcRows / 2;

    for (int outY = 0; outY < dstRows; ++outY) {
        const int y0 = outY * 2;
        const int y1 = y0 + 1;

        const auto* r0 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y0) * srcStride);
        const auto* r1 = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y1) * srcStride);
        auto* dst = dstBytes + static_cast<size_t>(outY) * dstStride;

        int x = 0;
        for (; x + 32 <= samplesPerRow; x += 32) {
            const __m512i a = _mm512_loadu_si512(reinterpret_cast<const void*>(r0 + x));
            const __m512i b = _mm512_loadu_si512(reinterpret_cast<const void*>(r1 + x));

            __m512i sum = _mm512_add_epi16(a, b);
            sum = _mm512_add_epi16(sum, add1);
            sum = _mm512_srli_epi16(sum, 1);
            sum = _mm512_add_epi16(sum, add2);
            sum = _mm512_srli_epi16(sum, 2);
            sum = _mm512_min_epu16(sum, max255);
            _mm512_mask_cvtepi16_storeu_epi8(dst + x, static_cast<__mmask32>(0xffffffffu), sum);
        }

        const __m256i add1_256 = _mm256_set1_epi16(1);
        const __m256i add2_256 = _mm256_set1_epi16(2);
        const __m256i max255_256 = _mm256_set1_epi16(255);
        for (; x + 16 <= samplesPerRow; x += 16) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r0 + x));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r1 + x));
            __m256i sum = _mm256_add_epi16(a, b);
            sum = _mm256_add_epi16(sum, add1_256);
            sum = _mm256_srli_epi16(sum, 1);
            sum = _mm256_add_epi16(sum, add2_256);
            sum = _mm256_srli_epi16(sum, 2);
            sum = _mm256_min_epu16(sum, max255_256);
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

inline void copyPlane10Avx512(const uint8_t* srcBytes, int srcStride,
                              uint8_t* dstBytes, int dstStride,
                              int samplesPerRow, int rows)
{
    for (int y = 0; y < rows; ++y) {
        const auto* src = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y) * srcStride);
        auto* dst = reinterpret_cast<uint16_t*>(dstBytes + static_cast<size_t>(y) * dstStride);
        int x = 0;
        for (; x + 32 <= samplesPerRow; x += 32) {
            const __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(src + x));
            _mm512_storeu_si512(reinterpret_cast<void*>(dst + x), v);
        }
        for (; x + 16 <= samplesPerRow; x += 16) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + x));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + x), v);
        }
        for (; x < samplesPerRow; ++x) dst[x] = src[x];
    }
}

inline void downsampleChroma10Box2Avx512(const uint8_t* srcBytes, int srcStride,
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
        for (; x + 32 <= samplesPerRow; x += 32) {
            const __m512i a = _mm512_loadu_si512(reinterpret_cast<const void*>(r0 + x));
            const __m512i b = _mm512_loadu_si512(reinterpret_cast<const void*>(r1 + x));
            const __m512i avg = _mm512_avg_epu16(a, b);
            _mm512_storeu_si512(reinterpret_cast<void*>(dst + x), avg);
        }
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

inline void downsampleChroma10InterlacedPhaseAvx512(const uint8_t* srcBytes, int srcStride,
                                                    uint8_t* dstBytes, int dstStride,
                                                    int samplesPerRow, int srcRows)
{
    const __m512i round2_512 = _mm512_set1_epi16(2);
    const __m256i round2_256 = _mm256_set1_epi16(2);
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
        for (; x + 32 <= samplesPerRow; x += 32) {
            const __m512i a = _mm512_loadu_si512(reinterpret_cast<const void*>(r0 + x));
            const __m512i b = _mm512_loadu_si512(reinterpret_cast<const void*>(r1 + x));
            __m512i weighted;
            if (field == 0) {
                weighted = _mm512_add_epi16(_mm512_add_epi16(a, a),
                                             _mm512_add_epi16(a, b));
            } else {
                weighted = _mm512_add_epi16(_mm512_add_epi16(b, b),
                                             _mm512_add_epi16(a, b));
            }
            weighted = _mm512_add_epi16(weighted, round2_512);
            weighted = _mm512_srli_epi16(weighted, 2);
            _mm512_storeu_si512(reinterpret_cast<void*>(dst + x), weighted);
        }
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
            weighted = _mm256_add_epi16(weighted, round2_256);
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

bool convert422p10To422p8Avx512(const Yuv422p10View& src,
                                const Yuv422p8View& dst)
{
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
    convertPlaneAvx512(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    convertPlaneAvx512(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    convertPlaneAvx512(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}


bool convert422p10To420p8ProgressiveAvx512(const Yuv422p10View& src,
                                           const Yuv420p8View& dst)
{
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
    convertPlaneAvx512(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    downsampleChromaBox2Avx512(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    downsampleChromaBox2Avx512(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}


bool convert422p10To420p10ProgressiveAvx512(const Yuv422p10View& src,
                                            const Yuv420p10View& dst)
{
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
    copyPlane10Avx512(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    downsampleChroma10Box2Avx512(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    downsampleChroma10Box2Avx512(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}

bool convert422p10To420p10InterlacedAvx512(const Yuv422p10View& src,
                                           const Yuv420p10View& dst)
{
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
    copyPlane10Avx512(src.data[0], src.stride[0], dst.data[0], dst.stride[0], src.width, src.height);
    downsampleChroma10InterlacedPhaseAvx512(src.data[1], src.stride[1], dst.data[1], dst.stride[1], src.width / 2, src.height);
    downsampleChroma10InterlacedPhaseAvx512(src.data[2], src.stride[2], dst.data[2], dst.stride[2], src.width / 2, src.height);
    return true;
#else
    (void)src;
    (void)dst;
    return false;
#endif
}

} // namespace pixel_convert
} // namespace nxframe
