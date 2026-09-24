/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * Lightweight SMPTE ST 334-1 Caption Distribution Packet (CDP) inspection.
 * This parser is intentionally transport-agnostic and does not decode caption
 * text. It validates the CDP envelope and summarizes cc_data() triplets so the
 * sender can prove that captured DID 0x61 / SDID 0x01 ANC is usable before
 * H.264/HEVC carriage is implemented.
 */

#pragma once

#include "core/metadata.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxframe {

struct CaptionCdpInfo
{
    bool is_caption_anc = false;
    bool valid = false;
    bool identifier_ok = false;
    bool length_ok = false;
    bool checksum_ok = false;
    bool footer_ok = false;
    bool sequence_ok = false;
    bool cc_data_present_flag = false;
    bool captions_present_flag = false;
    bool cc_data_section_found = false;

    uint8_t frame_rate_code = 0;
    uint16_t sequence = 0;
    uint16_t footer_sequence = 0;
    uint8_t cdp_length = 0;
    uint8_t cc_count = 0;
    uint8_t valid_608 = 0;
    uint8_t valid_708 = 0;
    uint8_t invalid_cc = 0;

    std::vector<uint8_t> cdp_bytes;
    std::vector<CaptionCcData> cc_data;
    std::string error;
};

inline CaptionCdpInfo inspectCaptionCdp(const AncPacket& packet)
{
    CaptionCdpInfo info;
    info.is_caption_anc = (packet.did == 0x61u && packet.sdid == 0x01u);
    if (!info.is_caption_anc) {
        info.error = "not-st334-caption-anc";
        return info;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(packet.user_words.size());
    for (uint16_t word : packet.user_words) {
        bytes.push_back(static_cast<uint8_t>(word & 0x00ffu));
    }

    // CDP header (identifier, length, rate, flags, sequence) is 7 bytes and
    // the footer is 4 bytes (0x74, sequence, checksum).
    if (bytes.size() < 11u) {
        info.error = "cdp-too-short";
        return info;
    }

    info.identifier_ok = (bytes[0] == 0x96u && bytes[1] == 0x69u);
    if (!info.identifier_ok) {
        info.error = "bad-cdp-identifier";
        return info;
    }

    info.cdp_length = bytes[2];
    info.length_ok = (info.cdp_length >= 11u &&
                      static_cast<size_t>(info.cdp_length) <= bytes.size());
    if (!info.length_ok) {
        info.error = "bad-cdp-length";
        return info;
    }

    const size_t cdpLen = static_cast<size_t>(info.cdp_length);
    info.cdp_bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cdpLen));
    info.frame_rate_code = static_cast<uint8_t>((bytes[3] >> 4) & 0x0fu);
    const uint8_t flags = bytes[4];
    info.cc_data_present_flag = (flags & 0x40u) != 0u;
    info.captions_present_flag = (flags & 0x02u) != 0u;
    info.sequence = static_cast<uint16_t>((static_cast<uint16_t>(bytes[5]) << 8) |
                                          static_cast<uint16_t>(bytes[6]));

    uint32_t checksum = 0;
    for (size_t i = 0; i < cdpLen; ++i) {
        checksum += bytes[i];
    }
    info.checksum_ok = ((checksum & 0xffu) == 0u);

    // The footer is fixed at the end of cdp_length. Validate it independently
    // so a malformed/unknown optional section cannot make us overrun.
    const size_t footerPos = cdpLen - 4u;
    info.footer_ok = (bytes[footerPos] == 0x74u);
    if (info.footer_ok) {
        info.footer_sequence = static_cast<uint16_t>(
            (static_cast<uint16_t>(bytes[footerPos + 1u]) << 8) |
             static_cast<uint16_t>(bytes[footerPos + 2u]));
        info.sequence_ok = (info.footer_sequence == info.sequence);
    }

    // Optional sections may appear before cc_data(), so scan the CDP body for
    // section id 0x72. Validate the complete section before accepting it.
    for (size_t pos = 7u; pos + 1u < footerPos; ++pos) {
        if (bytes[pos] != 0x72u) {
            continue;
        }

        const uint8_t countByte = bytes[pos + 1u];
        const uint8_t ccCount = static_cast<uint8_t>(countByte & 0x1fu);
        const size_t sectionBytes = 2u + static_cast<size_t>(ccCount) * 3u;
        if (pos + sectionBytes > footerPos) {
            continue;
        }

        info.cc_data_section_found = true;
        info.cc_count = ccCount;
        for (uint8_t i = 0; i < ccCount; ++i) {
            const size_t triplet = pos + 2u + static_cast<size_t>(i) * 3u;
            const uint8_t typeByte = bytes[triplet];
            CaptionCcData cc;
            cc.header = typeByte;
            cc.data1 = bytes[triplet + 1u];
            cc.data2 = bytes[triplet + 2u];
            info.cc_data.push_back(cc);
            const bool markerOk = ((typeByte & 0xf8u) == 0xf8u);
            const bool ccValid = (typeByte & 0x04u) != 0u;
            const uint8_t ccType = static_cast<uint8_t>(typeByte & 0x03u);

            if (!markerOk || !ccValid) {
                ++info.invalid_cc;
                continue;
            }

            if (ccType <= 1u) {
                ++info.valid_608;
            } else {
                ++info.valid_708;
            }
        }
        break;
    }

    if (!info.checksum_ok) {
        info.error = "bad-cdp-checksum";
    } else if (!info.footer_ok) {
        info.error = "missing-cdp-footer";
    } else if (!info.sequence_ok) {
        info.error = "cdp-sequence-mismatch";
    } else if (info.cc_data_present_flag && !info.cc_data_section_found) {
        info.error = "missing-cc-data-section";
    } else {
        info.valid = true;
    }

    return info;
}


inline CaptionSidecar makeCaptionSidecar(const AncPacket& packet,
                                         const CaptionCdpInfo& info)
{
    CaptionSidecar out;
    if (!info.valid || !info.is_caption_anc) {
        return out;
    }

    out.valid = true;
    out.did = packet.did;
    out.sdid = packet.sdid;
    out.line = packet.line;
    out.stream = packet.stream;
    out.frame_rate_code = info.frame_rate_code;
    out.sequence = info.sequence;
    out.cdp_bytes = info.cdp_bytes;
    out.cc_data = info.cc_data;
    return out;
}

} // namespace nxframe
