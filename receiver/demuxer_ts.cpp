/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/demuxer_ts.cpp
 * Description: Implements MPEG-TS demuxing and demuxed packet queues.
 */

#include "receiver/demuxer_ts.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

namespace {

static std::string ffErrStr(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), errnum);
    return std::string(buf);
}

static size_t packetBytes(const DemuxedPacket& pkt) noexcept
{
    return (pkt.pkt ? static_cast<size_t>(pkt.pkt->size) : 0u);
}

static bool sameRational(AVRational a, AVRational b) noexcept
{
    return a.num == b.num && a.den == b.den;
}

static bool sameChannelLayout(const AVChannelLayout& a, const AVChannelLayout& b) noexcept
{
    return av_channel_layout_compare(&a, &b) == 0;
}


std::mutex& avLogLevelMutex()
{
    static std::mutex m;
    return m;
}

class ScopedAvLogLevel
{
public:
    explicit ScopedAvLogLevel(int temporary_level)
        : lock_(avLogLevelMutex()), previous_level_(av_log_get_level())
    {
        av_log_set_level(temporary_level);
    }

    ~ScopedAvLogLevel()
    {
        av_log_set_level(previous_level_);
    }

    ScopedAvLogLevel(const ScopedAvLogLevel&) = delete;
    ScopedAvLogLevel& operator=(const ScopedAvLogLevel&) = delete;

private:
    std::unique_lock<std::mutex> lock_;
    int previous_level_ = AV_LOG_INFO;
};

static bool isAacCodec(AVCodecID codec_id) noexcept
{
    return codec_id == AV_CODEC_ID_AAC;
}

static bool sameCodecParameters(const AVCodecParameters* a, const AVCodecParameters* b) noexcept
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }

    if (a->codec_type != b->codec_type ||
        a->codec_id != b->codec_id) {
        return false;
    }

    if (a->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (a->codec_tag != b->codec_tag ||
            a->format != b->format ||
            a->bit_rate != b->bit_rate ||
            a->profile != b->profile ||
            a->level != b->level) {
            return false;
        }

        if (a->width != b->width ||
            a->height != b->height ||
            a->field_order != b->field_order ||
            a->color_range != b->color_range ||
            a->color_primaries != b->color_primaries ||
            a->color_trc != b->color_trc ||
            a->color_space != b->color_space ||
            a->chroma_location != b->chroma_location ||
            a->sample_aspect_ratio.num != b->sample_aspect_ratio.num ||
            a->sample_aspect_ratio.den != b->sample_aspect_ratio.den ||
            a->video_delay != b->video_delay) {
            return false;
        }
    }

    if (a->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (a->sample_rate != b->sample_rate ||
            !sameChannelLayout(a->ch_layout, b->ch_layout)) {
            return false;
        }

        // AAC in MPEG-TS/ADTS can have codec parameters populated/refined while
        // packets are being read. Those changes do not represent a new live
        // source and must not force a receiver generation reset on every packet.
        if (isAacCodec(a->codec_id)) {
            return true;
        }

        if (a->codec_tag != b->codec_tag ||
            a->format != b->format ||
            a->bit_rate != b->bit_rate ||
            a->profile != b->profile ||
            a->level != b->level ||
            a->frame_size != b->frame_size ||
            a->block_align != b->block_align ||
            a->initial_padding != b->initial_padding ||
            a->trailing_padding != b->trailing_padding) {
            return false;
        }
    }

    if (a->extradata_size != b->extradata_size) {
        return false;
    }
    if (a->extradata_size > 0) {
        if (!a->extradata || !b->extradata) {
            return false;
        }
        if (std::memcmp(a->extradata, b->extradata, static_cast<size_t>(a->extradata_size)) != 0) {
            return false;
        }
    }

    return true;
}

static bool sameStreamInfo(const DemuxerTS::StreamInfo& a,
                           const DemuxerTS::StreamInfo& b) noexcept
{
    if (a.stream_index != b.stream_index ||
        a.pid != b.pid ||
        a.media_type != b.media_type ||
        a.codec_id != b.codec_id ||
        !sameRational(a.time_base, b.time_base)) {
        return false;
    }

    if (a.media_type == AVMEDIA_TYPE_VIDEO) {
        return sameRational(a.avg_frame_rate, b.avg_frame_rate) &&
               sameRational(a.r_frame_rate, b.r_frame_rate);
    }

    if (a.media_type == AVMEDIA_TYPE_AUDIO) {
        return a.sample_rate == b.sample_rate &&
               a.channels == b.channels;
    }

    return true;
}

