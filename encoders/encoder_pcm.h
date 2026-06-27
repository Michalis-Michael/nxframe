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
 * PCM/ST 302M audio encoder declarations. EncoderPCM handles mapped PCM output, protected passthrough validation, fixed packet cadence, and packet handoff to the muxer.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/frame.h>
}

#include "../core/frame.h"

// PCM/ST 302M-style audio path. It validates channel routing, buffers incoming
// PCM samples, and emits fixed-cadence packets suitable for MPEG-TS muxing.
class EncoderPCM {
public:
    explicit EncoderPCM(const json& preset);
    ~EncoderPCM();

    bool initialize();

    AVPacket* encodeAudioFrame(const AudioFrame& frame);
    AVPacket* receivePacket();

    std::vector<AVPacket*> flush();
    AVCodecContext* getCodecContext() const;

private:
    struct PassthroughPair {
        std::string type;
        int chA = 0; // 1-based input channel number
        int chB = 0; // 1-based input channel number
    };

    bool parsePassthroughPairs();
    bool parseChannelMap();
    bool validateProtectedRouting() const;
    bool ensureInputFrameCapacity(int nb_samples);
    bool appendSelectedPcmToFifo(const AudioFrame& frame);
    bool handleInputPtsDiscontinuity(int64_t pts, int incomingSamples);
    int64_t normalizeInputPtsToSampleClock(const AudioFrame& frame) const;
    void resetBufferedAudioState(int64_t restartPts, const char* reason);
    bool buildFixedPacketFrame();
    bool encodeOneFixedPacket();
    void logConfigurationSummary() const;
    void drainEncoderPackets();
    AVPacket* popPendingPacket();
    void freePendingPackets();

private:
    json audioConfig;

    int sample_rate = 48000;
    int channels = 2;
    int bytes_per_sample = 2; // encoded S302M bytes/sample (2=s16, 4=s32/24-bit)
    int bits_per_raw_sample = 16;
    bool protected_passthrough_mode = false;
    int configured_input_channels = 0; // 0 = validate only against AudioFrame metadata
    std::string codecName = "pcm";
    std::string mode = "pcm";

    AVCodecContext* codec_ctx = nullptr;
    AVFrame* input_frame = nullptr;
    std::deque<AVPacket*> pending_packets;
    std::deque<int32_t> pcm_fifo_;
    int64_t next_audio_pts_ = 0;
    bool next_audio_pts_initialized_ = false;
    uint64_t discontinuity_count_ = 0;
    std::vector<PassthroughPair> passthrough_pairs;
    // 1-based input channel number for each encoded output channel slot.
    std::vector<int> channel_map_input_to_output;
};