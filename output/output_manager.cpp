/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * License / EULA notice:
 * This file is part of NxFrame. Use, redistribution, and modification are
 * governed by the project license and any written EULA or commercial license
 * agreement supplied with the project. If no separate written agreement is
 * supplied, the GPL-3.0-or-later terms apply.
 *
 * Description:
 * Output manager implementation. This module connects encoder packet queues to the muxer and selected transport, loads output-related preset settings, and manages sender and receiver SDI playout lifecycles.
 */

#include "output/output_manager.h"

#include "stage_timing.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {
int jsonIntOr(const json& obj, const char* key, int fallback)
{
    if (obj.contains(key) && obj[key].is_number_integer()) {
        return obj[key].get<int>();
    }
    return fallback;
}

int64_t jsonInt64Or(const json& obj, const char* key, int64_t fallback)
{
    if (obj.contains(key) && obj[key].is_number_integer()) {
        return obj[key].get<int64_t>();
    }
    return fallback;
}

std::string normalizedBitrateString(std::string value)
{
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) { return std::isspace(c); }),
                value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool parseBitrateString(const std::string& text, int64_t& out)
{
    std::string value = normalizedBitrateString(text);
    if (value.empty()) {
        return false;
    }

    long double multiplier = 1.0L;
    const auto remove_suffix = [&](const std::string& suffix) -> bool {
        if (value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
            value.resize(value.size() - suffix.size());
            return true;
        }
        return false;
    };

    if (remove_suffix("mbps") || remove_suffix("mbit") || remove_suffix("mb/s")) {
        multiplier = 1000000.0L;
    } else if (remove_suffix("kbps") || remove_suffix("kbit") || remove_suffix("kb/s")) {
        multiplier = 1000.0L;
    } else if (remove_suffix("gbps") || remove_suffix("gbit") || remove_suffix("gb/s")) {
        multiplier = 1000000000.0L;
    } else if (remove_suffix("m")) {
        multiplier = 1000000.0L;
    } else if (remove_suffix("k")) {
        multiplier = 1000.0L;
    } else if (remove_suffix("g")) {
        multiplier = 1000000000.0L;
    } else {
        remove_suffix("bps");
    }

    if (value.empty()) {
        return false;
    }

    try {
        size_t pos = 0;
        const long double numeric = std::stold(value, &pos);
        if (pos != value.size() || numeric < 0.0L) {
            return false;
        }
        out = static_cast<int64_t>(numeric * multiplier + 0.5L);
        return true;
    } catch (...) {
        return false;
    }
}

int64_t jsonBitrateOrAny(const json& obj, std::initializer_list<const char*> keys, int64_t fallback)
{
    for (const char* key : keys) {
        if (!obj.contains(key)) {
            continue;
        }
        const json& v = obj[key];
        if (v.is_number_integer()) {
            return v.get<int64_t>();
        }
        if (v.is_number_float()) {
            const double d = v.get<double>();
            return d > 0.0 ? static_cast<int64_t>(d + 0.5) : 0;
        }
        if (v.is_string()) {
            int64_t parsed = fallback;
            if (parseBitrateString(v.get<std::string>(), parsed)) {
                return parsed;
            }
        }
    }
    return fallback;
}

bool jsonBoolOr(const json& obj, const char* key, bool fallback)
{
    if (obj.contains(key) && obj[key].is_boolean()) {
        return obj[key].get<bool>();
    }
    return fallback;
}

std::string jsonStringOr(const json& obj, const char* key, const std::string& fallback)
{
    if (obj.contains(key) && obj[key].is_string()) {
        return obj[key].get<std::string>();
    }
    return fallback;
}

int jsonIntOrAny(const json& obj, std::initializer_list<const char*> keys, int fallback)
{
    for (const char* key : keys) {
        if (obj.contains(key) && obj[key].is_number_integer()) {
            return obj[key].get<int>();
        }
    }
    return fallback;
}



int64_t safeBitrateOrZero(const AVCodecContext* ctx)
{
    if (!ctx) {
        return 0;
    }

    int64_t bitrate = 0;
    if (ctx->rc_max_rate > 0) {
        bitrate = std::max<int64_t>(bitrate, ctx->rc_max_rate);
    }
    if (ctx->bit_rate > 0) {
        bitrate = std::max<int64_t>(bitrate, ctx->bit_rate);
    }
    return bitrate;
}

int64_t estimateUdpPacingBitrateBps(AVCodecContext* videoCtx,
                                    const std::vector<AVCodecContext*>& audioCtxs)
{
    int64_t media_bps = safeBitrateOrZero(videoCtx);
    for (size_t i = 0; i < audioCtxs.size(); ++i) {
        media_bps += safeBitrateOrZero(audioCtxs[i]);
    }

    // If a preset does not populate codec bitrates, keep a conservative live
    // default. Otherwise add MPEG-TS overhead and headroom so UDP is smoothed
    // without throttling the intended stream.
    if (media_bps <= 0) {
        return 50000000LL;
    }

    // UDP pacing should smooth the transport without becoming the bottleneck.
    // H.264/TS output can arrive in short mux bursts, especially around IDR
    // frames. Keep extra headroom so the output thread does not unnecessarily
    // fill encoded-packet queues while still avoiding wire-rate bursts.
    const long double with_overhead = static_cast<long double>(media_bps) * 1.45L;
    const int64_t estimated = static_cast<int64_t>(with_overhead);
    return std::max<int64_t>(estimated, media_bps + 10000000LL);
}

bool fileExistsLocal(const std::string& path)
{
    if (path.empty()) return false;
    std::ifstream f(path);
    return f.good();
}

std::string resolvePresetPathLocal(const std::string& presetPath)
{
    if (presetPath.empty()) return presetPath;
    const std::string withJson = (presetPath.size() >= 5 && presetPath.substr(presetPath.size()-5) == ".json")
        ? presetPath
        : presetPath + ".json";
    const std::string candidates[] = {
        presetPath,
        withJson,
        std::string("../preset/") + presetPath,
        std::string("../preset/") + withJson,
        std::string("preset/") + presetPath,
        std::string("preset/") + withJson,
    };
    for (const auto& c : candidates) {
        if (fileExistsLocal(c)) return c;
    }
    return presetPath;
}

} // namespace