static bool sameStreamVector(const std::vector<DemuxerTS::StreamInfo>& a,
                             const std::vector<DemuxerTS::StreamInfo>& b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!sameStreamInfo(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

static bool sameCodecParameterMap(
    const std::map<int, DemuxerTS::CodecParametersPtr>& a,
    const std::map<int, DemuxerTS::CodecParametersPtr>& b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }

    std::map<int, DemuxerTS::CodecParametersPtr>::const_iterator ia = a.begin();
    std::map<int, DemuxerTS::CodecParametersPtr>::const_iterator ib = b.begin();
    for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
        if (ia->first != ib->first ||
            !sameCodecParameters(ia->second.get(), ib->second.get())) {
            return false;
        }
    }
    return true;
}

} // namespace

DemuxerTS::DemuxerTS() = default;

DemuxerTS::~DemuxerTS()
{
    stop();
}

int DemuxerTS::interruptCallback(void* opaque)
{
    DemuxerTS* self = static_cast<DemuxerTS*>(opaque);
    if (!self) {
        return 0;
    }
    return self->stop_requested_.load(std::memory_order_acquire) ? 1 : 0;
}

DemuxerTS::CodecParametersPtr DemuxerTS::cloneCodecParametersShared(const AVCodecParameters* src)
{
    if (!src) {
        return CodecParametersPtr();
    }

    AVCodecParameters* cp = avcodec_parameters_alloc();
    if (!cp) {
        return CodecParametersPtr();
    }

    if (avcodec_parameters_copy(cp, src) < 0) {
        avcodec_parameters_free(&cp);
        return CodecParametersPtr();
    }

    return CodecParametersPtr(cp, [](AVCodecParameters* p) {
        if (p) {
            avcodec_parameters_free(&p);
        }
    });
}

bool DemuxerTS::start(const Config& config)
{
    stop();

    config_ = config;
    stop_requested_.store(false, std::memory_order_release);
    end_of_input_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    generation_.fetch_add(1, std::memory_order_acq_rel);

    {
        std::lock_guard<std::mutex> lk(health_mutex_);
        health_ = HealthSnapshot{};
        health_.generation = generation_.load(std::memory_order_acquire);
        ts_cc_by_pid_.clear();
    }

    video_queue_depth_.store(0, std::memory_order_release);
    audio_queue_depth_.store(0, std::memory_order_release);
    video_queued_bytes_.store(0, std::memory_order_release);
    audio_queued_bytes_.store(0, std::memory_order_release);
    input_buffered_bytes_.store(0, std::memory_order_release);

    logged_first_video_packet_ = false;
    acquired_first_video_key_packet_ = false;
    logged_first_audio_packet_by_stream_.clear();

    {
        std::lock_guard<std::mutex> lk(stream_mutex_);
        snapshot_.reset();
    }

    demux_thread_ = std::thread(&DemuxerTS::demuxLoop, this);
    std::cerr << "[DemuxerTS] Start requested. Waiting for transport-fed TS chunks...\n";
    return true;
}

void DemuxerTS::cleanupInput()
{
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    if (io_ctx_) {
        av_freep(&io_ctx_->buffer);
        avio_context_free(&io_ctx_);
        io_ctx_ = nullptr;
    }

    io_buffer_ = nullptr;
    ts_input_fmt_ = nullptr;
}

void DemuxerTS::clearInputBufferLocked()
{
    input_chunks_.clear();
    input_buffered_bytes_.store(0, std::memory_order_release);
}

