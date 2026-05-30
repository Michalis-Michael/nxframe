/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/receiver.cpp
 * Description: Implements the receiver orchestration for transport, demux, decode and playout queues.
 */

#include "receiver/receiver.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace {

static int64_t ptsToMicroseconds(int64_t pts, AVRational tb)
{
    if (pts == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0) {
        return std::numeric_limits<int64_t>::min();
    }

    const long double us = static_cast<long double>(pts) *
                           static_cast<long double>(tb.num) *
                           1000000.0L /
                           static_cast<long double>(tb.den);

    if (us > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        us < static_cast<long double>(std::numeric_limits<int64_t>::min())) {
        return std::numeric_limits<int64_t>::min();
    }

    return static_cast<int64_t>(us);
}

static int64_t ptsTo48kSamples(int64_t pts, AVRational tb)
{
    if (pts == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0) {
        return AV_NOPTS_VALUE;
    }

    return av_rescale_q_rnd(pts,
                            tb,
                            AVRational{1, 48000},
                            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

static bool isSupportedPackedAudioChannelCount(int channels)
{
    return channels == 2 || channels == 8 || channels == 16;
}

static int clampPositiveMs(int value, int fallback)
{
    return (value > 0) ? value : fallback;
}

} // namespace

Receiver::Receiver() = default;

Receiver::~Receiver()
{
    stop();
}

bool Receiver::start(const Config& config)
{
    stop();
    config_ = config;
    // Live receivers can join while the upstream sender is still acquiring a
    // fresh keyframe or while FFmpeg is probing partial PAT/PMT/PES state. Keep
    // startup clean by suppressing expected H.264 probe noise until the first
    // key packet is observed. Normal NxFrame receiver logs stay visible.
    config_.demux.suppress_initial_video_probe_logs = true;

    if (!isSupportedPackedAudioChannelCount(config_.packed_audio_channels)) {
        std::cerr << "[Receiver] Unsupported packed_audio_channels="
                  << config_.packed_audio_channels
                  << ". Supported values are 2, 8, or 16.\n";
        return false;
    }

    if ((config_.packed_audio_channels % 2) != 0) {
        std::cerr << "[Receiver] packed_audio_channels must be even.\n";
        return false;
    }

    if (config_.max_audio_pairs == 0) {
        std::cerr << "[Receiver] max_audio_pairs must be greater than zero.\n";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(route_mutex_);
        live_audio_pair_route_ = config_.audio_pair_route;
        source_stream_indices_.clear();
        source_pair_indices_.clear();
        logical_source_pairs_ = 0;
    }

    {
        std::lock_guard<std::mutex> lk(packed_audio_mutex_);
        packed_audio_frames_.clear();
        packed_audio_pts_ = 0;
    }

    last_video_pts_us_.store(std::numeric_limits<int64_t>::min(), std::memory_order_release);
    last_audio_pts_us_.store(std::numeric_limits<int64_t>::min(), std::memory_order_release);
    packed_audio_queue_depth_.store(0, std::memory_order_release);
    packed_audio_queued_bytes_.store(0, std::memory_order_release);
    packed_audio_high_water_depth_.store(0, std::memory_order_release);
    packed_audio_high_water_bytes_.store(0, std::memory_order_release);
    audio_fifo_samples_.store(0, std::memory_order_release);
    reconnect_reset_count_.store(0, std::memory_order_release);
    soft_transport_loss_count_.store(0, std::memory_order_release);
    hard_transport_loss_count_.store(0, std::memory_order_release);

    decode_chain_ready_.store(false, std::memory_order_release);
    audio_chain_ready_.store(false, std::memory_order_release);
    audio_packer_stop_requested_.store(false, std::memory_order_release);
    discontinuity_pending_.store(false, std::memory_order_release);

    // Start each receiver session with a clean audio cursor reset state.
    audio_cursor_reset_requested_.store(false, std::memory_order_release);

    std::cerr << "[Receiver] Starting receiver pipeline...\n";

    if (!demuxer_.start(config_.demux)) {
        std::cerr << "[Receiver] Failed to start TS demuxer.\n";
        return false;
    }

    bool transport_started = false;
    if (config_.transport == Transport::UDP) {
        transport_started = udp_input_.start(config_.udp);
        if (!transport_started) {
            std::cerr << "[Receiver] Failed to start UDP input: "
                      << udp_input_.getLastError() << "\n";
        }
    } else {
        transport_started = srt_input_.start(config_.srt);
        if (!transport_started) {
            std::cerr << "[Receiver] Failed to start SRT input: "
                      << srt_input_.getLastError() << "\n";
        }
    }

    if (!transport_started) {
        demuxer_.signalEndOfInput();
        demuxer_.stop();
        return false;
    }

    running_.store(true, std::memory_order_release);
    feeder_thread_ = std::thread(&Receiver::feederLoop, this);

    observed_demux_generation_.store(demuxer_.sourceGeneration(), std::memory_order_release);
    source_generation_.fetch_add(1, std::memory_order_acq_rel);

    if (!initializeDecodeChain(true)) {
        if (!running_.load(std::memory_order_acquire) || !demuxer_.isRunning()) {
            std::cerr << "[Receiver] Failed to initialize receiver decode chain.\n";
            stop();
            return false;
        }

        // Live SRT/RTP/UDP startup must not fail just because the first probe
        // window started in the middle of a PES/GOP or before complete audio
        // codec parameters were available. VLC behaves this way too: keep the
        // transport and demuxer alive and let maybeRecoverDecodeChain() build
        // the decoders as soon as a usable video stream snapshot appears.
        std::cerr << "[Receiver] Initial stream discovery not ready yet; "
                  << "continuing live acquisition.\n";
    }

    std::cerr << "[Receiver] Receiver pipeline started. source_generation="
              << source_generation_.load(std::memory_order_acquire) << "\n";
    return true;
}

void Receiver::stop()
{
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);

    srt_input_.stop();
    udp_input_.stop();

    if (feeder_thread_.joinable()) {
        feeder_thread_.join();
    }

    stopDecodeChain();

    demuxer_.signalEndOfInput();
    demuxer_.stop();

    clearPackedAudioState();

    decode_chain_ready_.store(false, std::memory_order_release);
    audio_chain_ready_.store(false, std::memory_order_release);
    audio_packer_stop_requested_.store(false, std::memory_order_release);
    discontinuity_pending_.store(false, std::memory_order_release);
    audio_cursor_reset_requested_.store(false, std::memory_order_release);
    reconnect_reset_count_.store(0, std::memory_order_release);
    soft_transport_loss_count_.store(0, std::memory_order_release);
    hard_transport_loss_count_.store(0, std::memory_order_release);
    observed_demux_generation_.store(0, std::memory_order_release);
    last_video_pts_us_.store(std::numeric_limits<int64_t>::min(), std::memory_order_release);

    if (was_running) {
        std::cerr << "[Receiver] Receiver pipeline stopped.\n";
    }
}