OutputManager::~OutputManager()
{
    shutdownDeckLinkPlayout();
    shutdownSender();
}

MpegTsMetadataConfig OutputManager::loadMpegTsMetadataConfig(const std::string& presetPath) const
{
    MpegTsMetadataConfig cfg;

    if (presetPath.empty()) {
        return cfg;
    }

    const std::string resolvedPresetPath = resolvePresetPathLocal(presetPath);
    std::ifstream file(resolvedPresetPath);
    if (!file) {
        std::cerr << "[OutputManager] WARNING: Failed to open preset for MPEG-TS metadata: "
                  << resolvedPresetPath << "\n";
        return cfg;
    }

    json presetJson;
    try {
        file >> presetJson;
    } catch (const json::parse_error& e) {
        std::cerr << "[OutputManager] WARNING: Failed to parse preset JSON for MPEG-TS metadata: "
                  << e.what() << "\n";
        return cfg;
    }

    const json* section = nullptr;
    if (presetJson.contains("mpegts") && presetJson["mpegts"].is_object()) {
        section = &presetJson["mpegts"];
    } else if (presetJson.contains("output") && presetJson["output"].is_object() &&
               presetJson["output"].contains("mpegts") && presetJson["output"]["mpegts"].is_object()) {
        section = &presetJson["output"]["mpegts"];
    }

    if (section) {
        if (section->contains("service_provider") && (*section)["service_provider"].is_string()) {
            const std::string v = (*section)["service_provider"].get<std::string>();
            if (!v.empty()) {
                cfg.serviceProvider = v;
            }
        }
        if (section->contains("service_name") && (*section)["service_name"].is_string()) {
            const std::string v = (*section)["service_name"].get<std::string>();
            if (!v.empty()) {
                cfg.serviceName = v;
            }
        }
        cfg.muxrateBps = jsonBitrateOrAny(*section,
                                          {"muxrate", "mux_rate", "ts", "ts_bitrate", "transport_rate", "ts_rate"},
                                          cfg.muxrateBps);
        cfg.nullStuffing = jsonBoolOr(*section, "null_stuffing", cfg.nullStuffing);
        cfg.nullStuffing = jsonBoolOr(*section, "null_packets", cfg.nullStuffing);
        cfg.nullStuffing = jsonBoolOr(*section, "true_cbr", cfg.nullStuffing);
        if (cfg.muxrateBps > 0 && cfg.muxrateBps < 1000000) {
            std::cerr << "[OutputManager] WARNING: MPEG-TS muxrate=" << cfg.muxrateBps
                      << "bps looks very low. Did you mean 56000000 for 56 Mbps?\n";
        }
    }

    return cfg;
}

SrtRuntimeConfig OutputManager::loadSrtRuntimeConfig(const std::string& presetPath,
                                                     const std::string& cliAddress,
                                                     int cliPort) const
{
    SrtRuntimeConfig cfg;
    cfg.streamer.address = cliAddress;
    cfg.streamer.port = cliPort;

    if (presetPath.empty()) {
        return cfg;
    }

    const std::string resolvedPresetPath = resolvePresetPathLocal(presetPath);
    std::ifstream file(resolvedPresetPath);
    if (!file) {
        std::cerr << "[OutputManager] WARNING: Failed to open preset for SRT config: "
                  << resolvedPresetPath << "\n";
        return cfg;
    }

    json presetJson;
    try {
        file >> presetJson;
    } catch (const json::parse_error& e) {
        std::cerr << "[OutputManager] WARNING: Failed to parse preset JSON for SRT config: "
                  << e.what() << "\n";
        return cfg;
    }

    const json* section = nullptr;
    if (presetJson.contains("srt") && presetJson["srt"].is_object()) {
        section = &presetJson["srt"];
    } else if (presetJson.contains("transport") && presetJson["transport"].is_object()) {
        section = &presetJson["transport"];
    } else if (presetJson.contains("output") && presetJson["output"].is_object() &&
               presetJson["output"].contains("srt") && presetJson["output"]["srt"].is_object()) {
        section = &presetJson["output"]["srt"];
    }

    if (!section) {
        return cfg;
    }

    const json& s = *section;

    cfg.streamer.mode = SRTStreamer::modeFromString(
        jsonStringOr(s, "mode", SRTStreamer::modeToString(cfg.streamer.mode)));
    cfg.streamer.bind_address = jsonStringOr(s, "bind_address", cfg.streamer.bind_address);
    cfg.streamer.listen_backlog = jsonIntOr(s, "listen_backlog", cfg.streamer.listen_backlog);
    cfg.streamer.latency = jsonIntOr(s, "latency", cfg.streamer.latency);
    cfg.streamer.peer_latency = jsonIntOr(s, "peer_latency", cfg.streamer.peer_latency);
    cfg.streamer.rcv_latency = jsonIntOr(s, "rcv_latency", cfg.streamer.rcv_latency);
    cfg.streamer.connect_timeout_ms = jsonIntOr(s, "connect_timeout_ms", cfg.streamer.connect_timeout_ms);
    cfg.streamer.send_timeout_ms = jsonIntOr(s, "send_timeout_ms", cfg.streamer.send_timeout_ms);
    cfg.streamer.recv_timeout_ms = jsonIntOr(s, "recv_timeout_ms", cfg.streamer.recv_timeout_ms);
    cfg.streamer.peer_idle_timeout_ms = jsonIntOr(s, "peer_idle_timeout_ms", cfg.streamer.peer_idle_timeout_ms);
    cfg.streamer.payload_size = jsonIntOr(s, "payload_size", cfg.streamer.payload_size);
    cfg.streamer.sndbuf = jsonIntOr(s, "sndbuf", cfg.streamer.sndbuf);
    cfg.streamer.rcvbuf = jsonIntOr(s, "rcvbuf", cfg.streamer.rcvbuf);
    cfg.streamer.oheadbw = jsonIntOr(s, "oheadbw", cfg.streamer.oheadbw);
    cfg.streamer.snddropdelay = jsonIntOr(s, "snddropdelay", cfg.streamer.snddropdelay);
    cfg.streamer.linger = jsonIntOr(s, "linger", cfg.streamer.linger);
    cfg.streamer.maxbw = jsonInt64Or(s, "maxbw", cfg.streamer.maxbw);
    cfg.streamer.inputbw = jsonInt64Or(s, "inputbw", cfg.streamer.inputbw);
    cfg.streamer.pacing_enabled = jsonBoolOr(s, "pacing_enabled", cfg.streamer.pacing_enabled);
    cfg.streamer.pacing_bitrate_bps = jsonBitrateOrAny(
        s, {"pacing_bitrate_bps", "pacing_bitrate", "send_pacing", "muxrate"},
        cfg.streamer.pacing_bitrate_bps);
    if (cfg.streamer.pacing_bitrate_bps > 0 && !s.contains("pacing_enabled")) {
        cfg.streamer.pacing_enabled = true;
    }
    cfg.streamer.sender = jsonBoolOr(s, "sender", cfg.streamer.sender);
    cfg.streamer.messageapi = jsonBoolOr(s, "messageapi", cfg.streamer.messageapi);
    cfg.streamer.tlpktdrop = jsonBoolOr(s, "tlpktdrop", cfg.streamer.tlpktdrop);
    cfg.streamer.nakreport = jsonBoolOr(s, "nakreport", cfg.streamer.nakreport);
    cfg.streamer.streamid = jsonStringOr(s, "streamid", cfg.streamer.streamid);
    cfg.streamer.passphrase = jsonStringOr(s, "passphrase", cfg.streamer.passphrase);
    cfg.streamer.pbkeylen = jsonIntOr(s, "pbkeylen", cfg.streamer.pbkeylen);
    cfg.streamer.stats_interval_ms = jsonIntOr(s, "stats_interval_ms", cfg.streamer.stats_interval_ms);
    cfg.streamer.reconnect_backoff_ms = jsonIntOrAny(
        s, {"reconnect_backoff_ms", "reconnect_delay_ms"}, cfg.streamer.reconnect_backoff_ms);
    cfg.streamer.reconnect_backoff_max_ms = jsonIntOr(
        s, "reconnect_backoff_max_ms", cfg.streamer.reconnect_backoff_max_ms);

    cfg.initAttempts = jsonIntOr(s, "init_attempts", cfg.initAttempts);
    cfg.reconnectAttempts = jsonIntOr(s, "reconnect_attempts", cfg.reconnectAttempts);
    cfg.streamer.reconnect_forever = jsonBoolOr(s, "reconnect_forever", cfg.streamer.reconnect_forever);
    cfg.initRetryDelayMs = jsonIntOrAny(
        s, {"init_retry_delay_ms", "reconnect_backoff_ms", "reconnect_delay_ms"}, cfg.initRetryDelayMs);

    return cfg;
}

