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
 * MPEG-TS muxer implementation. This module wraps FFmpeg libavformat with custom in-memory IO so encoded video/audio packets can be muxed into MPEG-TS chunks for live UDP, RTP, or SRT output.
 */

#include "output/muxer_ts.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <utility>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
}

namespace
{

static std::string ffErrStr(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), errnum);
    return std::string(buf);
}

} // namespace

// Preallocate reusable MPEG-TS output chunks. The muxer writes through FFmpeg
// custom IO into this pool so the transport thread can pop complete chunks
// without allocating for every callback.
MuxerTS::ChunkPool::ChunkPool(size_t chunkSize, size_t preallocate)
    : chunk_size_(chunkSize)
{
    storage_.reserve(preallocate);
    for (size_t i = 0; i < preallocate; ++i) {
        storage_.emplace_back(new uint8_t[chunk_size_]);
        free_indices_.push(i);
    }
}

std::shared_ptr<uint8_t> MuxerTS::ChunkPool::acquire()
{
    std::lock_guard<std::mutex> lk(mtx_);

    size_t idx = 0;
    if (!free_indices_.empty()) {
        idx = free_indices_.front();
        free_indices_.pop();
    } else {
        idx = storage_.size();
        storage_.emplace_back(new uint8_t[chunk_size_]);
    }

    return std::shared_ptr<uint8_t>(
        storage_[idx].get(),
        [this, idx](uint8_t*) {
            std::lock_guard<std::mutex> lk2(mtx_);
            free_indices_.push(idx);
        });
}

MuxerTS::MuxerTS()
    : chunk_pool_(kOutputChunkSize, kOutputChunkCount)
{
    ts_partial_packet_.reserve(static_cast<size_t>(kTsPacketSize));
}

MuxerTS::~MuxerTS()
{
    flushOutput();
    destroyFormatContext(true);

    if (video_cfg_.codecpar) {
        avcodec_parameters_free(&video_cfg_.codecpar);
    }

    for (size_t i = 0; i < audio_cfgs_.size(); ++i) {
        if (audio_cfgs_[i].codecpar) {
            avcodec_parameters_free(&audio_cfgs_[i].codecpar);
        }
    }
    audio_cfgs_.clear();

    if (capture_file_.is_open()) {
        capture_file_.flush();
        capture_file_.close();
    }
}

std::string MuxerTS::getLastError() const
{
    std::lock_guard<std::mutex> lk(err_mutex_);
    return last_error_;
}

MuxerTS::TsPacketStats MuxerTS::getTsPacketStats() const
{
    TsPacketStats s;
    s.media_packets = ts_media_packets_.load(std::memory_order_relaxed);
    s.null_packets = ts_null_packets_.load(std::memory_order_relaxed);
    s.sync_errors = ts_sync_errors_.load(std::memory_order_relaxed);
    s.partial_flushes = ts_partial_flushes_.load(std::memory_order_relaxed);
    s.output_bytes = ts_output_bytes_.load(std::memory_order_relaxed);
    s.partial_bytes = ts_partial_bytes_.load(std::memory_order_relaxed);
    return s;
}

void MuxerTS::makeNullPacket(uint8_t out[kTsPacketSize])
{
    if (!out) {
        return;
    }

    out[0] = 0x47;
    out[1] = 0x1f;
    out[2] = 0xff;
    out[3] = 0x10; // payload only, PID 0x1FFF, continuity counter unused
    std::memset(out + 4, 0xff, static_cast<size_t>(kTsPacketSize - 4));
}

bool MuxerTS::emitNullPacket()
{
    uint8_t nullPacket[kTsPacketSize];
    makeNullPacket(nullPacket);
    return appendCompleteTsPacket(nullPacket, true);
}

void MuxerTS::setServiceMetadata(const std::string& provider, const std::string& name)
{
    if (!provider.empty()) {
        service_provider_ = provider;
    }
    if (!name.empty()) {
        service_name_ = name;
    }
}

void MuxerTS::setMuxrateBps(int64_t muxrateBps)
{
    // User-facing muxrate is kept here for logging/session visibility only.
    // Do NOT pass it to FFmpeg's mpegts muxer option yet: FFmpeg's internal
    // CBR/null-packet muxrate mode can advance PCR ahead of NxFrame's live DTS
    // timeline and produce repeated "dts < pcr" warnings with this pipeline.
    // NxFrame currently uses this value as the transport pacing rate in
    // OutputManager/SRT/UDP. True TS null-packet padding is implemented in
    // NxFrame with setNullStuffingEnabled(true).
    muxrate_bps_ = std::max<int64_t>(0, muxrateBps);
}

void MuxerTS::setNullStuffingEnabled(bool enabled)
{
    if (null_stuffing_enabled_ == enabled) {
        return;
    }

    null_stuffing_enabled_ = enabled;

    // Switching modes must not leak bytes from the other output path.
    {
        std::lock_guard<std::mutex> lk(ready_chunks_mutex_);
        ready_chunks_.clear();
    }
    current_chunk_.reset();
    current_chunk_bytes_ = 0;
    clearPartialTsPacket();
    {
        std::lock_guard<std::mutex> lk(cbr_queue_mutex_);
        cbr_packet_queue_.clear();
    }
}

size_t MuxerTS::getCbrQueueDepth() const
{
    std::lock_guard<std::mutex> lk(cbr_queue_mutex_);
    return cbr_packet_queue_.size();
}

