/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: tests/test_signal_generator_layout.cpp
 * Description: NxFrame regression and validation tests.
 */

#include "input/test_signal_generator.h"

#include <cstdint>
#include <iostream>
#include <vector>

static bool check_tight_layout()
{
    const int width = 16;
    const int height = 4;
    const int y_stride = width * 2;
    const int uv_stride = (width / 2) * 2;
    const size_t bytes = static_cast<size_t>(y_stride) * height +
                         static_cast<size_t>(uv_stride) * height * 2;

    std::vector<uint8_t> buf(bytes, 0);
    TestSignalGenerator gen(width, height, 50);
    gen.generateFrame(buf.data(), width, height, y_stride);

    const uint16_t* y = reinterpret_cast<const uint16_t*>(buf.data());
    const uint16_t* u = reinterpret_cast<const uint16_t*>(buf.data() + y_stride * height);
    const uint16_t* v = reinterpret_cast<const uint16_t*>(buf.data() + y_stride * height + uv_stride * height);

    if (y[0] == 0 || u[0] == 0 || v[0] == 0) {
        std::cerr << "Generated test signal did not populate all planes.\n";
        return false;
    }

    if (y[0] != (180u << 2) || u[0] != (128u << 2) || v[0] != (128u << 2)) {
        std::cerr << "Unexpected first bar values in generated YUV422P10 frame.\n";
        return false;
    }

    return true;
}

static bool check_invalid_stride_fallback()
{
    const int width = 16;
    const int height = 4;
    const int y_stride = width * 2;
    const int uv_stride = (width / 2) * 2;
    const size_t bytes = static_cast<size_t>(y_stride) * height +
                         static_cast<size_t>(uv_stride) * height * 2;

    std::vector<uint8_t> buf(bytes, 0);
    TestSignalGenerator gen(width, height, 50);

    // This simulates the old producer bug where PTS was accidentally passed as
    // stride. The generator must not treat a tiny positive value as a real row
    // stride because that corrupts the planar U/V offsets.
    gen.generateFrame(buf.data(), width, height, 1);

    const uint16_t* y = reinterpret_cast<const uint16_t*>(buf.data());
    const uint16_t* u = reinterpret_cast<const uint16_t*>(buf.data() + y_stride * height);
    const uint16_t* v = reinterpret_cast<const uint16_t*>(buf.data() + y_stride * height + uv_stride * height);

    if (y[0] != (180u << 2) || u[0] != (128u << 2) || v[0] != (128u << 2)) {
        std::cerr << "Invalid stride fallback produced corrupt YUV422P10 layout.\n";
        return false;
    }

    return true;
}

int main()
{
    return (check_tight_layout() && check_invalid_stride_fallback()) ? 0 : 1;
}
