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
 * AAC encoder declarations. EncoderAAC handles channel selection, resampling, fixed-size audio frame buffering, AAC encoding, and packet handoff to the muxer.
 */

#pragma once

#include <deque>
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
}

#include "../core/frame.h"

// Preset-driven AAC encoder. It accepts NxFrame AudioFrame data, selects/maps
// channels, resamples when needed, buffers complete AAC frames, and returns packets.
class EncoderAAC {
public:
    explicit EncoderAAC(const json& preset);
    ~EncoderAAC();

    bool initialize();

    AVPacket* encodeAudioFrame(const AudioFrame& frame);
    AVPacket* receivePacket();

    std::vector<AVPacket*> flush();
    AVCodecContext* getCodecContext() const;

private:
    bool setupResampler(AVSampleFormat in_fmt, int in_rate, int in_channels);
    bool parseAacMode();
    void logConfigurationSummary(const char* encoderName) const;
    bool ensureResampleFrameCapacity(int nb_samples);
    bool ensureEncodeFrame();
    bool ensureFifoCapacity(int additional_samples);

    bool submitAudioFrame(const AudioFrame& frame);
    bool submitPCMWithFormat(const uint8_t* pcmData, int dataSizeBytes, int64_t pts_in_output_rate, int in_rate,
                             int in_channels, int bytes_per_sample, int valid_bits_per_sample,
                             int num_samples);
    int64_t normalizeInputPtsToOutputClock(const AudioFrame& frame, int in_rate) const;
    bool handleInputPtsDiscontinuity(int64_t pts, int incomingSamples, int in_rate);
    void resetBufferedAudioState(int64_t restartPts, const char* reason);

    bool sendOneFrameToEncoder(int samples_to_read, bool pad_with_silence);
    bool parseChannelMap();
    void drainEncoderPackets();
    AVPacket* popPendingPacket();
    void freePendingPackets();

private:
    json audioConfig;
    int sample_rate;
    int channels;
    int bitrate;
    std::string profile;
    std::string codec_name;
    std::string standard;
    std::string transport;
    AVCodecID codec_id;

    AVCodecContext* codec_ctx;
    AVFrame* input_frame;
    AVFrame* output_frame;
    AVFrame* encode_frame;
    SwrContext* swr_ctx;
    AVAudioFifo* fifo;

    AVSampleFormat last_input_fmt;
    int last_input_rate;
    int last_input_channels;

    int64_t next_pts;
    bool next_pts_initialized;
    int64_t expected_input_next_pts;
    bool input_pts_initialized;
    uint64_t discontinuity_count;
    std::vector<uint8_t> selected_input_buf;
    std::deque<AVPacket*> pending_packets;
    int configured_input_channels;
    // 1-based input channel number for each encoded output channel slot.
    std::vector<int> channel_map_input_to_output;
};