bool Receiver::isRunning() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

bool Receiver::stopRequested() const noexcept
{
    return !running_.load(std::memory_order_acquire) ||
           (config_.external_stop_flag &&
            config_.external_stop_flag->load(std::memory_order_acquire));
}

bool Receiver::hasReceiverQueueAvDelta() const noexcept
{
    return last_video_pts_us_.load(std::memory_order_acquire) != std::numeric_limits<int64_t>::min() &&
           last_audio_pts_us_.load(std::memory_order_acquire) != std::numeric_limits<int64_t>::min();
}

bool Receiver::setAudioPairRoute(const std::vector<int>& route)
{
    std::lock_guard<std::mutex> lk(route_mutex_);
    live_audio_pair_route_ = route;
    return true;
}

std::vector<int> Receiver::getAudioPairRoute() const
{
    std::lock_guard<std::mutex> lk(route_mutex_);
    return live_audio_pair_route_;
}

Receiver::AudioRoutingState Receiver::getAudioRoutingState() const
{
    AudioRoutingState state;
    state.running = running_.load(std::memory_order_acquire);
    state.audio_chain_ready = audio_chain_ready_.load(std::memory_order_acquire);
    state.packed_audio_channels = config_.packed_audio_channels;
    state.output_pairs = std::min<size_t>(config_.max_audio_pairs,
                                          static_cast<size_t>(config_.packed_audio_channels / 2));

    std::lock_guard<std::mutex> lk(route_mutex_);
    state.logical_source_pairs = logical_source_pairs_;
    state.current_route = live_audio_pair_route_;
    state.source_stream_indices = source_stream_indices_;
    state.source_pair_indices = source_pair_indices_;
    return state;
}

double Receiver::receiverQueueAvDeltaMs() const noexcept
{
    int64_t videoUs = last_video_pts_us_.load(std::memory_order_acquire);

    int64_t frontPts = AV_NOPTS_VALUE;
    AVRational frontTb{0, 1};
    if (video_decoder_.peekFrameTimestamp(frontPts, frontTb)) {
        const int64_t frontUs = ptsToMicroseconds(frontPts, frontTb);
        if (frontUs != std::numeric_limits<int64_t>::min()) {
            videoUs = frontUs;
        }
    }

    int64_t audioUs = last_audio_pts_us_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(packed_audio_mutex_);
        if (!packed_audio_frames_.empty()) {
            const int64_t frontUs = ptsToMicroseconds(packed_audio_frames_.front().pts,
                                                      packed_audio_frames_.front().time_base);
            if (frontUs != std::numeric_limits<int64_t>::min()) {
                audioUs = frontUs;
            }
        }
    }

    if (videoUs == std::numeric_limits<int64_t>::min() ||
        audioUs == std::numeric_limits<int64_t>::min()) {
        return 0.0;
    }

    return static_cast<double>(audioUs - videoUs) / 1000.0;
}

// Called by the output playout loop when a source generation change or
// hard resync is detected. The audio packer consumes this flag and discards
// its output cursor so the next anchor is computed from the new stream.
void Receiver::requestAudioCursorReset()
{
    audio_cursor_reset_requested_.store(true, std::memory_order_release);
}

bool Receiver::popVideoPacket(DemuxedPacket& out, int timeout_ms)
{
    return demuxer_.popVideoPacket(out, timeout_ms);
}

bool Receiver::popAudioPacket(DemuxedPacket& out, int timeout_ms)
{
    return demuxer_.popAudioPacket(out, timeout_ms);
}

bool Receiver::popVideoFrame(VideoFrame& out, int timeout_ms)
{
    const bool ok = video_decoder_.popFrame(out, timeout_ms);
    if (ok) {
        const AVRational tb =
            (out.pts_time_base.num > 0 && out.pts_time_base.den > 0)
                ? out.pts_time_base
                : out.time_base;
        last_video_pts_us_.store(ptsToMicroseconds(out.pts, tb), std::memory_order_release);
    }
    return ok;
}

bool Receiver::popAudioFrame(AudioFrame& out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(packed_audio_mutex_);
    if (!packed_audio_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            return !packed_audio_frames_.empty() || !running_.load(std::memory_order_acquire);
        })) {
        return false;
    }

    if (packed_audio_frames_.empty()) {
        return false;
    }

    const size_t frame_bytes = packed_audio_frames_.front().buffer_size;
    out = std::move(packed_audio_frames_.front());
    packed_audio_frames_.pop_front();

    last_audio_pts_us_.store(ptsToMicroseconds(out.pts, out.time_base), std::memory_order_release);

    packed_audio_queue_depth_.store(packed_audio_frames_.size(), std::memory_order_release);
    const size_t prev_bytes = packed_audio_queued_bytes_.load(std::memory_order_acquire);
    packed_audio_queued_bytes_.store(prev_bytes >= frame_bytes ? prev_bytes - frame_bytes : 0u,
                                     std::memory_order_release);
    return true;
}