void MuxerTS::enableTsFileCapture(const std::string& path)
{
    disableTsFileCapture();

    if (path.empty()) {
        return;
    }

    capture_file_.open(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!capture_file_) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to open TS capture file: " + path;
        return;
    }

    capture_path_ = path;
    std::cout << "[MuxerTS] TS capture enabled: " << capture_path_ << "\n";
}

void MuxerTS::disableTsFileCapture()
{
    if (capture_file_.is_open()) {
        capture_file_.flush();
        capture_file_.close();
    }
    capture_path_.clear();
}

void MuxerTS::enableTimestampDebug(bool enabled, int maxPacketsPerStream)
{
    timestamp_debug_enabled_ = enabled;

    video_debug_ = StreamDebugState{};
    video_debug_.max_to_print = std::max(0, maxPacketsPerStream);

    audio_debugs_.assign(std::max<size_t>(1, audio_cfgs_.size()), StreamDebugState{});
    for (size_t i = 0; i < audio_debugs_.size(); ++i) {
        audio_debugs_[i].max_to_print = std::max(0, maxPacketsPerStream);
    }
}

void MuxerTS::resetTimestampState(const char* reason)
{
    video_base_set_ = false;
    audio_base_set_.assign(audio_cfgs_.size(), 0);
    video_base_pts_ = AV_NOPTS_VALUE;
    audio_base_pts_.assign(audio_cfgs_.size(), AV_NOPTS_VALUE);
    last_pcr_90k_ = -1;
    session_base_90k_set_ = false;
    video_session_anchor_set_ = false;
    session_base_90k_ = AV_NOPTS_VALUE;
    first_video_base_90k_ = AV_NOPTS_VALUE;
    first_audio_base_90k_.assign(audio_cfgs_.size(), AV_NOPTS_VALUE);
    ++session_id_;

    video_debug_.last_pts = AV_NOPTS_VALUE;
    video_debug_.last_dts = AV_NOPTS_VALUE;
    video_debug_.last_duration = 0;
    video_debug_.printed = 0;

    for (size_t i = 0; i < audio_debugs_.size(); ++i) {
        audio_debugs_[i].last_pts = AV_NOPTS_VALUE;
        audio_debugs_[i].last_dts = AV_NOPTS_VALUE;
        audio_debugs_[i].last_duration = 0;
        audio_debugs_[i].printed = 0;
    }

    std::cout << "[MuxerTS][session=" << session_id_ << "] Timestamp state reset"
              << " reason=" << (reason ? reason : "unknown") << "\n";
}

AVCodecParameters* MuxerTS::cloneCodecParametersFromContext(AVCodecContext* ctx)
{
    if (!ctx) {
        return nullptr;
    }

    AVCodecParameters* cp = avcodec_parameters_alloc();
    if (!cp) {
        return nullptr;
    }

    if (avcodec_parameters_from_context(cp, ctx) < 0) {
        avcodec_parameters_free(&cp);
        return nullptr;
    }

    return cp;
}

void MuxerTS::setVideoCodecContext(AVCodecContext* ctx)
{
    if (video_cfg_.codecpar) {
        avcodec_parameters_free(&video_cfg_.codecpar);
    }

    video_cfg_.codecpar = cloneCodecParametersFromContext(ctx);
    video_cfg_.encoder_time_base = ctx ? ctx->time_base : AVRational{0, 1};

    if (!video_cfg_.codecpar) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to store video codec parameters.";
    }
}

void MuxerTS::setAudioCodecContext(AVCodecContext* ctx)
{
    std::vector<AVCodecContext*> tmp;
    if (ctx) {
        tmp.push_back(ctx);
    }
    setAudioCodecContexts(tmp);
}

void MuxerTS::setAudioCodecContexts(const std::vector<AVCodecContext*>& ctxs)
{
    for (size_t i = 0; i < audio_cfgs_.size(); ++i) {
        if (audio_cfgs_[i].codecpar) {
            avcodec_parameters_free(&audio_cfgs_[i].codecpar);
        }
    }
    audio_cfgs_.clear();

    for (size_t i = 0; i < ctxs.size(); ++i) {
        AVCodecContext* ctx = ctxs[i];
        if (!ctx) {
            continue;
        }

        StoredStreamConfig cfg;
        cfg.codecpar = cloneCodecParametersFromContext(ctx);
        cfg.encoder_time_base =
            (ctx->time_base.num > 0 && ctx->time_base.den > 0)
                ? ctx->time_base
                : AVRational{1, std::max(1, ctx->sample_rate)};

        if (!cfg.codecpar) {
            std::lock_guard<std::mutex> lk(err_mutex_);
            last_error_ = "Failed to store audio codec parameters for stream " + std::to_string(i);
            continue;
        }

        audio_cfgs_.push_back(cfg);
    }

    audio_base_set_.assign(audio_cfgs_.size(), 0);
    audio_base_pts_.assign(audio_cfgs_.size(), AV_NOPTS_VALUE);
    first_audio_base_90k_.assign(audio_cfgs_.size(), AV_NOPTS_VALUE);
    audio_debugs_.assign(std::max<size_t>(1, audio_cfgs_.size()), StreamDebugState{});
}