DeckLinkPlayoutConfig OutputManager::loadDeckLinkPlayoutConfig(const std::string& presetPath) const
{
    DeckLinkPlayoutConfig cfg;

    if (presetPath.empty()) {
        return cfg;
    }

    const std::string resolvedPresetPath = resolvePresetPathLocal(presetPath);
    std::ifstream file(resolvedPresetPath);
    if (!file) {
        std::cerr << "[OutputManager] WARNING: Failed to open preset for DeckLink playout config: "
                  << resolvedPresetPath << "\n";
        return cfg;
    }

    json presetJson;
    try {
        file >> presetJson;
    } catch (const json::parse_error& e) {
        std::cerr << "[OutputManager] WARNING: Failed to parse preset JSON for DeckLink playout config: "
                  << e.what() << "\n";
        return cfg;
    }

    const json* section = nullptr;
    if (presetJson.contains("decklink_playout") && presetJson["decklink_playout"].is_object()) {
        section = &presetJson["decklink_playout"];
    } else if (presetJson.contains("receiver") && presetJson["receiver"].is_object() &&
               presetJson["receiver"].contains("decklink_playout") &&
               presetJson["receiver"]["decklink_playout"].is_object()) {
        section = &presetJson["receiver"]["decklink_playout"];
    }

    if (!section) {
        return cfg;
    }

    cfg.videoPrerollFrames = static_cast<uint32_t>(std::max(1, jsonIntOrAny(*section, {"video_preroll_frames", "videoPrerollFrames"}, static_cast<int>(cfg.videoPrerollFrames))));
    cfg.maxVideoQueueFrames = static_cast<uint32_t>(std::max(1, jsonIntOrAny(*section, {"max_video_queue_frames", "maxVideoQueueFrames"}, static_cast<int>(cfg.maxVideoQueueFrames))));
    // The hardware queue must be allowed to grow beyond the startup preroll.
    // Otherwise a valid low-latency preset can start with enough frames, then
    // immediately drain to zero and trigger repeated live catch-up corrections.
    cfg.maxVideoQueueFrames = std::max<uint32_t>(cfg.maxVideoQueueFrames, cfg.videoPrerollFrames + 2);
    cfg.maxAudioQueueSamples = static_cast<uint32_t>(std::max(1, jsonIntOrAny(*section, {"max_audio_queue_samples", "maxAudioQueueSamples"}, static_cast<int>(cfg.maxAudioQueueSamples))));
    cfg.logStatusIntervalMs = std::max(100, jsonIntOrAny(*section, {"log_status_interval_ms", "logStatusIntervalMs"}, cfg.logStatusIntervalMs));
    cfg.sourceLossThresholdMs = std::max(1, jsonIntOrAny(*section, {"source_loss_ms", "sourceLossThresholdMs"}, cfg.sourceLossThresholdMs));
    cfg.blackFallbackThresholdMs = std::max(1, jsonIntOrAny(*section, {"black_fallback_ms", "blackFallbackThresholdMs"}, cfg.blackFallbackThresholdMs));
    cfg.startupAnchorTimeoutMs = std::max(1, jsonIntOrAny(*section, {"startup_anchor_timeout_ms", "startupAnchorTimeoutMs"}, cfg.startupAnchorTimeoutMs));

    return cfg;
}