void DemuxerTS::stop()
{
    stop_requested_.store(true, std::memory_order_release);
    end_of_input_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    input_cv_.notify_all();
    video_cv_.notify_all();
    audio_cv_.notify_all();
    stream_cv_.notify_all();

    if (demux_thread_.joinable()) {
        demux_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lk(input_mutex_);
        clearInputBufferLocked();
    }

    {
        std::lock_guard<std::mutex> lk(video_mutex_);
        video_packets_.clear();
        video_queue_depth_.store(0, std::memory_order_release);
        video_queued_bytes_.store(0, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lk(audio_mutex_);
        audio_packets_by_stream_.clear();
        audio_queue_depth_by_stream_.clear();
        audio_queued_bytes_by_stream_.clear();
        audio_queue_depth_.store(0, std::memory_order_release);
        audio_queued_bytes_.store(0, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lk(stream_mutex_);
        snapshot_.reset();
    }

    cleanupInput();
}

void DemuxerTS::trimInputBufferLocked()
{
    const size_t limit = std::max(config_.input_buffer_limit, packetSizeBytes());
    size_t buffered = input_buffered_bytes_.load(std::memory_order_acquire);

    if (buffered <= limit) {
        return;
    }

    const size_t packet_size = packetSizeBytes();
    size_t excess = buffered - limit;
    size_t bytes_to_drop = (excess / packet_size) * packet_size;
    if (bytes_to_drop == 0u && buffered > limit) {
        bytes_to_drop = std::min(packet_size, buffered);
    }

    while (bytes_to_drop > 0u && !input_chunks_.empty()) {
        InputChunk& front = input_chunks_.front();
        const size_t avail = front.remaining();
        if (avail == 0u) {
            input_chunks_.pop_front();
            continue;
        }

        const size_t drop = std::min(avail, bytes_to_drop);
        front.offset += drop;
        buffered -= drop;
        bytes_to_drop -= drop;

        if (front.offset >= front.data.size()) {
            input_chunks_.pop_front();
        }
    }

    input_buffered_bytes_.store(buffered, std::memory_order_release);
}

void DemuxerTS::updateTransportHealthFromTs(const uint8_t* data, size_t size)
{
    if (!data || size < packetSizeBytes()) {
        return;
    }

    const size_t packetSize = packetSizeBytes();
    size_t offset = 0;
    if (data[0] != 0x47) {
        // Try to recover alignment within the first TS packet. This keeps the
        // health monitor useful for UDP/RTP/SRT chunks without assuming the
        // caller's packet boundaries are always exactly TS-aligned.
        while (offset < packetSize && offset < size && data[offset] != 0x47) {
            ++offset;
        }
        if (offset >= packetSize || offset >= size) {
            std::lock_guard<std::mutex> lk(health_mutex_);
            ++health_.invalid_sync;
            ++health_.discontinuities;
            health_.discontinuity_detected = true;
            health_.generation = generation_.load(std::memory_order_acquire);
            return;
        }
    }

    std::lock_guard<std::mutex> lk(health_mutex_);
    bool sawDiscontinuity = false;

    for (; offset + packetSize <= size; offset += packetSize) {
        const uint8_t* p = data + offset;
        if (p[0] != 0x47) {
            ++health_.invalid_sync;
            sawDiscontinuity = true;
            continue;
        }

        ++health_.transport_packets;

        const bool transportError = (p[1] & 0x80) != 0;
        const bool payloadStart = (p[1] & 0x40) != 0;
        const int pid = ((p[1] & 0x1f) << 8) | p[2];
        const int adaptationControl = (p[3] >> 4) & 0x03;
        const int cc = p[3] & 0x0f;
        const bool hasPayload = (adaptationControl == 1 || adaptationControl == 3);

        if (transportError) {
            ++health_.continuity_errors;
            sawDiscontinuity = true;
            continue;
        }

        if (!hasPayload || pid == 0x1fff) {
            continue;
        }

        auto it = ts_cc_by_pid_.find(pid);
        if (payloadStart || it == ts_cc_by_pid_.end()) {
            ts_cc_by_pid_[pid] = cc;
            continue;
        }

        const int expected = (it->second + 1) & 0x0f;
        if (cc != expected) {
            ++health_.continuity_errors;
            sawDiscontinuity = true;
        }
        it->second = cc;
    }

    if (sawDiscontinuity) {
        ++health_.discontinuities;
        health_.discontinuity_detected = true;
    }
    health_.generation = generation_.load(std::memory_order_acquire);
}

void DemuxerTS::pushData(const uint8_t* data, size_t size)
{
    if (!data || size == 0u) {
        return;
    }

    updateTransportHealthFromTs(data, size);

    {
        std::lock_guard<std::mutex> lk(input_mutex_);

        InputChunk chunk;
        chunk.data.assign(data, data + size);
        chunk.offset = 0;
        input_chunks_.push_back(std::move(chunk));

        const size_t new_buffered =
            input_buffered_bytes_.load(std::memory_order_acquire) + size;
        input_buffered_bytes_.store(new_buffered, std::memory_order_release);

        trimInputBufferLocked();
    }

    input_cv_.notify_one();
}

void DemuxerTS::signalEndOfInput()
{
    end_of_input_.store(true, std::memory_order_release);
    input_cv_.notify_all();
}

int DemuxerTS::readPacket(void* opaque, uint8_t* buf, int buf_size)
{
    return static_cast<DemuxerTS*>(opaque)->readPacketImpl(buf, buf_size);
}

int DemuxerTS::readPacketImpl(uint8_t* buf, int buf_size)
{
    if (!buf || buf_size <= 0) {
        return AVERROR(EINVAL);
    }

    std::unique_lock<std::mutex> lk(input_mutex_);
    input_cv_.wait(lk, [&] {
        return !input_chunks_.empty() ||
               stop_requested_.load(std::memory_order_acquire) ||
               end_of_input_.load(std::memory_order_acquire);
    });

    if (stop_requested_.load(std::memory_order_acquire)) {
        return AVERROR_EXIT;
    }

    if (input_chunks_.empty()) {
        return AVERROR_EOF;
    }

    int copied = 0;
    size_t buffered = input_buffered_bytes_.load(std::memory_order_acquire);

    while (copied < buf_size && !input_chunks_.empty()) {
        InputChunk& front = input_chunks_.front();
        const size_t avail = front.remaining();
        if (avail == 0u) {
            input_chunks_.pop_front();
            continue;
        }

        const int take =
            static_cast<int>(std::min<size_t>(avail, static_cast<size_t>(buf_size - copied)));
        std::memcpy(buf + copied, front.data.data() + front.offset, static_cast<size_t>(take));
        front.offset += static_cast<size_t>(take);
        copied += take;
        buffered -= static_cast<size_t>(take);

        if (front.offset >= front.data.size()) {
            input_chunks_.pop_front();
        }
    }

    input_buffered_bytes_.store(buffered, std::memory_order_release);

    if (copied <= 0) {
        return end_of_input_.load(std::memory_order_acquire) ? AVERROR_EOF : AVERROR(EAGAIN);
    }

    return copied;
}

bool DemuxerTS::openInput()
{
    cleanupInput();

    io_buffer_ = static_cast<uint8_t*>(av_malloc(config_.io_buffer_size));
    if (!io_buffer_) {
        std::cerr << "[DemuxerTS] Failed to allocate AVIO buffer.\n";
        return false;
    }

    io_ctx_ = avio_alloc_context(io_buffer_,
                                 static_cast<int>(config_.io_buffer_size),
                                 0,
                                 this,
                                 &DemuxerTS::readPacket,
                                 nullptr,
                                 nullptr);
    if (!io_ctx_) {
        std::cerr << "[DemuxerTS] avio_alloc_context failed.\n";
        av_freep(&io_buffer_);
        return false;
    }

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        std::cerr << "[DemuxerTS] avformat_alloc_context failed.\n";
        cleanupInput();
        return false;
    }

    fmt_ctx_->pb = io_ctx_;
    fmt_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt_ctx_->flags |= AVFMT_FLAG_DISCARD_CORRUPT;
    fmt_ctx_->flags |= AVFMT_FLAG_NOBUFFER;
    fmt_ctx_->probesize = config_.probe_size;
    fmt_ctx_->max_analyze_duration = config_.max_analyze_duration_us;
    fmt_ctx_->interrupt_callback.callback = &DemuxerTS::interruptCallback;
    fmt_ctx_->interrupt_callback.opaque = this;

    ts_input_fmt_ = av_find_input_format("mpegts");
    if (!ts_input_fmt_) {
        std::cerr << "[DemuxerTS] Failed to locate FFmpeg mpegts demuxer.\n";
        cleanupInput();
        return false;
    }

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "scan_all_pmts", "1", 0);

    const int ret = avformat_open_input(&fmt_ctx_, nullptr, ts_input_fmt_, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        if (!stop_requested_.load(std::memory_order_acquire)) {
            std::cerr << "[DemuxerTS] avformat_open_input failed: " << ffErrStr(ret) << "\n";
        }
        cleanupInput();
        return false;
    }

    if (config_.find_stream_info) {
        int info_ret = 0;
        if (config_.suppress_initial_video_probe_logs) {
            ScopedAvLogLevel quiet_startup(AV_LOG_FATAL);
            info_ret = avformat_find_stream_info(fmt_ctx_, nullptr);
        } else {
            info_ret = avformat_find_stream_info(fmt_ctx_, nullptr);
        }
        if (info_ret < 0 && !stop_requested_.load(std::memory_order_acquire)) {
            std::cerr << "[DemuxerTS] avformat_find_stream_info failed: " << ffErrStr(info_ret) << "\n";
        }
    }

    updateStreamInfoFromFormat();
    return true;
}

