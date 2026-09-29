/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal SMPTE ST 2038 ANC payload serializer/parser used for RP-188 / ST 12-2
 * ATC carriage in MPEG-2 transport streams. FFmpeg supplies/removes the PES
 * header; these helpers operate on the ST 2038 PES payload only.
 */
#pragma once

#include "core/metadata.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxframe {
namespace smpte2038 {

inline bool isAtcPacket(const AncPacket& p) noexcept
{
    return (p.did & 0xffu) == 0x60u && (p.sdid & 0xffu) == 0x60u;
}

inline uint16_t addEvenParity(uint8_t value) noexcept
{
    unsigned ones = 0;
    for (unsigned i = 0; i < 8; ++i) {
        ones += (value >> i) & 1u;
    }
    const uint16_t parity = static_cast<uint16_t>(ones & 1u); // makes total count even
    return static_cast<uint16_t>(value) |
           static_cast<uint16_t>(parity << 8) |
           static_cast<uint16_t>((parity ^ 1u) << 9);
}

enum class AtcPayloadType : uint8_t {
    Ltc = 0x00,
    Vitc1 = 0x01,
    Vitc2 = 0x02
};

inline AtcPayloadType payloadTypeForTimecode(const SmpteTimecode& tc) noexcept
{
    if (tc.source == "rp188-ltc") {
        return AtcPayloadType::Ltc;
    }
    if (tc.source == "rp188-vitc2") {
        return AtcPayloadType::Vitc2;
    }
    // RP188Any, RP188HighFrameRate and VITC1 are emitted as ATC_VITC1 when
    // the SDK does not identify a more specific payload. This is preferable
    // to inventing an LTC source that was not reported by the input device.
    return AtcPayloadType::Vitc1;
}

inline uint16_t preferredHdLine(AtcPayloadType type) noexcept
{
    switch (type) {
        case AtcPayloadType::Ltc:   return 10u;
        case AtcPayloadType::Vitc2: return 571u;
        case AtcPayloadType::Vitc1:
        default:                    return 9u;
    }
}

inline uint8_t userNibble(const SmpteTimecode& tc, unsigned group) noexcept
{
    if (!tc.has_user_bits || group >= 8u) {
        return 0u;
    }
    return static_cast<uint8_t>((tc.user_bits >> (group * 4u)) & 0x0fu);
}

inline uint16_t atcUdwWord(uint8_t timeOrUserNibble, bool dbbBit) noexcept
{
    // ST 12-2: time-code/user-data nibble occupies b7..b4, DBB occupies b3,
    // b2..b0 are zero; b8 is even parity over b7..b0 and b9 is !b8.
    uint8_t low = static_cast<uint8_t>((timeOrUserNibble & 0x0fu) << 4u);
    if (dbbBit) {
        low |= 0x08u;
    }
    return addEvenParity(low);
}

inline bool timecodeFromAtcPacket(const AncPacket& packet, SmpteTimecode& tc)
{
    if (!isAtcPacket(packet) || packet.user_words.size() != 16u) {
        return false;
    }

    uint8_t nibble[16] = {};
    uint8_t dbb1 = 0u;
    for (unsigned i = 0; i < 16u; ++i) {
        const uint8_t low = static_cast<uint8_t>(packet.user_words[i] & 0xffu);
        nibble[i] = static_cast<uint8_t>((low >> 4u) & 0x0fu);
        if (i < 8u && (low & 0x08u) != 0u) {
            dbb1 |= static_cast<uint8_t>(1u << i);
        }
    }

    const uint8_t frames = static_cast<uint8_t>((nibble[2] & 0x03u) * 10u + (nibble[0] & 0x0fu));
    const uint8_t seconds = static_cast<uint8_t>((nibble[6] & 0x07u) * 10u + (nibble[4] & 0x0fu));
    const uint8_t minutes = static_cast<uint8_t>((nibble[10] & 0x07u) * 10u + (nibble[8] & 0x0fu));
    const uint8_t hours = static_cast<uint8_t>((nibble[14] & 0x03u) * 10u + (nibble[12] & 0x0fu));
    if (hours > 23u || minutes > 59u || seconds > 59u || frames > 59u) {
        return false;
    }

    SmpteTimecode out{};
    out.valid = true;
    out.hours = hours;
    out.minutes = minutes;
    out.seconds = seconds;
    out.frames = frames;
    if ((nibble[2] & 0x04u) != 0u) {
        out.flags |= 0x01u;
    }

    uint32_t userBits = 0u;
    for (unsigned group = 0; group < 8u; ++group) {
        const unsigned udw = group * 2u + 1u;
        userBits |= static_cast<uint32_t>(nibble[udw] & 0x0fu) << (group * 4u);
    }
    out.user_bits = userBits;
    out.has_user_bits = true;

    switch (dbb1) {
        case static_cast<uint8_t>(AtcPayloadType::Ltc):
            out.source = "rp188-ltc";
            break;
        case static_cast<uint8_t>(AtcPayloadType::Vitc2):
            out.source = "rp188-vitc2";
            break;
        case static_cast<uint8_t>(AtcPayloadType::Vitc1):
        default:
            out.source = "rp188-vitc1";
            break;
    }

    tc = std::move(out);
    return true;
}

inline bool buildAtcPacketFromTimecode(const SmpteTimecode& tc, AncPacket& out)
{
    if (!tc.valid || tc.hours > 23u || tc.minutes > 59u ||
        tc.seconds > 59u || tc.frames > 59u) {
        return false;
    }

    const AtcPayloadType type = payloadTypeForTimecode(tc);
    const uint8_t dbb1 = static_cast<uint8_t>(type);
    const uint8_t dbb2 = 0u; // HD digital: line select/duplication "don't care", validity good.

    uint8_t nibble[16] = {};
    nibble[0]  = static_cast<uint8_t>(tc.frames % 10u);
    nibble[1]  = userNibble(tc, 0u);
    nibble[2]  = static_cast<uint8_t>((tc.frames / 10u) & 0x03u);
    // DeckLink's drop-frame flag maps to the ST 12-1 drop-frame position.
    if ((tc.flags & 0x01u) != 0u) {
        nibble[2] |= 0x04u;
    }
    nibble[3]  = userNibble(tc, 1u);
    nibble[4]  = static_cast<uint8_t>(tc.seconds % 10u);
    nibble[5]  = userNibble(tc, 2u);
    nibble[6]  = static_cast<uint8_t>((tc.seconds / 10u) & 0x07u);
    nibble[7]  = userNibble(tc, 3u);
    nibble[8]  = static_cast<uint8_t>(tc.minutes % 10u);
    nibble[9]  = userNibble(tc, 4u);
    nibble[10] = static_cast<uint8_t>((tc.minutes / 10u) & 0x07u);
    nibble[11] = userNibble(tc, 5u);
    nibble[12] = static_cast<uint8_t>(tc.hours % 10u);
    nibble[13] = userNibble(tc, 6u);
    nibble[14] = static_cast<uint8_t>((tc.hours / 10u) & 0x03u);
    nibble[15] = userNibble(tc, 7u);

    out = AncPacket{};
    out.did = 0x60u;
    out.sdid = 0x60u;
    out.line = preferredHdLine(type);
    out.stream = 0u;
    out.user_words.reserve(16u);
    for (unsigned i = 0; i < 16u; ++i) {
        const bool dbb = (i < 8u)
                             ? ((dbb1 >> i) & 1u) != 0u
                             : ((dbb2 >> (i - 8u)) & 1u) != 0u;
        out.user_words.push_back(atcUdwWord(nibble[i], dbb));
    }
    return true;
}

inline uint16_t checksumWord(uint16_t did,
                             uint16_t sdid,
                             uint16_t dc,
                             const std::vector<uint16_t>& udw) noexcept
{
    uint32_t sum = (did & 0x01ffu) + (sdid & 0x01ffu) + (dc & 0x01ffu);
    for (uint16_t word : udw) {
        sum += (word & 0x01ffu);
    }
    const uint16_t value = static_cast<uint16_t>(sum & 0x01ffu);
    const uint16_t bit8 = static_cast<uint16_t>((value >> 8) & 1u);
    return static_cast<uint16_t>(value | ((bit8 ^ 1u) << 9));
}

class BitWriter
{
public:
    void put(uint32_t value, unsigned bits)
    {
        for (unsigned i = 0; i < bits; ++i) {
            const unsigned shift = bits - 1u - i;
            const uint8_t bit = static_cast<uint8_t>((value >> shift) & 1u);
            if ((bit_count_ & 7u) == 0u) {
                bytes_.push_back(0u);
            }
            if (bit) {
                bytes_.back() |= static_cast<uint8_t>(1u << (7u - (bit_count_ & 7u)));
            }
            ++bit_count_;
        }
    }

