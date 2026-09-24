/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * Converts NxFrame caption sidecar data into the raw ATSC A/53 cc_data byte
 * stream expected by FFmpeg AV_FRAME_DATA_A53_CC. FFmpeg/libx264 is then
 * responsible for constructing the registered ITU-T T.35 H.264 SEI message.
 */

#pragma once

#include "core/metadata.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxframe {

inline CaptionSidecar parseA53CcData(const uint8_t* data, size_t size)
{
    CaptionSidecar caption;

    // FFmpeg exposes AV_FRAME_DATA_A53_CC as a packed sequence of three-byte
    // cc_data() constructs. A malformed side-data block must never be allowed
    // to leak into the receiver metadata path.
    if (!data || size == 0u || (size % 3u) != 0u || size > (31u * 3u)) {
        return caption;
    }

    caption.valid = true;
    caption.cc_data.reserve(size / 3u);

    for (size_t i = 0; i < size; i += 3u) {
        CaptionCcData cc;
        cc.header = data[i + 0u];
        cc.data1 = data[i + 1u];
        cc.data2 = data[i + 2u];
        caption.cc_data.push_back(cc);
    }

    // The compressed A/53 representation does not carry the original SDI ANC
    // line, DID/SDID, CDP sequence, or complete CDP packet. Those are rebuilt
    // later when the receiver creates ST 334 VANC for DeckLink output.
    return caption;
}

inline std::vector<uint8_t> buildA53CcData(const CaptionSidecar& caption)
{
    std::vector<uint8_t> out;
    if (!caption.valid || caption.cc_data.empty()) {
        return out;
    }

    // A/53 uses a 5-bit cc_count, so one user-data structure can carry at most
    // 31 three-byte cc_data() constructs.
    const size_t count = std::min<size_t>(caption.cc_data.size(), 31u);
    out.reserve(count * 3u);

    for (size_t i = 0; i < count; ++i) {
        const CaptionCcData& cc = caption.cc_data[i];
        out.push_back(cc.header);
        out.push_back(cc.data1);
        out.push_back(cc.data2);
    }

    return out;
}

} // namespace nxframe
