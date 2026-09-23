#include "core/caption_a53.h"
#include "core/caption_cdp.h"
#include "core/caption_cdp_builder.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

AncPacket packetFromSidecar(const CaptionSidecar& caption)
{
    AncPacket packet;
    packet.did = caption.did;
    packet.sdid = caption.sdid;
    packet.line = caption.line;
    packet.stream = caption.stream;
    for (uint8_t b : caption.cdp_bytes) {
        packet.user_words.push_back(static_cast<uint16_t>(b));
    }
    return packet;
}

} // namespace

int main()
{
    const std::vector<uint8_t> a53 = {
        0xFC, 0x94, 0x20,
        0xFD, 0x94, 0x20,
        0xFE, 0x11, 0x22,
        0xFA, 0x00, 0x00
    };

    CaptionSidecar caption = nxframe::parseA53CcData(a53.data(), a53.size());
    if (!caption.valid) {
        std::cerr << "A53 input did not parse\n";
        return 1;
    }

    if (!nxframe::rebuildCaptionCdp(caption, 0x1234u, 25, 1)) {
        std::cerr << "25 fps CDP rebuild failed\n";
        return 1;
    }

    if (caption.did != 0x61u || caption.sdid != 0x01u ||
        caption.line != 9u || caption.stream != 0u ||
        caption.frame_rate_code != 0x03u || caption.sequence != 0x1234u ||
        caption.cdp_bytes.size() != 25u) {
        std::cerr << "rebuilt CDP metadata is wrong\n";
        return 1;
    }

    const nxframe::CaptionCdpInfo info = nxframe::inspectCaptionCdp(packetFromSidecar(caption));
    if (!info.valid || !info.checksum_ok || !info.footer_ok || !info.sequence_ok ||
        info.frame_rate_code != 0x03u || info.sequence != 0x1234u ||
        info.cc_count != 4u || info.valid_608 != 2u || info.valid_708 != 1u ||
        info.invalid_cc != 1u || info.cc_data.size() != 4u) {
        std::cerr << "rebuilt CDP failed parser validation\n";
        return 1;
    }

    for (size_t i = 0; i < a53.size() / 3u; ++i) {
        const CaptionCcData& cc = info.cc_data[i];
        if (cc.header != static_cast<uint8_t>(a53[i * 3u] | 0xF8u) ||
            cc.data1 != a53[i * 3u + 1u] ||
            cc.data2 != a53[i * 3u + 2u]) {
            std::cerr << "rebuilt CDP did not preserve cc_data triplets\n";
            return 1;
        }
    }

    CaptionSidecar unsupported = nxframe::parseA53CcData(a53.data(), a53.size());
    if (nxframe::rebuildCaptionCdp(unsupported, 0u, 27, 1)) {
        std::cerr << "unsupported frame rate was accepted\n";
        return 1;
    }
    if (!unsupported.cdp_bytes.empty()) {
        std::cerr << "failed rebuild mutated caption sidecar\n";
        return 1;
    }

    if (nxframe::captionCdpFrameRateByte(24000, 1001) != 0x1Fu ||
        nxframe::captionCdpFrameRateByte(24, 1) != 0x2Fu ||
        nxframe::captionCdpFrameRateByte(25, 1) != 0x3Fu ||
        nxframe::captionCdpFrameRateByte(30000, 1001) != 0x4Fu ||
        nxframe::captionCdpFrameRateByte(30, 1) != 0x5Fu ||
        nxframe::captionCdpFrameRateByte(50, 1) != 0x6Fu ||
        nxframe::captionCdpFrameRateByte(60000, 1001) != 0x7Fu ||
        nxframe::captionCdpFrameRateByte(60, 1) != 0x8Fu) {
        std::cerr << "CDP frame-rate mapping is wrong\n";
        return 1;
    }

    return 0;
}