UdpRuntimeConfig OutputManager::loadUdpRuntimeConfig(const std::string& presetPath,
                                                     const std::string& cliAddress,
                                                     int cliPort) const
{
    UdpRuntimeConfig cfg;
    cfg.streamer.address = cliAddress;
    cfg.streamer.port = cliPort;

    if (presetPath.empty()) {
        return cfg;
    }

    const std::string resolvedPresetPath = resolvePresetPathLocal(presetPath);
    std::ifstream file(resolvedPresetPath);
    if (!file) {
        std::cerr << "[OutputManager] WARNING: Failed to open preset for UDP config: "
                  << resolvedPresetPath << "\n";
        return cfg;
    }

    json presetJson;
    try {
        file >> presetJson;
    } catch (const json::parse_error& e) {
        std::cerr << "[OutputManager] WARNING: Failed to parse preset JSON for UDP config: "
                  << e.what() << "\n";
        return cfg;
    }

    const json* section = nullptr;
    if (presetJson.contains("udp") && presetJson["udp"].is_object()) {
        section = &presetJson["udp"];
    } else if (presetJson.contains("transport") && presetJson["transport"].is_object() &&
               presetJson["transport"].contains("udp") && presetJson["transport"]["udp"].is_object()) {
        section = &presetJson["transport"]["udp"];
    } else if (presetJson.contains("output") && presetJson["output"].is_object() &&
               presetJson["output"].contains("udp") && presetJson["output"]["udp"].is_object()) {
        section = &presetJson["output"]["udp"];
    }

    if (!section) {
        return cfg;
    }

    const json& u = *section;
    cfg.streamer.bind_address = jsonStringOr(u, "bind_address", cfg.streamer.bind_address);
    cfg.streamer.payload_size = jsonIntOr(u, "payload_size", cfg.streamer.payload_size);
    cfg.streamer.sndbuf = jsonIntOr(u, "sndbuf", cfg.streamer.sndbuf);
    cfg.streamer.ttl = jsonIntOrAny(u, {"ttl", "multicast_ttl"}, cfg.streamer.ttl);
    cfg.streamer.multicast_loop = jsonBoolOr(u, "multicast_loop", cfg.streamer.multicast_loop);
    cfg.streamer.pacing_enabled = jsonBoolOr(u, "pacing_enabled", cfg.streamer.pacing_enabled);
    cfg.streamer.pacing_bitrate_bps = jsonBitrateOrAny(
        u, {"pacing_bitrate_bps", "pacing_bitrate", "muxrate"},
        cfg.streamer.pacing_bitrate_bps);

    return cfg;
}

