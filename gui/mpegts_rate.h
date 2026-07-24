/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace nxframe_gui {

constexpr long long kTsMuxrateStepBps = 100000;
constexpr long long kMaximumSupportedMuxrateBps = 400000000;

inline long long roundUpMuxrate(long long value)
{
    return ((value + kTsMuxrateStepBps - 1) / kTsMuxrateStepBps) * kTsMuxrateStepBps;
}

inline int calculateRecommendedTsMuxrate(int videoBitrateBps, long long totalAudioBitrateBps)
{
    if (videoBitrateBps <= 0) {
        throw std::invalid_argument("video bitrate must be positive");
    }
    if (totalAudioBitrateBps < 0) {
        throw std::invalid_argument("total audio bitrate cannot be negative");
    }

    const long long payload =
        static_cast<long long>(videoBitrateBps) + totalAudioBitrateBps;

    // Keep enough headroom for MPEG-TS packet headers, PSI/SI tables, PCR,
    // adaptation fields and short-term packetisation variation without
    // wasting a fixed percentage at high contribution bitrates.
    const long double percentageHeadroom =
        static_cast<long double>(payload) * 1.05L;
    const long double fixedHeadroom =
        static_cast<long double>(payload) + 1000000.0L;
    const long double required = std::max(percentageHeadroom, fixedHeadroom);

    const long long rounded = roundUpMuxrate(
        static_cast<long long>(std::ceil(required)));
    if (rounded > kMaximumSupportedMuxrateBps) {
        throw std::out_of_range(
            "selected video and audio settings exceed the supported constant MPEG-TS mux rate");
    }
    return static_cast<int>(rounded);
}

} // namespace nxframe_gui