bool DemuxerTS::updateStreamInfoFromFormat()
{
    if (!fmt_ctx_) {
        return false;
    }

    std::shared_ptr<ProgramSnapshot> next(new ProgramSnapshot());
    next->generation = generation_.load(std::memory_order_acquire);

    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream* st = fmt_ctx_->streams[i];
        if (!st || !st->codecpar) {
            continue;
        }

        StreamInfo info;
        info.stream_index = static_cast<int>(i);
        info.pid = st->id;
        info.media_type = st->codecpar->codec_type;
        info.codec_id = st->codecpar->codec_id;
        info.time_base = st->time_base;
        info.avg_frame_rate = st->avg_frame_rate;
        info.r_frame_rate = st->r_frame_rate;
        info.sample_rate = st->codecpar->sample_rate;
        info.channels = st->codecpar->ch_layout.nb_channels;

        // SMPTE 302M in MPEG-TS is carried as private audio data and FFmpeg
        // may initially report incomplete channel metadata while still being
        // able to demux valid packets. NxFrame writes one S302M stream per
        // embedded stereo pair, so expose a deterministic 2ch/48k snapshot
        // when the demuxer leaves these fields unspecified. This avoids
        // receiver-side "s302m, 0 channels" setup and keeps logical pair
        // routing stable from stream discovery onward.
        if (info.media_type == AVMEDIA_TYPE_AUDIO &&
            st->codecpar->codec_id == AV_CODEC_ID_S302M) {
            AVCodecParameters* cp = st->codecpar;
            if (cp->ch_layout.nb_channels <= 0) {
                av_channel_layout_uninit(&cp->ch_layout);
                cp->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
                cp->ch_layout.nb_channels = 2;
                info.channels = 2;
            }
            if (cp->sample_rate <= 0) {
                cp->sample_rate = 48000;
                info.sample_rate = 48000;
            }
        }

        if (info.media_type == AVMEDIA_TYPE_VIDEO && next->video_stream_index < 0) {
            next->video_stream_index = info.stream_index;
            next->video_time_base = st->time_base;
            next->video_avg_frame_rate = st->avg_frame_rate;
            next->video_r_frame_rate = st->r_frame_rate;
            next->codecpar_by_stream[info.stream_index] = cloneCodecParametersShared(st->codecpar);
        } else if (info.media_type == AVMEDIA_TYPE_AUDIO) {
            next->audio_stream_indices.push_back(info.stream_index);
            next->audio_time_base_by_stream[info.stream_index] = st->time_base;
            next->codecpar_by_stream[info.stream_index] = cloneCodecParametersShared(st->codecpar);

            if (next->primary_audio_stream_index < 0) {
                next->primary_audio_stream_index = info.stream_index;
                next->primary_audio_time_base = st->time_base;
            }
        }

        if (info.media_type == AVMEDIA_TYPE_VIDEO || info.media_type == AVMEDIA_TYPE_AUDIO) {
            next->streams.push_back(info);
        }
    }

    std::sort(next->audio_stream_indices.begin(), next->audio_stream_indices.end());

    bool changed = false;
    bool had_previous_snapshot = false;
    {
        std::lock_guard<std::mutex> lk(stream_mutex_);
        had_previous_snapshot = static_cast<bool>(snapshot_);

        if (!snapshot_) {
            changed = true;
        } else {
            if (snapshot_->video_stream_index != next->video_stream_index ||
                snapshot_->primary_audio_stream_index != next->primary_audio_stream_index ||
                snapshot_->audio_stream_indices != next->audio_stream_indices ||
                !sameRational(snapshot_->video_time_base, next->video_time_base) ||
                !sameRational(snapshot_->video_avg_frame_rate, next->video_avg_frame_rate) ||
                !sameRational(snapshot_->video_r_frame_rate, next->video_r_frame_rate) ||
                !sameRational(snapshot_->primary_audio_time_base, next->primary_audio_time_base) ||
                !sameStreamVector(snapshot_->streams, next->streams) ||
                !sameCodecParameterMap(snapshot_->codecpar_by_stream, next->codecpar_by_stream)) {
                changed = true;
            }
        }

        if (changed && snapshot_) {
            next->generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1u;
        } else {
            next->generation = generation_.load(std::memory_order_acquire);
        }

        snapshot_ = next;
    }

    if (changed) {
        if (had_previous_snapshot) {
            {
                std::lock_guard<std::mutex> lk(video_mutex_);
                video_packets_.clear();
                video_queue_depth_.store(0, std::memory_order_release);
                video_queued_bytes_.store(0, std::memory_order_release);
            }

            {
                std::lock_guard<std::mutex> lk(audio_mutex_);
                audio_packets_by_stream_.clear();
                audio_queue_depth_by_stream_.clear();
                audio_queued_bytes_by_stream_.clear();
                audio_queue_depth_.store(0, std::memory_order_release);
                audio_queued_bytes_.store(0, std::memory_order_release);
            }

            video_cv_.notify_all();
            audio_cv_.notify_all();
        }

        {
            std::lock_guard<std::mutex> lk(audio_mutex_);
            for (size_t i = 0; i < next->audio_stream_indices.size(); ++i) {
                const int idx = next->audio_stream_indices[i];
                if (audio_packets_by_stream_.find(idx) == audio_packets_by_stream_.end()) {
                    audio_packets_by_stream_[idx] = std::deque<DemuxedPacket>();
                }
                if (audio_queue_depth_by_stream_.find(idx) == audio_queue_depth_by_stream_.end()) {
                    audio_queue_depth_by_stream_[idx] = 0u;
                }
                if (audio_queued_bytes_by_stream_.find(idx) == audio_queued_bytes_by_stream_.end()) {
                    audio_queued_bytes_by_stream_[idx] = 0u;
                }
            }
        }

        std::cerr << "[DemuxerTS] Stream snapshot updated: video="
                  << next->video_stream_index
                  << " primary_audio=" << next->primary_audio_stream_index
                  << " audio_count=" << next->audio_stream_indices.size()
                  << " generation=" << next->generation
                  << (had_previous_snapshot ? " output_queues_flushed=yes" : "")
                  << "\n";

        stream_cv_.notify_all();
    }

    return changed;
}

