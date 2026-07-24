/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/demuxer_ts.h
 * Description: Declares the MPEG-TS demuxer interface and packet ownership contract.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
}

#include "core/packet_item.h"

struct DemuxedPacket
{
    AVPacketPtr pkt;
    AVRational time_base{1, 90000};
    int stream_index = -1;
    bool is_video = false;
    bool is_audio = false;
    uint64_t generation = 0;
};

// MPEG-TS demuxer. Incoming TS bytes are converted into owned AVPacketPtr
// items grouped by video/audio stream.
class DemuxerTS
{
public:
    struct Config
    {
        size_t io_buffer_size = 64 * 1024;
        size_t input_buffer_limit = 4 * 1024 * 1024;
        size_t output_queue_limit = 512;
        bool find_stream_info = true;
        int probe_size = 1024 * 1024;
        int64_t max_analyze_duration_us = 2 * AV_TIME_BASE;

        // UDP live joins often start in the middle of an H.264 GOP. During
        // stream probing FFmpeg may print repeated PPS/keyframe acquisition
        // errors until SPS/PPS and an IDR arrive. Keep this disabled by
        // default for file/SRT diagnostics and enable it only for UDP input.
        bool suppress_initial_video_probe_logs = false;
    };

    struct StreamInfo
    {
        int stream_index = -1;
        int pid = -1;
        AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
        AVCodecID codec_id = AV_CODEC_ID_NONE;
        AVRational time_base{0, 1};
        AVRational avg_frame_rate{0, 1};
        AVRational r_frame_rate{0, 1};
        int sample_rate = 0;
        int channels = 0;
    };

    using CodecParametersPtr = std::shared_ptr<AVCodecParameters>;

    struct HealthSnapshot
    {
        uint64_t transport_packets = 0;
        uint64_t invalid_sync = 0;
        uint64_t continuity_errors = 0;
        uint64_t discontinuities = 0;
        uint64_t generation = 0;
        bool discontinuity_detected = false;
    };

    struct ProgramSnapshot
    {
        uint64_t generation = 0;

        int video_stream_index = -1;
        int primary_audio_stream_index = -1;

        AVRational video_time_base{1, 90000};
        AVRational video_avg_frame_rate{0, 1};
        AVRational video_r_frame_rate{0, 1};

        AVRational primary_audio_time_base{1, 48000};

        std::vector<StreamInfo> streams;
        std::vector<int> audio_stream_indices;
        std::map<int, AVRational> audio_time_base_by_stream;
        std::map<int, CodecParametersPtr> codecpar_by_stream;
    };

    DemuxerTS();
    ~DemuxerTS();

    bool start(const Config& config);
    void stop();

    void pushData(const uint8_t* data, size_t size);
    void signalEndOfInput();

    bool popVideoPacket(DemuxedPacket& out, int timeout_ms = 100);
    bool popAudioPacket(DemuxedPacket& out, int timeout_ms = 100);
    bool popAudioPacketForStream(int stream_index, DemuxedPacket& out, int timeout_ms = 100);

    int videoStreamIndex() const noexcept;
    int audioStreamIndex() const noexcept;
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    bool waitForVideoStream(int timeout_ms);
    bool waitForAudioStream(int timeout_ms);
    bool waitForAudioStreams(size_t min_count, int timeout_ms);
    bool waitForAnyStream(int timeout_ms);

    std::shared_ptr<const ProgramSnapshot> snapshot() const;
    HealthSnapshot healthSnapshot() const;

    AVCodecParameters* cloneVideoCodecParameters() const;
    AVCodecParameters* cloneAudioCodecParameters() const;
    AVCodecParameters* cloneAudioCodecParametersForStream(int stream_index) const;

    AVRational videoTimeBase() const noexcept;
    AVRational videoAvgFrameRate() const noexcept;
    AVRational videoRFrameRate() const noexcept;
    AVRational audioTimeBase() const noexcept;
    AVRational audioTimeBaseForStream(int stream_index) const noexcept;

    std::vector<int> audioStreamIndices() const;

