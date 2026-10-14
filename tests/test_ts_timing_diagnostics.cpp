// SPDX-License-Identifier: GPL-3.0-or-later
#include "output/ts_timing_diagnostics.h"
#include <array>
#include <iostream>
#include <sstream>
#include <string>

using Packet = std::array<uint8_t, 188>;
static void stamp(uint8_t* p, uint64_t value, unsigned prefix)
{
    p[0] = (prefix << 4) | ((value >> 29) & 14) | 1;
    p[1] = value >> 22; p[2] = ((value >> 14) & 0xfe) | 1;
    p[3] = value >> 7; p[4] = ((value << 1) & 0xfe) | 1;
}
static Packet packet(uint64_t ticks, uint64_t pts, uint64_t dts)
{
    Packet p; p.fill(0xff);
    p[0] = 0x47; p[1] = 0x42; p[2] = 0; p[3] = 0x30; p[4] = 7; p[5] = 0x10;
    const auto base = ticks / 300; const auto ext = ticks % 300;
    p[6] = base >> 25; p[7] = base >> 17; p[8] = base >> 9; p[9] = base >> 1;
    p[10] = ((base & 1) << 7) | 0x7e | (ext >> 8); p[11] = ext;
    auto* q = p.data() + 12;
    q[0] = q[1] = 0; q[2] = 1; q[3] = 0xe0; q[4] = q[5] = 0;
    q[6] = 0x80; q[7] = 0xc0; q[8] = 10;
    stamp(q + 9, pts, 3); stamp(q + 14, dts, 1);
    return p;
}
static std::string report(TsTimingDiagnostics& d)
{
    std::ostringstream out; d.report(out); return out.str();
}
int main()
{
    int failures = 0;
    auto expect = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << message << '\n'; }
    };
    auto field = [&](const std::string& text, const char* value) {
        expect(text.find(value) != std::string::npos, value);
    };
    TsTimingDiagnostics::Sample s{1, 1000000000, 1000500000, 1000501000, 9000, 5, 2, 3};
    auto first = packet(0, 0, 0); auto next = packet(1080000, 3600, 3600);
    const auto unchanged = first;
    TsTimingDiagnostics normal(512, 20000000);
    normal.observe(first.data(), first.size(), s, true);
    s.scheduledNs += 40000000; s.beginNs += 40000000; s.endNs += 40000000;
    normal.observe(next.data(), next.size(), s, true);
    expect(first == unchanged, "diagnostics changed packet bytes");
    auto text = report(normal);
    field(text, "pcr_samples=2"); field(text, "video_pes=2");
    field(text, "pcr_value_gap_ns_min=40000000"); field(text, "pcr_call_gap_ns_min=40000000");
    field(text, "pcr_vs_packet_clock_ns_max=39924800");
    field(text, "schedule_lateness_ns_min=500000"); field(text, "call_duration_ns_max=1000");
    field(text, "pes_pts_90k=3600"); field(text, "pes_dts_90k=3600");
    field(text, "mux_input_dts_90k=9000"); field(text, "ts_queue_max=5");
    field(text, "send_begin_gap_ns_min=40000000");
    field(text, "send_spacing_error_ns_min=0");
    field(text, "pcr_schedule_gap_ns_min=40000000");
    field(text, "pcr_call_vs_value_gap_error_ns_min=0");
    field(text, "estimated_ts_queue_delay_ns_count=2");
    field(text, "ts_queue_avg=5");
    field(text, "ts_queue_min=5");
    field(text, "ts_queue_flat_samples=1");
    field(text, "ts_queue_growing_samples=0");
    normal.recovery(true, true, 1800000);
    normal.recovery(false, true, 2500000);
    text = report(normal);
    field(text, "cbr_recovery=1"); field(text, "vbr_recovery=1");
    field(text, "resyncs=2"); field(text, "recovery_lateness_ns_max=2500000");
    field(text, "recovery_delay_total_ns=4300000");
    s.beginNs += 40000000; s.endNs += 40000000;
    auto third = packet(2160000, 7200, 7200);
    normal.observe(third.data(), third.size(), s, true);
    text = report(normal);
    field(text, "pcr_samples=1"); field(text, "pcr_value_gap_ns_min=40000000");
    field(text, "pcr_vs_packet_clock_ns_max=79849600"); // anchor survives a report

    TsTimingDiagnostics stall(512, 20000000);
    stall.observe(first.data(), first.size(), s, true);
    Packet null; null.fill(0xff); null[0] = 0x47; null[1] = 0x1f; null[2] = 0xff; null[3] = 0x10;
    auto stalled = s; stalled.beginNs += 2000000000; stalled.endNs += 2000000000;
    stall.observe(null.data(), null.size(), stalled, true);
    stall.observe(next.data(), next.size(), stalled, false);
    stall.observe(next.data(), next.size(), stalled, true);
    text = report(stall);
    field(text, "failed=1"); field(text, "accepted_bytes=564"); field(text, "pcr_samples=2");
    field(text, "pcr_call_gap_ns_max=2000000000"); field(text, "pcr_value_gap_ns_max=40000000");
    field(text, "pcr_vs_packet_clock_ns_max=39849600");

    constexpr uint64_t wrap = (uint64_t{1} << 33) * 300;
    TsTimingDiagnostics wrapping(512, 20000000);
    auto beforeWrap = packet(wrap - 540000, 0, 0); auto afterWrap = packet(540000, 0, 0);
    wrapping.observe(beforeWrap.data(), 188, s, true);
    wrapping.observe(afterWrap.data(), 188, stalled, true);
    text = report(wrapping);
    field(text, "pcr_value_gap_ns_max=40000000"); field(text, "clock_events=1");
    auto backward = packet(0, 0, 0);
    wrapping.observe(backward.data(), 188, s, true);
    text = report(wrapping);
    field(text, "clock_events=1"); field(text, "pcr_value_gap_ns_count=0");
    field(text, "pcr_vs_packet_clock_ns_max=0");

    TsTimingDiagnostics resets(512, 20000000);
    resets.observe(first.data(), 188, s, true);
    auto disc = next; disc[5] |= 0x80;
    resets.observe(disc.data(), 188, s, true);
    auto newSession = s; newSession.session = 2;
    resets.observe(first.data(), 188, newSession, true);
    text = report(resets);
    field(text, "session=2"); field(text, "clock_events=3");
    field(text, "pcr_value_gap_ns_count=0"); field(text, "pcr_vs_packet_clock_ns_max=0");

    TsTimingDiagnostics malformed(512, 0);
    auto bad = first; bad[4] = 184;
    malformed.observe(bad.data(), 188, s, true);
    malformed.observe(first.data(), 187, s, true);
    bad = first; bad[1] |= 0x80; malformed.observe(bad.data(), 188, s, true);
    bad = first; bad[10] |= 1; bad[11] = 255; malformed.observe(bad.data(), 188, s, true);
    bad = first; bad[4] = 177; bad[5] = 0; malformed.observe(bad.data(), 188, s, true);
    text = report(malformed);
    field(text, "invalid=3"); field(text, "unaligned=1"); field(text, "pcr_samples=0");

    TsTimingDiagnostics vbr(512, 0);
    auto unpaced = s; unpaced.scheduledNs = -1;
    vbr.observe(first.data(), 188, unpaced, true);
    text = report(vbr);
    field(text, "schedule_lateness_ns_count=0"); field(text, "pcr_vs_packet_clock_ns_count=0");

    // Sustained 20 Mbps payload schedule. PCRs occur at known TS byte positions;
    // their 27 MHz values match that clock exactly (400 slots = 30.08 ms).
    TsTimingDiagnostics load(512, 20000000);
    std::array<uint8_t, 1316> payload;
    auto paced = s; paced.beginNs = paced.scheduledNs = 1000000000; paced.endNs = paced.beginNs;
    for (uint64_t call = 0; call < 10000; ++call) {
        for (uint64_t j = 0; j < 7; ++j) {
            const uint64_t index = call * 7 + j;
            const auto p = index % 400 == 0
                ? packet((index / 400) * 812160, index * 7, index * 7) : null;
            std::copy(p.begin(), p.end(), payload.begin() + j * 188);
        }
        const auto original = payload;
        load.observe(payload.data(), payload.size(), paced, true);
        expect(original == payload, "sustained load changed TS bytes");
        paced.beginNs += 526400; paced.endNs += 526400; paced.scheduledNs += 526400;
    }
    text = report(load);
    field(text, "calls=10000"); field(text, "accepted_bytes=13160000"); field(text, "pcr_samples=175");
    field(text, "pcr_value_gap_ns_min=30080000"); field(text, "pcr_vs_packet_clock_ns_max=0");
    field(text, "schedule_lateness_ns_max=0"); field(text, "invalid=0");

    TsTimingDiagnostics adaptation(512, 20000000);
    auto only = first; only[1] = 2; only[3] = 0x2f; only[4] = 183;
    adaptation.observe(only.data(), 188, s, true);
    text = report(adaptation);
    field(text, "pcr_samples=1"); field(text, "video_pes=0");
    if (!failures) std::cout << "TS timing diagnostics tests passed\n";
    return failures ? 1 : 0;
}