void DemuxerTS::pushVideoPacket(DemuxedPacket&& pkt)
{
    const size_t pkt_bytes = packetBytes(pkt);
    video_packet_bytes_total_.fetch_add(static_cast<uint64_t>(pkt_bytes),
                                        std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(video_mutex_);

    while (video_packets_.size() >= config_.output_queue_limit && !video_packets_.empty()) {
        const size_t dropped = packetBytes(video_packets_.front());
        video_packets_.pop_front();

        const size_t prev = video_queued_bytes_.load(std::memory_order_acquire);
        video_queued_bytes_.store(prev >= dropped ? prev - dropped : 0u, std::memory_order_release);
    }

    video_packets_.emplace_back(std::move(pkt));
    video_queue_depth_.store(video_packets_.size(), std::memory_order_release);
    video_queued_bytes_.fetch_add(pkt_bytes, std::memory_order_acq_rel);
    video_cv_.notify_one();
}

void DemuxerTS::pushAudioPacket(int stream_index, DemuxedPacket&& pkt)
{
    const size_t pkt_bytes = packetBytes(pkt);

    std::lock_guard<std::mutex> lk(audio_mutex_);
    std::deque<DemuxedPacket>& q = audio_packets_by_stream_[stream_index];

    while (q.size() >= config_.output_queue_limit && !q.empty()) {
        const size_t dropped = packetBytes(q.front());
        q.pop_front();

        size_t& stream_bytes = audio_queued_bytes_by_stream_[stream_index];
        stream_bytes = (stream_bytes >= dropped) ? (stream_bytes - dropped) : 0u;

        const size_t prev_total = audio_queued_bytes_.load(std::memory_order_acquire);
        audio_queued_bytes_.store(prev_total >= dropped ? prev_total - dropped : 0u,
                                  std::memory_order_release);
    }

    q.emplace_back(std::move(pkt));
    audio_queue_depth_by_stream_[stream_index] = q.size();
    audio_queued_bytes_by_stream_[stream_index] += pkt_bytes;

    size_t total_depth = 0u;
    for (std::map<int, std::deque<DemuxedPacket> >::const_iterator it = audio_packets_by_stream_.begin();
         it != audio_packets_by_stream_.end(); ++it) {
        total_depth += it->second.size();
    }

    audio_queue_depth_.store(total_depth, std::memory_order_release);
    audio_queued_bytes_.fetch_add(pkt_bytes, std::memory_order_acq_rel);
    audio_cv_.notify_all();
}

bool DemuxerTS::popVideoPacketInternal(DemuxedPacket& out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(video_mutex_);
    if (!video_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            return !video_packets_.empty() || !running_.load(std::memory_order_acquire);
        })) {
        return false;
    }

    if (video_packets_.empty()) {
        return false;
    }

    const size_t pkt_bytes = packetBytes(video_packets_.front());
    out = std::move(video_packets_.front());
    video_packets_.pop_front();

    video_queue_depth_.store(video_packets_.size(), std::memory_order_release);
    const size_t prev = video_queued_bytes_.load(std::memory_order_acquire);
    video_queued_bytes_.store(prev >= pkt_bytes ? prev - pkt_bytes : 0u, std::memory_order_release);

    return true;
}

