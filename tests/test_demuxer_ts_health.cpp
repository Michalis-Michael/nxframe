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

    p[1] = static_cast<uint8_t>(
        ((payloadStart ? 0x40 : 0x00) |
         ((pid >> 8) & 0x1f)) &
        0xff);

    p[2] = static_cast<uint8_t>(pid & 0xff);

    // payload only + continuity counter
    p[3] = static_cast<uint8_t>(0x10 | (cc & 0x0f));

    p[4] = 0x00;

    return p;
}

int main()
{
    DemuxerTS d;
    DemuxerTS::Config cfg;

    assert(d.start(cfg));

    // ------------------------------------------------------------
    // Test 1:
    // Normal continuity across a PUSI boundary must NOT generate
    // a continuity error.
    //
    // cc=0 PUSI
    // cc=1
    // cc=2 PUSI
    //
    // Expected: zero errors.
    // ------------------------------------------------------------

    auto p0 = makeTsPacket(256, 0, true);
    auto p1 = makeTsPacket(256, 1, false);
    auto p2 = makeTsPacket(256, 2, true);

    d.pushData(p0.data(), p0.size());
    d.pushData(p1.data(), p1.size());
    d.pushData(p2.data(), p2.size());

    DemuxerTS::HealthSnapshot h = d.healthSnapshot();

    assert(h.transport_packets == 3);
    assert(h.continuity_errors == 0);
    assert(h.discontinuities == 0);

    std::cout
        << "PASS: continuous CC across PUSI produced no false error\n";

    // ------------------------------------------------------------
    // Test 2:
    // Missing CC immediately before a PUSI packet MUST still be
    // detected.
    //
    // Last known CC = 2
    // Expected next = 3
    // Receive cc=4 with PUSI -> cc=3 was lost.
    //
    // Old broken behavior would effectively hide this because PUSI
    // reset continuity state.
    // Correct behavior must count the error.
    // ------------------------------------------------------------

    auto p4_pusi = makeTsPacket(256, 4, true);

    d.pushData(p4_pusi.data(), p4_pusi.size());

    h = d.healthSnapshot();

    assert(h.transport_packets == 4);
    assert(h.continuity_errors == 1);
    assert(h.discontinuities == 1);
    assert(h.discontinuity_detected);

    std::cout
        << "PASS: CC gap on PUSI packet was detected\n";

    // ------------------------------------------------------------
    // Test 3:
    // Verify tracking continues normally after the detected gap.
    // ------------------------------------------------------------

    auto p5 = makeTsPacket(256, 5, false);

    d.pushData(p5.data(), p5.size());

    h = d.healthSnapshot();

    assert(h.transport_packets == 5);
    assert(h.continuity_errors == 1);
    assert(h.discontinuities == 1);

    std::cout
        << "PASS: continuity tracking recovered after PUSI gap\n";

    // ------------------------------------------------------------
    // Test 4:
    // Different PID starts its own continuity history.
    // ------------------------------------------------------------

    auto otherPid = makeTsPacket(300, 11, true);

    d.pushData(otherPid.data(), otherPid.size());

    h = d.healthSnapshot();

    assert(h.transport_packets == 6);
    assert(h.continuity_errors == 1);

    std::cout
        << "PASS: first packet on new PID did not cause false error\n";

    // ------------------------------------------------------------
    // Existing invalid-sync sanity check.
    // ------------------------------------------------------------

    std::vector<uint8_t> bad(188, 0x00);

    d.pushData(bad.data(), bad.size());

    h = d.healthSnapshot();

    assert(h.invalid_sync >= 1);

    std::cout
        << "PASS: invalid TS sync detected\n";

    d.stop();

    std::cout
        << "demuxer TS PUSI continuity test passed\n";

    return 0;
}