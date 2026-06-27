/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/receiver.h
 * Description: Declares the receiver facade and decoded media queue contract.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <climits>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include "core/frame.h"
#include "receiver/decoder_audio.h"
#include "receiver/decoder_video.h"
#include "receiver/demuxer_ts.h"
#include "receiver/srt_input.h"
#include "receiver/udp_input.h"

// Receiver facade. Owns transport/demux/decode lifecycle and exposes decoded
// media queues to the playout/output layer.
class Receiver
{
public:
    enum class Transport {
        SRT,
        UDP
    };

    struct Config
    {
        Transport transport = Transport::SRT;
        SRTInput::Config srt;
        UDPInput::Config udp;
        DemuxerTS::Config demux;
        DecoderVideo::Config video_decoder;
        DecoderAudio::Config audio_decoder;

        int video_stream_wait_ms = 5000;
        int audio_stream_wait_ms = 1000;

        int packed_audio_channels = 16;
        size_t packed_audio_queue_capacity = 64;
        size_t audio_fifo_max_frames = 32;
        size_t max_audio_pairs = 8;
        std::vector<int> audio_pair_route;

        int reconnect_gap_ms = 750;

        // UDP/RTP live inputs can see tiny isolated packet losses without
        // requiring a full decoder/demuxer hard reset. These thresholds keep
        // the receiver playing through small losses while still resetting for
        // larger bursts, source changes, local queue drops, or full transport
        // gaps.
        int soft_rtp_gap_packet_threshold = 128;
        int soft_ts_cc_error_threshold = 8;

        // Optional process-level shutdown flag used to abort startup waits promptly.
        const std::atomic<bool>* external_stop_flag = nullptr;
    };

    Receiver();
    ~Receiver();

    bool start(const Config& config);
    void stop();

    bool isRunning() const noexcept;

    bool popVideoPacket(DemuxedPacket& out, int timeout_ms = 100);
    bool popAudioPacket(DemuxedPacket& out, int timeout_ms = 100);

    bool popVideoFrame(VideoFrame& out, int timeout_ms = 100);
    bool popAudioFrame(AudioFrame& out, int timeout_ms = 100);

    SRTInput& srtInput() noexcept { return srt_input_; }
    UDPInput& udpInput() noexcept { return udp_input_; }
    bool isUdpTransport() const noexcept { return config_.transport == Transport::UDP; }
    bool isRtpTransport() const noexcept { return config_.transport == Transport::UDP && config_.udp.rtp_depacketize; }
    UDPInput::Diagnostics udpDiagnostics() const noexcept { return udp_input_.diagnostics(); }
    DemuxerTS& demuxer() noexcept { return demuxer_; }
    DecoderVideo& videoDecoder() noexcept { return video_decoder_; }
    DecoderAudio& audioDecoder() noexcept { return audio_decoder_; }

    size_t packedAudioQueueDepth() const noexcept
    {
        return packed_audio_queue_depth_.load(std::memory_order_acquire);
    }

    size_t packedAudioQueuedBytes() const noexcept
    {
        return packed_audio_queued_bytes_.load(std::memory_order_acquire);
    }

    size_t packedAudioHighWaterDepth() const noexcept
    {
        return packed_audio_high_water_depth_.load(std::memory_order_acquire);
    }

    size_t packedAudioHighWaterBytes() const noexcept
    {
        return packed_audio_high_water_bytes_.load(std::memory_order_acquire);
    }

    size_t audioFifoSamples() const noexcept
    {
        return audio_fifo_samples_.load(std::memory_order_acquire);
    }

    uint64_t reconnectResetCount() const noexcept
    {
        return reconnect_reset_count_.load(std::memory_order_acquire);
    }

    uint64_t softTransportLossCount() const noexcept
    {
        return soft_transport_loss_count_.load(std::memory_order_acquire);
    }

    uint64_t hardTransportLossCount() const noexcept
    {
        return hard_transport_loss_count_.load(std::memory_order_acquire);
    }

    bool hasReceiverQueueAvDelta() const noexcept;
    double receiverQueueAvDeltaMs() const noexcept;

    // Backward-compatible wrappers for older call sites. The value is not a
    // final DeckLink output sync measurement; it is the receiver queue delta.
    bool hasReceiverAvOffset() const noexcept { return hasReceiverQueueAvDelta(); }
    double receiverAvOffsetMs() const noexcept { return receiverQueueAvDeltaMs(); }

    uint64_t sourceGeneration() const noexcept
    {
        return source_generation_.load(std::memory_order_acquire);
    }

    void requestAudioCursorReset();

    struct AudioRoutingState
    {
        bool running = false;
        bool audio_chain_ready = false;
        int packed_audio_channels = 0;
        size_t output_pairs = 0;
        size_t logical_source_pairs = 0;
        std::vector<int> current_route;
        std::vector<int> source_stream_indices;
        std::vector<int> source_pair_indices;
    };

    bool setAudioPairRoute(const std::vector<int>& route);
    std::vector<int> getAudioPairRoute() const;
    AudioRoutingState getAudioRoutingState() const;

private:
    void feederLoop();
    void audioPackerLoop();