// Create the FFmpeg MPEG-TS muxing context and attach a custom AVIO writer.
// The writer appends bytes into NxFrame-owned output chunks instead of writing
// directly to a file descriptor.
bool MuxerTS::createFormatContext()
{
    destroyFormatContext(false);

    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, "mpegts", nullptr);
    if (ret < 0 || !format_ctx_) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to create MPEG-TS format context.";
        format_ctx_ = nullptr;
        return false;
    }

    io_buffer_ = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
    if (!io_buffer_) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to allocate muxer IO buffer.";
        destroyFormatContext(false);
        return false;
    }

    io_ctx_ = avio_alloc_context(io_buffer_,
                                 static_cast<int>(kIoBufferSize),
                                 1,
                                 this,
                                 nullptr,
                                 &MuxerTS::writePacketCallback,
                                 nullptr);
    if (!io_ctx_) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to allocate AVIOContext.";
        destroyFormatContext(false);
        return false;
    }

    format_ctx_->pb = io_ctx_;
    format_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;
    format_ctx_->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    format_ctx_->max_interleave_delta = 0;

    return true;
}

void MuxerTS::destroyFormatContext(bool writeTrailer)
{
    if (format_ctx_) {
        if (writeTrailer && header_written_) {
            const int ret = av_write_trailer(format_ctx_);
            if (ret < 0) {
                std::cerr << "[MuxerTS] WARNING: av_write_trailer failed: "
                          << ffErrStr(ret) << "\n";
            }
        }
        if (format_ctx_->pb) {
            avio_flush(format_ctx_->pb);
        }
        flushPartialTsPacket();

        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }

    if (io_ctx_) {
        if (io_ctx_->buffer) {
            av_free(io_ctx_->buffer);
            io_ctx_->buffer = nullptr;
        }
        avio_context_free(&io_ctx_);
    }

    io_buffer_ = nullptr;
    video_stream_ = nullptr;
    audio_streams_.clear();
    header_written_ = false;
}

// Rebuild libavformat streams from cloned codec parameters. Encoder contexts
// may outlive or restart independently, so the muxer stores its own stream
// description before writing the transport header.
bool MuxerTS::configureStreamsFromStored()
{
    if (!format_ctx_ || !video_cfg_.codecpar) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "No stored video codec parameters available.";
        return false;
    }

    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to create MPEG-TS video stream.";
        return false;
    }

    if (avcodec_parameters_copy(video_stream_->codecpar, video_cfg_.codecpar) < 0) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to copy stored video codec parameters.";
        return false;
    }

    video_stream_->time_base = AVRational{1, 90000};
    video_stream_->id = 256;
    video_stream_->codecpar->codec_tag = 0;

    if (video_cfg_.codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        audio_streams_.clear();

        int nextId = 257;
        for (size_t i = 0; i < audio_cfgs_.size(); ++i) {
            if (!audio_cfgs_[i].codecpar) {
                continue;
            }

            AVStream* st = avformat_new_stream(format_ctx_, nullptr);
            if (!st) {
                std::lock_guard<std::mutex> lk(err_mutex_);
                last_error_ = "Failed to create MPEG-TS audio stream " + std::to_string(i);
                return false;
            }

            if (avcodec_parameters_copy(st->codecpar, audio_cfgs_[i].codecpar) < 0) {
                std::lock_guard<std::mutex> lk(err_mutex_);
                last_error_ = "Failed to copy stored audio codec parameters for stream " +
                              std::to_string(i);
                return false;
            }

            st->time_base = AVRational{1, 90000};
            st->id = nextId++;
            st->codecpar->codec_tag = 0;
            audio_streams_.push_back(st);
        }
    }

    audio_base_set_.assign(audio_streams_.size(), 0);
    audio_base_pts_.assign(audio_streams_.size(), AV_NOPTS_VALUE);
    first_audio_base_90k_.assign(audio_streams_.size(), AV_NOPTS_VALUE);
    audio_debugs_.assign(std::max<size_t>(1, audio_streams_.size()), StreamDebugState{});

    return true;
}

bool MuxerTS::initialize()
{
    if (session_id_ == 0) {
        resetTimestampState("initial-start");
    }

    if (!video_cfg_.codecpar) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "No video codec context configured before muxer initialize.";
        return false;
    }

    if (!format_ctx_ && !createFormatContext()) {
        return false;
    }

    if (!video_stream_ && !configureStreamsFromStored()) {
        return false;
    }

    format_ctx_->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    format_ctx_->max_interleave_delta = 0;

    if (format_ctx_->priv_data) {
        av_opt_set(format_ctx_->priv_data,
                   "mpegts_flags",
                   "+initial_discontinuity+resend_headers",
                   0);
        // Production safety: do not set FFmpeg's "muxrate" option here.
        // See setMuxrateBps() above. The user preset muxrate is applied as
        // transport pacing, not FFmpeg PCR/null-packet pacing.
    }

    if (!service_provider_.empty()) {
        av_dict_set(&format_ctx_->metadata, "service_provider", service_provider_.c_str(), 0);
    }
    if (!service_name_.empty()) {
        av_dict_set(&format_ctx_->metadata, "service_name", service_name_.c_str(), 0);
    }

    const int ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to write MPEG-TS header: " + ffErrStr(ret);
        return false;
    }

    header_written_ = true;
    flushOutput();

    std::cout << "[MuxerTS][session=" << session_id_ << "] Header written successfully. audio_streams="
              << audio_streams_.size()
              << " transport_muxrate=" << muxrate_bps_ << "bps"
              << " ffmpeg_muxrate=disabled"
              << " video_tb=" << video_stream_->time_base.num << "/" << video_stream_->time_base.den;
    for (size_t i = 0; i < audio_streams_.size(); ++i) {
        if (audio_streams_[i]) {
            std::cout << " audio" << i << "_tb="
                      << audio_streams_[i]->time_base.num << "/"
                      << audio_streams_[i]->time_base.den;
        }
    }
    std::cout << "\n";
    return true;
}

