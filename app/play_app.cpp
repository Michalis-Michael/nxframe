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
 * Receiver application runners. This file starts the receiver pipeline, manages optional DeckLink playout, and exposes lightweight live audio-routing control files.
 */

#include "app/play_app.h"

#include "cli/transport_url.h"
#include "core/receiver_dashboard.h"
#include "output/output_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

using json = nlohmann::json;

namespace {

struct ReceiverControlFiles {
    std::string commandPath;
    std::string statePath;
};

// Live receiver control is file-based for now so a local GUI or simple scripts
// can inspect state and update audio routing without coupling to the media loop.
ReceiverControlFiles makeReceiverControlFiles()
{
    const int pid = static_cast<int>(::getpid());
    ReceiverControlFiles f;
    f.commandPath = "/tmp/nxframe_receiver_control_" + std::to_string(pid) + ".json";
    f.statePath = "/tmp/nxframe_receiver_state_" + std::to_string(pid) + ".json";
    return f;
}

std::string formatPid(int pid)
{
    if (pid < 0) {
        return "-";
    }
    char text[16];
    std::snprintf(text, sizeof(text), "0x%04X", pid);
    return text;
}

std::string fieldOrderName(AVFieldOrder order)
{
    switch (order) {
    case AV_FIELD_TT:
    case AV_FIELD_TB:
        return "tff";
    case AV_FIELD_BB:
    case AV_FIELD_BT:
        return "bff";
    case AV_FIELD_PROGRESSIVE:
        return "progressive";
    default:
        return "unknown";
    }
}

bool isInterlacedFieldOrder(AVFieldOrder order)
{
    return order == AV_FIELD_TT || order == AV_FIELD_TB ||
           order == AV_FIELD_BB || order == AV_FIELD_BT;
}

void addPixelFormatDetails(json& item, int format)
{
    if (format < 0) {
        return;
    }

    const AVPixelFormat pixelFormat = static_cast<AVPixelFormat>(format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixelFormat);
    const char* name = av_get_pix_fmt_name(pixelFormat);
    if (name) {
        item["pixel_format"] = name;
    }
    if (!descriptor) {
        return;
    }

    int bitDepth = 0;
    for (int component = 0; component < descriptor->nb_components; ++component) {
        bitDepth = std::max(bitDepth, static_cast<int>(descriptor->comp[component].depth));
    }
    if (bitDepth > 0) {
        item["bit_depth"] = bitDepth;
    }

    if ((descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0) {
        item["chroma"] = "rgb";
    } else if (descriptor->nb_components <= 1) {
        item["chroma"] = "400";
    } else if (descriptor->log2_chroma_w == 1 && descriptor->log2_chroma_h == 1) {
        item["chroma"] = "420";
    } else if (descriptor->log2_chroma_w == 1 && descriptor->log2_chroma_h == 0) {
        item["chroma"] = "422";
    } else if (descriptor->log2_chroma_w == 0 && descriptor->log2_chroma_h == 0) {
        item["chroma"] = "444";
    }
}

void writeReceiverStateFile(const ReceiverControlFiles& files,
                            Receiver& receiver,
                            const std::string& inputUrl,
                            const std::string& outputName)
{
    const Receiver::AudioRoutingState st = receiver.getAudioRoutingState();
    json j;
    j["running"] = st.running;
    j["audio_chain_ready"] = st.audio_chain_ready;
    j["input_url"] = inputUrl;
    j["output"] = outputName;
    j["packed_audio_channels"] = st.packed_audio_channels;
    j["output_pairs"] = st.output_pairs;
    j["logical_source_pairs"] = st.logical_source_pairs;
    j["current_route"] = st.current_route;
    j["video"] = nullptr;
    j["audio_streams"] = json::array();
    j["source_pairs"] = json::array();

    const DemuxerTS::HealthSnapshot demuxHealth = receiver.demuxer().healthSnapshot();
    json stats = {
        {"sample_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()},
        {"video_packet_bytes_total", receiver.demuxer().videoPacketBytesTotal()},
        {"decoder_video_queue", receiver.videoDecoder().queueDepth()},
        {"decoder_video_queue_high_water", receiver.videoDecoder().highWaterQueueDepth()},
        {"decoder_video_drops", receiver.videoDecoder().queueDroppedFrameCount()},
        {"acquisition_dropped_packets", receiver.videoDecoder().acquisitionDroppedPacketCount()},
        {"demux_video_queue", receiver.demuxer().videoQueueDepth()},
        {"demux_audio_queue", receiver.demuxer().audioQueueDepth()},
        {"demux_input_buffer_bytes", receiver.demuxer().inputBufferedBytes()},
        {"continuity_errors", demuxHealth.continuity_errors},
        {"invalid_sync", demuxHealth.invalid_sync},
        {"discontinuities", demuxHealth.discontinuities},
        {"transport_dropped_packets", receiver.isUdpTransport()
            ? receiver.udpInput().droppedPackets()
            : receiver.srtInput().droppedPackets()},
        {"packed_audio_queue", receiver.packedAudioQueueDepth()},
        {"packed_audio_queue_high_water", receiver.packedAudioHighWaterDepth()},
        {"source_generation", receiver.sourceGeneration()}
    };
    j["stats"] = std::move(stats);

    const std::shared_ptr<const DemuxerTS::ProgramSnapshot> snapshot = receiver.demuxer().snapshot();
    if (snapshot) {
        for (const DemuxerTS::StreamInfo& stream : snapshot->streams) {
            json item = {
                {"stream_index", stream.stream_index},
                {"pid", stream.pid},
                {"pid_hex", formatPid(stream.pid)},
                {"codec", avcodec_get_name(stream.codec_id)}
            };
            if (!stream.language.empty()) {
                item["language"] = stream.language;
            }
            if (!stream.title.empty()) {
                item["title"] = stream.title;
            }
            const auto cpIt = snapshot->codecpar_by_stream.find(stream.stream_index);
            if (stream.media_type == AVMEDIA_TYPE_VIDEO) {
                if (cpIt != snapshot->codecpar_by_stream.end() && cpIt->second) {
                    const AVCodecParameters* parameters = cpIt->second.get();
                    item["width"] = parameters->width;
                    item["height"] = parameters->height;
                    item["interlaced"] = isInterlacedFieldOrder(parameters->field_order);
                    item["field_order"] = fieldOrderName(parameters->field_order);
                    if (parameters->bit_rate > 0) {
                        item["bit_rate"] = parameters->bit_rate;
                    }
                    if (parameters->level > 0) {
                        item["level"] = parameters->level;
                    }
                    addPixelFormatDetails(item, parameters->format);
                }
                if (stream.avg_frame_rate.num > 0 && stream.avg_frame_rate.den > 0) {
                    item["frame_rate_num"] = stream.avg_frame_rate.num;
                    item["frame_rate_den"] = stream.avg_frame_rate.den;
                }
                j["video"] = item;
            } else if (stream.media_type == AVMEDIA_TYPE_AUDIO) {
                item["sample_rate"] = stream.sample_rate;
                item["channels"] = stream.channels;
                if (cpIt != snapshot->codecpar_by_stream.end() && cpIt->second) {
                    const AVCodecParameters* parameters = cpIt->second.get();
                    if (parameters->bit_rate > 0) {
                        item["bit_rate"] = parameters->bit_rate;
                    }
                    if (parameters->profile >= 0) {
                        item["profile"] = parameters->profile;
                    }
                }
                j["audio_streams"].push_back(item);
            }
        }
    }

    if (j["video"].is_object()) {
        AVRational nominalFrameRate{0, 1};
        bool decodedInterlaced = false;
        if (receiver.videoDecoder().getCadenceHint(nominalFrameRate, decodedInterlaced)) {
            j["video"]["frame_rate_num"] = nominalFrameRate.num;
            j["video"]["frame_rate_den"] = nominalFrameRate.den;
            j["video"]["interlaced"] = decodedInterlaced;
        }
    }

    for (size_t i = 0; i < st.logical_source_pairs; ++i) {
        const int streamIndex = i < st.source_stream_indices.size() ? st.source_stream_indices[i] : -1;
        json item;
        item["logical_pair"] = static_cast<int>(i + 1);
        item["stream_index"] = streamIndex;
        item["pair_index"] = i < st.source_pair_indices.size() ? st.source_pair_indices[i] : -1;
        if (snapshot) {
            for (const DemuxerTS::StreamInfo& stream : snapshot->streams) {
                if (stream.stream_index == streamIndex) {
                    item["pid"] = stream.pid;
                    item["pid_hex"] = formatPid(stream.pid);
                    item["codec"] = avcodec_get_name(stream.codec_id);
                    item["sample_rate"] = stream.sample_rate;
                    if (!stream.language.empty()) {
                        item["language"] = stream.language;
                    }
                    if (!stream.title.empty()) {
                        item["title"] = stream.title;
                    }
                    break;
                }
            }
        }
        j["source_pairs"].push_back(item);
    }

    const std::string tmpPath = files.statePath + ".tmp";
    // Write atomically to avoid readers observing a partially written JSON file.
    std::ofstream out(tmpPath);
    out << j.dump(2) << "\n";
    out.close();
    std::rename(tmpPath.c_str(), files.statePath.c_str());
}

void processReceiverControlFile(const ReceiverControlFiles& files, Receiver& receiver)
{
    std::ifstream in(files.commandPath);
    if (!in) {
        return;
    }

    json j;
    try {
        in >> j;
    } catch (...) {
        in.close();
        std::remove(files.commandPath.c_str());
        return;
    }
    in.close();

    if (j.contains("audio_pair_route")) {
        if (!j["audio_pair_route"].is_array()) {
            std::cerr << "[Receiver] Ignoring live audio route update: audio_pair_route must be an array\n";
            std::remove(files.commandPath.c_str());
            return;
        }

        std::vector<int> route;
        size_t index = 0;
        for (const auto& v : j["audio_pair_route"]) {
            ++index;
            if (!v.is_number_integer()) {
                std::cerr << "[Receiver] Ignoring live audio route update: item #"
                          << index << " must be an integer\n";
                std::remove(files.commandPath.c_str());
                return;
            }
            const int value = v.get<int>();
            if (value < 0 || value > 64) {
                std::cerr << "[Receiver] Ignoring live audio route update: item #"
                          << index << " outside allowed range 0..64\n";
                std::remove(files.commandPath.c_str());
                return;
            }
            route.push_back(value);
        }
        receiver.setAudioPairRoute(route);
        std::cerr << "[Receiver] Live audio route updated: " << routeToString(route) << "\n";
    }

    std::remove(files.commandPath.c_str());
}


struct ReceiverDashboardSampler {
    nxframe::SystemResourceMonitor resources;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point previous = started;
    uint64_t lastBytes = 0;
    uint64_t lastVideo = 0;
    uint64_t lastAudio = 0;

    nxframe::ReceiverDashboardInterval sample(Receiver& receiver, DeckLinkOutput* decklink = nullptr)
    {
        nxframe::ReceiverDashboardInterval p;
        const auto now = std::chrono::steady_clock::now();
        p.seconds = std::chrono::duration<double>(now - previous).count();
        if (p.seconds <= 0.0) p.seconds = 1.0;
        p.uptime = std::chrono::duration<double>(now - started).count();
        p.resources = resources.sample();

        uint64_t bytes = 0;
        uint64_t packets = 0;
        uint64_t drops = 0;
        std::string network;
        if (receiver.isUdpTransport()) {
            const auto d = receiver.udpDiagnostics();
            bytes = d.received_bytes;
            packets = d.received_packets;
            drops = d.dropped_packets;
            p.rtp = receiver.isRtpTransport();
            p.rtpGaps = d.rtp_sequence_gaps;
            p.rtpOutOfOrder = d.rtp_out_of_order;
            p.rtpDuplicates = d.rtp_duplicates;
            p.rtpMalformed = d.rtp_malformed;
            p.tsSyncErrors = d.ts_sync_errors;
            p.tsContinuityErrors = d.ts_continuity_errors;
            network = UDPInput::stateToString(receiver.udpInput().getState());
        } else {
            bytes = receiver.srtInput().receivedBytes();
            packets = receiver.srtInput().receivedPackets();
            drops = receiver.srtInput().droppedPackets();
            network = SRTInput::stateToString(receiver.srtInput().getState());
        }
        p.receiveMbps = (bytes >= lastBytes) ? ((bytes - lastBytes) * 8.0 / 1000000.0 / p.seconds) : 0.0;
        p.receivedBytes = bytes;
        p.receivedPackets = packets;
        p.transportDrops = drops;
        lastBytes = bytes;

        const auto health = receiver.demuxer().healthSnapshot();
        p.demuxPackets = health.transport_packets;
        p.demuxSyncErrors = health.invalid_sync;
        p.demuxContinuityErrors = health.continuity_errors;
        p.demuxDiscontinuities = health.discontinuities;
        p.demuxOverflowEvents = health.input_overflow_events;
        p.demuxVideoDrops = health.video_output_queue_drop_packets;
        p.demuxAudioDrops = health.audio_output_queue_drop_packets;
        p.demuxInputBytes = receiver.demuxer().inputBufferedBytes();
        p.demuxVideoQ = receiver.demuxer().videoQueueDepth();
        p.demuxAudioQ = receiver.demuxer().audioQueueDepth();

        p.decodedVideoTotal = receiver.videoDecoder().decodedFrameCount();
        p.decodedAudioTotal = receiver.audioDecoder().decodedFrameCount();
        p.decodedVideoFps = (p.decodedVideoTotal >= lastVideo) ? (p.decodedVideoTotal - lastVideo) / p.seconds : 0.0;
        p.decodedAudioFps = (p.decodedAudioTotal >= lastAudio) ? (p.decodedAudioTotal - lastAudio) / p.seconds : 0.0;
        lastVideo = p.decodedVideoTotal;
        lastAudio = p.decodedAudioTotal;
        p.decodedVideoQ = receiver.videoDecoder().queueDepth();
        p.decodedAudioQ = receiver.audioDecoder().queueDepth();
        p.decodedVideoPeak = receiver.videoDecoder().highWaterQueueDepth();
        p.decodedAudioPeak = receiver.audioDecoder().highWaterQueueDepth();
        p.decoderVideoDrops = receiver.videoDecoder().queueDroppedFrameCount();
        p.acquisitionDrops = receiver.videoDecoder().acquisitionDroppedPacketCount();
        p.packedAudioQ = receiver.packedAudioQueueDepth();
        p.packedAudioPeak = receiver.packedAudioHighWaterDepth();
        p.fifoSamples = receiver.audioFifoSamples();
        p.queueAvDeltaValid = receiver.hasReceiverQueueAvDelta();
        p.queueAvDeltaMs = p.queueAvDeltaValid ? receiver.receiverQueueAvDeltaMs() : 0.0;
        p.softLoss = receiver.softTransportLossCount();
        p.hardLoss = receiver.hardTransportLossCount();
        p.reconnectResets = receiver.reconnectResetCount();
        p.sourceGeneration = receiver.sourceGeneration();

        if (decklink) {
            p.decklink = true;
            p.outputVideoTotal = decklink->outputVideoFrames();
            p.outputVideoDrops = decklink->droppedVideoFrames();
            p.outputAudioDrops = decklink->droppedAudioFrames();
            p.scheduleFailures = decklink->scheduleFailures();
            p.completionWarnings = decklink->completionWarnings();
            p.hwVideoQ = decklink->bufferedVideoFrames();
            p.hwAudioSamples = decklink->bufferedAudioSamples();
            p.scheduledVideo = decklink->scheduledVideoFrames();
            p.scheduledAvDeltaValid = decklink->scheduledAvDeltaValid();
            p.scheduledAvDeltaMs = p.scheduledAvDeltaValid ? decklink->scheduledAvDeltaMs() : 0.0;
        }

        nxframe::receiverDashboard().update([&](nxframe::ReceiverDashboardState& d) {
            d.network = network.empty() ? "WAITING" : network;
            const auto snapshot = receiver.demuxer().snapshot();
            if (snapshot) {
                const auto it = snapshot->codecpar_by_stream.find(snapshot->video_stream_index);
                if (it != snapshot->codecpar_by_stream.end() && it->second) {
                    const AVCodecParameters* cp = it->second.get();
                    std::ostringstream v;
                    v << avcodec_get_name(cp->codec_id);
                    if (cp->width > 0 && cp->height > 0) v << " | " << cp->width << "x" << cp->height;
                    AVRational fr = snapshot->video_avg_frame_rate.num > 0 ? snapshot->video_avg_frame_rate : snapshot->video_r_frame_rate;
                    if (fr.num > 0 && fr.den > 0) v << " @ " << nxframe::receiverDashboardNumber(double(fr.num) / fr.den, 2);
                    d.video = v.str();
                }
                const auto ai = snapshot->codecpar_by_stream.find(snapshot->primary_audio_stream_index);
                if (ai != snapshot->codecpar_by_stream.end() && ai->second) {
                    const AVCodecParameters* cp = ai->second.get();
                    std::ostringstream a;
                    a << avcodec_get_name(cp->codec_id);
                    if (cp->sample_rate > 0) a << " | " << cp->sample_rate << " Hz";
                    if (cp->ch_layout.nb_channels > 0) a << " | " << cp->ch_layout.nb_channels << " ch";
                    d.audio = a.str();
                }
            }
        });

        previous = now;
        return p;
    }
};

nxframe::ReceiverDashboardState makeReceiverDashboardState(const std::string& inputUrl,
                                                            const Receiver::Config& cfg,
                                                            const std::string& destination)
{
    nxframe::ReceiverDashboardState d;
    d.source = inputUrl;
    d.destination = destination;
    d.endpoint = inputUrl;
    d.mode = receiverTransportModeString(cfg);
    if (cfg.transport == Receiver::Transport::SRT) {
        d.transport = "SRT";
        d.latency = std::to_string(cfg.srt.latency) + " ms";
    } else {
        d.transport = cfg.udp.rtp_depacketize ? "RTP" : "UDP";
        d.latency = "connectionless";
    }
    return d;
}

} // namespace

int runPlayTest(const std::string& inputUrl,
                const ReceiverCliOptions& options,
                std::atomic<bool>& shutdownRequested)
{
    // Test playout validates and runs the full receiver path, but drains decoded
    // audio/video in this process instead of scheduling frames to SDI hardware.
    const TransportUrl src = parseTransportUrl(inputUrl);
    if (!src.valid) {
        std::cerr << "[Main] Error: Invalid transport URL '" << inputUrl << "'. Expected srt://host:port, udp://host:port, or rtp://host:port\n";
        return -1;
    }

    Receiver receiver;
    Receiver::Config cfg;
    configureReceiverTransport(cfg, src);
    applyReceiverCliOptions(cfg, options);
    cfg.external_stop_flag = &shutdownRequested;

    nxframe::receiverDashboard().start(makeReceiverDashboardState(inputUrl, cfg, "test"));

    std::cout << "[Main] Play source: " << inputUrl << "\n";
    std::cout << "[Main] Play destination: test\n";
    std::cout << "[Main] Receiver mode: " << receiverTransportModeString(cfg) << "\n";
    std::cout << "[Main] Receiver audio packing: channels=" << cfg.packed_audio_channels
              << " max_pairs=" << cfg.max_audio_pairs
              << " route=" << routeToString(cfg.audio_pair_route) << "\n";

    if (!receiver.start(cfg)) {
        std::cerr << "[Main] Failed to start receiver pipeline.\n";
        nxframe::ReceiverDashboardInterval failed;
        nxframe::receiverDashboard().finish(failed);
        return -1;
    }

    ReceiverDashboardSampler dashboardSampler;
    nxframe::ReceiverDashboardInterval dashboardLast;
    const ReceiverControlFiles controlFiles = makeReceiverControlFiles();
    bool loggedVideoInfo = false;
    bool loggedAudioInfo = false;
    auto lastPerf = std::chrono::steady_clock::now();
    auto lastControl = std::chrono::steady_clock::now();

    while (!shutdownRequested.load(std::memory_order_acquire)) {
        bool progressed = false;

        // Pop with short timeouts so control-file handling and shutdown remain
        // responsive even when one elementary stream is temporarily missing.
        VideoFrame vf;
        if (receiver.popVideoFrame(vf, 20)) {
            progressed = true;
            if (!loggedVideoInfo) {
                std::cout << "[PLAY-TEST] Video: "
                          << vf.width << "x" << vf.height
                          << " pix_fmt=" << av_get_pix_fmt_name(vf.pix_fmt)
                          << " interlaced=" << (vf.interlaced ? "yes" : "no")
                          << " tff=" << (vf.tff ? "yes" : "no")
                          << " time_base=" << vf.time_base.num << "/" << vf.time_base.den
                          << "\n";
                loggedVideoInfo = true;
            }
        }

        AudioFrame af;
        if (receiver.popAudioFrame(af, 20)) {
            progressed = true;
            if (!loggedAudioInfo) {
                std::cout << "[PLAY-TEST] Audio: "
                          << af.sample_rate << " Hz channels=" << af.channels
                          << " bytes_per_sample=" << af.bytes_per_sample
                          << " time_base=" << af.time_base.num << "/" << af.time_base.den
                          << "\n";
                loggedAudioInfo = true;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if ((now - lastControl) >= std::chrono::milliseconds(200)) {
            processReceiverControlFile(controlFiles, receiver);
            writeReceiverStateFile(controlFiles, receiver, inputUrl, "test");
            lastControl = now;
        }

        if ((now - lastPerf) >= std::chrono::seconds(1)) {
            dashboardLast = dashboardSampler.sample(receiver);
            nxframe::receiverDashboard().recordInterval(dashboardLast);
            nxframe::receiverDashboard().render(dashboardLast);
            lastPerf = now;
        }

        if (!progressed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    dashboardLast = dashboardSampler.sample(receiver);
    receiver.stop();
    nxframe::receiverDashboard().finish(dashboardLast);
    std::remove(controlFiles.commandPath.c_str());
    std::remove(controlFiles.statePath.c_str());
    return 0;
}

int runPlayDeckLink(const std::string& inputUrl,
                    int deviceIndex,
                    const ReceiverCliOptions& options,
                    std::atomic<bool>& shutdownRequested)
{
    // DeckLink playout uses Receiver for demux/decode/sync and OutputManager for
    // hardware scheduling. The app layer only controls startup/shutdown order.
    const TransportUrl src = parseTransportUrl(inputUrl);
    if (!src.valid) {
        std::cerr << "[Main] Error: Invalid transport URL '" << inputUrl << "'. Expected srt://host:port, udp://host:port, or rtp://host:port\n";
        return -1;
    }

    Receiver receiver;
    Receiver::Config cfg;
    configureReceiverTransport(cfg, src);
    applyReceiverCliOptions(cfg, options);
    cfg.external_stop_flag = &shutdownRequested;

    nxframe::receiverDashboard().start(
        makeReceiverDashboardState(inputUrl, cfg, std::string("decklink ") + std::to_string(deviceIndex)));

    std::cout << "[Main] Play source: " << inputUrl << "\n";
    std::cout << "[Main] Play destination: decklink " << deviceIndex << "\n";
    std::cout << "[Main] Receiver mode: " << receiverTransportModeString(cfg) << "\n";
    std::cout << "[Main] Receiver audio packing: channels=" << cfg.packed_audio_channels
              << " max_pairs=" << cfg.max_audio_pairs
              << " route=" << routeToString(cfg.audio_pair_route) << "\n";

    if (!receiver.start(cfg)) {
        std::cerr << "[Main] Failed to start receiver pipeline.\n";
        nxframe::ReceiverDashboardInterval failed;
        nxframe::receiverDashboard().finish(failed);
        return -1;
    }

    OutputManager outputManager;
    if (!outputManager.initializeDeckLinkPlayout(deviceIndex, options.presetPath)) {
        receiver.stop();
        nxframe::ReceiverDashboardInterval failed;
        nxframe::receiverDashboard().finish(failed);
        return -1;
    }
    nxframe::receiverDashboard().update([&](nxframe::ReceiverDashboardState& d) {
        d.outputDevice = outputManager.decklinkOutput().getDeviceName();
        d.output = "DeckLink scheduled SDI";
    });

    const ReceiverControlFiles controlFiles = makeReceiverControlFiles();
    std::atomic<bool> controlStop{false};
    // Keep the low-rate control/state file work outside the playout loop so SDI
    // scheduling is not delayed by filesystem I/O.
    std::thread controlThread([&]() {
        while (!controlStop.load(std::memory_order_acquire) && !shutdownRequested.load(std::memory_order_acquire)) {
            processReceiverControlFile(controlFiles, receiver);
            writeReceiverStateFile(controlFiles, receiver, inputUrl, std::string("decklink ") + std::to_string(deviceIndex));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    ReceiverDashboardSampler dashboardSampler;
    nxframe::ReceiverDashboardInterval dashboardLast;
    std::atomic<bool> dashboardStop{false};
    std::thread dashboardThread([&]() {
        auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!dashboardStop.load(std::memory_order_acquire) &&
               !shutdownRequested.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            if (dashboardStop.load(std::memory_order_acquire) ||
                shutdownRequested.load(std::memory_order_acquire)) {
                break;
            }
            dashboardLast = dashboardSampler.sample(receiver, &outputManager.decklinkOutput());
            nxframe::receiverDashboard().recordInterval(dashboardLast);
            nxframe::receiverDashboard().render(dashboardLast);
            next += std::chrono::seconds(1);
        }
    });

    const int rc = outputManager.runDeckLinkPlayout(receiver, shutdownRequested);
    controlStop.store(true, std::memory_order_release);
    dashboardStop.store(true, std::memory_order_release);
    if (controlThread.joinable()) {
        controlThread.join();
    }
    if (dashboardThread.joinable()) {
        dashboardThread.join();
    }
    dashboardLast = dashboardSampler.sample(receiver, &outputManager.decklinkOutput());
    receiver.stop();
    outputManager.shutdownDeckLinkPlayout();
    nxframe::receiverDashboard().finish(dashboardLast);
    std::remove(controlFiles.commandPath.c_str());
    std::remove(controlFiles.statePath.c_str());
    return rc;
}