    uint64_t sourceGeneration() const noexcept
    {
        return generation_.load(std::memory_order_acquire);
    }

    size_t inputBufferedBytes() const noexcept
    {
        return input_buffered_bytes_.load(std::memory_order_acquire);
    }

    size_t videoQueueDepth() const noexcept
    {
        return video_queue_depth_.load(std::memory_order_acquire);
    }

    size_t audioQueueDepth() const noexcept
    {
        return audio_queue_depth_.load(std::memory_order_acquire);
    }

    size_t videoQueuedBytes() const noexcept
    {
        return video_queued_bytes_.load(std::memory_order_acquire);
    }

    uint64_t videoPacketBytesTotal() const noexcept
    {
        return video_packet_bytes_total_.load(std::memory_order_acquire);
    }

    size_t audioQueuedBytes() const noexcept
    {
        return audio_queued_bytes_.load(std::memory_order_acquire);
    }

    size_t audioQueueDepthForStream(int stream_index) const noexcept;
    size_t audioQueuedBytesForStream(int stream_index) const noexcept;

private:
    struct InputChunk
    {
        std::vector<uint8_t> data;
        size_t offset = 0;

        size_t remaining() const noexcept
        {
            return (offset < data.size()) ? (data.size() - offset) : 0u;
        }
    };

    static int interruptCallback(void* opaque);
    static int readPacket(void* opaque, uint8_t* buf, int buf_size);

    int readPacketImpl(uint8_t* buf, int buf_size);

    bool openInput();
    void demuxLoop();
    void cleanupInput();

    void clearInputBufferLocked();
    void trimInputBufferLocked();

    bool updateStreamInfoFromFormat();
    static CodecParametersPtr cloneCodecParametersShared(const AVCodecParameters* src);

    void pushVideoPacket(DemuxedPacket&& pkt);
    void pushAudioPacket(int stream_index, DemuxedPacket&& pkt);

    bool popVideoPacketInternal(DemuxedPacket& out, int timeout_ms);
    bool popAudioPacketInternal(int stream_index, DemuxedPacket& out, int timeout_ms);

    static size_t packetSizeBytes() noexcept { return 188u; }
    void updateTransportHealthFromTs(const uint8_t* data, size_t size);

private:
    Config config_{};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> end_of_input_{false};
    std::atomic<uint64_t> generation_{0};

    std::atomic<size_t> input_buffered_bytes_{0};
    std::atomic<size_t> video_queue_depth_{0};
    std::atomic<size_t> audio_queue_depth_{0};
    std::atomic<size_t> video_queued_bytes_{0};
    std::atomic<size_t> audio_queued_bytes_{0};
    std::atomic<uint64_t> video_packet_bytes_total_{0};

    mutable std::mutex input_mutex_;
    std::condition_variable input_cv_;
    std::deque<InputChunk> input_chunks_;

    mutable std::mutex video_mutex_;
    std::condition_variable video_cv_;
    std::deque<DemuxedPacket> video_packets_;

    mutable std::mutex audio_mutex_;
    std::condition_variable audio_cv_;
    std::map<int, std::deque<DemuxedPacket> > audio_packets_by_stream_;
    std::map<int, size_t> audio_queue_depth_by_stream_;
    std::map<int, size_t> audio_queued_bytes_by_stream_;

    mutable std::mutex stream_mutex_;
    std::condition_variable stream_cv_;
    std::shared_ptr<const ProgramSnapshot> snapshot_;

    mutable std::mutex health_mutex_;
    HealthSnapshot health_{};
    std::map<int, int> ts_cc_by_pid_;

    std::thread demux_thread_;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVIOContext* io_ctx_ = nullptr;
    uint8_t* io_buffer_ = nullptr;
    const AVInputFormat* ts_input_fmt_ = nullptr;

    AVPacketPool packet_pool_{256};

    bool logged_first_video_packet_ = false;
    bool acquired_first_video_key_packet_ = false;
    std::map<int, bool> logged_first_audio_packet_by_stream_;
};