// Prepare the sender output chain. Encoder contexts are used only to describe
// the MPEG-TS streams; actual encoded packet flow begins later in the sender
// runtime thread.
bool OutputManager::initializeSender(const std::string& presetPath,
                                     SenderTransport transport,
                                     const std::string& cliAddress,
                                     int cliPort,
                                     EncoderManager& encoder,
                                     const SenderInitOptions& options)
{
    shutdownSender();

    mpegts_metadata_ = loadMpegTsMetadataConfig(presetPath);
    muxer_.setServiceMetadata(mpegts_metadata_.serviceProvider, mpegts_metadata_.serviceName);
    muxer_.setMuxrateBps(mpegts_metadata_.muxrateBps);
    muxer_.setNullStuffingEnabled(mpegts_metadata_.nullStuffing && mpegts_metadata_.muxrateBps > 0);
    std::cout << "[OutputManager] MPEG-TS service_provider: " << mpegts_metadata_.serviceProvider << "\n";
    std::cout << "[OutputManager] MPEG-TS service_name: " << mpegts_metadata_.serviceName << "\n";
    if (mpegts_metadata_.muxrateBps > 0) {
        std::cout << "[OutputManager] MPEG-TS muxrate: " << mpegts_metadata_.muxrateBps
                  << (muxer_.isNullStuffingEnabled()
                          ? " bps (NxFrame true-CBR null stuffing; FFmpeg muxrate disabled)\n"
                          : " bps (transport pacing only; FFmpeg muxrate disabled)\n");
    }

    if (options.tsDebug) {
        muxer_.enableTimestampDebug(true, 12);
    }
    if (!options.tsCapturePath.empty()) {
        muxer_.enableTsFileCapture(options.tsCapturePath);
    }

    muxer_.setVideoCodecContext(encoder.getVideoCodecContext());
    const std::vector<AVCodecContext*> audioCodecContexts = encoder.getAudioCodecContexts();
    if (!audioCodecContexts.empty()) {
        std::cout << "[OutputManager] Configured audio streams: " << audioCodecContexts.size() << "\n";
        muxer_.setAudioCodecContexts(audioCodecContexts);
    } else if (AVCodecContext* audioCtx = encoder.getAudioCodecContext()) {
        muxer_.setAudioCodecContext(audioCtx);
    } else {
        std::cerr << "[OutputManager] No audio codec context found. Audio disabled.\n";
    }

    if (!muxer_.initialize()) {
        std::cerr << "[OutputManager] Muxer initialization failed.\n";
        return false;
    }

    sender_transport_ = transport;
    if (sender_transport_ == SenderTransport::UDP || sender_transport_ == SenderTransport::RTP) {
        udp_runtime_ = loadUdpRuntimeConfig(presetPath, cliAddress, cliPort);
        udp_runtime_.streamer.rtp_packetize = (sender_transport_ == SenderTransport::RTP);
        if (udp_runtime_.streamer.rtp_packetize) {
            udp_runtime_.streamer.rtp_payload_type = 33;
        }
        if (muxer_.isNullStuffingEnabled()) {
            // True-CBR mode is paced by OutputManager/MuxerTS, so do not also
            // pace in the UDP/RTP transport adapter.
            udp_runtime_.streamer.pacing_enabled = false;
            udp_runtime_.streamer.pacing_bitrate_bps = 0;
        } else if (mpegts_metadata_.muxrateBps > 0 && udp_runtime_.streamer.pacing_bitrate_bps <= 0) {
            udp_runtime_.streamer.pacing_bitrate_bps = mpegts_metadata_.muxrateBps;
            udp_runtime_.streamer.pacing_enabled = true;
        }
        if (udp_runtime_.streamer.pacing_enabled && udp_runtime_.streamer.pacing_bitrate_bps <= 0) {
            udp_runtime_.streamer.pacing_bitrate_bps =
                estimateUdpPacingBitrateBps(encoder.getVideoCodecContext(), audioCodecContexts);
        }
        std::cout << "[OutputManager] "
                  << (sender_transport_ == SenderTransport::RTP ? "RTP endpoint: " : "UDP endpoint: ")
                  << udp_runtime_.streamer.address << ":" << udp_runtime_.streamer.port
                  << " payload=" << udp_runtime_.streamer.payload_size
                  << " ttl=" << udp_runtime_.streamer.ttl
                  << " pacing=" << (udp_runtime_.streamer.pacing_enabled ? udp_runtime_.streamer.pacing_bitrate_bps : 0) << "bps"
                  << "\n";
        if (!udp_runtime_.streamer.bind_address.empty()) {
            std::cout << "[OutputManager] UDP bind_address: " << udp_runtime_.streamer.bind_address << "\n";
        }
    } else {
        srt_runtime_ = loadSrtRuntimeConfig(presetPath, cliAddress, cliPort);
        if (mpegts_metadata_.muxrateBps > 0) {
            if (muxer_.isNullStuffingEnabled()) {
                // True-CBR scheduler performs the pacing. Keep SRT inputbw so
                // libsrt understands the real wire rate, but disable app pacing.
                srt_runtime_.streamer.pacing_enabled = false;
                srt_runtime_.streamer.pacing_bitrate_bps = 0;
            } else {
                if (srt_runtime_.streamer.pacing_bitrate_bps <= 0) {
                    srt_runtime_.streamer.pacing_bitrate_bps = mpegts_metadata_.muxrateBps;
                }
                srt_runtime_.streamer.pacing_enabled = true;
            }
            if (srt_runtime_.streamer.inputbw <= 0) {
                srt_runtime_.streamer.inputbw = mpegts_metadata_.muxrateBps;
            }
        }
        std::cout << "[OutputManager] SRT mode: " << SRTStreamer::modeToString(srt_runtime_.streamer.mode) << "\n";
        std::cout << "[OutputManager] SRT endpoint: "
                  << srt_runtime_.streamer.address << ":" << srt_runtime_.streamer.port
                  << " latency=" << srt_runtime_.streamer.latency
                  << " payload=" << srt_runtime_.streamer.payload_size
                  << " pacing=" << (srt_runtime_.streamer.pacing_enabled ? srt_runtime_.streamer.pacing_bitrate_bps : 0) << "bps"
                  << " inputbw=" << srt_runtime_.streamer.inputbw
                  << " reconnect_attempts=" << srt_runtime_.reconnectAttempts
                  << " reconnect_forever=" << (srt_runtime_.streamer.reconnect_forever ? "true" : "false")
                  << " reconnect_delay_ms=" << srt_runtime_.streamer.reconnect_backoff_ms
                  << "\n";
        if (!srt_runtime_.streamer.bind_address.empty()) {
            std::cout << "[OutputManager] SRT bind_address: " << srt_runtime_.streamer.bind_address << "\n";
        }
        if (!srt_runtime_.streamer.streamid.empty()) {
            std::cout << "[OutputManager] SRT streamid: " << srt_runtime_.streamer.streamid << "\n";
        }
        if (!srt_runtime_.streamer.passphrase.empty()) {
            std::cout << "[OutputManager] SRT encryption: ENABLED (pbkeylen="
                      << srt_runtime_.streamer.pbkeylen << ")\n";
        }

        srt_streamer_.setExternalStopFlag(options.externalStopFlag);
    }

    // Do not open/connect the live transport here. In listener-style transports,
    // the initial wait/retry can block until a receiver connects. A production
    // sender must start DeckLink capture and encoding immediately, even with no
    // receiver online. The runtime sender thread owns transport setup and keeps
    // packet output gated until a fresh video keyframe is available.
    sender_initialized_ = true;
    return true;
}

// Start the live sender thread that drains encoded packet queues, feeds the
// muxer, and releases MPEG-TS chunks to the selected transport.
bool OutputManager::startSenderRuntime(BoundedQueue<EncodedPacket>& videoPktQ,
                                       BoundedQueue<EncodedPacket>& audioPktQ,
                                       EncoderManager& encoder,
                                       PipelineTelemetry& telemetry,
                                       StopToken& stop,
                                       std::atomic<bool>& videoThreadDone,
                                       std::atomic<bool>& audioThreadDone,
                                       std::atomic<bool>& transportRecovering,
                                       std::atomic<bool>& waitForFreshKeyframe)
{
    if (!sender_initialized_) {
        std::cerr << "[OutputManager] Sender runtime requested before sender initialization.\n";
        return false;
    }

    stopSenderRuntime();

    // Start every live TS/SRT session on a clean video access point.
    // This prevents audio-only or mid-GOP TS data from reaching late joiners
    // before SPS/PPS/IDR has been emitted, which otherwise causes receiver
    // startup errors such as "non-existing PPS" or unspecified video size.
    waitForFreshKeyframe.store(true, std::memory_order_release);
    encoder.requestVideoKeyFrame();

    sender_stop_token_ = &stop;
    sender_thread_ = std::thread(&OutputManager::runSenderLoop,
                                 this,
                                 std::ref(videoPktQ),
                                 std::ref(audioPktQ),
                                 std::ref(encoder),
                                 std::ref(telemetry),
                                 std::ref(stop),
                                 std::ref(videoThreadDone),
                                 std::ref(audioThreadDone),
                                 std::ref(transportRecovering),
                                 std::ref(waitForFreshKeyframe));
    return true;
}

