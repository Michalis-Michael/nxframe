// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/checked_json_int.h"
#include "config/preset_validator.h"
#include <iostream>
#include <limits>

using nlohmann::json;
int main()
{
    int failures = 0;
    auto expect = [&](bool ok, const std::string& label) {
        if (!ok) { std::cerr << label << "\n"; ++failures; }
    };
    const json base = {{"codec", "x264"}, {"width", 1920}, {"height", 1080},
                       {"framerate", 25}, {"bitrate", 25000000}};
    auto valid = [&](const json& ts) {
        auto preset = base; preset["mpegts"] = ts;
        return PresetValidator::validateJson(preset, PresetValidator::Kind::Sender).ok();
    };
    expect(valid(json::object()), "omitted PID fields must retain defaults");
    expect(valid({{"timing_diagnostics", false}}), "disabled timing diagnostics rejected");
    expect(valid({{"timing_diagnostics", true}}), "enabled timing diagnostics rejected");
    for (const auto& value : std::vector<json>{1, "true", nullptr})
        expect(!valid({{"timing_diagnostics", value}}), "non-boolean timing diagnostics accepted");
    for (const char* alias : {"null_stuffing", "null_packets", "true_cbr"}) {
        expect(valid({{alias, true}, {"muxrate", 30000000}}), std::string(alias) + " true rejected");
        expect(valid({{alias, false}}), std::string(alias) + " false rejected");
        for (const auto& bad : std::vector<json>{1, "true", nullptr})
            expect(!valid({{alias, bad}}), std::string(alias) + " accepted non-boolean");
    }
    expect(valid({{"null_stuffing", true}, {"true_cbr", true}}),
           "matching CBR aliases rejected");
    expect(!valid({{"null_stuffing", true}, {"true_cbr", false}}),
           "conflicting CBR aliases accepted");
    expect(!valid({{"null_stuffing", true}, {"muxrate", 20000000}}),
           "CBR muxrate lower than video bitrate accepted");
    expect(valid({{"null_stuffing", true}, {"muxrate", 30000000}}),
           "CBR muxrate above video bitrate rejected");
    expect(valid({{"transmission_profile", "low_latency"}}), "low-latency profile rejected");
    expect(valid({{"transmission_profile", "broadcast"}}), "broadcast profile rejected");
    expect(!valid({{"transmission_profile", "bad"}}), "invalid transmission profile accepted");
    expect(valid({{"service_id", 1}, {"video_pid", 512}, {"pcr_pid", 512},
                  {"pmt_pid", 2048}, {"audio_pids", {4112, 4128, 4144, 4160}},
                  {"pcr_interval_ms", 30}}), "requested profile rejected");
    for (const auto& field : {std::string("service_id"), std::string("pcr_interval_ms"),
                              std::string("video_pid"), std::string("pmt_pid")}) {
        const int minimum = field == "service_id" || field == "pcr_interval_ms" ? 1 : 32;
        const int maximum = field == "service_id" ? 65535 : field == "pcr_interval_ms" ? 40 : 8190;
        for (const int boundary : {minimum, maximum}) {
            expect(valid({{field, boundary}}), field + " valid boundary rejected");
            int result = -99;
            expect(nxframe::checkedJsonInt(json(static_cast<uint64_t>(boundary)), minimum, maximum, result)
                   && result == boundary, field + " unsigned boundary rejected");
        }
        const std::vector<json> bad = {minimum - 1, maximum + 1, -1, 1.5, "512", true, nullptr,
            4294967297LL, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(),
            std::numeric_limits<uint64_t>::max()};
        for (const auto& value : bad) {
            const json ts = {{field, value}};
            expect(!valid(ts), field + " accepted " + value.dump());
            bool rejected = false;
            try { nxframe::optionalJsonInt(ts, field.c_str(), 123, minimum, maximum, "mpegts"); }
            catch (const std::invalid_argument&) { rejected = true; }
            expect(rejected, field + " loader conversion accepted " + value.dump());
        }
    }
    expect(nxframe::optionalJsonInt(json::object(), "pcr_interval_ms", 30, 1, 40, "mpegts") == 30,
           "omitted interval lost 30ms default");
    expect(valid({{"video_pid", 32}, {"pcr_pid", 32}, {"audio_pids", {8190}}}), "PID boundaries rejected");
    for (const auto& value : std::vector<json>{31, 8191, -1, 1.5, "4112", true, nullptr,
                                             4294971408LL, std::numeric_limits<uint64_t>::max()}) {
        expect(!valid({{"audio_pids", json::array({value})}}), "invalid audio PID accepted");
    }
    expect(!valid({{"audio_pids", {4112, 4112}}}), "duplicate audio PIDs accepted");
    expect(!valid({{"audio_pids", {256}}}), "default video collision accepted");
    expect(!valid({{"audio_pids", {4096}}}), "default PMT collision accepted");
    expect(!valid({{"video_pid", 512}, {"audio_pids", {512}}}), "custom video collision accepted");
    expect(!valid({{"pmt_pid", 2048}, {"audio_pids", {2048}}}), "custom PMT collision accepted");
    expect(!valid({{"video_pid", 2048}, {"pmt_pid", 2048}}), "video/PMT collision accepted");
    expect(!valid({{"video_pid", 512}, {"pcr_pid", 256}}), "PCR/video mismatch accepted");
    expect(!valid({{"audio_pids", "4112"}}), "non-array audio PIDs accepted");
    expect(!valid({{"audio_pids", {32, 33, 34, 35, 36, 37, 38, 39, 40}}}), "nine audio PIDs accepted");
    auto nested = base;
    nested["output"]["mpegts"] = {{"service_id", 4294967297LL}};
    expect(!PresetValidator::validateJson(nested, PresetValidator::Kind::Sender).ok(),
           "nested MPEG-TS overflow accepted");
    if (!failures) std::cout << "MPEG-TS PID validation regression checks passed\n";
    return failures ? 1 : 0;
}
