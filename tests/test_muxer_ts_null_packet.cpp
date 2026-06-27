#include "output/muxer_ts.h"

#include <cstdint>
#include <iostream>

int main()
{
    uint8_t packet[MuxerTS::kTsPacketSize] = {0};
    MuxerTS::makeNullPacket(packet);

    if (packet[0] != 0x47) {
        std::cerr << "sync byte mismatch\n";
        return 1;
    }

    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1Fu) << 8) | packet[2]);
    if (pid != 0x1FFFu) {
        std::cerr << "PID mismatch: " << pid << "\n";
        return 1;
    }

    if (packet[3] != 0x10u) {
        std::cerr << "transport/control byte mismatch\n";
        return 1;
    }

    for (int i = 4; i < MuxerTS::kTsPacketSize; ++i) {
        if (packet[i] != 0xFFu) {
            std::cerr << "null payload byte mismatch at " << i << "\n";
            return 1;
        }
    }

    return 0;
}