bool DemuxerTS::popAudioPacketInternal(int stream_index, DemuxedPacket& out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(audio_mutex_);
    if (!audio_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            std::map<int, std::deque<DemuxedPacket> >::iterator it = audio_packets_by_stream_.find(stream_index);
            return (it != audio_packets_by_stream_.end() && !it->second.empty()) ||
                   !running_.load(std::memory_order_acquire);
        })) {
        return false;
    }

    std::map<int, std::deque<DemuxedPacket> >::iterator it = audio_packets_by_stream_.find(stream_index);
    if (it == audio_packets_by_stream_.end() || it->second.empty()) {
        return false;
    }

    std::deque<DemuxedPacket>& q = it->second;
    const size_t pkt_bytes = packetBytes(q.front());
    out = std::move(q.front());
    q.pop_front();

    audio_queue_depth_by_stream_[stream_index] = q.size();

    size_t& stream_bytes = audio_queued_bytes_by_stream_[stream_index];
    stream_bytes = (stream_bytes >= pkt_bytes) ? (stream_bytes - pkt_bytes) : 0u;

    size_t total_depth = 0u;
    for (std::map<int, std::deque<DemuxedPacket> >::const_iterator jt = audio_packets_by_stream_.begin();
         jt != audio_packets_by_stream_.end(); ++jt) {
        total_depth += jt->second.size();
    }
    audio_queue_depth_.store(total_depth, std::memory_order_release);

    const size_t prev_total = audio_queued_bytes_.load(std::memory_order_acquire);
    audio_queued_bytes_.store(prev_total >= pkt_bytes ? prev_total - pkt_bytes : 0u,
                              std::memory_order_release);

    return true;
}

bool DemuxerTS::popVideoPacket(DemuxedPacket& out, int timeout_ms)
{
    return popVideoPacketInternal(out, timeout_ms);
}

