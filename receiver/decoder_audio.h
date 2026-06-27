/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/decoder_audio.h
 * Description: Declares the audio decoder and decoded-frame output contract.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "core/frame.h"
#include "receiver/demuxer_ts.h"

// Audio decoder worker. Normalizes decoded audio into the configured sample
// format/channel layout before publishing AudioFrame objects.
class DecoderAudio
{
public:
    struct Config
    {
        int output_sample_rate = 48000;
        int output_channels = 0; // 0 = preserve decoded channel count
        AVSampleFormat output_sample_fmt = AV_SAMPLE_FMT_S16;
        size_t queue_capacity = 32;
        bool drop_oldest_on_full = true;
        int input_stream_index = -1;
        int thread_count = 0; // 0 = FFmpeg default
    };

    DecoderAudio();
    ~DecoderAudio();

    bool init(DemuxerTS& demuxer, const Config& config);
    void stop();

    bool popFrame(AudioFrame& out, int timeout_ms = 100);
    std::string getLastError() const;

    size_t queueDepth() const noexcept
    {
        return queue_depth_.load(std::memory_order_acquire);
    }

    size_t queuedBytes() const noexcept
    {
        return queued_bytes_.load(std::memory_order_acquire);
    }

    size_t highWaterQueueDepth() const noexcept
    {
        return high_water_queue_depth_.load(std::memory_order_acquire);
    }

    size_t highWaterQueuedBytes() const noexcept
    {
        return high_water_queued_bytes_.load(std::memory_order_acquire);
    }

    int streamIndex() const noexcept { return stream_index_; }

    int decodedChannels() const noexcept
    {
        return decoded_channels_.load(std::memory_order_acquire);
    }

    uint64_t boundGeneration() const noexcept
    {
        return bound_generation_.load(std::memory_order_acquire);
    }

private:
    struct ResampleSourceState
    {
        AVChannelLayout in_ch_layout{};
        AVSampleFormat in_sample_fmt = AV_SAMPLE_FMT_NONE;
        int in_sample_rate = 0;

        AVChannelLayout out_ch_layout{};
        AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_NONE;
        int out_sample_rate = 0;

        bool valid = false;
    };

private:
    bool openDecoder();
    void closeDecoder();
    void flushDecoder();
    void decodeLoop();

    bool ensureResamplerForFrame(const AVFrame* src, int output_channels, bool& use_passthrough);
    void resetResampler();
    bool convertFrame(const AVFrame* src, AudioFrame& out);
    void pushFrame(AudioFrame&& out);
    void setLastError(const std::string& err);

private:
    DemuxerTS* demuxer_ = nullptr;
    Config config_{};

    std::shared_ptr<const DemuxerTS::ProgramSnapshot> snapshot_;
    uint64_t opened_generation_ = 0;

    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    ResampleSourceState resample_state_;

    int stream_index_ = -1;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint64_t> bound_generation_{0};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<AudioFrame> frames_;

    std::atomic<size_t> queue_depth_{0};
    std::atomic<size_t> queued_bytes_{0};
    std::atomic<size_t> high_water_queue_depth_{0};
    std::atomic<size_t> high_water_queued_bytes_{0};
    std::atomic<int> decoded_channels_{0};

    int64_t next_output_pts_ = AV_NOPTS_VALUE;
    bool next_output_pts_valid_ = false;

    mutable std::mutex err_mutex_;
    std::string last_error_;
};