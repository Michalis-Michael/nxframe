/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/decoder_video.h
 * Description: Declares the video decoder and decoded-frame output contract.
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
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include "core/frame.h"
#include "receiver/demuxer_ts.h"

// Video decoder worker. Publishes NxFrame VideoFrame objects rather than raw
// FFmpeg frames so downstream playout can use a stable internal contract.
class DecoderVideo
{
public:
    struct Config
    {
        size_t queue_capacity = 8;
        bool drop_oldest_on_full = true;
        bool low_delay = true;
        // Low-latency SDI receiver default: avoid FF_THREAD_FRAME because it
        // adds decoder pipeline delay and can make DeckLink playout chase video
        // while audio remains continuous. Slice threading keeps the receiver
        // deterministic; this matches the known-good alpha.3 receiver->SDI path.
        int thread_count = 1;
        int thread_type = FF_THREAD_SLICE;

        // Receiver acquisition guard: wait for the next key packet when joining
        // an already-running SRT/UDP MPEG-TS stream. This prevents the receiver
        // from locking A/V from an arbitrary mid-GOP decoder state after a
        // receiver-only restart.
        bool require_keyframe_on_start = true;
    };

    DecoderVideo();
    ~DecoderVideo();

    bool init(DemuxerTS& demuxer, const Config& config);
    void stop();

    bool popFrame(VideoFrame& out, int timeout_ms = 100);
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

    uint64_t decodedFrameCount() const noexcept
    {
        return decoded_frame_count_.load(std::memory_order_acquire);
    }

    uint64_t queueDroppedFrameCount() const noexcept
    {
        return queue_dropped_frame_count_.load(std::memory_order_acquire);
    }

    uint64_t acquisitionDroppedPacketCount() const noexcept
    {
        return dropped_until_keyframe_.load(std::memory_order_acquire);
    }

    int estimatedAudioFrameSamples() const noexcept
    {
        return estimated_audio_frame_samples_.load(std::memory_order_acquire);
    }

    bool peekFrameTimestamp(int64_t& pts, AVRational& time_base) const;
    bool getCadenceHint(AVRational& nominal_frame_rate, bool& interlaced) const;

    int streamIndex() const noexcept { return stream_index_; }
    uint64_t boundGeneration() const noexcept
    {
        return bound_generation_.load(std::memory_order_acquire);
    }

private:
    bool openDecoder();
    void closeDecoder();
    void flushDecoder();
    void decodeLoop();

    bool copyFrame(const AVFrame* src, VideoFrame& out);
    void releaseScaler();
    void updateCadenceEstimate(const VideoFrame& frame);
    void pushFrame(VideoFrame&& frame);
    void setLastError(const std::string& err);

private:
    DemuxerTS* demuxer_ = nullptr;
    Config config_{};

    std::shared_ptr<const DemuxerTS::ProgramSnapshot> snapshot_;
    uint64_t opened_generation_ = 0;

    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    int stream_index_ = -1;

    SwsContext* sws_ctx_ = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint64_t> bound_generation_{0};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<VideoFrame> frames_;

    std::atomic<size_t> queue_depth_{0};
    std::atomic<size_t> queued_bytes_{0};
    std::atomic<size_t> high_water_queue_depth_{0};
    std::atomic<size_t> high_water_queued_bytes_{0};
    std::atomic<uint64_t> decoded_frame_count_{0};
    std::atomic<uint64_t> queue_dropped_frame_count_{0};
    std::atomic<int> estimated_audio_frame_samples_{1920};

    int64_t last_pts_ = AV_NOPTS_VALUE;
    AVRational last_time_base_{1, 0};
    AVRational last_nominal_frame_rate_{0, 1};
    bool last_interlaced_ = false;

    bool waiting_for_start_keyframe_ = true;
    std::atomic<uint64_t> dropped_until_keyframe_{0};

    mutable std::mutex err_mutex_;
    std::string last_error_;
};