bool DemuxerTS::popAudioPacket(DemuxedPacket& out, int timeout_ms)
{
    const int primary = audioStreamIndex();
    if (primary < 0) {
        return false;
    }
    return popAudioPacketInternal(primary, out, timeout_ms);
}

bool DemuxerTS::popAudioPacketForStream(int stream_index, DemuxedPacket& out, int timeout_ms)
{
    return popAudioPacketInternal(stream_index, out, timeout_ms);
}

int DemuxerTS::videoStreamIndex() const noexcept
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    return s ? s->video_stream_index : -1;
}

int DemuxerTS::audioStreamIndex() const noexcept
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    return s ? s->primary_audio_stream_index : -1;
}

bool DemuxerTS::waitForVideoStream(int timeout_ms)
{
    std::unique_lock<std::mutex> lk(stream_mutex_);
    return stream_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
               return (snapshot_ && snapshot_->video_stream_index >= 0) ||
                      !running_.load(std::memory_order_acquire) ||
                      stop_requested_.load(std::memory_order_acquire);
           }) &&
           snapshot_ && snapshot_->video_stream_index >= 0;
}

bool DemuxerTS::waitForAudioStream(int timeout_ms)
{
    std::unique_lock<std::mutex> lk(stream_mutex_);
    return stream_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
               return (snapshot_ && snapshot_->primary_audio_stream_index >= 0) ||
                      !running_.load(std::memory_order_acquire) ||
                      stop_requested_.load(std::memory_order_acquire);
           }) &&
           snapshot_ && snapshot_->primary_audio_stream_index >= 0;
}

bool DemuxerTS::waitForAudioStreams(size_t min_count, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(stream_mutex_);
    return stream_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
               return (snapshot_ && snapshot_->audio_stream_indices.size() >= min_count) ||
                      !running_.load(std::memory_order_acquire) ||
                      stop_requested_.load(std::memory_order_acquire);
           }) &&
           snapshot_ && snapshot_->audio_stream_indices.size() >= min_count;
}

bool DemuxerTS::waitForAnyStream(int timeout_ms)
{
    std::unique_lock<std::mutex> lk(stream_mutex_);
    return stream_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
               return (snapshot_ &&
                       (snapshot_->video_stream_index >= 0 || !snapshot_->audio_stream_indices.empty())) ||
                      !running_.load(std::memory_order_acquire) ||
                      stop_requested_.load(std::memory_order_acquire);
           }) &&
           snapshot_ &&
           (snapshot_->video_stream_index >= 0 || !snapshot_->audio_stream_indices.empty());
}

std::shared_ptr<const DemuxerTS::ProgramSnapshot> DemuxerTS::snapshot() const
{
    std::lock_guard<std::mutex> lk(stream_mutex_);
    return snapshot_;
}

DemuxerTS::HealthSnapshot DemuxerTS::healthSnapshot() const
{
    std::lock_guard<std::mutex> lk(health_mutex_);
    return health_;
}

AVCodecParameters* DemuxerTS::cloneVideoCodecParameters() const
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    if (!s || s->video_stream_index < 0) {
        return nullptr;
    }

    std::map<int, CodecParametersPtr>::const_iterator it =
        s->codecpar_by_stream.find(s->video_stream_index);
    if (it == s->codecpar_by_stream.end() || !it->second) {
        return nullptr;
    }

    AVCodecParameters* out = avcodec_parameters_alloc();
    if (!out) {
        return nullptr;
    }

    if (avcodec_parameters_copy(out, it->second.get()) < 0) {
        avcodec_parameters_free(&out);
        return nullptr;
    }

    return out;
}

AVCodecParameters* DemuxerTS::cloneAudioCodecParameters() const
{
    return cloneAudioCodecParametersForStream(audioStreamIndex());
}

AVCodecParameters* DemuxerTS::cloneAudioCodecParametersForStream(int stream_index) const
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    if (!s || stream_index < 0) {
        return nullptr;
    }

    std::map<int, CodecParametersPtr>::const_iterator it = s->codecpar_by_stream.find(stream_index);
    if (it == s->codecpar_by_stream.end() || !it->second) {
        return nullptr;
    }

    AVCodecParameters* out = avcodec_parameters_alloc();
    if (!out) {
        return nullptr;
    }

    if (avcodec_parameters_copy(out, it->second.get()) < 0) {
        avcodec_parameters_free(&out);
        return nullptr;
    }

    return out;
}

AVRational DemuxerTS::videoTimeBase() const noexcept
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    return s ? s->video_time_base : AVRational{1, 90000};
}

AVRational DemuxerTS::videoAvgFrameRate() const noexcept
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    return s ? s->video_avg_frame_rate : AVRational{0, 1};
}

AVRational DemuxerTS::videoRFrameRate() const noexcept
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    return s ? s->video_r_frame_rate : AVRational{0, 1};
}

AVRational DemuxerTS::audioTimeBase() const noexcept
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    return s ? s->primary_audio_time_base : AVRational{1, 48000};
}