int64_t MuxerTS::packetTimestampReference(const AVPacket* pkt)
{
    if (!pkt) {
        return AV_NOPTS_VALUE;
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        return pkt->dts;
    }
    return pkt->pts;
}

void MuxerTS::ensurePacketPtsDtsPair(AVPacket* pkt)
{
    if (!pkt) {
        return;
    }

    if (pkt->pts == AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE) {
        pkt->pts = pkt->dts;
    }
    if (pkt->dts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) {
        pkt->dts = pkt->pts;
    }
}

// Live contribution streams should start each muxer session from a stable
// local timestamp base. This avoids very large incoming encoder timestamps
// leaking into the MPEG-TS output after recovery/restart.
void MuxerTS::normalizePacketTsToLiveSessionBase(AVPacket* pkt, const char* tag)
{
    if (!pkt) {
        return;
    }

    ensurePacketPtsDtsPair(pkt);

    const int64_t ref90k = packetTimestampReference(pkt);
    if (!session_base_90k_set_ && ref90k != AV_NOPTS_VALUE) {
        session_base_90k_ = ref90k;
        session_base_90k_set_ = true;
        std::cout << "[MuxerTS][session=" << session_id_
                  << "] Live timestamp base anchored by "
                  << (tag ? tag : "unknown")
                  << " base90k=" << session_base_90k_ << "\n";
    }

    if (!session_base_90k_set_) {
        return;
    }

    if (pkt->pts != AV_NOPTS_VALUE) {
        pkt->pts -= session_base_90k_;
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        pkt->dts -= session_base_90k_;
    }
}

void MuxerTS::debugPrintPacket(const char* phase,
                               const char* tag,
                               const AVPacket* pkt,
                               AVRational tb,
                               bool normalized,
                               int64_t basePts,
                               const StreamDebugState& st) const
{
    if (!timestamp_debug_enabled_ || !pkt || st.printed >= st.max_to_print) {
        return;
    }

    const double ptsSec =
        (pkt->pts != AV_NOPTS_VALUE && tb.den > 0)
            ? av_q2d(tb) * static_cast<double>(pkt->pts)
            : -1.0;
    const double dtsSec =
        (pkt->dts != AV_NOPTS_VALUE && tb.den > 0)
            ? av_q2d(tb) * static_cast<double>(pkt->dts)
            : -1.0;
    const double durSec =
        (pkt->duration > 0 && tb.den > 0)
            ? av_q2d(tb) * static_cast<double>(pkt->duration)
            : 0.0;

    std::cout << "[MuxerTS][" << tag << "][" << phase << "] "
              << "pts=" << pkt->pts
              << " dts=" << pkt->dts
              << " dur=" << pkt->duration
              << " pts_s=" << std::fixed << std::setprecision(6) << ptsSec
              << " dts_s=" << dtsSec
              << " dur_s=" << durSec
              << " normalized=" << (normalized ? "yes" : "no")
              << " base=" << basePts
              << "\n";
}

void MuxerTS::logTimestampBase(const char* tag,
                                 int streamIndex,
                                 int64_t rawBasePts,
                                 AVRational rawTimeBase,
                                 int64_t base90k)
{
    if (!timestamp_debug_enabled_) {
        return;
    }

    const double baseSec =
        (rawBasePts != AV_NOPTS_VALUE && rawTimeBase.den > 0)
            ? av_q2d(rawTimeBase) * static_cast<double>(rawBasePts)
            : -1.0;

    std::cout << "[MuxerTS][session=" << session_id_ << "][" << tag << "] first timestamp base"
              << " stream_index=" << streamIndex
              << " raw_base=" << rawBasePts
              << " raw_tb=" << rawTimeBase.num << "/" << rawTimeBase.den
              << " raw_s=" << std::fixed << std::setprecision(6) << baseSec
              << " base90k=" << base90k;

    if (std::strcmp(tag, "video") == 0) {
        first_video_base_90k_ = base90k;
    } else if (std::strcmp(tag, "audio") == 0 && streamIndex >= 0) {
        const int audioIndex = streamIndex - 1;
        if (audioIndex >= 0) {
            if (audioIndex >= static_cast<int>(first_audio_base_90k_.size())) {
                first_audio_base_90k_.resize(static_cast<size_t>(audioIndex + 1), AV_NOPTS_VALUE);
            }
            first_audio_base_90k_[static_cast<size_t>(audioIndex)] = base90k;
        }

        if (first_video_base_90k_ != AV_NOPTS_VALUE && base90k != AV_NOPTS_VALUE) {
            const int64_t delta90k = base90k - first_video_base_90k_;
            const double deltaMs = static_cast<double>(delta90k) / 90.0;
            std::cout << " av_offset_vs_video_ms=" << std::setprecision(3) << deltaMs;
        }
    }

    std::cout << "\n";
}

