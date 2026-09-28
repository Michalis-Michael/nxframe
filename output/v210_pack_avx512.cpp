/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AVX-512 planar YUV422P10LE -> v210 packer.
 */
#include "output/v210_pack_simd.h"
#include "output/v210_pack.h"

#include <cstdint>
#include <immintrin.h>

namespace nxframe {
namespace {

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
static inline __m512i normalizeGather16(const uint16_t* base, __m512i sampleIndices)
{
    __m512i v = _mm512_i32gather_epi32(sampleIndices, reinterpret_cast<const int*>(base), 2);
    v = _mm512_and_si512(v, _mm512_set1_epi32(0xffff));

    const __m512i limit = _mm512_set1_epi32(1023);
    const __m512i shifted = _mm512_srli_epi32(_mm512_add_epi32(v, _mm512_set1_epi32(32)), 6);
    const __mmask16 needsShift = _mm512_cmpgt_epi32_mask(v, limit);
    v = _mm512_mask_mov_epi32(v, needsShift, shifted);
    return _mm512_min_epi32(v, limit);
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

static inline void packRowAvx512(const uint16_t* yRow,
                                 const uint16_t* uRow,
                                 const uint16_t* vRow,
                                 int width,
                                 uint32_t* out)
{
    const __m512i groupY = _mm512_setr_epi32(
        0, 6, 12, 18, 24, 30, 36, 42,
        48, 54, 60, 66, 72, 78, 84, 90);
    const __m512i groupC = _mm512_setr_epi32(
        0, 3, 6, 9, 12, 15, 18, 21,
        24, 27, 30, 33, 36, 39, 42, 45);
    int x = 0;
    for (; x + 96 < width; x += 96) {
        const int c = x / 2;
        const __m512i yBaseIdx = _mm512_add_epi32(groupY, _mm512_set1_epi32(x));
        const __m512i cBaseIdx = _mm512_add_epi32(groupC, _mm512_set1_epi32(c));

        const __m512i U0 = normalizeGather16(uRow, cBaseIdx);
        const __m512i Y0 = normalizeGather16(yRow, yBaseIdx);
        const __m512i V0 = normalizeGather16(vRow, cBaseIdx);

        const __m512i Y1 = normalizeGather16(yRow, _mm512_add_epi32(yBaseIdx, _mm512_set1_epi32(1)));
        const __m512i U1 = normalizeGather16(uRow, _mm512_add_epi32(cBaseIdx, _mm512_set1_epi32(1)));
        const __m512i Y2 = normalizeGather16(yRow, _mm512_add_epi32(yBaseIdx, _mm512_set1_epi32(2)));

        const __m512i V1 = normalizeGather16(vRow, _mm512_add_epi32(cBaseIdx, _mm512_set1_epi32(1)));
        const __m512i Y3 = normalizeGather16(yRow, _mm512_add_epi32(yBaseIdx, _mm512_set1_epi32(3)));
        const __m512i U2 = normalizeGather16(uRow, _mm512_add_epi32(cBaseIdx, _mm512_set1_epi32(2)));

        const __m512i Y4 = normalizeGather16(yRow, _mm512_add_epi32(yBaseIdx, _mm512_set1_epi32(4)));
        const __m512i V2 = normalizeGather16(vRow, _mm512_add_epi32(cBaseIdx, _mm512_set1_epi32(2)));
        const __m512i Y5 = normalizeGather16(yRow, _mm512_add_epi32(yBaseIdx, _mm512_set1_epi32(5)));

        const __m512i w0 = _mm512_or_si512(U0, _mm512_or_si512(_mm512_slli_epi32(Y0, 10), _mm512_slli_epi32(V0, 20)));
        const __m512i w1 = _mm512_or_si512(Y1, _mm512_or_si512(_mm512_slli_epi32(U1, 10), _mm512_slli_epi32(Y2, 20)));
        const __m512i w2 = _mm512_or_si512(V1, _mm512_or_si512(_mm512_slli_epi32(Y3, 10), _mm512_slli_epi32(U2, 20)));
        const __m512i w3 = _mm512_or_si512(Y4, _mm512_or_si512(_mm512_slli_epi32(V2, 10), _mm512_slli_epi32(Y5, 20)));

        storeFourGroups(out + 0,
                        _mm512_castsi512_si128(w0),
                        _mm512_castsi512_si128(w1),
                        _mm512_castsi512_si128(w2),
                        _mm512_castsi512_si128(w3));
        storeFourGroups(out + 16,
                        _mm512_extracti32x4_epi32(w0, 1),
                        _mm512_extracti32x4_epi32(w1, 1),
                        _mm512_extracti32x4_epi32(w2, 1),
                        _mm512_extracti32x4_epi32(w3, 1));
        storeFourGroups(out + 32,
                        _mm512_extracti32x4_epi32(w0, 2),
                        _mm512_extracti32x4_epi32(w1, 2),
                        _mm512_extracti32x4_epi32(w2, 2),
                        _mm512_extracti32x4_epi32(w3, 2));
        storeFourGroups(out + 48,
                        _mm512_extracti32x4_epi32(w0, 3),
                        _mm512_extracti32x4_epi32(w1, 3),
                        _mm512_extracti32x4_epi32(w2, 3),
                        _mm512_extracti32x4_epi32(w3, 3));
        out += 64;
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

void packYuv422p10ToV210Avx512(const uint16_t* yBase,
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
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
    for (int y = 0; y < height; ++y) {
        packRowAvx512(yBase + static_cast<size_t>(y) * static_cast<size_t>(yStride),
                      uBase + static_cast<size_t>(y) * static_cast<size_t>(uStride),
                      vBase + static_cast<size_t>(y) * static_cast<size_t>(vStride),
                      width,
                      reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * static_cast<size_t>(dstRowBytes)));
    }
#else
    packYuv422p10ToV210Avx2(yBase, uBase, vBase,
                            yStride, uStride, vStride,
                            width, height, dst, dstRowBytes);
#endif
}

} // namespace nxframe
