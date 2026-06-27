/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * License / EULA notice:
 * This file is part of NxFrame. Use, redistribution, and modification are
 * governed by the project license and any written EULA or commercial license
 * agreement supplied with the project. If no separate written agreement is
 * supplied, the GPL-3.0-or-later terms apply.
 *
 * Description:
 * Portable scalar v210 unpack implementation and x86 AVX2 capability detection.
 * This file must remain buildable without AVX2 compiler flags so it can provide
 * the fallback path and the runtime CPU feature check on all supported builds.
 */

#include "simd_v210_avx2.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
  #if defined(__GNUC__) || defined(__clang__)
    #include <cpuid.h>
  #endif
#endif

bool cpu_has_avx2()
{
#if defined(__x86_64__) || defined(__i386) || defined(_M_X64) || defined(_M_IX86)
  #if defined(__GNUC__) || defined(__clang__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7) return false;

    __cpuid(1, eax, ebx, ecx, edx);
    const bool osxsave = (ecx & (1u << 27)) != 0;
    const bool avx     = (ecx & (1u << 28)) != 0;
    if (!osxsave || !avx) return false;

    // CPUID alone is not enough. The OS must also enable XMM/YMM state,
    // otherwise executing AVX/AVX2 instructions can fault even on AVX2 CPUs.
    uint32_t xcr0_eax = 0;
    uint32_t xcr0_edx = 0;
    __asm__ volatile ("xgetbv" : "=a"(xcr0_eax), "=d"(xcr0_edx) : "c"(0));
    const uint64_t xcr0 = (static_cast<uint64_t>(xcr0_edx) << 32) | xcr0_eax;
    if ((xcr0 & 0x6u) != 0x6u) return false;

    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return (ebx & (1u << 5)) != 0;
  #else
    return false;
  #endif
#else
    return false;
#endif
}

namespace {

// v210 stores six visible Y samples and three Cb/Cr pairs in four little-endian
// 32-bit words. This helper handles both full six-pixel groups and the final
// visible tail when a row width is not a multiple of six.
static inline void write_group6_visible(const uint32_t* row,
                                        int visiblePixels,
                                        uint16_t*& yPtr,
                                        uint16_t*& uPtr,
                                        uint16_t*& vPtr)
{
    const uint32_t a = row[0];
    const uint32_t b = row[1];
    const uint32_t c = row[2];
    const uint32_t d = row[3];

    const uint16_t s0  = static_cast<uint16_t>((a >>  0) & 0x3FFu);
    const uint16_t s1  = static_cast<uint16_t>((a >> 10) & 0x3FFu);
    const uint16_t s2  = static_cast<uint16_t>((a >> 20) & 0x3FFu);
    const uint16_t s3  = static_cast<uint16_t>((b >>  0) & 0x3FFu);
    const uint16_t s4  = static_cast<uint16_t>((b >> 10) & 0x3FFu);
    const uint16_t s5  = static_cast<uint16_t>((b >> 20) & 0x3FFu);
    const uint16_t s6  = static_cast<uint16_t>((c >>  0) & 0x3FFu);
    const uint16_t s7  = static_cast<uint16_t>((c >> 10) & 0x3FFu);
    const uint16_t s8  = static_cast<uint16_t>((c >> 20) & 0x3FFu);
    const uint16_t s9  = static_cast<uint16_t>((d >>  0) & 0x3FFu);
    const uint16_t s10 = static_cast<uint16_t>((d >> 10) & 0x3FFu);
    const uint16_t s11 = static_cast<uint16_t>((d >> 20) & 0x3FFu);

    const uint16_t yy[6] = { s1, s3, s5, s7, s9, s11 };
    const uint16_t uu[3] = { s0, s4, s8 };
    const uint16_t vv[3] = { s2, s6, s10 };

    const int yCount = std::max(0, std::min(visiblePixels, 6));
    // The DeckLink modes used by NxFrame are even-width 4:2:2 formats. Keep the
    // tail path bounded anyway so the converter never writes outside the visible
    // plane region if a non-standard width is supplied during testing.
    const int cCount = yCount / 2;

    for (int i = 0; i < yCount; ++i) yPtr[i] = yy[i];
    for (int i = 0; i < cCount; ++i) {
        uPtr[i] = uu[i];
        vPtr[i] = vv[i];
    }

    yPtr += yCount;
    uPtr += cCount;
    vPtr += cCount;
}

static inline void unpack12_scalar_from8words(const uint32_t* row,
                                              uint16_t*& yPtr,
                                              uint16_t*& uPtr,
                                              uint16_t*& vPtr)
{
    write_group6_visible(row,     6, yPtr, uPtr, vPtr);
    write_group6_visible(row + 4, 6, yPtr, uPtr, vPtr);
}

static inline void unpack_row_scalar(const uint8_t* srcRow,
                                     int width,
                                     uint16_t* yRow,
                                     uint16_t* uRow,
                                     uint16_t* vRow)
{
    // Output samples are stored as numeric 10-bit values in 16-bit containers.
    // They are not shifted to the high bits; this matches the internal
    // YUV422P10LE bus contract used by the sender encoder path.
    const uint32_t* row = reinterpret_cast<const uint32_t*>(srcRow);
    uint16_t* yPtr = yRow;
    uint16_t* uPtr = uRow;
    uint16_t* vPtr = vRow;

    int x = 0;
    while (x + 11 < width) {
        unpack12_scalar_from8words(row, yPtr, uPtr, vPtr);
        row += 8;
        x += 12;
    }
    while (x + 5 < width) {
        write_group6_visible(row, 6, yPtr, uPtr, vPtr);
        row += 4;
        x += 6;
    }

    const int remaining = width - x;
    if (remaining > 0) {
        write_group6_visible(row, remaining, yPtr, uPtr, vPtr);
    }
}

static inline void process_rows_scalar(const uint8_t* src,
                                       int srcRowBytes,
                                       int width,
                                       int startRow,
                                       int endRow,
                                       uint16_t* dstY,
                                       uint16_t* dstU,
                                       uint16_t* dstV)
{
    const int chromaWidth = width / 2;
    for (int y = startRow; y < endRow; ++y) {
        const uint8_t* srcRow = src + static_cast<size_t>(y) * static_cast<size_t>(srcRowBytes);
        uint16_t* yRow = dstY + static_cast<size_t>(y) * static_cast<size_t>(width);
        uint16_t* uRow = dstU + static_cast<size_t>(y) * static_cast<size_t>(chromaWidth);
        uint16_t* vRow = dstV + static_cast<size_t>(y) * static_cast<size_t>(chromaWidth);
        unpack_row_scalar(srcRow, width, yRow, uRow, vRow);
    }
}

} // namespace

void v210_to_yuv422p10le_scalar(
    const uint8_t* src, int srcRowBytes, int w, int h,
    uint16_t* dstY, uint16_t* dstU, uint16_t* dstV)
{
    process_rows_scalar(src, srcRowBytes, w, 0, h, dstY, dstU, dstV);
}
