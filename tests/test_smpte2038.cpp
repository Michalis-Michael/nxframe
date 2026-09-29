#include "core/smpte2038.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
    AncPacket atc;
    atc.did = 0x60;
    atc.sdid = 0x60;
    atc.line = 9;
    atc.stream = 0;
    // Representative 10-bit ATC UDW values. The serializer intentionally
    // preserves all ten bits instead of collapsing them to eight-bit bytes.
    atc.user_words = {
        0x200, 0x101, 0x202, 0x103,
        0x204, 0x105, 0x206, 0x107,
        0x208, 0x109, 0x20a, 0x10b,
        0x20c, 0x10d, 0x20e, 0x10f
    };

    AncPacket caption;
    caption.did = 0x61;
    caption.sdid = 0x01;
    caption.line = 9;
    caption.user_words = {0x155, 0x2aa};

    std::vector<uint8_t> payload;
    assert(nxframe::smpte2038::serializePayload({atc, caption}, payload, true));
    assert(!payload.empty());

    std::vector<AncPacket> parsed;
    assert(nxframe::smpte2038::parsePayload(payload.data(), payload.size(), parsed, true));
    assert(parsed.size() == 1u);
    assert(parsed[0].did == atc.did);
    assert(parsed[0].sdid == atc.sdid);
    assert(parsed[0].line == atc.line);
    assert(parsed[0].user_words == atc.user_words);

    // DeckLink can expose RP-188 through GetTimecode() without exposing the
    // corresponding DID/SDID 0x60/0x60 ANC packet. Verify synthesis of a valid
    // ATC_VITC1 packet while the independent ST 334 / CEA-608 packet remains
    // excluded from this ST 2038 RP-188 path.
    SmpteTimecode tc;
    tc.valid = true;
    tc.hours = 1;
    tc.minutes = 23;
    tc.seconds = 45;
    tc.frames = 12;
    tc.source = "rp188-vitc1";
    tc.user_bits = 0x12345678u;
    tc.has_user_bits = true;

    AncPacket synthesized;
    assert(nxframe::smpte2038::buildAtcPacketFromTimecode(tc, synthesized));
    assert(synthesized.did == 0x60u);
    assert(synthesized.sdid == 0x60u);
    assert(synthesized.line == 9u);
    assert(synthesized.user_words.size() == 16u);
    // DBB1=01h for ATC_VITC1: UDW1 b3 set, UDW2..8 b3 clear.
    assert((synthesized.user_words[0] & 0x08u) != 0u);
    for (size_t i = 1; i < 8u; ++i) {
        assert((synthesized.user_words[i] & 0x08u) == 0u);
    }

    std::vector<uint8_t> synthesizedPayload;
    assert(nxframe::smpte2038::serializePayload({synthesized, caption}, synthesizedPayload, true));
    std::vector<AncPacket> synthesizedParsed;
    assert(nxframe::smpte2038::parsePayload(synthesizedPayload.data(), synthesizedPayload.size(),
                                            synthesizedParsed, true));
    assert(synthesizedParsed.size() == 1u);
    assert(synthesizedParsed[0].did == 0x60u);
    assert(synthesizedParsed[0].sdid == 0x60u);
    assert(synthesizedParsed[0].user_words == synthesized.user_words);

    SmpteTimecode recovered;
    assert(nxframe::smpte2038::timecodeFromAtcPacket(synthesizedParsed[0], recovered));
    assert(recovered.valid);
    assert(recovered.hours == tc.hours);
    assert(recovered.minutes == tc.minutes);
    assert(recovered.seconds == tc.seconds);
    assert(recovered.frames == tc.frames);
    assert(recovered.user_bits == tc.user_bits);
    assert(recovered.has_user_bits);
    assert(recovered.source == "rp188-vitc1");

    std::cout << "SMPTE ST 2038 ATC payload + RP-188 synthesis/decode tests OK, bytes="
              << payload.size() << "\n";
    return 0;
}
