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
 * Output manager declarations. OutputManager coordinates MPEG-TS muxing, SRT/UDP/RTP transport, and DeckLink SDI playout without owning encoder or receiver internals.
 */

#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "core/bounded_queue.h"
#include "core/packet_item.h"
#include "core/pipeline_telemetry.h"
#include "core/stop_token.h"
#include "encoders/encoder_manager.h"
#include "output/decklink_output.h"
#include "output/muxer_ts.h"
#include "output/srt.h"
#include "output/udp_streamer.h"
#include "receiver/receiver.h"

// MPEG-TS service information written into the output transport stream.
struct MpegTsMetadataConfig {
    std::string serviceProvider = "NxFrame";
    std::string serviceName = "NxFrame Contribution Feed";
};

// SRT preset-derived runtime settings and retry policy.
struct SrtRuntimeConfig {
    SRTStreamer::Config streamer;
    int initAttempts = 5;
    int reconnectAttempts = 3;
    int initRetryDelayMs = 2000;
};

// UDP/RTP preset-derived runtime settings.
struct UdpRuntimeConfig {
    UDPStreamer::Config streamer;
};

// Coordinates sender muxing/transport and receiver SDI playout. It connects
// existing pipeline modules but does not perform codec work itself.
class OutputManager {
public:
    // Selected sender network transport for MPEG-TS output.
    enum class SenderTransport {
        SRT,
        UDP,
        RTP
    };

    struct SenderInitOptions {
        bool tsDebug = false;
        std::string tsCapturePath;
        const std::atomic<bool>* externalStopFlag = nullptr;
    };

    OutputManager() = default;
    ~OutputManager();

    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    bool initializeSender(const std::string& presetPath,
                          SenderTransport transport,
                          const std::string& cliAddress,
                          int cliPort,
                          EncoderManager& encoder,
                          const SenderInitOptions& options);

    bool startSenderRuntime(BoundedQueue<EncodedPacket>& videoPktQ,
                            BoundedQueue<EncodedPacket>& audioPktQ,
                            EncoderManager& encoder,
                            PipelineTelemetry& telemetry,
                            StopToken& stop,
                            std::atomic<bool>& videoThreadDone,
                            std::atomic<bool>& audioThreadDone,
                            std::atomic<bool>& transportRecovering,
                            std::atomic<bool>& waitForFreshKeyframe);

    void stopSenderRuntime();
    void shutdownSender();

    bool initializeDeckLinkPlayout(int deviceIndex, const std::string& presetPath = std::string());
    int runDeckLinkPlayout(Receiver& receiver, const std::atomic<bool>& stopFlag);
    void shutdownDeckLinkPlayout();

    MuxerTS& muxer() noexcept { return muxer_; }
    SRTStreamer& srtStreamer() noexcept { return srt_streamer_; }
    UDPStreamer& udpStreamer() noexcept { return udp_streamer_; }
    DeckLinkOutput& decklinkOutput() noexcept { return decklink_output_; }
    const SrtRuntimeConfig& srtRuntime() const noexcept { return srt_runtime_; }
    const UdpRuntimeConfig& udpRuntime() const noexcept { return udp_runtime_; }
    const MpegTsMetadataConfig& mpegtsMetadata() const noexcept { return mpegts_metadata_; }
    const DeckLinkPlayoutConfig& decklinkPlayoutConfig() const noexcept { return decklink_playout_config_; }

private:
    static constexpr int kMaxAudioPacketsPerLoop = 8;

    MpegTsMetadataConfig loadMpegTsMetadataConfig(const std::string& presetPath) const;
    SrtRuntimeConfig loadSrtRuntimeConfig(const std::string& presetPath,
                                          const std::string& cliAddress,
                                          int cliPort) const;
    UdpRuntimeConfig loadUdpRuntimeConfig(const std::string& presetPath,
                                          const std::string& cliAddress,
                                          int cliPort) const;
    DeckLinkPlayoutConfig loadDeckLinkPlayoutConfig(const std::string& presetPath) const;
    void runSenderLoop(BoundedQueue<EncodedPacket>& videoPktQ,
                       BoundedQueue<EncodedPacket>& audioPktQ,
                       EncoderManager& encoder,
                       PipelineTelemetry& telemetry,
                       StopToken& stop,
                       std::atomic<bool>& videoThreadDone,
                       std::atomic<bool>& audioThreadDone,
                       std::atomic<bool>& transportRecovering,
                       std::atomic<bool>& waitForFreshKeyframe);

    MuxerTS muxer_;
    SRTStreamer srt_streamer_;
    UDPStreamer udp_streamer_;
    DeckLinkOutput decklink_output_;
    MpegTsMetadataConfig mpegts_metadata_;
    SrtRuntimeConfig srt_runtime_;
    UdpRuntimeConfig udp_runtime_;
    SenderTransport sender_transport_ = SenderTransport::SRT;
    DeckLinkPlayoutConfig decklink_playout_config_;
    std::thread sender_thread_;
    StopToken* sender_stop_token_ = nullptr;
    bool sender_initialized_ = false;
    bool decklink_initialized_ = false;
};