    bool initializeDecodeChain(bool wait_for_streams);
    void stopDecodeChain();
    void stopDecodeChainLocked();
    void clearPackedAudioState();
    bool stopRequested() const noexcept;
    void handleSourceDiscontinuity();
    void maybeRecoverDecodeChain();
    void handleDemuxerGenerationChange();

    struct StereoFifo
    {
        // Hot-path audio FIFO for one stereo logical pair.
        //
        // The old implementation used std::deque<int16_t> and removed audio
        // with pop_front() for every consumed sample. That is correct, but it
        // creates unnecessary allocator/cache pressure in the receiver audio
        // packer. This vector-backed FIFO keeps a read offset and compacts
        // only occasionally, so normal trim/consume operations are O(1).
        std::vector<int16_t> samples;
        size_t read_offset = 0; // int16_t elements, always stereo aligned
        bool seen = false;
        bool has_pts = false;
        int64_t start_pts = AV_NOPTS_VALUE;

        size_t queuedSamples() const noexcept
        {
            return (read_offset <= samples.size()) ? ((samples.size() - read_offset) / 2u) : 0u;
        }

        bool empty() const noexcept
        {
            return queuedSamples() == 0u;
        }

        void clearAudio()
        {
            samples.clear();
            read_offset = 0u;
        }

        void resetTimeline()
        {
            clearAudio();
            has_pts = false;
            start_pts = AV_NOPTS_VALUE;
        }

        void appendStereo(int16_t left, int16_t right)
        {
            samples.push_back(left);
            samples.push_back(right);
        }

        int16_t sampleAt(size_t stereo_offset, size_t channel) const
        {
            return samples[read_offset + stereo_offset * 2u + channel];
        }

        void compactIfNeeded()
        {
            if (read_offset == 0u) {
                return;
            }
            if (read_offset >= samples.size()) {
                samples.clear();
                read_offset = 0u;
                return;
            }

            // Avoid memmove on every small consume. Compact once the skipped
            // prefix is large enough to matter or dominates the live data.
            if (read_offset >= 8192u && read_offset >= (samples.size() / 2u)) {
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(read_offset));
                read_offset = 0u;
            }
        }

        void dropSamples(size_t stereo_samples)
        {
            const size_t available = queuedSamples();
            const size_t drop = std::min(stereo_samples, available);
            if (drop == 0u) {
                return;
            }

            read_offset += drop * 2u;
            if (has_pts && start_pts != AV_NOPTS_VALUE) {
                start_pts += static_cast<int64_t>(drop);
            }

            if (queuedSamples() == 0u) {
                resetTimeline();
            } else {
                compactIfNeeded();
            }
        }

        void trimToMaxSamples(size_t max_stereo_samples)
        {
            const size_t available = queuedSamples();
            if (available > max_stereo_samples) {
                dropSamples(available - max_stereo_samples);
            }
        }
    };

    struct LogicalPairKey
    {
        DecoderAudio* decoder = nullptr;
        int pair_index = 0;

        bool operator<(const LogicalPairKey& other) const noexcept
        {
            return (decoder < other.decoder) ||
                   (decoder == other.decoder && pair_index < other.pair_index);
        }
    };

    struct PairSource
    {
        DecoderAudio* decoder = nullptr;
        int stream_index = -1;
        int pair_index = 0;
    };

    void rebuildLogicalPairState(const std::vector<PairSource>& logical_pairs);
    void pushPackedAudioFrame(AudioFrame&& out);

private:
    SRTInput srt_input_;
    UDPInput udp_input_;
    DemuxerTS demuxer_;
    DecoderVideo video_decoder_;
    DecoderAudio audio_decoder_;
    std::vector<std::unique_ptr<DecoderAudio> > extra_audio_decoders_;

    std::thread feeder_thread_;
    std::thread audio_packer_thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> decode_chain_ready_{false};
    std::atomic<bool> audio_chain_ready_{false};
    std::atomic<bool> audio_packer_stop_requested_{false};
    std::atomic<bool> discontinuity_pending_{false};
    std::atomic<bool> audio_cursor_reset_requested_{false};
    std::atomic<uint64_t> reconnect_reset_count_{0};
    std::atomic<uint64_t> soft_transport_loss_count_{0};
    std::atomic<uint64_t> hard_transport_loss_count_{0};
    std::atomic<uint64_t> source_generation_{0};
    std::atomic<uint64_t> observed_demux_generation_{0};

    Config config_{};

    mutable std::mutex pipeline_mutex_;

    mutable std::mutex route_mutex_;
    std::vector<int> live_audio_pair_route_;
    std::vector<int> source_stream_indices_;
    std::vector<int> source_pair_indices_;
    size_t logical_source_pairs_ = 0;

    mutable std::mutex packed_audio_mutex_;
    std::condition_variable packed_audio_cv_;
    std::deque<AudioFrame> packed_audio_frames_;
    int64_t packed_audio_pts_ = 0;

    std::atomic<size_t> packed_audio_queue_depth_{0};
    std::atomic<size_t> packed_audio_queued_bytes_{0};
    std::atomic<size_t> packed_audio_high_water_depth_{0};
    std::atomic<size_t> packed_audio_high_water_bytes_{0};
    std::atomic<size_t> audio_fifo_samples_{0};

    std::atomic<int64_t> last_video_pts_us_{INT64_MIN};
    std::atomic<int64_t> last_audio_pts_us_{INT64_MIN};
};