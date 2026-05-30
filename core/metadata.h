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
 * Broadcast metadata sidecar structures used for SMPTE timecode and ANC/VANC foundations carried alongside frames and packets.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Broadcast-safe per-frame metadata foundation.
// This intentionally models SDI/broadcast metadata only. Do not add MISB/UAS/
// military KLV fields here; those workflows are outside NxFrame's broadcast
// contribution scope.

// Raw ANC/VANC packet placeholder. Payload words are stored as 10-bit words in 16-bit containers.
struct AncPacket
{
    uint16_t did = 0;
    uint16_t sdid = 0;
    uint16_t line = 0;
    uint16_t stream = 0;
    std::vector<uint16_t> user_words;
};

// SMPTE timecode sidecar. Source identifies where the code came from, for example RP188 VITC/LTC.
struct SmpteTimecode
{
    bool valid = false;
    uint8_t hours = 0;
    uint8_t minutes = 0;
    uint8_t seconds = 0;
    uint8_t frames = 0;
    uint32_t flags = 0;
    uint32_t user_bits = 0;
    bool has_user_bits = false;
    std::string source;   // e.g. rp188-any, rp188-vitc1, rp188-ltc
    std::string text;     // SDK formatted string when available

    void clear()
    {
        valid = false;
        hours = minutes = seconds = frames = 0;
        flags = 0;
        user_bits = 0;
        has_user_bits = false;
        source.clear();
        text.clear();
    }

    std::string toString() const
    {
        if (!valid) {
            return std::string();
        }
        if (!text.empty()) {
            return text;
        }

        char buf[16] = {0};
        const char sep = (flags & 0x1u) ? ';' : ':'; // drop-frame flag is bit 0 in DeckLink SDK
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u%c%02u",
                 static_cast<unsigned>(hours),
                 static_cast<unsigned>(minutes),
                 static_cast<unsigned>(seconds),
                 sep,
                 static_cast<unsigned>(frames));
        return std::string(buf);
    }
};

// Per-frame metadata sidecar propagated independently from the encoded elementary stream.
struct FrameMetadata
{
    SmpteTimecode timecode;
    std::vector<AncPacket> vanc_packets;

    bool hasTimecode() const noexcept { return timecode.valid; }

    void clear()
    {
        timecode.clear();
        vanc_packets.clear();
    }
};
