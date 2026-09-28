/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SIMD helpers for packing planar 10-bit 4:2:2 into DeckLink v210.
 */
#pragma once

#include <cstdint>

namespace nxframe {

void packYuv422p10ToV210Avx2(const uint16_t* yBase,
                              const uint16_t* uBase,
                              const uint16_t* vBase,
                              int yStride,
                              int uStride,
                              int vStride,
                              int width,
                              int height,
                              uint8_t* dst,
                              int dstRowBytes);

void packYuv422p10ToV210Avx512(const uint16_t* yBase,
                                const uint16_t* uBase,
                                const uint16_t* vBase,
                                int yStride,
                                int uStride,
                                int vStride,
                                int width,
                                int height,
                                uint8_t* dst,
                                int dstRowBytes);

} // namespace nxframe
