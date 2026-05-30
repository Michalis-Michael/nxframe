/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: tests/test_demuxer_ts_health.cpp
 * Description: NxFrame regression and validation tests.
 */

#include "receiver/demuxer_ts.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

static std::vector<uint8_t> makeTsPacket(int pid, int cc, bool payloadStart = false)
{
    std::vector<uint8_t> p(188, 0xff);
    p[0] = 0x47;
    p[1] = static_cast<uint8_t>(((payloadStart ? 0x40 : 0x00) | ((pid >> 8) & 0x1f)) & 0xff);
    p[2] = static_cast<uint8_t>(pid & 0xff);
    p[3] = static_cast<uint8_t>(0x10 | (cc & 0x0f)); // payload only + continuity counter
    p[4] = 0x00;
    return p;
}

int main()
{
    DemuxerTS d;
    DemuxerTS::Config cfg;
    assert(d.start(cfg));

    auto p0 = makeTsPacket(256, 0, true);
    auto p1 = makeTsPacket(256, 1, false);
    d.pushData(p0.data(), p0.size());
    d.pushData(p1.data(), p1.size());
    DemuxerTS::HealthSnapshot h = d.healthSnapshot();
    assert(h.transport_packets == 2);
    assert(h.continuity_errors == 0);

    auto p3 = makeTsPacket(256, 3, false); // missing cc=2
    d.pushData(p3.data(), p3.size());
    h = d.healthSnapshot();
    assert(h.continuity_errors == 1);
    assert(h.discontinuities == 1);
    assert(h.discontinuity_detected);

    std::vector<uint8_t> bad(188, 0x00);
    d.pushData(bad.data(), bad.size());
    h = d.healthSnapshot();
    assert(h.invalid_sync >= 1);

    d.stop();
    std::cout << "demuxer TS health test passed\n";
    return 0;
}
