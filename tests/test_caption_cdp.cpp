#include "core/caption_cdp.h"
#include "core/caption_a53.h"
#include "core/metadata_tracker.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

std::vector<uint8_t> makeCdp(bool corruptChecksum = false)
{
    // Minimal synthetic 25-fps-style CDP with one cc_data section containing
    // two valid 608 triplets and one valid DTVCC triplet.
    std::vector<uint8_t> b = {
        0x96, 0x69, 0x00, 0x30, 0x43, 0x12, 0x34,
        0x72, 0xE3,
        0xFC, 0x94, 0x20, // valid type 0 (608)
        0xFD, 0x94, 0x20, // valid type 1 (608)
        0xFE, 0x11, 0x22, // valid type 2 (708/DTVCC)
        0x74, 0x12, 0x34,
        0x00
    };
    b[2] = static_cast<uint8_t>(b.size());
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < b.size(); ++i) {
        sum += b[i];
    }
    b.back() = static_cast<uint8_t>((0u - sum) & 0xffu);
    if (corruptChecksum) {
        b.back() ^= 0x01u;
    }
    return b;
}

AncPacket packetFrom(const std::vector<uint8_t>& bytes)
{
    AncPacket packet;
    packet.did = 0x61;
    packet.sdid = 0x01;
    packet.line = 9;
    for (uint8_t b : bytes) {
        packet.user_words.push_back(b);
    }
    return packet;
}

} // namespace

int main()
{
    {
        const AncPacket packet = packetFrom(makeCdp());
        const auto info = nxframe::inspectCaptionCdp(packet);
        if (!info.valid || !info.identifier_ok || !info.length_ok ||
            !info.checksum_ok || !info.footer_ok || !info.sequence_ok ||
            !info.cc_data_section_found || info.cc_count != 3 ||
            info.valid_608 != 2 || info.valid_708 != 1 ||
            info.cdp_bytes.size() != makeCdp().size() ||
            info.cc_data.size() != 3) {
            std::cerr << "valid CDP was not parsed as expected\n";
            return 1;
        }

        const CaptionSidecar sidecar = nxframe::makeCaptionSidecar(packet, info);
        if (!sidecar.valid || sidecar.did != 0x61u || sidecar.sdid != 0x01u ||
            sidecar.line != 9u || sidecar.frame_rate_code != 3u ||
            sidecar.sequence != 0x1234u || sidecar.cc_data.size() != 3u ||
            !sidecar.cc_data[0].valid() || sidecar.cc_data[0].type() != 0u ||
            sidecar.cc_data[0].data1 != 0x94u || sidecar.cc_data[0].data2 != 0x20u) {
            std::cerr << "caption sidecar was not built as expected\n";
            return 1;
        }
    }

    {
        const auto info = nxframe::inspectCaptionCdp(packetFrom(makeCdp(true)));
        if (info.valid || info.checksum_ok || info.error != "bad-cdp-checksum") {
            std::cerr << "bad checksum was not rejected\n";
            return 1;
        }
    }

    {
        AncPacket packet;
        packet.did = 0x60;
        packet.sdid = 0x60;
        const auto info = nxframe::inspectCaptionCdp(packet);
        if (info.is_caption_anc || info.valid) {
            std::cerr << "non-caption ANC was misidentified\n";
            return 1;
        }
    }

    {
        const AncPacket packet = packetFrom(makeCdp());
        const auto info = nxframe::inspectCaptionCdp(packet);
        const CaptionSidecar sidecar = nxframe::makeCaptionSidecar(packet, info);

        nxframe::FrameMetadataTracker tracker;
        FrameMetadata m10;
        m10.caption = sidecar;
        m10.caption.sequence = 10;
        FrameMetadata m11;
        m11.caption = sidecar;
        m11.caption.sequence = 11;
        tracker.remember(10, m10);
        tracker.remember(11, m11);

        FrameMetadata out;
        if (!tracker.take(10, out) || !out.hasCaption() ||
            out.caption.sequence != 10 || tracker.size() != 1u) {
            std::cerr << "metadata tracker did not preserve PTS association\n";
            return 1;
        }
        if (!tracker.take(11, out) || out.caption.sequence != 11 ||
            tracker.size() != 0u) {
            std::cerr << "metadata tracker did not drain in PTS order\n";
            return 1;
        }
    }

    {
        const AncPacket packet = packetFrom(makeCdp());
        const auto info = nxframe::inspectCaptionCdp(packet);
        const CaptionSidecar sidecar = nxframe::makeCaptionSidecar(packet, info);
        const std::vector<uint8_t> a53 = nxframe::buildA53CcData(sidecar);

        if (a53.size() != 9u ||
            a53[0] != 0xFCu || a53[1] != 0x94u || a53[2] != 0x20u ||
            a53[3] != 0xFDu || a53[4] != 0x94u || a53[5] != 0x20u ||
            a53[6] != 0xFEu || a53[7] != 0x11u || a53[8] != 0x22u) {
            std::cerr << "A53 cc_data payload was not built as expected\n";
            return 1;
        }
    }

    return 0;
}