bool MuxerTS::validatePacketPreRescale(const char* tag, AVPacket* pkt, StreamDebugState& st)
{
    if (!pkt) {
        return false;
    }

    if (pkt->duration < 0) {
        std::cerr << "[MuxerTS][" << tag << "] WARNING: negative duration before rescale: "
                  << pkt->duration << "\n";
    }

    if (pkt->pts != AV_NOPTS_VALUE &&
        pkt->dts != AV_NOPTS_VALUE &&
        pkt->pts < pkt->dts) {
        std::cerr << "[MuxerTS][" << tag << "] WARNING: pts < dts before rescale"
                  << " pts=" << pkt->pts << " dts=" << pkt->dts << "\n";
    }

    return true;
}

bool MuxerTS::validatePacketPostRescale(const char* tag,
                                        AVPacket* pkt,
                                        StreamDebugState& st,
                                        AVRational tb,
                                        bool normalized,
                                        int64_t basePts) const
{
    if (!pkt) {
        return false;
    }

    if (pkt->duration < 0) {
        std::cerr << "[MuxerTS][" << tag << "] WARNING: negative duration after rescale: "
                  << pkt->duration << "\n";
    }

    if (pkt->pts != AV_NOPTS_VALUE &&
        pkt->dts != AV_NOPTS_VALUE &&
        pkt->pts < pkt->dts) {
        std::cerr << "[MuxerTS][" << tag << "] WARNING: pts < dts after rescale"
                  << " pts=" << pkt->pts << " dts=" << pkt->dts << "\n";
    }

    if (st.last_dts != AV_NOPTS_VALUE &&
        pkt->dts != AV_NOPTS_VALUE &&
        pkt->dts < st.last_dts) {
        std::cerr << "[MuxerTS][" << tag << "] WARNING: non-monotonic DTS"
                  << " prev=" << st.last_dts << " curr=" << pkt->dts << "\n";
    }

    // Video packets are written in decode order. When B-frames are enabled,
    // DTS remains monotonic but PTS may legally move backwards in packet order
    // because presentation order differs from decode order. Only warn about
    // non-monotonic PTS for streams without an explicit decode timeline.
    const bool videoWithDecodeTimeline =
        tag && std::strcmp(tag, "video") == 0 && pkt->dts != AV_NOPTS_VALUE;
    if (!videoWithDecodeTimeline &&
        st.last_pts != AV_NOPTS_VALUE &&
        pkt->pts != AV_NOPTS_VALUE &&
        pkt->pts < st.last_pts) {
        std::cerr << "[MuxerTS][" << tag << "] WARNING: non-monotonic PTS"
                  << " prev=" << st.last_pts << " curr=" << pkt->pts << "\n";
    }

    debugPrintPacket("post", tag, pkt, tb, normalized, basePts, st);
    return true;
}

// Common video/audio packet path: validate, repair simple timestamp issues,
// rescale to the output stream time base, then hand the packet to FFmpeg for
// MPEG-TS muxing.
bool MuxerTS::writePacketInternal(AVPacket* pkt,
                                  AVStream* stream,
                                  AVRational encTb,
                                  StreamDebugState& dbg,
                                  int64_t& basePts,
                                  bool& baseSet,
                                  const char* tag,
                                  bool updatePcr)
{
    if (!format_ctx_ || !header_written_ || !pkt || !stream) {
        return false;
    }

    AVPacket local;

    if (av_packet_ref(&local, pkt) < 0) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = std::string("av_packet_ref failed for ") + tag;
        return false;
    }

    if (std::strcmp(tag, "video") == 0 && local.duration <= 0) {
        local.duration = 1;
    }

    validatePacketPreRescale(tag, &local, dbg);

    ensurePacketPtsDtsPair(&local);

    const int64_t rawRef = packetTimestampReference(&local);
    const bool firstBaseForStream = !baseSet && rawRef != AV_NOPTS_VALUE;
    const int64_t rawBase90k =
        (firstBaseForStream && encTb.num > 0 && encTb.den > 0)
            ? av_rescale_q(rawRef, encTb, AVRational{1, 90000})
            : AV_NOPTS_VALUE;

    if (firstBaseForStream) {
        basePts = rawRef;
        baseSet = true;
        logTimestampBase(tag, stream->index, rawRef, encTb, rawBase90k);
    }

    av_packet_rescale_ts(&local, encTb, stream->time_base);
    normalizePacketTsToLiveSessionBase(&local, tag);

    local.stream_index = stream->index;
    local.pos = -1;

    // The muxer must not become the timestamp authority. It may fill a missing
    // PTS/DTS pair and a missing duration, but it must not rewrite broken
    // monotonic timestamps. A violation here means the upstream encoder/timing
    // path has produced an invalid packet for MPEG-TS output.
    if (std::strcmp(tag, "video") == 0) {
        if (local.pts == AV_NOPTS_VALUE && local.dts != AV_NOPTS_VALUE) {
            local.pts = local.dts;
        }
        if (local.dts == AV_NOPTS_VALUE && local.pts != AV_NOPTS_VALUE) {
            local.dts = local.pts;
        }

        int64_t nominal_step = av_rescale_q(1, encTb, stream->time_base);
        if (nominal_step <= 0) {
            nominal_step = 1;
        }

        if (local.duration <= 0) {
            local.duration = nominal_step;
        }

        if (dbg.last_dts != AV_NOPTS_VALUE &&
            local.dts != AV_NOPTS_VALUE &&
            local.dts <= dbg.last_dts) {
            const uint64_t violations =
                video_dts_repair_count_.fetch_add(1, std::memory_order_relaxed) + 1;

            std::lock_guard<std::mutex> lk(err_mutex_);
            last_error_ = "MuxerTS rejected non-monotonic video DTS: prev=" +
                          std::to_string(dbg.last_dts) +
                          " curr=" + std::to_string(local.dts) +
                          " violations=" + std::to_string(violations);

            std::cerr << "[MuxerTS][video] ERROR: " << last_error_
                      << "; upstream timestamp generation must be fixed.\n";
            av_packet_unref(&local);
            return false;
        }

        // Do not reject non-monotonic video PTS here. MPEG-TS packets are
        // emitted in decode order, and with B-frames the presentation timestamp
        // can go backwards while DTS/PCR still increases monotonically. The DTS
        // check above remains the hard guard against invalid decode order. For
        // no-B-frame streams DTS==PTS, so the DTS check still protects the
        // existing low-latency path.
    }

    validatePacketPostRescale(tag, &local, dbg, stream->time_base, baseSet, basePts);

    dbg.last_pts = local.pts;
    dbg.last_dts = local.dts;
    dbg.last_duration = local.duration;
    if (dbg.printed < dbg.max_to_print) {
        ++dbg.printed;
    }

    if (updatePcr) {
        const int64_t pcr = (local.dts != AV_NOPTS_VALUE) ? local.dts : local.pts;
        if (pcr != AV_NOPTS_VALUE) {
            last_pcr_90k_ = pcr;
        }
    }

    const int ret = av_interleaved_write_frame(format_ctx_, &local);
    av_packet_unref(&local);

    if (ret < 0) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = std::string("av_interleaved_write_frame failed for ") +
                      tag + ": " + ffErrStr(ret);
        return false;
    }

    return true;
}