// Sender hot loop. Keep this focused on packet movement and recovery logic:
// encoded packet queues -> muxer -> output chunks -> SRT/UDP/RTP.
void OutputManager::runSenderLoop(BoundedQueue<EncodedPacket>& videoPktQ,
                                  BoundedQueue<EncodedPacket>& audioPktQ,
                                  EncoderManager& encoder,
                                  PipelineTelemetry& telemetry,
                                  StopToken& stop,
                                  std::atomic<bool>& videoThreadDone,
                                  std::atomic<bool>& audioThreadDone,
                                  std::atomic<bool>& transportRecovering,
                                  std::atomic<bool>& waitForFreshKeyframe)
{
    using clock = std::chrono::steady_clock;

    static stage_timing::StageStats& loopStat = stage_timing::get("mux_loop");
    static stage_timing::StageStats& videoWriteStat = stage_timing::get("mux_video_write");
    static stage_timing::StageStats& audioWriteStat = stage_timing::get("mux_audio_write");
    static stage_timing::StageStats& flushStat = stage_timing::get("mux_flush");
    static stage_timing::StageStats& sendStat = stage_timing::get("srt_send");
    static stage_timing::StageStats& reconnectStat = stage_timing::get("srt_reconnect");
    static stage_timing::StageStats& idleSleepStat = stage_timing::get("mux_idle_sleep");

    auto drainEncodedQueues = [&]() {
        EncodedPacket stale;
        uint64_t droppedVideo = 0;
        uint64_t droppedAudio = 0;
        while (videoPktQ.try_pop(stale)) { ++droppedVideo; }
        while (audioPktQ.try_pop(stale)) { ++droppedAudio; }
        if (droppedVideo > 0) {
            telemetry.dropVideoWhileRecovering.fetch_add(droppedVideo, std::memory_order_relaxed);
        }
        if (droppedAudio > 0) {
            telemetry.dropAudioWhileRecovering.fetch_add(droppedAudio, std::memory_order_relaxed);
        }
    };

    uint64_t lastMuxVideoDtsRepairs = muxer_.getVideoDtsRepairCount();
    uint64_t lastMuxVideoPtsRepairs = muxer_.getVideoPtsRepairCount();
    auto syncMuxRepairTelemetry = [&]() {
        const uint64_t curDts = muxer_.getVideoDtsRepairCount();
        const uint64_t curPts = muxer_.getVideoPtsRepairCount();
        if (curDts > lastMuxVideoDtsRepairs) {
            telemetry.muxVideoDtsRepairs.fetch_add(curDts - lastMuxVideoDtsRepairs, std::memory_order_relaxed);
        }
        if (curPts > lastMuxVideoPtsRepairs) {
            telemetry.muxVideoPtsRepairs.fetch_add(curPts - lastMuxVideoPtsRepairs, std::memory_order_relaxed);
        }
        lastMuxVideoDtsRepairs = curDts;
        lastMuxVideoPtsRepairs = curPts;
    };

    auto waitForInitialTransport = [&]() -> bool {
        transportRecovering.store(true, std::memory_order_release);
        waitForFreshKeyframe.store(true, std::memory_order_release);
        drainEncodedQueues();

        if (sender_transport_ == SenderTransport::UDP || sender_transport_ == SenderTransport::RTP) {
            std::cout << "[OutputManager] Sender pipeline is live. Opening "
                      << (sender_transport_ == SenderTransport::RTP ? "RTP MPEG-TS" : "UDP MPEG-TS")
                      << " output.\n";
            if (!udp_streamer_.init(udp_runtime_.streamer)) {
                std::cerr << "[OutputManager] Failed to open UDP/RTP output: "
                          << udp_streamer_.getLastError() << "\n";
                transportRecovering.store(true, std::memory_order_release);
                return false;
            }
            drainEncodedQueues();
            waitForFreshKeyframe.store(true, std::memory_order_release);
            transportRecovering.store(false, std::memory_order_release);
            encoder.requestVideoKeyFrame();
            std::cout << "[OutputManager] "
                      << (sender_transport_ == SenderTransport::RTP ? "RTP" : "UDP")
                      << " output ready. Waiting for a fresh video keyframe before sending TS.\n";
            return true;
        }

        const bool retryForever = srt_runtime_.streamer.reconnect_forever;
        const int maxAttempts = std::max(1, srt_runtime_.initAttempts);
        int delayMs = std::max(250, srt_runtime_.initRetryDelayMs);
        const int maxDelayMs = std::max(delayMs, srt_runtime_.streamer.reconnect_backoff_max_ms);

        std::cout << "[OutputManager] Sender pipeline is live. Waiting for SRT transport before releasing output.\n";

        for (int attempt = 0; !stop.stop_requested() && (retryForever || attempt < maxAttempts); ++attempt) {
            {
                stage_timing::ScopedTimer reconnectTimer(reconnectStat);
                if (srt_streamer_.init(srt_runtime_.streamer)) {
                    drainEncodedQueues();
                    waitForFreshKeyframe.store(true, std::memory_order_release);
                    transportRecovering.store(false, std::memory_order_release);
                    encoder.requestVideoKeyFrame();
                    std::cout << "[OutputManager] SRT transport connected. Waiting for a fresh video keyframe before resuming output.\n";
                    return true;
                }
            }

            if (!retryForever && attempt + 1 >= maxAttempts) {
                break;
            }
            if (stop.stop_requested()) {
                break;
            }

            std::cerr << "[OutputManager] SRT initial connection failed. Retrying in "
                      << delayMs << " ms"
                      << " last_error='" << srt_streamer_.getLastError() << "'\n";

            int waitedMs = 0;
            while (!stop.stop_requested() && waitedMs < delayMs) {
                const int sliceMs = std::min(100, delayMs - waitedMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(sliceMs));
                waitedMs += sliceMs;
            }
            delayMs = std::min(delayMs * 2, maxDelayMs);
        }

        transportRecovering.store(true, std::memory_order_release);
        if (stop.stop_requested()) {
            std::cerr << "[OutputManager] Initial SRT wait stopped due to shutdown request.\n";
        } else {
            std::cerr << "[OutputManager] Failed to establish initial SRT transport.\n";
        }
        return false;
    };

    if (!waitForInitialTransport()) {
        if (!stop.stop_requested()) {
            stop.request_stop();
        }
        return;
    }

    auto sendTransportPacket = [&](const unsigned char* data, int size) -> bool {
        if (sender_transport_ == SenderTransport::UDP || sender_transport_ == SenderTransport::RTP) {
            return udp_streamer_.sendPacket(data, size);
        }
        return srt_streamer_.sendPacket(data, size);
    };

    const bool trueCbrEnabled = muxer_.isNullStuffingEnabled() && mpegts_metadata_.muxrateBps > 0;
    size_t cbrPayloadBytes = 1316;
    if (sender_transport_ == SenderTransport::UDP || sender_transport_ == SenderTransport::RTP) {
        cbrPayloadBytes = static_cast<size_t>(std::max(188, udp_runtime_.streamer.payload_size));
    } else {
        cbrPayloadBytes = static_cast<size_t>(std::max(188, srt_runtime_.streamer.payload_size));
    }
    cbrPayloadBytes = (cbrPayloadBytes / static_cast<size_t>(MuxerTS::kTsPacketSize)) *
                      static_cast<size_t>(MuxerTS::kTsPacketSize);
    if (cbrPayloadBytes < static_cast<size_t>(MuxerTS::kTsPacketSize)) {
        cbrPayloadBytes = 1316;
    }

    const long double cbrIntervalNsD = trueCbrEnabled
        ? ((static_cast<long double>(cbrPayloadBytes) * 8.0L * 1000000000.0L) /
           static_cast<long double>(mpegts_metadata_.muxrateBps))
        : 0.0L;
    const std::chrono::nanoseconds cbrIntervalNs(
        std::max<int64_t>(1, static_cast<int64_t>(cbrIntervalNsD + 0.5L)));
    clock::time_point cbrNextSend = clock::time_point{};

    auto resetCbrClock = [&]() {
        if (trueCbrEnabled) {
            cbrNextSend = clock::now();
        }
    };

    std::function<bool()> recoverTransport;

    auto sendCbrDuePayloads = [&]() -> bool {
        if (!trueCbrEnabled || waitForFreshKeyframe.load(std::memory_order_acquire)) {
            return true;
        }
        if (cbrNextSend == clock::time_point{}) {
            cbrNextSend = clock::now();
        }

        int sentThisLoop = 0;
        while (!stop.stop_requested()) {
            const clock::time_point now = clock::now();
            if (now < cbrNextSend) {
                break;
            }

            MuxerTS::OutputChunk chunk;
            bool containsMedia = false;
            if (!muxer_.popCbrPayload(chunk, cbrPayloadBytes, containsMedia)) {
                telemetry.muxFail.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[OutputManager] True-CBR payload generation failed: "
                          << muxer_.getLastError() << "\n";
                stop.request_stop();
                return false;
            }

            telemetry.attemptedSendBytes.fetch_add(chunk.size, std::memory_order_relaxed);
            {
                stage_timing::ScopedTimer timer(sendStat);
                if (!sendTransportPacket(chunk.data.get(), static_cast<int>(chunk.size))) {
                    telemetry.sendFail.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[OutputManager] Transport send failed. Entering live recovery...\n";
                    if (!recoverTransport()) {
                        return false;
                    }
                    resetCbrClock();
                    return true;
                }
                telemetry.sendBytes.fetch_add(chunk.size, std::memory_order_relaxed);
            }

            cbrNextSend += cbrIntervalNs;
            if (++sentThisLoop >= 32) {
                // Avoid starving encoded packet draining if the scheduler is
                // catching up after a short OS scheduling delay.
                break;
            }
        }

        return true;
    };

    recoverTransport = [&]() -> bool {
        transportRecovering.store(true, std::memory_order_release);
        waitForFreshKeyframe.store(true, std::memory_order_release);
        drainEncodedQueues();
        if (!muxer_.resetLiveSession()) {
            telemetry.muxFail.fetch_add(1, std::memory_order_relaxed);
            stop.request_stop();
            return false;
        }
        telemetry.liveSessionResets.fetch_add(1, std::memory_order_relaxed);

        stage_timing::ScopedTimer reconnectTimer(reconnectStat);
        if (sender_transport_ == SenderTransport::UDP || sender_transport_ == SenderTransport::RTP) {
            udp_streamer_.closeSocket();
            if (!udp_streamer_.init(udp_runtime_.streamer)) {
                std::cerr << "[OutputManager] UDP/RTP transport recovery failed: "
                          << udp_streamer_.getLastError() << "\n";
                stop.request_stop();
                return false;
            }
        } else {
            if (!srt_streamer_.reconnect(srt_runtime_.streamer, srt_runtime_.reconnectAttempts)) {
                if (stop.stop_requested()) {
                    std::cerr << "[OutputManager] Transport recovery stopped due to shutdown request.\n";
                } else {
                    std::cerr << "[OutputManager] Transport recovery failed.\n";
                }
                stop.request_stop();
                return false;
            }
        }

        drainEncodedQueues();
        waitForFreshKeyframe.store(true, std::memory_order_release);
        transportRecovering.store(false, std::memory_order_release);
        encoder.requestVideoKeyFrame();
        resetCbrClock();
        std::cout << "[OutputManager] Transport recovered. Waiting for a fresh video keyframe before resuming output.\n";
        return true;
    };

    while (!stop.stop_requested()) {
        stage_timing::ScopedTimer loopTimer(loopStat);
        bool didWork = false;

        EncodedPacket vp;
        if (videoPktQ.try_pop(vp)) {
            if (vp.pkt) {
                if (waitForFreshKeyframe.load(std::memory_order_acquire)) {
                    if ((vp.pkt->flags & AV_PKT_FLAG_KEY) == 0) {
                        telemetry.dropVideoWhileWaitingKeyframe.fetch_add(1, std::memory_order_relaxed);
                        telemetry.observeQueues(0, 0, videoPktQ.size(), audioPktQ.size());
                    } else {
                        waitForFreshKeyframe.store(false, std::memory_order_release);
                        telemetry.freshKeyframesAccepted.fetch_add(1, std::memory_order_relaxed);
                        resetCbrClock();
                        std::cout << "[OutputManager] Fresh keyframe accepted after reconnect. Resuming live TS session.\n";
                        stage_timing::ScopedTimer timer(videoWriteStat);
                        if (!muxer_.writeVideoPacket(vp.pkt.get())) {
                            telemetry.muxFail.fetch_add(1, std::memory_order_relaxed);
                            stop.request_stop();
                            break;
                        }
                        syncMuxRepairTelemetry();
                        didWork = true;
                    }
                } else {
                    stage_timing::ScopedTimer timer(videoWriteStat);
                    if (!muxer_.writeVideoPacket(vp.pkt.get())) {
                        telemetry.muxFail.fetch_add(1, std::memory_order_relaxed);
                        stop.request_stop();
                        break;
                    }
                    syncMuxRepairTelemetry();
                    didWork = true;
                }
            }
        }

        bool audioWriteFailed = false;
        for (int i = 0; i < kMaxAudioPacketsPerLoop; ++i) {
            EncodedPacket ap;
            if (!audioPktQ.try_pop(ap)) {
                break;
            }
            if (!ap.pkt) {
                continue;
            }
            if (waitForFreshKeyframe.load(std::memory_order_acquire)) {
                telemetry.dropAudioWhileWaitingKeyframe.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            stage_timing::ScopedTimer timer(audioWriteStat);
            if (!muxer_.writeAudioPacket(ap.pkt.get())) {
                telemetry.muxFail.fetch_add(1, std::memory_order_relaxed);
                stop.request_stop();
                audioWriteFailed = true;
                break;
            }
            didWork = true;
        }
        if (audioWriteFailed) {
            break;
        }

        if (didWork) {
            {
                stage_timing::ScopedTimer timer(flushStat);
                muxer_.flushOutput();
            }

            if (trueCbrEnabled) {
                if (!sendCbrDuePayloads()) {
                    break;
                }
            } else {
                MuxerTS::OutputChunk chunk;
                while (muxer_.popOutputChunk(chunk)) {
                    if (chunk.size == 0 || !chunk.data) {
                        continue;
                    }

                    telemetry.attemptedSendBytes.fetch_add(chunk.size, std::memory_order_relaxed);

                    {
                        stage_timing::ScopedTimer timer(sendStat);
                        if (!sendTransportPacket(chunk.data.get(), static_cast<int>(chunk.size))) {
                            telemetry.sendFail.fetch_add(1, std::memory_order_relaxed);
                            std::cerr << "[OutputManager] Transport send failed. Entering live recovery...\n";
                            if (!recoverTransport()) {
                                break;
                            }
                            continue;
                        }
                        telemetry.sendBytes.fetch_add(chunk.size, std::memory_order_relaxed);
                    }
                }
            }
        } else {
            telemetry.idleMux.fetch_add(1, std::memory_order_relaxed);

            const bool videoDone = videoThreadDone.load(std::memory_order_acquire) && (videoPktQ.size() == 0);
            const bool audioDone = audioThreadDone.load(std::memory_order_acquire) && (audioPktQ.size() == 0);

            if (videoDone && audioDone) {
                {
                    stage_timing::ScopedTimer timer(flushStat);
                    muxer_.flushOutput();
                }
                if (!trueCbrEnabled) {
                    MuxerTS::OutputChunk chunk;
                    while (muxer_.popOutputChunk(chunk)) {
                        if (chunk.size == 0 || !chunk.data) {
                            continue;
                        }
                        telemetry.attemptedSendBytes.fetch_add(chunk.size, std::memory_order_relaxed);
                        stage_timing::ScopedTimer timer(sendStat);
                        if (sendTransportPacket(chunk.data.get(), static_cast<int>(chunk.size))) {
                            telemetry.sendBytes.fetch_add(chunk.size, std::memory_order_relaxed);
                        } else {
                            telemetry.sendFail.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                break;
            }

            if (trueCbrEnabled && !waitForFreshKeyframe.load(std::memory_order_acquire)) {
                if (!sendCbrDuePayloads()) {
                    break;
                }

                const clock::time_point now = clock::now();
                const auto sleepStart = now;
                if (cbrNextSend > now) {
                    const clock::time_point maxSleep = now + std::chrono::milliseconds(1);
                    std::this_thread::sleep_until(std::min(cbrNextSend, maxSleep));
                } else {
                    std::this_thread::yield();
                }
                const auto sleepEnd = clock::now();
                stage_timing::add_duration(
                    idleSleepStat,
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(sleepEnd - sleepStart).count()));
            } else {
                const auto sleepStart = clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                const auto sleepEnd = clock::now();
                stage_timing::add_duration(
                    idleSleepStat,
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(sleepEnd - sleepStart).count()));
            }
        }
    }
}

void OutputManager::stopSenderRuntime()
{
    // Make this safe as a standalone lifecycle call, not only as part of
    // shutdownSender(). The sender thread can be blocked in SRT listener/caller
    // setup, reconnect, or UDP/SRT send, so request pipeline stop and wake the
    // transport before joining.
    if (sender_stop_token_) {
        sender_stop_token_->request_stop();
    }
    srt_streamer_.requestStop();
    srt_streamer_.closeSocket();
    udp_streamer_.requestStop();
    udp_streamer_.closeSocket();

    if (sender_thread_.joinable()) {
        sender_thread_.join();
    }
    sender_stop_token_ = nullptr;
}

void OutputManager::shutdownSender()
{
    stopSenderRuntime();
    sender_initialized_ = false;
}

// Receiver-side SDI output setup. The actual video mode is resolved later from
// the first decoded receiver frame, but preset policy can tune queue/preroll
// behavior before playout starts.
bool OutputManager::initializeDeckLinkPlayout(int deviceIndex, const std::string& presetPath)
{
    shutdownDeckLinkPlayout();
    decklink_playout_config_ = loadDeckLinkPlayoutConfig(presetPath);
    if (!decklink_output_.init(deviceIndex)) {
        return false;
    }
    decklink_initialized_ = true;
    return true;
}

int OutputManager::runDeckLinkPlayout(Receiver& receiver, const std::atomic<bool>& stopFlag)
{
    if (!decklink_initialized_) {
        std::cerr << "[OutputManager] DeckLink playout requested before initialization.\n";
        return -1;
    }

    return decklink_output_.runPlayout(receiver, stopFlag, decklink_playout_config_);
}

void OutputManager::shutdownDeckLinkPlayout()
{
    if (decklink_initialized_) {
        decklink_output_.stop();
        decklink_initialized_ = false;
    }
}