bool Receiver::initializeDecodeChain(bool wait_for_streams)
{
    std::lock_guard<std::mutex> lk(pipeline_mutex_);

    if (!running_.load(std::memory_order_acquire) || !demuxer_.isRunning()) {
        return false;
    }

    const bool video_already_ready = decode_chain_ready_.load(std::memory_order_acquire);

    if (!video_already_ready) {
        bool have_video = false;

        if (wait_for_streams) {
            std::cerr << "[Receiver] Waiting for video stream discovery...\n";
            const int total_wait_ms = clampPositiveMs(config_.video_stream_wait_ms, 5000);
            int waited_ms = 0;
            while (!stopRequested() && waited_ms < total_wait_ms) {
                const int slice_ms = std::min(100, total_wait_ms - waited_ms);
                if (demuxer_.waitForVideoStream(slice_ms)) {
                    have_video = true;
                    break;
                }
                waited_ms += slice_ms;
            }
        } else {
            std::shared_ptr<const DemuxerTS::ProgramSnapshot> s = demuxer_.snapshot();
            have_video = (s && s->video_stream_index >= 0);
        }

        if (!have_video) {
            if (wait_for_streams) {
                std::cerr << "[Receiver] Timed out waiting for video stream discovery.\n";
            }
            return false;
        }

        if (!video_decoder_.init(demuxer_, config_.video_decoder)) {
            std::cerr << "[Receiver] Failed to initialize video decoder: "
                      << video_decoder_.getLastError() << "\n";
            return false;
        }

        decode_chain_ready_.store(true, std::memory_order_release);
    }

    if (audio_chain_ready_.load(std::memory_order_acquire)) {
        return true;
    }

    bool have_audio = false;
    if (wait_for_streams) {
        const int total_wait_ms = clampPositiveMs(config_.audio_stream_wait_ms, 1000);
        int waited_ms = 0;
        while (!stopRequested() && waited_ms < total_wait_ms) {
            const int slice_ms = std::min(100, total_wait_ms - waited_ms);
            if (demuxer_.waitForAudioStream(slice_ms)) {
                have_audio = true;
                break;
            }
            waited_ms += slice_ms;
        }
    } else {
        std::shared_ptr<const DemuxerTS::ProgramSnapshot> s = demuxer_.snapshot();
        have_audio = (s && !s->audio_stream_indices.empty());
    }

    if (!have_audio) {
        if (wait_for_streams) {
            std::cerr << "[Receiver] No audio stream discovered in startup window. Continuing video-only.\n";
        }
        return true;
    }

    std::shared_ptr<const DemuxerTS::ProgramSnapshot> s = demuxer_.snapshot();
    if (!s || s->audio_stream_indices.empty()) {
        return true;
    }

    std::vector<int> audio_streams = s->audio_stream_indices;
    std::sort(audio_streams.begin(), audio_streams.end());

    DecoderAudio::Config primary_cfg = config_.audio_decoder;
    primary_cfg.output_channels = 0;
    primary_cfg.input_stream_index = audio_streams[0];

    if (!audio_decoder_.init(demuxer_, primary_cfg)) {
        std::cerr << "[Receiver] Failed to initialize primary audio decoder: "
                  << audio_decoder_.getLastError() << "\n";
        if (!video_already_ready) {
            video_decoder_.stop();
            decode_chain_ready_.store(false, std::memory_order_release);
        }
        return false;
    }

    for (size_t i = 1; i < audio_streams.size(); ++i) {
        std::unique_ptr<DecoderAudio> dec(new DecoderAudio());
        DecoderAudio::Config extra_cfg = config_.audio_decoder;
        extra_cfg.output_channels = 0;
        extra_cfg.input_stream_index = audio_streams[i];

        if (!dec->init(demuxer_, extra_cfg)) {
            std::cerr << "[Receiver] Failed to initialize audio decoder for stream "
                      << audio_streams[i] << ": " << dec->getLastError() << "\n";

            for (size_t j = 0; j < extra_audio_decoders_.size(); ++j) {
                extra_audio_decoders_[j]->stop();
            }
            extra_audio_decoders_.clear();
            audio_decoder_.stop();

            if (!video_already_ready) {
                video_decoder_.stop();
                decode_chain_ready_.store(false, std::memory_order_release);
            }
            return false;
        }

        extra_audio_decoders_.push_back(std::move(dec));
    }

    audio_packer_stop_requested_.store(false, std::memory_order_release);

    if (audio_packer_thread_.joinable()) {
        audio_packer_thread_.join();
    }

    audio_packer_thread_ = std::thread(&Receiver::audioPackerLoop, this);
    audio_chain_ready_.store(true, std::memory_order_release);

    std::cerr << "[Receiver] Initialized " << audio_streams.size()
              << " audio decoder(s) for packed "
              << config_.packed_audio_channels
              << "-channel output bus.\n";

    return true;
}

void Receiver::stopDecodeChainLocked()
{
    audio_packer_stop_requested_.store(true, std::memory_order_release);
    audio_fifo_samples_.store(0, std::memory_order_release);
    packed_audio_cv_.notify_all();

    if (audio_packer_thread_.joinable()) {
        audio_packer_thread_.join();
    }

    for (size_t i = 0; i < extra_audio_decoders_.size(); ++i) {
        extra_audio_decoders_[i]->stop();
    }
    extra_audio_decoders_.clear();

    audio_decoder_.stop();
    video_decoder_.stop();

    audio_chain_ready_.store(false, std::memory_order_release);
    decode_chain_ready_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(route_mutex_);
        source_stream_indices_.clear();
        source_pair_indices_.clear();
        logical_source_pairs_ = 0;
    }
}

void Receiver::stopDecodeChain()
{
    std::lock_guard<std::mutex> lk(pipeline_mutex_);
    stopDecodeChainLocked();
}

void Receiver::clearPackedAudioState()
{
    std::lock_guard<std::mutex> lk(packed_audio_mutex_);
    packed_audio_frames_.clear();
    packed_audio_pts_ = 0;
    packed_audio_queue_depth_.store(0, std::memory_order_release);
    packed_audio_queued_bytes_.store(0, std::memory_order_release);
    packed_audio_high_water_depth_.store(0, std::memory_order_release);
    packed_audio_high_water_bytes_.store(0, std::memory_order_release);
    audio_fifo_samples_.store(0, std::memory_order_release);
    last_audio_pts_us_.store(std::numeric_limits<int64_t>::min(), std::memory_order_release);
    packed_audio_cv_.notify_all();
}

