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
 * Public declarations for the DeckLink v210 unpack helpers. The input hot path
 * uses cpu_has_avx2() during DeckLink initialization and dispatches either the
 * scalar or AVX2 implementation at runtime. Both unpackers convert packed v210
 * into the NxFrame internal planar 10-bit 4:2:2 bus.
 */

#pragma once

#include <cstdint>

bool cpu_has_avx2();
bool cpu_has_avx512_v210();

// Portable reference implementation. Used when SIMD is unavailable and by the
// SIMD translation units as their compile-time fallback.
void v210_to_yuv422p10le_scalar(
    const uint8_t* src,
    int srcRowBytes,
    int w,
    int h,
    uint16_t* dstY,
    uint16_t* dstU,
    uint16_t* dstV);

// AVX2 implementation. The caller is responsible for dispatching this only on
// machines where cpu_has_avx2() returned true. If this source file is compiled
// without AVX2 enabled, the function safely falls back to the scalar unpacker.
void v210_to_yuv422p10le_avx2(
    const uint8_t* src,
    int srcRowBytes,
    int w,
    int h,
    uint16_t* dstY,
    uint16_t* dstU,
    uint16_t* dstV);

// AVX-512 implementation. Runtime dispatch must call this only when
// cpu_has_avx512_v210() returned true. The implementation requires AVX-512F,
// AVX-512BW and AVX-512VL; if built without them it falls back to AVX2.
void v210_to_yuv422p10le_avx512(
    const uint8_t* src,
    int srcRowBytes,
    int w,
    int h,
    uint16_t* dstY,
    uint16_t* dstU,
    uint16_t* dstV);
