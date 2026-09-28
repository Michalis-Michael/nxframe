/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AVX2 planar YUV422P10LE -> v210 packer.
 */
#include "output/v210_pack_simd.h"
#include "output/v210_pack.h"

#include <algorithm>
#include <cstdint>
#include <immintrin.h>

namespace nxframe {
namespace {

#if defined(__AVX2__)
static inline __m256i normalizeGather16(const uint16_t* base, __m256i sampleIndices)
{
    // AVX2 has no 16-bit gather. Gather 32 bits at each 16-bit sample address,
    // then keep only the requested low 16-bit sample. SIMD processing stops
    // before the final source sample so every 32-bit gather remains in-bounds.
    __m256i v = _mm256_i32gather_epi32(reinterpret_cast<const int*>(base), sampleIndices, 2);
    v = _mm256_and_si256(v, _mm256_set1_epi32(0xffff));

    const __m256i limit = _mm256_set1_epi32(1023);
    const __m256i shifted = _mm256_srli_epi32(_mm256_add_epi32(v, _mm256_set1_epi32(32)), 6);
    const __m256i needsShift = _mm256_cmpgt_epi32(v, limit);
    v = _mm256_blendv_epi8(v, shifted, needsShift);
    return _mm256_min_epi32(v, limit);
}

static inline void storeFourGroups(uint32_t* out,
                                   __m128i w0,
                                   __m128i w1,
                                   __m128i w2,
                                   __m128i w3)
{
    const __m128i abLo = _mm_unpacklo_epi32(w0, w1);
    const __m128i abHi = _mm_unpackhi_epi32(w0, w1);
    const __m128i cdLo = _mm_unpacklo_epi32(w2, w3);
    const __m128i cdHi = _mm_unpackhi_epi32(w2, w3);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 0),  _mm_unpacklo_epi64(abLo, cdLo));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 4),  _mm_unpackhi_epi64(abLo, cdLo));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 8),  _mm_unpacklo_epi64(abHi, cdHi));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 12), _mm_unpackhi_epi64(abHi, cdHi));
}

static inline void packRowAvx2(const uint16_t* yRow,
                               const uint16_t* uRow,
                               const uint16_t* vRow,
                               int width,
                               uint32_t* out)
{
    const __m256i groupY = _mm256_setr_epi32(0, 6, 12, 18, 24, 30, 36, 42);
    const __m256i groupC = _mm256_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21);

    int x = 0;
    // Leave at least one complete 48-pixel block for the scalar tail. This
    // keeps every 32-bit gather within the active source arrays.
    for (; x + 48 < width; x += 48) {
        const int c = x / 2;
        const __m256i yBaseIdx = _mm256_add_epi32(groupY, _mm256_set1_epi32(x));
        const __m256i cBaseIdx = _mm256_add_epi32(groupC, _mm256_set1_epi32(c));

        const __m256i U0 = normalizeGather16(uRow, cBaseIdx);
        const __m256i Y0 = normalizeGather16(yRow, yBaseIdx);
        const __m256i V0 = normalizeGather16(vRow, cBaseIdx);

        const __m256i Y1 = normalizeGather16(yRow, _mm256_add_epi32(yBaseIdx, _mm256_set1_epi32(1)));
        const __m256i U1 = normalizeGather16(uRow, _mm256_add_epi32(cBaseIdx, _mm256_set1_epi32(1)));
        const __m256i Y2 = normalizeGather16(yRow, _mm256_add_epi32(yBaseIdx, _mm256_set1_epi32(2)));

        const __m256i V1 = normalizeGather16(vRow, _mm256_add_epi32(cBaseIdx, _mm256_set1_epi32(1)));
        const __m256i Y3 = normalizeGather16(yRow, _mm256_add_epi32(yBaseIdx, _mm256_set1_epi32(3)));
        const __m256i U2 = normalizeGather16(uRow, _mm256_add_epi32(cBaseIdx, _mm256_set1_epi32(2)));

        const __m256i Y4 = normalizeGather16(yRow, _mm256_add_epi32(yBaseIdx, _mm256_set1_epi32(4)));
        const __m256i V2 = normalizeGather16(vRow, _mm256_add_epi32(cBaseIdx, _mm256_set1_epi32(2)));
        const __m256i Y5 = normalizeGather16(yRow, _mm256_add_epi32(yBaseIdx, _mm256_set1_epi32(5)));

        const __m256i w0 = _mm256_or_si256(U0, _mm256_or_si256(_mm256_slli_epi32(Y0, 10), _mm256_slli_epi32(V0, 20)));
        const __m256i w1 = _mm256_or_si256(Y1, _mm256_or_si256(_mm256_slli_epi32(U1, 10), _mm256_slli_epi32(Y2, 20)));
        const __m256i w2 = _mm256_or_si256(V1, _mm256_or_si256(_mm256_slli_epi32(Y3, 10), _mm256_slli_epi32(U2, 20)));
        const __m256i w3 = _mm256_or_si256(Y4, _mm256_or_si256(_mm256_slli_epi32(V2, 10), _mm256_slli_epi32(Y5, 20)));

        storeFourGroups(out,
                        _mm256_castsi256_si128(w0),
                        _mm256_castsi256_si128(w1),
                        _mm256_castsi256_si128(w2),
                        _mm256_castsi256_si128(w3));
        storeFourGroups(out + 16,
                        _mm256_extracti128_si256(w0, 1),
                        _mm256_extracti128_si256(w1, 1),
                        _mm256_extracti128_si256(w2, 1),
                        _mm256_extracti128_si256(w3, 1));
        out += 32;
    }

    if (x < width) {
        packYuv422p10RowToV210(yRow + x,
                               uRow + x / 2,
                               vRow + x / 2,
                               width - x,
                               out);
    }
}
#endif

} // namespace

void packYuv422p10ToV210Avx2(const uint16_t* yBase,
                              const uint16_t* uBase,
                              const uint16_t* vBase,
                              int yStride,
                              int uStride,
                              int vStride,
                              int width,
                              int height,
                              uint8_t* dst,
                              int dstRowBytes)
{
#if defined(__AVX2__)
    for (int y = 0; y < height; ++y) {
        packRowAvx2(yBase + static_cast<size_t>(y) * static_cast<size_t>(yStride),
                    uBase + static_cast<size_t>(y) * static_cast<size_t>(uStride),
                    vBase + static_cast<size_t>(y) * static_cast<size_t>(vStride),
                    width,
                    reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * static_cast<size_t>(dstRowBytes)));
    }
#else
    for (int y = 0; y < height; ++y) {
        packYuv422p10RowToV210(yBase + static_cast<size_t>(y) * static_cast<size_t>(yStride),
                               uBase + static_cast<size_t>(y) * static_cast<size_t>(uStride),
                               vBase + static_cast<size_t>(y) * static_cast<size_t>(vStride),
                               width,
                               reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * static_cast<size_t>(dstRowBytes)));
    }
#endif
}

} // namespace nxframe