bool MuxerTS::writeVideoPacket(AVPacket* pkt)
{
    if (!video_stream_ || !video_cfg_.codecpar) {
        return false;
    }

    if (!pkt) {
        return false;
    }

    if (!video_session_anchor_set_) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) == 0) {
            std::lock_guard<std::mutex> lk(err_mutex_);
            last_error_ = "MuxerTS live session must start on a video keyframe";
            std::cerr << "[MuxerTS][video] ERROR: " << last_error_ << "\n";
            return false;
        }
        video_session_anchor_set_ = true;
    }

    return writePacketInternal(pkt,
                               video_stream_,
                               (video_cfg_.encoder_time_base.num > 0 &&
                                video_cfg_.encoder_time_base.den > 0)
                                   ? video_cfg_.encoder_time_base
                                   : video_stream_->time_base,
                               video_debug_,
                               video_base_pts_,
                               video_base_set_,
                               "video",
                               true);
}

bool MuxerTS::writeAudioPacket(AVPacket* pkt)
{
    if (!pkt || audio_streams_.empty()) {
        return false;
    }

    if (!video_session_anchor_set_) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "MuxerTS rejected audio before video keyframe session anchor";
        std::cerr << "[MuxerTS][audio] ERROR: " << last_error_ << "\n";
        return false;
    }

    int logicalIndex = pkt->stream_index;
    if (logicalIndex < 0 || logicalIndex >= static_cast<int>(audio_streams_.size())) {
        std::cerr << "[MuxerTS] WARNING: invalid logical audio stream_index="
                  << pkt->stream_index << ", forcing 0\n";
        logicalIndex = 0;
    }

    bool baseSet = (audio_base_set_[logicalIndex] != 0);
    const bool ok = writePacketInternal(pkt,
                                        audio_streams_[logicalIndex],
                                        audio_cfgs_[logicalIndex].encoder_time_base,
                                        audio_debugs_[logicalIndex],
                                        audio_base_pts_[logicalIndex],
                                        baseSet,
                                        "audio",
                                        false);
    audio_base_set_[logicalIndex] = baseSet ? 1 : 0;
    return ok;
}

bool MuxerTS::ensureCurrentChunk()
{
    if (current_chunk_) {
        return true;
    }

    current_chunk_ = chunk_pool_.acquire();
    current_chunk_bytes_ = 0;
    return static_cast<bool>(current_chunk_);
}

void MuxerTS::sealCurrentChunk()
{
    if (!current_chunk_ || current_chunk_bytes_ == 0) {
        current_chunk_.reset();
        current_chunk_bytes_ = 0;
        return;
    }

    std::lock_guard<std::mutex> lk(ready_chunks_mutex_);
    ready_chunks_.push_back(OutputChunk{current_chunk_, current_chunk_bytes_});
    current_chunk_.reset();
    current_chunk_bytes_ = 0;
}

