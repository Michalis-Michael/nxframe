/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * Rebuilds a SMPTE ST 334-1 Caption Distribution Packet (CDP) from the
 * CTA-708 cc_data() triplets recovered by the decoder from A/53 side data.
 * This does not write SDI ANC itself; it prepares the standards-compliant CDP
 * payload that the DeckLink output stage can later place into DID 0x61/SDID 0x01.
 */

#pragma once

#include "core/metadata.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxframe {

// Return the complete CDP frame-rate byte defined by ST 334-2. The high nibble
// is cdp_frame_rate and the low nibble is reserved and must be all ones.
inline uint8_t captionCdpFrameRateByte(int num, int den) noexcept
{
    if (num == 24000 && den == 1001) return 0x1Fu;
    if (num == 24 && den == 1)       return 0x2Fu;
    if (num == 25 && den == 1)       return 0x3Fu;
    if (num == 30000 && den == 1001) return 0x4Fu;
    if (num == 30 && den == 1)       return 0x5Fu;
    if (num == 50 && den == 1)       return 0x6Fu;
    if (num == 60000 && den == 1001) return 0x7Fu;
    if (num == 60 && den == 1)       return 0x8Fu;
    return 0u;
}


// Build a synthetic CEA-608 "Erase Displayed Memory" caption frame.
// This is used by the receiver when a previously-active 608 caption stream
// disappears entirely. 608 decoders retain displayed memory until a control
// command clears it, so simply omitting VANC can leave the last subtitle frozen.
//
// Keep the same cc_count as the preceding source frame when possible. This
// preserves the source cadence/layout while carrying one valid 608 construct
// for each analog field (cc_type 0 and cc_type 1). The receiver repeats this
// clear CDP on consecutive video frames, which is the normal 608 redundancy
// model for control commands. All remaining triplets are invalid padding.
inline CaptionSidecar makeCea608EraseDisplayedMemory(size_t cc_count)
{
    CaptionSidecar caption;
    if (cc_count < 2u) {
        cc_count = 2u;
    } else if (cc_count > 31u) {
        cc_count = 31u;
    }

    caption.valid = true;
    caption.cc_data.resize(cc_count);

    // 0x94 0x2C is the odd-parity encoded CEA-608 Erase Displayed Memory
    // (EDM) control pair. Carry it once for field 1 (cc_type=0) and once for
    // field 2 (cc_type=1). This mirrors the source layout observed on the wire
    // (one valid 608 construct per field) and avoids placing two constructs of
    // the same field type in a single cc_data() block.
    caption.cc_data[0].header = 0xFCu; // marker=11111, cc_valid=1, cc_type=0
    caption.cc_data[0].data1 = 0x94u;
    caption.cc_data[0].data2 = 0x2Cu;

    caption.cc_data[1].header = 0xFDu; // marker=11111, cc_valid=1, cc_type=1
    caption.cc_data[1].data1 = 0x94u;
    caption.cc_data[1].data2 = 0x2Cu;

    // Invalid DTVCC padding, matching the form commonly used in CDP frames.
    for (size_t i = 2u; i < cc_count; ++i) {
        caption.cc_data[i].header = 0xFAu;
        caption.cc_data[i].data1 = 0x00u;
        caption.cc_data[i].data2 = 0x00u;
    }

    return caption;
}

// Rebuild the CDP envelope around already-decoded cc_data() triplets.
// Returns false without mutating the sidecar when the input is unusable or
// the nominal frame rate cannot be represented by ST 334-2.
inline bool rebuildCaptionCdp(CaptionSidecar& caption,
                              uint16_t sequence,
                              int frame_rate_num,
                              int frame_rate_den,
                              uint16_t line = 9u,
                              uint16_t stream = 0u)
{
    if (!caption.valid || caption.cc_data.empty() || caption.cc_data.size() > 31u) {
        return false;
    }

    const uint8_t rateByte = captionCdpFrameRateByte(frame_rate_num, frame_rate_den);
    if (rateByte == 0u) {
        return false;
    }

    // Header (7) + cc_data section header (2) + triplets + footer (4).
    const size_t cdpLength = 13u + caption.cc_data.size() * 3u;
    if (cdpLength > 255u) {
        return false;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(cdpLength);

    bytes.push_back(0x96u);
    bytes.push_back(0x69u);
    bytes.push_back(static_cast<uint8_t>(cdpLength));
    bytes.push_back(rateByte);

    // ccdata_present | caption_service_active | reserved(always one)
    bytes.push_back(0x43u);
    bytes.push_back(static_cast<uint8_t>((sequence >> 8) & 0xffu));
    bytes.push_back(static_cast<uint8_t>(sequence & 0xffu));

    bytes.push_back(0x72u); // cc_data section id
    bytes.push_back(static_cast<uint8_t>(0xE0u | caption.cc_data.size()));

    for (const CaptionCcData& cc : caption.cc_data) {
        // ST 334 requires the five high marker/reserved bits to be ones.
        bytes.push_back(static_cast<uint8_t>(cc.header | 0xF8u));
        bytes.push_back(cc.data1);
        bytes.push_back(cc.data2);
    }

    bytes.push_back(0x74u); // footer id
    bytes.push_back(static_cast<uint8_t>((sequence >> 8) & 0xffu));
    bytes.push_back(static_cast<uint8_t>(sequence & 0xffu));
    bytes.push_back(0u); // checksum placeholder

    uint32_t sum = 0u;
    for (uint8_t b : bytes) {
        sum += b;
    }
    bytes.back() = static_cast<uint8_t>((0u - sum) & 0xffu);

    caption.did = 0x61u;
    caption.sdid = 0x01u;
    caption.line = line;
    caption.stream = stream;
    caption.frame_rate_code = static_cast<uint8_t>((rateByte >> 4) & 0x0Fu);
    caption.sequence = sequence;
    caption.cdp_bytes = std::move(bytes);
    return true;
}

} // namespace nxframe
