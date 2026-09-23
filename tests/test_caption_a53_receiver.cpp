#include "core/caption_a53.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main()
{
    const std::vector<uint8_t> a53 = {
        0xFC, 0x94, 0x20,
        0xFD, 0x94, 0x20,
        0xFE, 0x11, 0x22,
        0xFA, 0x00, 0x00
    };

    const CaptionSidecar caption = nxframe::parseA53CcData(a53.data(), a53.size());
    if (!caption.valid || caption.cc_data.size() != 4u ||
        !caption.cc_data[0].valid() || caption.cc_data[0].type() != 0u ||
        !caption.cc_data[1].valid() || caption.cc_data[1].type() != 1u ||
        !caption.cc_data[2].valid() || caption.cc_data[2].type() != 2u ||
        caption.cc_data[3].valid()) {
        std::cerr << "A53 receiver parser did not preserve cc_data constructs\
";
        return 1;
    }

    const std::vector<uint8_t> round_trip = nxframe::buildA53CcData(caption);
    if (round_trip != a53) {
        std::cerr << "A53 receiver parser did not round-trip exactly\
";
        return 1;
    }

    const uint8_t malformed[] = {0xFC, 0x94};
    if (nxframe::parseA53CcData(malformed, sizeof(malformed)).valid) {
        std::cerr << "Malformed A53 side data was accepted\
";
        return 1;
    }

    if (nxframe::parseA53CcData(nullptr, 0u).valid) {
        std::cerr << "Empty A53 side data was accepted\
";
        return 1;
    }

    return 0;
}