// Low-level byte appender used after the MPEG-TS packetizer has accepted
// complete media packets, and by the null packet generator. This preserves the
// existing chunk-pool behaviour while allowing the future true-CBR scheduler to
// reason in 188-byte TS packet units.
bool MuxerTS::appendOutputRawBytes(const uint8_t* buf, size_t len)
{
    if (!buf || len == 0) {
        return true;
    }

    if (capture_file_.is_open()) {
        capture_file_.write(reinterpret_cast<const char*>(buf),
                            static_cast<std::streamsize>(len));
    }

    ts_output_bytes_.fetch_add(static_cast<uint64_t>(len), std::memory_order_relaxed);

    while (len > 0) {
        if (!ensureCurrentChunk()) {
            std::lock_guard<std::mutex> lk(err_mutex_);
            last_error_ = "Failed to acquire output chunk.";
            return false;
        }

        const size_t capacity = chunk_pool_.chunkSize();
        const size_t space = capacity - current_chunk_bytes_;
        const size_t copyBytes = std::min(space, len);

        std::memcpy(current_chunk_.get() + current_chunk_bytes_, buf, copyBytes);
        current_chunk_bytes_ += copyBytes;
        buf += copyBytes;
        len -= copyBytes;

        if (current_chunk_bytes_ == capacity) {
            sealCurrentChunk();
        }
    }

    return true;
}

bool MuxerTS::appendCompleteTsPacket(const uint8_t* packet, bool isNullPacket)
{
    if (!packet) {
        return true;
    }

    if (packet[0] != 0x47) {
        const uint64_t syncErrors =
            ts_sync_errors_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (syncErrors <= 10 || (syncErrors % 1000) == 0) {
            std::cerr << "[MuxerTS] WARNING: MPEG-TS packet sync byte mismatch"
                      << " count=" << syncErrors
                      << " first_byte=0x" << std::hex << static_cast<int>(packet[0])
                      << std::dec << "\n";
        }
    }

    if (isNullPacket) {
        ts_null_packets_.fetch_add(1, std::memory_order_relaxed);
    } else {
        ts_media_packets_.fetch_add(1, std::memory_order_relaxed);
    }

    if (null_stuffing_enabled_) {
        if (isNullPacket) {
            return appendOutputRawBytes(packet, static_cast<size_t>(kTsPacketSize));
        }
        return enqueueCbrPacket(packet);
    }

    return appendOutputRawBytes(packet, static_cast<size_t>(kTsPacketSize));
}

bool MuxerTS::enqueueCbrPacket(const uint8_t* packet)
{
    if (!packet) {
        return true;
    }

    std::vector<uint8_t> stored(static_cast<size_t>(kTsPacketSize));
    std::memcpy(stored.data(), packet, static_cast<size_t>(kTsPacketSize));

    std::lock_guard<std::mutex> lk(cbr_queue_mutex_);
    // Keep this bounded enough to avoid unbounded memory growth if the output
    // muxrate is lower than the media rate. 4096 TS packets is ~770 KiB and is
    // already far more than a live sender should accumulate.
    static const size_t kMaxQueuedPackets = 4096;
    if (cbr_packet_queue_.size() >= kMaxQueuedPackets) {
        cbr_queue_overflows_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> errlk(err_mutex_);
        last_error_ = "true-CBR TS queue overflow; muxrate is too low or sender is blocked";
        return false;
    }

    cbr_packet_queue_.push_back(std::move(stored));
    return true;
}

bool MuxerTS::popQueuedCbrPacket(uint8_t* dst)
{
    if (!dst) {
        return false;
    }

    std::lock_guard<std::mutex> lk(cbr_queue_mutex_);
    if (cbr_packet_queue_.empty()) {
        return false;
    }

    std::memcpy(dst, cbr_packet_queue_.front().data(), static_cast<size_t>(kTsPacketSize));
    cbr_packet_queue_.pop_front();
    return true;
}