AVRational DemuxerTS::audioTimeBaseForStream(int stream_index) const noexcept
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    if (!s) {
        return AVRational{1, 48000};
    }

    std::map<int, AVRational>::const_iterator it = s->audio_time_base_by_stream.find(stream_index);
    return (it != s->audio_time_base_by_stream.end()) ? it->second : AVRational{1, 48000};
}

std::vector<int> DemuxerTS::audioStreamIndices() const
{
    std::shared_ptr<const ProgramSnapshot> s = snapshot();
    return s ? s->audio_stream_indices : std::vector<int>();
}

size_t DemuxerTS::audioQueueDepthForStream(int stream_index) const noexcept
{
    std::lock_guard<std::mutex> lk(audio_mutex_);
    std::map<int, size_t>::const_iterator it = audio_queue_depth_by_stream_.find(stream_index);
    return (it != audio_queue_depth_by_stream_.end()) ? it->second : 0u;
}

size_t DemuxerTS::audioQueuedBytesForStream(int stream_index) const noexcept
{
    std::lock_guard<std::mutex> lk(audio_mutex_);
    std::map<int, size_t>::const_iterator it = audio_queued_bytes_by_stream_.find(stream_index);
    return (it != audio_queued_bytes_by_stream_.end()) ? it->second : 0u;
}

void DemuxerTS::demuxLoop()
{
    if (!openInput()) {
        running_.store(false, std::memory_order_release);
        stream_cv_.notify_all();
        video_cv_.notify_all();
        audio_cv_.notify_all();
        return;
    }

    AVPacket* raw = av_packet_alloc();
    if (!raw) {
        std::cerr << "[DemuxerTS] av_packet_alloc failed.\n";
        running_.store(false, std::memory_order_release);
        stream_cv_.notify_all();
        video_cv_.notify_all();
        audio_cv_.notify_all();
        return;
    }

    while (!stop_requested_.load(std::memory_order_acquire)) {
        int ret = 0;
        if (config_.suppress_initial_video_probe_logs && !acquired_first_video_key_packet_) {
            ScopedAvLogLevel quiet_startup(AV_LOG_FATAL);
            ret = av_read_frame(fmt_ctx_, raw);
        } else {
            ret = av_read_frame(fmt_ctx_, raw);
        }
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                if (end_of_input_.load(std::memory_order_acquire)) {
                    std::cerr << "[DemuxerTS] End of TS input.\n";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (ret == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            if (ret == AVERROR_EXIT && stop_requested_.load(std::memory_order_acquire)) {
                break;
            }

            if (!stop_requested_.load(std::memory_order_acquire)) {
                std::cerr << "[DemuxerTS] av_read_frame failed: " << ffErrStr(ret) << "\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        updateStreamInfoFromFormat();

        std::shared_ptr<const ProgramSnapshot> s = snapshot();
        if (!s) {
            av_packet_unref(raw);
            continue;
        }

        AVPacketPtr pkt = packet_pool_.acquire();
        if (!pkt) {
            av_packet_unref(raw);
            continue;
        }

        av_packet_unref(pkt.get());
        if (av_packet_ref(pkt.get(), raw) < 0) {
            av_packet_unref(raw);
            continue;
        }

        DemuxedPacket out;
        out.pkt = std::move(pkt);
        out.stream_index = raw->stream_index;
        out.generation = s->generation;

        if (raw->stream_index >= 0 &&
            raw->stream_index < static_cast<int>(fmt_ctx_->nb_streams) &&
            fmt_ctx_->streams[raw->stream_index]) {
            out.time_base = fmt_ctx_->streams[raw->stream_index]->time_base;
        }

        out.is_video = (raw->stream_index == s->video_stream_index);
        out.is_audio = (s->audio_time_base_by_stream.find(raw->stream_index) != s->audio_time_base_by_stream.end());

        if (out.is_video && (raw->flags & AV_PKT_FLAG_KEY)) {
            acquired_first_video_key_packet_ = true;
        }

        if (out.is_video && !logged_first_video_packet_) {
            logged_first_video_packet_ = true;
            std::cerr << "[DemuxerTS] First video packet received from stream "
                      << out.stream_index << "\n";
        }

        if (out.is_audio && !logged_first_audio_packet_by_stream_[out.stream_index]) {
            logged_first_audio_packet_by_stream_[out.stream_index] = true;
            std::cerr << "[DemuxerTS] First audio packet received from stream "
                      << out.stream_index << "\n";
        }

        av_packet_unref(raw);

        if (out.is_video) {
            pushVideoPacket(std::move(out));
        } else if (out.is_audio) {
            pushAudioPacket(out.stream_index, std::move(out));
        }
    }

    av_packet_free(&raw);

    running_.store(false, std::memory_order_release);
    stream_cv_.notify_all();
    video_cv_.notify_all();
    audio_cv_.notify_all();
}