    void padOnesToByte()
    {
        while ((bit_count_ & 7u) != 0u) {
            put(1u, 1u);
        }
    }

    const std::vector<uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    size_t bit_count_ = 0;
};

class BitReader
{
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool get(unsigned bits, uint32_t& value)
    {
        if (!data_ || bits > 32u || bit_pos_ + bits > size_ * 8u) {
            return false;
        }
        value = 0u;
        for (unsigned i = 0; i < bits; ++i) {
            const size_t byte_index = bit_pos_ >> 3u;
            const unsigned bit_index = 7u - static_cast<unsigned>(bit_pos_ & 7u);
            value = (value << 1u) | ((data_[byte_index] >> bit_index) & 1u);
            ++bit_pos_;
        }
        return true;
    }

    bool skipToByte()
    {
        while ((bit_pos_ & 7u) != 0u) {
            uint32_t ignored = 0;
            if (!get(1u, ignored)) return false;
        }
        return true;
    }

    size_t bytePosition() const noexcept { return bit_pos_ >> 3u; }
    size_t remainingBytes() const noexcept
    {
        const size_t p = bytePosition();
        return p < size_ ? size_ - p : 0u;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t bit_pos_ = 0;
};

inline bool serializePayload(const std::vector<AncPacket>& packets,
                             std::vector<uint8_t>& out,
                             bool atcOnly = true)
{
    BitWriter w;
    bool wrote = false;

    for (const AncPacket& p : packets) {
        if (atcOnly && !isAtcPacket(p)) {
            continue;
        }
        if (p.user_words.size() > 255u) {
            return false;
        }

        const uint16_t did = addEvenParity(static_cast<uint8_t>(p.did & 0xffu));
        const uint16_t sdid = addEvenParity(static_cast<uint8_t>(p.sdid & 0xffu));
        const uint16_t dc = addEvenParity(static_cast<uint8_t>(p.user_words.size()));
        const uint16_t checksum = checksumWord(did, sdid, dc, p.user_words);

        // ST 2038 ANC packet placement header. DeckLink's packet interface does
        // not expose C/Y or horizontal offset, so preserve the known line and
        // mark horizontal placement as unspecified (0xFFF).
        w.put(0u, 6u);
        w.put(0u, 1u); // c_not_y_channel_flag: Y
        w.put(static_cast<uint32_t>(p.line) & 0x07ffu, 11u);
        w.put(0x0fffu, 12u); // horizontal_offset unspecified
        w.put(did, 10u);
        w.put(sdid, 10u);
        w.put(dc, 10u);
        for (uint16_t word : p.user_words) {
            w.put(static_cast<uint32_t>(word & 0x03ffu), 10u);
        }
        w.put(checksum, 10u);
        w.padOnesToByte();
        wrote = true;
    }

    out = w.bytes();
    return wrote;
}

inline bool parsePayload(const uint8_t* data,
                         size_t size,
                         std::vector<AncPacket>& packets,
                         bool atcOnly = true)
{
    packets.clear();
    if (!data || size == 0u) {
        return false;
    }

    BitReader r(data, size);
    while (r.remainingBytes() > 0u) {
        // Trailing ST 2038 stuffing is 0xFF.
        const size_t bytePos = r.bytePosition();
        if (bytePos < size && data[bytePos] == 0xffu) {
            bool allStuffing = true;
            for (size_t i = bytePos; i < size; ++i) {
                if (data[i] != 0xffu) {
                    allStuffing = false;
                    break;
                }
            }
            if (allStuffing) {
                break;
            }
        }

        uint32_t reserved = 0, cNotY = 0, line = 0, horizontal = 0;
        uint32_t did10 = 0, sdid10 = 0, dc10 = 0;
        if (!r.get(6u, reserved) || !r.get(1u, cNotY) || !r.get(11u, line) ||
            !r.get(12u, horizontal) || !r.get(10u, did10) ||
            !r.get(10u, sdid10) || !r.get(10u, dc10)) {
            return false;
        }
        if (reserved != 0u) {
            return false;
        }

        const size_t count = static_cast<size_t>(dc10 & 0xffu);
        AncPacket p;
        p.did = static_cast<uint16_t>(did10 & 0xffu);
        p.sdid = static_cast<uint16_t>(sdid10 & 0xffu);
        p.line = static_cast<uint16_t>(line & 0x07ffu);
        p.stream = 0u;
        p.user_words.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            uint32_t word = 0;
            if (!r.get(10u, word)) {
                return false;
            }
            p.user_words.push_back(static_cast<uint16_t>(word & 0x03ffu));
        }

        uint32_t checksum = 0;
        if (!r.get(10u, checksum)) {
            return false;
        }
        if (!r.skipToByte()) {
            return false;
        }

        const uint16_t expected = checksumWord(static_cast<uint16_t>(did10),
                                               static_cast<uint16_t>(sdid10),
                                               static_cast<uint16_t>(dc10),
                                               p.user_words);
        if ((checksum & 0x03ffu) != expected) {
            return false;
        }

        if (!atcOnly || isAtcPacket(p)) {
            packets.push_back(std::move(p));
        }
    }

    return !packets.empty();
}

} // namespace smpte2038
} // namespace nxframe