bool MuxerTS::popCbrPayload(OutputChunk& out, size_t payloadBytes, bool& containsMedia)
{
    out = OutputChunk{};
    containsMedia = false;

    if (!null_stuffing_enabled_) {
        return false;
    }

    payloadBytes = (payloadBytes / static_cast<size_t>(kTsPacketSize)) * static_cast<size_t>(kTsPacketSize);
    if (payloadBytes < static_cast<size_t>(kTsPacketSize)) {
        payloadBytes = static_cast<size_t>(kTsPacketSize);
    }

    std::shared_ptr<uint8_t> payload = chunk_pool_.acquire();
    if (!payload) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "Failed to acquire true-CBR output payload.";
        return false;
    }

    if (payloadBytes > chunk_pool_.chunkSize()) {
        payloadBytes = (chunk_pool_.chunkSize() / static_cast<size_t>(kTsPacketSize)) * static_cast<size_t>(kTsPacketSize);
    }

    uint8_t nullPacket[kTsPacketSize];
    makeNullPacket(nullPacket);

    for (size_t pos = 0; pos < payloadBytes; pos += static_cast<size_t>(kTsPacketSize)) {
        if (popQueuedCbrPacket(payload.get() + pos)) {
            containsMedia = true;
        } else {
            std::memcpy(payload.get() + pos, nullPacket, static_cast<size_t>(kTsPacketSize));
            ts_null_packets_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (capture_file_.is_open()) {
        capture_file_.write(reinterpret_cast<const char*>(payload.get()),
                            static_cast<std::streamsize>(payloadBytes));
    }
    ts_output_bytes_.fetch_add(static_cast<uint64_t>(payloadBytes), std::memory_order_relaxed);

    out.data = payload;
    out.size = payloadBytes;
    return true;
}

bool MuxerTS::flushPartialTsPacket()
{
    if (ts_partial_packet_.empty()) {
        ts_partial_bytes_.store(0, std::memory_order_relaxed);
        return true;
    }

    const size_t partialBytes = ts_partial_packet_.size();
    ts_partial_flushes_.fetch_add(1, std::memory_order_relaxed);
    ts_partial_bytes_.store(0, std::memory_order_relaxed);

    std::cerr << "[MuxerTS] WARNING: flushing partial MPEG-TS packet bytes="
              << partialBytes << "; expected complete 188-byte packets from FFmpeg\n";

    if (null_stuffing_enabled_) {
        // Never inject a partial TS packet into a true-CBR stream. A final tail
        // here means FFmpeg/custom IO ended mid-packet; drop it and keep the
        // transmitted stream packet-aligned.
        ts_partial_packet_.clear();
        return true;
    }

    const bool ok = appendOutputRawBytes(ts_partial_packet_.data(), partialBytes);
    ts_partial_packet_.clear();
    return ok;
}

void MuxerTS::clearPartialTsPacket()
{
    ts_partial_packet_.clear();
    ts_partial_bytes_.store(0, std::memory_order_relaxed);
}

// FFmpeg calls this through the custom AVIO callback. Bytes are normalized into
// complete 188-byte TS packets first, then appended unchanged to the existing
// output chunk pool. Phase 1 is intentionally a no-op for content: it only adds
// packet-boundary accounting and sync-byte validation.
bool MuxerTS::appendOutputBytes(const uint8_t* buf, size_t len)
{
    if (!buf || len == 0) {
        return true;
    }

    while (len > 0) {
        const size_t need = static_cast<size_t>(kTsPacketSize) - ts_partial_packet_.size();
        const size_t copyBytes = std::min(need, len);

        ts_partial_packet_.insert(ts_partial_packet_.end(), buf, buf + copyBytes);
        ts_partial_bytes_.store(ts_partial_packet_.size(), std::memory_order_relaxed);

        buf += copyBytes;
        len -= copyBytes;

        if (ts_partial_packet_.size() == static_cast<size_t>(kTsPacketSize)) {
            if (!appendCompleteTsPacket(ts_partial_packet_.data(), false)) {
                return false;
            }
            ts_partial_packet_.clear();
            ts_partial_bytes_.store(0, std::memory_order_relaxed);
        }
    }

    return true;
}

void MuxerTS::flushOutput()
{
    if (format_ctx_ && format_ctx_->pb) {
        avio_flush(format_ctx_->pb);
    }

    // Do not flush ts_partial_packet_ here. flushOutput() is called frequently
    // by the live sender loop, and a custom AVIO callback may split data at an
    // arbitrary point. Holding <188 bytes until the next callback preserves TS
    // packet alignment for the transport layer. Any final tail is flushed only
    // when the muxer session is destroyed/recreated.

    if (capture_file_.is_open()) {
        capture_file_.flush();
    }

    sealCurrentChunk();
}

bool MuxerTS::popOutputChunk(OutputChunk& out)
{
    std::lock_guard<std::mutex> lk(ready_chunks_mutex_);
    if (ready_chunks_.empty()) {
        return false;
    }

    out = std::move(ready_chunks_.front());
    ready_chunks_.pop_front();
    return true;
}

void MuxerTS::clearOutputQueue()
{
    std::lock_guard<std::mutex> lk(ready_chunks_mutex_);
    ready_chunks_.clear();
}

size_t MuxerTS::getReadyChunkCount() const
{
    std::lock_guard<std::mutex> lk(ready_chunks_mutex_);
    return ready_chunks_.size();
}

bool MuxerTS::recreateSession(bool writeTrailer)
{
    {
        std::lock_guard<std::mutex> lk(ready_chunks_mutex_);
        ready_chunks_.clear();
    }
    current_chunk_.reset();
    current_chunk_bytes_ = 0;
    clearPartialTsPacket();
    {
        std::lock_guard<std::mutex> lk(cbr_queue_mutex_);
        cbr_packet_queue_.clear();
    }

    resetTimestampState("live-session-reset");

    if (!createFormatContext()) {
        return false;
    }

    if (!configureStreamsFromStored()) {
        destroyFormatContext(false);
        return false;
    }

    if (!initialize()) {
        destroyFormatContext(false);
        return false;
    }

    return true;
}

// Start a new live muxing session while keeping the configured streams. This
// is used by transport recovery so output resumes with clean timestamp state.
bool MuxerTS::resetLiveSession()
{
    if (!video_cfg_.codecpar) {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_ = "resetLiveSession called without stored video codec parameters.";
        return false;
    }

    if (!recreateSession(false)) {
        std::cerr << "[MuxerTS] ERROR: Failed to restart live TS session: "
                  << getLastError() << "\n";
        return false;
    }

    std::cout << "[MuxerTS][session=" << session_id_
              << "] Live session restarted with fresh MPEG-TS header\n";
    return true;
}

int MuxerTS::writePacketCallback(void* opaque, const uint8_t* buf, int buf_size)
{
    MuxerTS* muxer = static_cast<MuxerTS*>(opaque);
    if (!muxer || !buf || buf_size <= 0) {
        return 0;
    }

    return muxer->appendOutputBytes(buf, static_cast<size_t>(buf_size))
               ? buf_size
               : AVERROR(ENOMEM);
}