void Receiver::handleSourceDiscontinuity()
{
    std::lock_guard<std::mutex> lk(pipeline_mutex_);

    const uint64_t next_reset_total =
        reconnect_reset_count_.load(std::memory_order_acquire) + 1;

    hard_transport_loss_count_.fetch_add(1, std::memory_order_acq_rel);

    std::cerr << "[Receiver] Source discontinuity detected. Flushing pipeline. reset_total="
              << next_reset_total << "\n";

    stopDecodeChainLocked();
    clearPackedAudioState();
    last_video_pts_us_.store(std::numeric_limits<int64_t>::min(), std::memory_order_release);

    demuxer_.signalEndOfInput();
    demuxer_.stop();

    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    if (!demuxer_.start(config_.demux)) {
        std::cerr << "[Receiver] Failed to restart TS demuxer after discontinuity.\n";
        return;
    }

    observed_demux_generation_.store(demuxer_.sourceGeneration(), std::memory_order_release);
    reconnect_reset_count_.fetch_add(1, std::memory_order_acq_rel);
    discontinuity_pending_.store(false, std::memory_order_release);
    source_generation_.fetch_add(1, std::memory_order_acq_rel);

    std::cerr << "[Receiver] Media pipeline reset complete. source_generation="
              << source_generation_.load(std::memory_order_acquire)
              << " reset_total=" << reconnect_reset_count_.load(std::memory_order_acquire)
              << "\n";
}


