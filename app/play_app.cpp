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

    std::cout << "[Main] Play source: " << inputUrl << "\n";
    std::cout << "[Main] Play destination: test\n";
    std::cout << "[Main] Receiver mode: " << receiverTransportModeString(cfg) << "\n";
    std::cout << "[Main] Receiver audio packing: channels=" << cfg.packed_audio_channels
              << " max_pairs=" << cfg.max_audio_pairs
              << " route=" << routeToString(cfg.audio_pair_route) << "\n";

    if (!receiver.start(cfg)) {
        std::cerr << "[Main] Failed to start receiver pipeline.\n";
        return -1;
    }

    const ReceiverControlFiles controlFiles = makeReceiverControlFiles();
    bool loggedVideoInfo = false;
    bool loggedAudioInfo = false;
    uint64_t decodedVideo = 0;
    uint64_t decodedAudioFrames = 0;
    uint64_t decodedAudioSamples = 0;
    auto lastPerf = std::chrono::steady_clock::now();
    auto lastControl = std::chrono::steady_clock::now();

    while (!shutdownRequested.load(std::memory_order_acquire)) {
        bool progressed = false;

        // Pop with short timeouts so control-file handling and shutdown remain
        // responsive even when one elementary stream is temporarily missing.
        VideoFrame vf;
        if (receiver.popVideoFrame(vf, 20)) {
            progressed = true;
            ++decodedVideo;
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
            ++decodedAudioFrames;
            decodedAudioSamples += static_cast<uint64_t>(af.num_samples);
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
            std::cout << "[PLAY-TEST] decoded_vfps=" << decodedVideo
                      << " decoded_afps=" << decodedAudioFrames
                      << " decoded_asps=" << decodedAudioSamples
                      << " packed_audio_q=" << receiver.packedAudioQueueDepth()
                      << " packed_audio_bytes=" << receiver.packedAudioQueuedBytes()
                      << " fifo_samples=" << receiver.audioFifoSamples()
                      << " demux_video_stream=" << receiver.demuxer().videoStreamIndex()
                      << " demux_audio_stream=" << receiver.demuxer().audioStreamIndex()
                      << "\n";
            decodedVideo = 0;
            decodedAudioFrames = 0;
            decodedAudioSamples = 0;
            lastPerf = now;
        }

        if (!progressed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    receiver.stop();
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

    std::cout << "[Main] Play source: " << inputUrl << "\n";
    std::cout << "[Main] Play destination: decklink " << deviceIndex << "\n";
    std::cout << "[Main] Receiver mode: " << receiverTransportModeString(cfg) << "\n";
    std::cout << "[Main] Receiver audio packing: channels=" << cfg.packed_audio_channels
              << " max_pairs=" << cfg.max_audio_pairs
              << " route=" << routeToString(cfg.audio_pair_route) << "\n";

    if (!receiver.start(cfg)) {
        std::cerr << "[Main] Failed to start receiver pipeline.\n";
        return -1;
    }

    OutputManager outputManager;
    if (!outputManager.initializeDeckLinkPlayout(deviceIndex, options.presetPath)) {
        receiver.stop();
        return -1;
    }

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

    const int rc = outputManager.runDeckLinkPlayout(receiver, shutdownRequested);
    controlStop.store(true, std::memory_order_release);
    if (controlThread.joinable()) {
        controlThread.join();
    }
    receiver.stop();
    outputManager.shutdownDeckLinkPlayout();
    std::remove(controlFiles.commandPath.c_str());
    std::remove(controlFiles.statePath.c_str());
    return rc;
}