void Receiver::handleDemuxerGenerationChange()
{
    if (!running_.load(std::memory_order_acquire) || !demuxer_.isRunning()) {
        return;
    }

    const uint64_t current = demuxer_.sourceGeneration();
    uint64_t previous = observed_demux_generation_.load(std::memory_order_acquire);
    if (current == 0u || current == previous) {
        return;
    }

    if (!observed_demux_generation_.compare_exchange_strong(previous,
                                                            current,
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lk(pipeline_mutex_);

    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    std::cerr << "[Receiver] Demux stream generation changed "
              << previous << " -> " << current
              << ". Rebuilding decode chain.\n";

    stopDecodeChainLocked();
    clearPackedAudioState();
    last_video_pts_us_.store(std::numeric_limits<int64_t>::min(), std::memory_order_release);
    audio_cursor_reset_requested_.store(true, std::memory_order_release);
    source_generation_.fetch_add(1, std::memory_order_acq_rel);
}

void Receiver::maybeRecoverDecodeChain()
{
    if (!running_.load(std::memory_order_acquire) || !demuxer_.isRunning()) {
        return;
    }

    const bool need_video = !decode_chain_ready_.load(std::memory_order_acquire);

    std::shared_ptr<const DemuxerTS::ProgramSnapshot> s = demuxer_.snapshot();
    if (!s) {
        return;
    }

    // Do not treat an absent audio stream as a failed audio chain.
    // This avoids repeated recovery attempts/log spam for valid video-only sources,
    // while still allowing the audio chain to be initialized later if audio appears
    // in a newer demuxer snapshot/generation.
    const bool audio_available = !s->audio_stream_indices.empty();
    const bool need_audio = audio_available &&
                            !audio_chain_ready_.load(std::memory_order_acquire);

    if (!need_video && !need_audio) {
        return;
    }

    if (need_video && s->video_stream_index < 0) {
        return;
    }

    if (initializeDecodeChain(false)) {
        std::cerr << "[Receiver] Decode chain recovered. video="
                  << (decode_chain_ready_.load(std::memory_order_acquire) ? "yes" : "no")
                  << " audio="
                  << (audio_chain_ready_.load(std::memory_order_acquire) ? "yes" : "no")
                  << " reset_total=" << reconnect_reset_count_.load(std::memory_order_acquire)
                  << "\n";
    }
}

void Receiver::feederLoop()
{
    const char* feederTransportName = (config_.transport == Transport::UDP)
        ? (config_.udp.rtp_depacketize ? "rtp" : "udp")
        : "srt";
    std::cerr << "[Receiver] Transport feeder thread started. transport="
              << feederTransportName << "\n";

    bool saw_transport = false;
    bool gap_armed = false;
    const char* transport_name = (config_.transport == Transport::UDP)
        ? (config_.udp.rtp_depacketize ? "RTP" : "UDP")
        : "SRT";
    uint64_t observed_queue_drops = (config_.transport == Transport::UDP)
        ? udp_input_.droppedPackets()
        : srt_input_.droppedPackets();
    UDPInput::Diagnostics observed_udp_diag;
    if (config_.transport == Transport::UDP) {
        observed_udp_diag = udp_input_.diagnostics();
    }
    DemuxerTS::HealthSnapshot observed_demux_health = demuxer_.healthSnapshot();
    const uint64_t softRtpGapThreshold = static_cast<uint64_t>(std::max(0, config_.soft_rtp_gap_packet_threshold));
    const uint64_t softTsCcThreshold = static_cast<uint64_t>(std::max(0, config_.soft_ts_cc_error_threshold));

    auto droppedPackets = [&]() -> uint64_t {
        return (config_.transport == Transport::UDP)
            ? udp_input_.droppedPackets()
            : srt_input_.droppedPackets();
    };

    auto observeDemuxerHealthDiagnostics = [&]() {
        const DemuxerTS::HealthSnapshot h = demuxer_.healthSnapshot();
        const uint64_t new_disc = h.discontinuities >= observed_demux_health.discontinuities
            ? h.discontinuities - observed_demux_health.discontinuities
            : 0;
        const uint64_t new_cc = h.continuity_errors >= observed_demux_health.continuity_errors
            ? h.continuity_errors - observed_demux_health.continuity_errors
            : 0;
        const uint64_t new_sync = h.invalid_sync >= observed_demux_health.invalid_sync
            ? h.invalid_sync - observed_demux_health.invalid_sync
            : 0;

        if (new_disc || new_cc || new_sync) {
            const bool can_continue = decode_chain_ready_.load(std::memory_order_acquire);

            if (!can_continue) {
                // During live startup/probing, FFmpeg may see a partial TS/PES
                // view before the first clean PAT/PMT/keyframe epoch is fully
                // available. Do not hard-reset or fail startup here; keep
                // feeding packets until stream discovery succeeds. Once the
                // decoder is active, the same health counters are evaluated by
                // the normal soft/hard loss policy below.
                soft_transport_loss_count_.fetch_add(1, std::memory_order_acq_rel);
                std::cerr << "[Receiver] Startup TS probe warning:"
                          << " disc+=" << new_disc
                          << " cc_err+=" << new_cc
                          << " sync_err+=" << new_sync
                          << " totals[disc=" << h.discontinuities
                          << " cc=" << h.continuity_errors
                          << " sync=" << h.invalid_sync
                          << " packets=" << h.transport_packets
                          << "]. Stream discovery is still in progress; no hard reset.\n";
            } else {
                const bool soft_ts_loss =
                    new_sync == 0u &&
                    new_cc > 0u &&
                    new_cc <= softTsCcThreshold &&
                    new_disc <= new_cc;

                if (soft_ts_loss) {
                    soft_transport_loss_count_.fetch_add(1, std::memory_order_acq_rel);
                    std::cerr << "[Receiver] Soft TS continuity loss:"
                              << " cc_err+=" << new_cc
                              << " disc+=" << new_disc
                              << " totals[disc=" << h.discontinuities
                              << " cc=" << h.continuity_errors
                              << " sync=" << h.invalid_sync
                              << " packets=" << h.transport_packets
                              << "]. Decoder remains active; no hard reset.\n";
                } else {
                    gap_armed = true;
                    discontinuity_pending_.store(true, std::memory_order_release);
                    std::cerr << "[Receiver] TS demux health discontinuity detected:"
                              << " disc+=" << new_disc
                              << " cc_err+=" << new_cc
                              << " sync_err+=" << new_sync
                              << " totals[disc=" << h.discontinuities
                              << " cc=" << h.continuity_errors
                              << " sync=" << h.invalid_sync
                              << " packets=" << h.transport_packets
                              << "]. Waiting for fresh packets before reset.\n";
                }
            }
        }

        observed_demux_health = h;
    };

    auto observeUdpSequenceDiagnostics = [&]() {
        if (config_.transport != Transport::UDP) {
            return;
        }

        const UDPInput::Diagnostics d = udp_input_.diagnostics();
        const uint64_t new_rtp_gaps =
            d.rtp_sequence_gaps >= observed_udp_diag.rtp_sequence_gaps
                ? d.rtp_sequence_gaps - observed_udp_diag.rtp_sequence_gaps
                : 0;
        const uint64_t new_rtp_ooo =
            d.rtp_out_of_order >= observed_udp_diag.rtp_out_of_order
                ? d.rtp_out_of_order - observed_udp_diag.rtp_out_of_order
                : 0;
        const uint64_t new_ts_cc =
            d.ts_continuity_errors >= observed_udp_diag.ts_continuity_errors
                ? d.ts_continuity_errors - observed_udp_diag.ts_continuity_errors
                : 0;
        const uint64_t new_rtp_source_changes =
            d.rtp_source_changes >= observed_udp_diag.rtp_source_changes
                ? d.rtp_source_changes - observed_udp_diag.rtp_source_changes
                : 0;

        if (new_rtp_gaps || new_rtp_ooo || new_ts_cc || new_rtp_source_changes) {
            const bool can_continue = decode_chain_ready_.load(std::memory_order_acquire);
            const bool soft_rtp_loss = can_continue &&
                                       new_rtp_source_changes == 0u &&
                                       new_rtp_ooo == 0u &&
                                       new_rtp_gaps > 0u &&
                                       new_rtp_gaps <= softRtpGapThreshold &&
                                       new_ts_cc <= softTsCcThreshold;
            const bool soft_ts_only_loss = can_continue &&
                                           new_rtp_source_changes == 0u &&
                                           new_rtp_ooo == 0u &&
                                           new_rtp_gaps == 0u &&
                                           new_ts_cc > 0u &&
                                           new_ts_cc <= softTsCcThreshold;

            if (soft_rtp_loss || soft_ts_only_loss) {
                soft_transport_loss_count_.fetch_add(1, std::memory_order_acq_rel);
                std::cerr << "[Receiver] Soft " << transport_name
                          << " discontinuity:"
                          << " rtp_gap+=" << new_rtp_gaps
                          << " rtp_ooo+=" << new_rtp_ooo
                          << " rtp_src_change+=" << new_rtp_source_changes
                          << " ts_cc_err+=" << new_ts_cc
                          << " totals[rtp_gap=" << d.rtp_sequence_gaps
                          << " rtp_ooo=" << d.rtp_out_of_order
                          << " rtp_src_change=" << d.rtp_source_changes
                          << " ts_cc=" << d.ts_continuity_errors
                          << " last_expected=" << d.last_rtp_expected_sequence
                          << " last_observed=" << d.last_rtp_gap_observed_sequence
                          << " last_missing=" << d.last_rtp_gap_missing
                          << "]. Decoder remains active; no hard reset.\n";
            } else {
                gap_armed = true;
                discontinuity_pending_.store(true, std::memory_order_release);
                std::cerr << "[Receiver] " << transport_name
                          << " sequence discontinuity detected:"
                          << " rtp_gap+=" << new_rtp_gaps
                          << " rtp_ooo+=" << new_rtp_ooo
                          << " rtp_src_change+=" << new_rtp_source_changes
                          << " ts_cc_err+=" << new_ts_cc
                          << " totals[rtp_gap=" << d.rtp_sequence_gaps
                          << " rtp_ooo=" << d.rtp_out_of_order
                          << " rtp_src_change=" << d.rtp_source_changes
                          << " ts_cc=" << d.ts_continuity_errors
                          << " last_expected=" << d.last_rtp_expected_sequence
                          << " last_observed=" << d.last_rtp_gap_observed_sequence
                          << " last_missing=" << d.last_rtp_gap_missing
                          << "]. Waiting for fresh packets before reset.\n";
            }
        }

        observed_udp_diag = d;
    };

    auto popTransportPacket = [&](std::vector<uint8_t>& data, int timeout_ms) -> bool {
        data.clear();
        if (config_.transport == Transport::UDP) {
            UDPInput::Packet pkt;
            if (!udp_input_.popPacket(pkt, timeout_ms)) {
                return false;
            }
            data = std::move(pkt.data);
            return true;
        }

        SRTInput::Packet pkt;
        if (!srt_input_.popPacket(pkt, timeout_ms)) {
            return false;
        }
        data = std::move(pkt.data);
        return true;
    };

    auto last_packet_at = std::chrono::steady_clock::now();
    const auto reconnect_gap =
        std::chrono::milliseconds(std::max(100, config_.reconnect_gap_ms));

    std::vector<uint8_t> packet_data;

    while (running_.load(std::memory_order_acquire)) {
        observeUdpSequenceDiagnostics();

        const uint64_t current_queue_drops = droppedPackets();
        if (current_queue_drops != observed_queue_drops) {
            const uint64_t dropped_now = current_queue_drops - observed_queue_drops;
            observed_queue_drops = current_queue_drops;
            gap_armed = true;
            discontinuity_pending_.store(true, std::memory_order_release);
            std::cerr << "[Receiver] Local " << transport_name << " receive queue dropped "
                      << dropped_now
                      << " packet(s). Waiting for fresh packets before reset. total_drops="
                      << current_queue_drops
                      << "\n";
        }

        if (!popTransportPacket(packet_data, 100)) {
            const auto now = std::chrono::steady_clock::now();
            if (saw_transport && !gap_armed && (now - last_packet_at) >= reconnect_gap) {
                gap_armed = true;
                discontinuity_pending_.store(true, std::memory_order_release);
                std::cerr << "[Receiver] Transport gap detected (>= "
                          << reconnect_gap.count()
                          << " ms). Waiting for fresh packets before reset.\n";
            }

            handleDemuxerGenerationChange();
            maybeRecoverDecodeChain();
            continue;
        }

        if (packet_data.empty()) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();

        observeUdpSequenceDiagnostics();

        const uint64_t post_pop_queue_drops = droppedPackets();
        if (post_pop_queue_drops != observed_queue_drops) {
            const uint64_t dropped_now = post_pop_queue_drops - observed_queue_drops;
            observed_queue_drops = post_pop_queue_drops;
            gap_armed = true;
            discontinuity_pending_.store(true, std::memory_order_release);
            std::cerr << "[Receiver] Local " << transport_name << " receive queue dropped "
                      << dropped_now
                      << " packet(s). Resetting on this fresh packet. total_drops="
                      << post_pop_queue_drops
                      << "\n";
        }

        if (gap_armed || discontinuity_pending_.load(std::memory_order_acquire)) {
            handleSourceDiscontinuity();
            gap_armed = false;
            saw_transport = false;
        }

        saw_transport = true;
        last_packet_at = now;

        demuxer_.pushData(packet_data.data(), packet_data.size());
        observeDemuxerHealthDiagnostics();
        if (gap_armed || discontinuity_pending_.load(std::memory_order_acquire)) {
            handleSourceDiscontinuity();
            gap_armed = false;
            saw_transport = false;
            observed_demux_health = demuxer_.healthSnapshot();
            continue;
        }
        handleDemuxerGenerationChange();
        maybeRecoverDecodeChain();
    }

    std::cerr << "[Receiver] Transport feeder thread stopped.\n";
}

void Receiver::rebuildLogicalPairState(const std::vector<PairSource>& logical_pairs)
{
    std::lock_guard<std::mutex> lk(route_mutex_);
    source_stream_indices_.clear();
    source_pair_indices_.clear();
    source_stream_indices_.reserve(logical_pairs.size());
    source_pair_indices_.reserve(logical_pairs.size());

    for (size_t i = 0; i < logical_pairs.size(); ++i) {
        source_stream_indices_.push_back(logical_pairs[i].stream_index);
        source_pair_indices_.push_back(logical_pairs[i].pair_index + 1);
    }

    logical_source_pairs_ = logical_pairs.size();
}

void Receiver::pushPackedAudioFrame(AudioFrame&& out)
{
    const size_t frame_bytes = out.buffer_size;

    std::lock_guard<std::mutex> lk(packed_audio_mutex_);

    while (packed_audio_frames_.size() >= config_.packed_audio_queue_capacity &&
           !packed_audio_frames_.empty()) {
        const size_t dropped_bytes = packed_audio_frames_.front().buffer_size;
        packed_audio_frames_.pop_front();

        const size_t prev_bytes = packed_audio_queued_bytes_.load(std::memory_order_acquire);
        packed_audio_queued_bytes_.store(
            prev_bytes >= dropped_bytes ? prev_bytes - dropped_bytes : 0u,
            std::memory_order_release);
    }

    packed_audio_frames_.push_back(std::move(out));
    packed_audio_queue_depth_.store(packed_audio_frames_.size(), std::memory_order_release);

    const size_t new_bytes =
        packed_audio_queued_bytes_.fetch_add(frame_bytes, std::memory_order_acq_rel) + frame_bytes;
    const size_t new_depth = packed_audio_frames_.size();

    if (new_depth > packed_audio_high_water_depth_.load(std::memory_order_acquire)) {
        packed_audio_high_water_depth_.store(new_depth, std::memory_order_release);
    }
    if (new_bytes > packed_audio_high_water_bytes_.load(std::memory_order_acquire)) {
        packed_audio_high_water_bytes_.store(new_bytes, std::memory_order_release);
    }

    packed_audio_cv_.notify_one();
}

void Receiver::audioPackerLoop()
{
    const int outputChannels = config_.packed_audio_channels;
    const size_t outputPairs =
        std::min<size_t>(config_.max_audio_pairs,
                         static_cast<size_t>(outputChannels / 2));
    const int fixedChunkSamples = 480; // 10 ms @ 48 kHz

    std::vector<DecoderAudio*> decoders;
    if (audio_decoder_.streamIndex() >= 0) {
        decoders.push_back(&audio_decoder_);
    }
    for (size_t i = 0; i < extra_audio_decoders_.size(); ++i) {
        if (extra_audio_decoders_[i] && extra_audio_decoders_[i]->streamIndex() >= 0) {
            decoders.push_back(extra_audio_decoders_[i].get());
        }
    }

    std::vector<PairSource> logical_pairs;
    for (size_t i = 0; i < decoders.size(); ++i) {
        DecoderAudio* dec = decoders[i];
        if (!dec) continue;
        int decodedChannels = dec->decodedChannels();
        if (decodedChannels < 2) decodedChannels = 2;
        const int pairCount = std::max(1, decodedChannels / 2);
        for (int pairIdx = 0; pairIdx < pairCount; ++pairIdx) {
            PairSource src;
            src.decoder = dec;
            src.stream_index = dec->streamIndex();
            src.pair_index = pairIdx;
            logical_pairs.push_back(src);
        }
    }

    rebuildLogicalPairState(logical_pairs);

    std::vector<StereoFifo> fifos(logical_pairs.size());
    std::map<LogicalPairKey, size_t> pairLookup;
    for (size_t i = 0; i < logical_pairs.size(); ++i) {
        LogicalPairKey key;
        key.decoder = logical_pairs[i].decoder;
        key.pair_index = logical_pairs[i].pair_index;
        pairLookup[key] = i;
    }

    auto trimFifoToCursor = [&](StereoFifo& fifo, int64_t cursor48k) {
        if (!fifo.has_pts || fifo.start_pts == AV_NOPTS_VALUE) return;
        if (cursor48k <= fifo.start_pts) return;

        int64_t dropSamples = cursor48k - fifo.start_pts;
        int64_t available = static_cast<int64_t>(fifo.queuedSamples());
        if (dropSamples >= available) {
            fifo.resetTimeline();
            return;
        }
        fifo.dropSamples(static_cast<size_t>(dropSamples));
    };

    auto routedLogicalIndex = [&](size_t outPair, const std::vector<int>& liveRoute) -> int {
        if (outPair >= outputPairs) return -1;
        if (!liveRoute.empty() && outPair < liveRoute.size()) {
            const int route = liveRoute[outPair];
            if (route <= 0) return -1;
            const size_t idx = static_cast<size_t>(route - 1);
            return (idx < logical_pairs.size()) ? static_cast<int>(idx) : -1;
        }
        return (outPair < logical_pairs.size()) ? static_cast<int>(outPair) : -1;
    };

    auto earliestRoutedPts = [&](const std::vector<int>& liveRoute) -> int64_t {
        int64_t earliest = AV_NOPTS_VALUE;
        for (size_t outPair = 0; outPair < outputPairs; ++outPair) {
            const int logicalIdx = routedLogicalIndex(outPair, liveRoute);
            if (logicalIdx < 0) continue;
            const StereoFifo& fifo = fifos[static_cast<size_t>(logicalIdx)];
            if (!fifo.has_pts || fifo.start_pts == AV_NOPTS_VALUE) continue;
            if (earliest == AV_NOPTS_VALUE || fifo.start_pts < earliest) {
                earliest = fifo.start_pts;
            }
        }
        return earliest;
    };

    // outputCursor48k is initialized from the first audio frame
    // received rather than by peeking the video decoder without a lock. This
    // avoids the race where peekFrameTimestamp is called on a decoder that may
    // be mid-reset under pipeline_mutex_, and avoids the ~10ms mis-alignment
    // that occurred when the video PTS didn't land on a fixedChunkSamples
    // boundary in 48kHz space.
    //
    // The cursor is still snapped to a chunk boundary (so every emitted frame
    // starts on an exact 480-sample boundary), but now the reference point is
    // the earliest audio data actually present in the FIFOs rather than a
    // speculative video PTS peek. This matches what tryEstablishAnchor expects:
    // the first audio frame it receives should already be on (or very close to)
    // the cursor boundary, minimising the trim in trimAudioFrameFrontToPts.
    int64_t outputCursor48k = AV_NOPTS_VALUE;

    while (running_.load(std::memory_order_acquire) &&
           !audio_packer_stop_requested_.load(std::memory_order_acquire)) {

        // If the playout loop detected a source generation change
        // or hard resync, reset our output cursor so the next anchor is computed
        // fresh from the new stream's audio data. Without this, the packer
        // continues emitting frames at the old timeline position, and
        // tryEstablishAnchor receives audio with a PTS that can't be trimmed to
        // the new video anchor, causing the anchor to fail or drift badly.
        if (audio_cursor_reset_requested_.load(std::memory_order_acquire)) {
            audio_cursor_reset_requested_.store(false, std::memory_order_release);
            outputCursor48k = AV_NOPTS_VALUE;
            for (size_t i = 0; i < fifos.size(); ++i) {
                fifos[i].resetTimeline();
                fifos[i].seen = false;
            }
            std::cerr << "[Receiver] audioPackerLoop: cursor reset on source generation change.\n";
        }

        bool anyData = false;

        const size_t maxSamplesPerPair =
            static_cast<size_t>(std::max<size_t>(8u, config_.audio_fifo_max_frames)) *
            static_cast<size_t>(fixedChunkSamples);

        for (size_t decoderIdx = 0; decoderIdx < decoders.size(); ++decoderIdx) {
            DecoderAudio* dec = decoders[decoderIdx];
            if (!dec) continue;

            size_t harvested = 0;
            AudioFrame in;
            while (dec->popFrame(in, 0)) {
                if (!in.buffer || in.channels < 2 || in.bytes_per_sample != 2 || in.num_samples <= 0) {
                    continue;
                }

                const int16_t* src = reinterpret_cast<const int16_t*>(in.buffer.get());
                const int channelCount = std::max(2, in.channels);
                const int pairCount = std::max(1, channelCount / 2);
                const int64_t framePts48k = ptsTo48kSamples(in.pts, in.time_base);

                for (int pairIdx = 0; pairIdx < pairCount; ++pairIdx) {
                    LogicalPairKey key;
                    key.decoder = dec;
                    key.pair_index = pairIdx;
                    std::map<LogicalPairKey, size_t>::const_iterator it = pairLookup.find(key);
                    if (it == pairLookup.end()) {
                        continue;
                    }

                    StereoFifo& fifo = fifos[it->second];
                    if (fifo.empty()) {
                        fifo.has_pts = (framePts48k != AV_NOPTS_VALUE);
                        fifo.start_pts = fifo.has_pts ? framePts48k : AV_NOPTS_VALUE;
                    } else if (framePts48k != AV_NOPTS_VALUE && fifo.has_pts) {
                        const int64_t queuedSamples = static_cast<int64_t>(fifo.queuedSamples());
                        const int64_t expectedPts = fifo.start_pts + queuedSamples;
                        if (std::llabs(framePts48k - expectedPts) > 4) {
                            fifo.clearAudio();
                            fifo.has_pts = true;
                            fifo.start_pts = framePts48k;
                        }
                    }

                    for (int sampleIdx = 0; sampleIdx < in.num_samples; ++sampleIdx) {
                        fifo.appendStereo(src[sampleIdx * channelCount + pairIdx * 2 + 0],
                                          src[sampleIdx * channelCount + pairIdx * 2 + 1]);
                    }

                    fifo.trimToMaxSamples(maxSamplesPerPair);

                    if (fifo.empty()) {
                        fifo.resetTimeline();
                    }
                    fifo.seen = true;
                }
                anyData = true;
                if (++harvested >= config_.audio_fifo_max_frames) {
                    break;
                }
            }
        }

        size_t totalQueuedSamples = 0;
        for (size_t i = 0; i < fifos.size(); ++i) {
            totalQueuedSamples += fifos[i].samples.size() / 2u;
        }
        audio_fifo_samples_.store(totalQueuedSamples, std::memory_order_release);

        std::vector<int> liveRoute;
        {
            std::lock_guard<std::mutex> lk(route_mutex_);
            liveRoute = live_audio_pair_route_;
        }

        if (outputCursor48k == AV_NOPTS_VALUE) {
            // Initialize cursor from audio data only; do not peek the video decoder.
            // peek. Use the earliest routed FIFO PTS, snapped to a chunk boundary.
            // This is safe from races because we only touch our own FIFOs here,
            // and the video decoder is not accessed at all from this thread.
            outputCursor48k = earliestRoutedPts(liveRoute);
            if (outputCursor48k == AV_NOPTS_VALUE) {
                if (!anyData) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                continue;
            }
            outputCursor48k = (outputCursor48k / fixedChunkSamples) * fixedChunkSamples;
            std::cerr << "[Receiver] audioPackerLoop: cursor initialized at "
                      << outputCursor48k << " (48kHz samples)\n";
        }

        bool canEmit = false;
        for (size_t outPair = 0; outPair < outputPairs; ++outPair) {
            const int logicalIdx = routedLogicalIndex(outPair, liveRoute);
            if (logicalIdx < 0) {
                continue;
            }
            StereoFifo& fifo = fifos[static_cast<size_t>(logicalIdx)];
            trimFifoToCursor(fifo, outputCursor48k);
            const size_t available = fifo.queuedSamples();
            if (available >= static_cast<size_t>(fixedChunkSamples)) {
                canEmit = true;
                break;
            }
            if (fifo.has_pts && fifo.start_pts != AV_NOPTS_VALUE && fifo.start_pts > outputCursor48k) {
                canEmit = true;
                break;
            }
        }

        if (!canEmit) {
            if (!anyData) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            continue;
        }

        AudioFrame out;
        out.sample_rate = 48000;
        out.channels = outputChannels;
        out.bytes_per_sample = 2;
        out.num_samples = fixedChunkSamples;
        out.time_base = AVRational{1, 48000};
        out.pts = outputCursor48k;
        out.buffer_size = static_cast<size_t>(fixedChunkSamples) * static_cast<size_t>(outputChannels) * sizeof(int16_t);
        out.buffer = make_shared_u8(out.buffer_size > 0 ? out.buffer_size : 1);
        std::memset(out.buffer.get(), 0, out.buffer_size);
        int16_t* dst = reinterpret_cast<int16_t*>(out.buffer.get());

        /*
            Important routing rule:

            One logical input pair may be routed to multiple output pairs, e.g.
            audio_pair_route=[1,1,1,1,1,1,1,1] to duplicate SDI channels 1/2
            onto every embedded output pair.

            The previous implementation consumed the source FIFO inside this
            outPair loop. With duplicated routing, output pair 1 consumed the
            FIFO first, so output pairs 2..8 saw an empty/advanced FIFO and
            remained silent. Copy first for all output pairs, then consume each
            logical FIFO once after the routing pass.
        */
        std::vector<int64_t> consumeUntil48k(fifos.size(), AV_NOPTS_VALUE);

        for (size_t outPair = 0; outPair < outputPairs; ++outPair) {
            const int logicalIdx = routedLogicalIndex(outPair, liveRoute);
            if (logicalIdx < 0) {
                continue;
            }

            const size_t li = static_cast<size_t>(logicalIdx);
            StereoFifo& fifo = fifos[li];
            trimFifoToCursor(fifo, outputCursor48k);
            if (!fifo.has_pts || fifo.start_pts == AV_NOPTS_VALUE || fifo.empty()) {
                continue;
            }

            const int64_t fifoStart = fifo.start_pts;
            const int64_t available = static_cast<int64_t>(fifo.queuedSamples());
            const int64_t fifoEnd = fifoStart + available;
            const int64_t outStart = outputCursor48k;
            const int64_t outEnd = outputCursor48k + fixedChunkSamples;
            const int64_t copyStart = std::max<int64_t>(fifoStart, outStart);
            const int64_t copyEnd = std::min<int64_t>(fifoEnd, outEnd);
            if (copyEnd <= copyStart) {
                continue;
            }

            const int64_t srcOffset = copyStart - fifoStart;
            const int64_t dstOffset = copyStart - outStart;
            const int64_t copySamples = copyEnd - copyStart;
            for (int64_t sampleIdx = 0; sampleIdx < copySamples; ++sampleIdx) {
                const size_t srcOffsetSamples = static_cast<size_t>(srcOffset + sampleIdx);
                int16_t* frameBase = dst + static_cast<size_t>(dstOffset + sampleIdx) * static_cast<size_t>(outputChannels);
                frameBase[outPair * 2u + 0u] = fifo.sampleAt(srcOffsetSamples, 0u);
                frameBase[outPair * 2u + 1u] = fifo.sampleAt(srcOffsetSamples, 1u);
            }

            const int64_t consumeUntil = std::min<int64_t>(fifoEnd, outEnd);
            if (consumeUntil48k[li] == AV_NOPTS_VALUE || consumeUntil > consumeUntil48k[li]) {
                consumeUntil48k[li] = consumeUntil;
            }
        }

        for (size_t li = 0; li < fifos.size(); ++li) {
            StereoFifo& fifo = fifos[li];
            const int64_t consumeUntil = consumeUntil48k[li];
            if (consumeUntil == AV_NOPTS_VALUE ||
                !fifo.has_pts || fifo.start_pts == AV_NOPTS_VALUE || fifo.empty()) {
                continue;
            }

            const int64_t available = static_cast<int64_t>(fifo.queuedSamples());
            const int64_t fifoEnd = fifo.start_pts + available;
            const int64_t clampedConsumeUntil = std::min<int64_t>(consumeUntil, fifoEnd);
            const int64_t consumeSamples = std::max<int64_t>(0, clampedConsumeUntil - fifo.start_pts);

            if (consumeSamples > 0) {
                fifo.dropSamples(static_cast<size_t>(consumeSamples));
            }
        }

        pushPackedAudioFrame(std::move(out));
        packed_audio_pts_ = outputCursor48k + fixedChunkSamples;
        outputCursor48k += fixedChunkSamples;
    }

    audio_fifo_samples_.store(0, std::memory_order_release);
}