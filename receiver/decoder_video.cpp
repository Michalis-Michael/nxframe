/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/decoder_video.cpp
 * Description: Implements video decoding and decoded VideoFrame queueing.
 */

#include "receiver/decoder_video.h"

#include <chrono>
#include <cstring>
#include <iostream>

extern "C" {
#include <libavutil/error.h>
}

namespace {

static std::string ffErr(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), errnum);
    return std::string(buf);
}

static AVRational chooseNominalRate(const std::shared_ptr<const DemuxerTS::ProgramSnapshot>& snapshot)
{
    if (!snapshot) {
        return AVRational{0, 1};
    }

    if (snapshot->video_avg_frame_rate.num > 0 && snapshot->video_avg_frame_rate.den > 0) {
        return snapshot->video_avg_frame_rate;
    }

    if (snapshot->video_r_frame_rate.num > 0 && snapshot->video_r_frame_rate.den > 0) {
        return snapshot->video_r_frame_rate;
    }

    return AVRational{0, 1};
}

static bool isInterlacedFieldOrder(AVFieldOrder order) noexcept
{
    return order == AV_FIELD_TT ||
           order == AV_FIELD_BB ||
           order == AV_FIELD_TB ||
           order == AV_FIELD_BT;
}

static bool isProgressiveFieldOrder(AVFieldOrder order) noexcept
{
    return order == AV_FIELD_PROGRESSIVE;
}

static bool isTopFieldFirst(AVFieldOrder order) noexcept
{
    // FFmpeg field order naming:
    //   TT/TB = top field first, BB/BT = bottom field first.
    // Unknown/progressive keeps the broadcast-safe default of TFF.
    if (order == AV_FIELD_BB || order == AV_FIELD_BT) {
        return false;
    }
    return true;
}

static AVFieldOrder snapshotVideoFieldOrder(
    const std::shared_ptr<const DemuxerTS::ProgramSnapshot>& snapshot,
    int stream_index) noexcept
{
    if (!snapshot) {
        return AV_FIELD_UNKNOWN;
    }

    const std::map<int, DemuxerTS::CodecParametersPtr>::const_iterator it =
        snapshot->codecpar_by_stream.find(stream_index);
    if (it == snapshot->codecpar_by_stream.end() || !it->second) {
        return AV_FIELD_UNKNOWN;
    }

    return it->second->field_order;
}

static AVFieldOrder chooseFieldOrderHint(
    const AVCodecContext* codec_ctx,
    const std::shared_ptr<const DemuxerTS::ProgramSnapshot>& snapshot,
    int stream_index) noexcept
{
    if (codec_ctx && codec_ctx->field_order != AV_FIELD_UNKNOWN) {
        return codec_ctx->field_order;
    }

    return snapshotVideoFieldOrder(snapshot, stream_index);
}

static void applyInterlaceMetadata(const AVFrame* src,
                                   const AVCodecContext* codec_ctx,
                                   const std::shared_ptr<const DemuxerTS::ProgramSnapshot>& snapshot,
                                   int stream_index,
                                   VideoFrame& out) noexcept
{
    const bool frame_interlaced = (src->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    const bool frame_tff = (src->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;

    // Prefer explicit per-frame flags when the decoder provides them. Some H.264
    // streams do not set these reliably, so fall back to codec/stream field_order.
    if (frame_interlaced) {
        out.interlaced = true;
        out.tff = frame_tff;
        return;
    }

    const AVFieldOrder order = chooseFieldOrderHint(codec_ctx, snapshot, stream_index);
    if (isInterlacedFieldOrder(order)) {
        out.interlaced = true;
        out.tff = isTopFieldFirst(order);
        return;
    }

    if (isProgressiveFieldOrder(order)) {
        out.interlaced = false;
        out.tff = true;
        return;
    }

    out.interlaced = false;
    out.tff = true;
}

} // namespace

DecoderVideo::DecoderVideo() = default;

DecoderVideo::~DecoderVideo()
{
    stop();
}

void DecoderVideo::setLastError(const std::string& err)
{
    std::lock_guard<std::mutex> lk(err_mutex_);
    last_error_ = err;
}

bool DecoderVideo::init(DemuxerTS& demuxer, const Config& config)
{
    stop();

    demuxer_ = &demuxer;
    config_ = config;
    stop_requested_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    queue_depth_.store(0, std::memory_order_release);
    queued_bytes_.store(0, std::memory_order_release);
    high_water_queue_depth_.store(0, std::memory_order_release);
    high_water_queued_bytes_.store(0, std::memory_order_release);
    estimated_audio_frame_samples_.store(1920, std::memory_order_release);
    bound_generation_.store(0, std::memory_order_release);

    last_pts_ = AV_NOPTS_VALUE;
    last_time_base_ = AVRational{1, 0};
    last_nominal_frame_rate_ = AVRational{0, 1};
    last_interlaced_ = false;
    waiting_for_start_keyframe_ = config_.require_keyframe_on_start;
    dropped_until_keyframe_ = 0;

    {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_.clear();
    }

    if (!openDecoder()) {
        return false;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&DecoderVideo::decodeLoop, this);
    return true;
}


void DecoderVideo::releaseScaler()
{
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
}

void DecoderVideo::closeDecoder()
{
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    releaseScaler();

    codec_ = nullptr;
    stream_index_ = -1;
    snapshot_.reset();
    opened_generation_ = 0;
    bound_generation_.store(0, std::memory_order_release);
}

void DecoderVideo::stop()
{
    stop_requested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    queue_cv_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        frames_.clear();
        queue_depth_.store(0, std::memory_order_release);
        queued_bytes_.store(0, std::memory_order_release);
    }

    closeDecoder();
}

std::string DecoderVideo::getLastError() const
{
    std::lock_guard<std::mutex> lk(err_mutex_);
    return last_error_;
}

bool DecoderVideo::openDecoder()
{
    if (!demuxer_) {
        setLastError("DecoderVideo::openDecoder called without demuxer.");
        return false;
    }

    snapshot_ = demuxer_->snapshot();
    if (!snapshot_) {
        setLastError("No demux snapshot available for video decoder.");
        return false;
    }

    stream_index_ = snapshot_->video_stream_index;
    if (stream_index_ < 0) {
        setLastError("No video stream found in demux snapshot.");
        snapshot_.reset();
        return false;
    }

    std::map<int, DemuxerTS::CodecParametersPtr>::const_iterator cp_it =
        snapshot_->codecpar_by_stream.find(stream_index_);
    if (cp_it == snapshot_->codecpar_by_stream.end() || !cp_it->second) {
        setLastError("Missing video codec parameters in demux snapshot.");
        snapshot_.reset();
        stream_index_ = -1;
        return false;
    }

    codec_ = avcodec_find_decoder(cp_it->second->codec_id);
    if (!codec_) {
        setLastError("FFmpeg video decoder not found for codec_id=" +
                     std::to_string(cp_it->second->codec_id));
        snapshot_.reset();
        stream_index_ = -1;
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        setLastError("Failed to allocate video decoder context.");
        snapshot_.reset();
        stream_index_ = -1;
        return false;
    }

    int ret = avcodec_parameters_to_context(codec_ctx_, cp_it->second.get());
    if (ret < 0) {
        setLastError(std::string("avcodec_parameters_to_context(video) failed: ") + ffErr(ret));
        closeDecoder();
        return false;
    }

    codec_ctx_->pkt_timebase = snapshot_->video_time_base;
    codec_ctx_->thread_count = config_.thread_count;
    codec_ctx_->thread_type = config_.thread_type;

    if (config_.low_delay) {
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }

    ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (ret < 0) {
        setLastError(std::string("Failed to open video decoder: ") + ffErr(ret));
        closeDecoder();
        return false;
    }

    opened_generation_ = snapshot_->generation;
    bound_generation_.store(opened_generation_, std::memory_order_release);

    std::cerr << "[DecoderVideo] Opened decoder codec=" << codec_->name
              << " stream_index=" << stream_index_
              << " generation=" << opened_generation_
              << " thread_count=" << codec_ctx_->thread_count
              << " thread_type=" << codec_ctx_->thread_type
              << " low_delay=" << (config_.low_delay ? "yes" : "no")
              << "\n";

    return true;
}

void DecoderVideo::flushDecoder()
{
    if (!codec_ctx_) {
        return;
    }

    avcodec_flush_buffers(codec_ctx_);
}

bool DecoderVideo::copyFrame(const AVFrame* src, VideoFrame& out)
{
    if (!src || !codec_ctx_) {
        return false;
    }

    const AVPixelFormat src_fmt = static_cast<AVPixelFormat>(src->format);
    if (src->width <= 0 || src->height <= 0 || src_fmt == AV_PIX_FMT_NONE) {
        return false;
    }

    if (!src->data[0]) {
        setLastError("Decoded video frame has no readable software plane.");
        return false;
    }

    // DeckLink output is v210, so normalize every decoded source format to
    // planar 10-bit 4:2:2 here. This makes the receiver accept common decode
    // formats such as yuv420p, yuv422p, nv12, yuv420p10le, yuv422p10le, etc.
    const AVPixelFormat dst_fmt = AV_PIX_FMT_YUV422P10LE;

    const int buf_size = av_image_get_buffer_size(dst_fmt, src->width, src->height, 32);
    if (buf_size <= 0) {
        return false;
    }

    out = VideoFrame();
    out.buffer = make_shared_u8(static_cast<size_t>(buf_size));
    out.buffer_size = static_cast<size_t>(buf_size);
    out.width = src->width;
    out.height = src->height;
    out.pix_fmt = dst_fmt;

    out.pts = (src->best_effort_timestamp != AV_NOPTS_VALUE)
                  ? src->best_effort_timestamp
                  : src->pts;

    out.pts_time_base =
        (codec_ctx_->pkt_timebase.num > 0 && codec_ctx_->pkt_timebase.den > 0)
            ? codec_ctx_->pkt_timebase
            : (snapshot_ ? snapshot_->video_time_base : AVRational{1, 25});

    out.time_base = out.pts_time_base;
    out.nominal_frame_rate = chooseNominalRate(snapshot_);
    applyInterlaceMetadata(src, codec_ctx_, snapshot_, stream_index_, out);

    out.color_primaries = (src->color_primaries != AVCOL_PRI_UNSPECIFIED)
                              ? src->color_primaries
                              : codec_ctx_->color_primaries;
    out.color_trc = (src->color_trc != AVCOL_TRC_UNSPECIFIED)
                        ? src->color_trc
                        : codec_ctx_->color_trc;
    out.colorspace = (src->colorspace != AVCOL_SPC_UNSPECIFIED)
                         ? src->colorspace
                         : codec_ctx_->colorspace;
    out.color_range = (src->color_range != AVCOL_RANGE_UNSPECIFIED)
                          ? src->color_range
                          : codec_ctx_->color_range;
    out.chroma_location = (src->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
                              ? src->chroma_location
                              : codec_ctx_->chroma_sample_location;

    if (const AVFrameSideData* sd = av_frame_get_side_data(src, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        if (sd->data && sd->size >= sizeof(AVMasteringDisplayMetadata)) {
            std::memcpy(&out.mastering_display, sd->data, sizeof(AVMasteringDisplayMetadata));
            out.has_mastering_display = true;
        }
    }
    if (const AVFrameSideData* sd = av_frame_get_side_data(src, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        if (sd->data && sd->size >= sizeof(AVContentLightMetadata)) {
            std::memcpy(&out.content_light, sd->data, sizeof(AVContentLightMetadata));
            out.has_content_light = true;
        }
    }

    uint8_t* dst_data[4] = {nullptr, nullptr, nullptr, nullptr};
    int dst_linesize[4] = {0, 0, 0, 0};

    const int fill_ret = av_image_fill_arrays(dst_data,
                                              dst_linesize,
                                              out.buffer.get(),
                                              dst_fmt,
                                              out.width,
                                              out.height,
                                              32);
    if (fill_ret < 0) {
        setLastError(std::string("av_image_fill_arrays(video) failed: ") + ffErr(fill_ret));
        return false;
    }

    if (src_fmt == dst_fmt) {
        av_image_copy(dst_data,
                      dst_linesize,
                      const_cast<const uint8_t**>(src->data),
                      src->linesize,
                      dst_fmt,
                      out.width,
                      out.height);
    } else {
        sws_ctx_ = sws_getCachedContext(sws_ctx_,
                                        src->width,
                                        src->height,
                                        src_fmt,
                                        src->width,
                                        src->height,
                                        dst_fmt,
                                        SWS_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr);
        if (!sws_ctx_) {
            setLastError("Failed to create/reuse swscale context for video conversion.");
            return false;
        }

        const uint8_t* const src_data[4] = {
            src->data[0], src->data[1], src->data[2], src->data[3]
        };

        const int scaled = sws_scale(sws_ctx_,
                                     src_data,
                                     src->linesize,
                                     0,
                                     src->height,
                                     dst_data,
                                     dst_linesize);
        if (scaled != src->height) {
            setLastError("sws_scale(video) did not convert a complete frame.");
            return false;
        }
    }

    for (int i = 0; i < 3; ++i) {
        out.data[i] = dst_data[i];
        out.linesize[i] = dst_linesize[i];
    }

    return true;
}

void DecoderVideo::updateCadenceEstimate(const VideoFrame& frame)
{
    if (frame.nominal_frame_rate.num > 0 && frame.nominal_frame_rate.den > 0) {
        const double samples_per_frame =
            48000.0 *
            static_cast<double>(frame.nominal_frame_rate.den) /
            static_cast<double>(frame.nominal_frame_rate.num);

        const int samples = static_cast<int>(samples_per_frame + 0.5);
        if (samples >= 160 && samples <= 4000) {
            estimated_audio_frame_samples_.store(samples, std::memory_order_release);
        }

        last_nominal_frame_rate_ = frame.nominal_frame_rate;
        last_interlaced_ = frame.interlaced;
    }

    if (frame.pts == AV_NOPTS_VALUE ||
        frame.pts_time_base.num <= 0 ||
        frame.pts_time_base.den <= 0) {
        return;
    }

    if ((last_nominal_frame_rate_.num <= 0 || last_nominal_frame_rate_.den <= 0) &&
        last_pts_ != AV_NOPTS_VALUE &&
        last_time_base_.num == frame.pts_time_base.num &&
        last_time_base_.den == frame.pts_time_base.den) {
        const int64_t delta_pts = frame.pts - last_pts_;
        if (delta_pts > 0) {
            const double seconds_per_frame =
                av_q2d(frame.pts_time_base) * static_cast<double>(delta_pts);
            const int samples = static_cast<int>(seconds_per_frame * 48000.0 + 0.5);
            if (samples >= 160 && samples <= 4000) {
                estimated_audio_frame_samples_.store(samples, std::memory_order_release);
            }
        }
    }

    last_pts_ = frame.pts;
    last_time_base_ = frame.pts_time_base;
}

void DecoderVideo::pushFrame(VideoFrame&& frame)
{
    const size_t out_bytes = frame.buffer_size;

    std::lock_guard<std::mutex> lk(queue_mutex_);

    if (config_.drop_oldest_on_full) {
        while (frames_.size() >= config_.queue_capacity && !frames_.empty()) {
            const size_t dropped_bytes = frames_.front().buffer_size;
            frames_.pop_front();

            const size_t prev_bytes = queued_bytes_.load(std::memory_order_acquire);
            queued_bytes_.store(prev_bytes >= dropped_bytes ? prev_bytes - dropped_bytes : 0u,
                                std::memory_order_release);
        }
    } else {
        if (frames_.size() >= config_.queue_capacity) {
            return;
        }
    }

    frames_.push_back(std::move(frame));
    queue_depth_.store(frames_.size(), std::memory_order_release);

    const size_t new_bytes =
        queued_bytes_.fetch_add(out_bytes, std::memory_order_acq_rel) + out_bytes;
    const size_t new_depth = frames_.size();

    if (new_depth > high_water_queue_depth_.load(std::memory_order_acquire)) {
        high_water_queue_depth_.store(new_depth, std::memory_order_release);
    }
    if (new_bytes > high_water_queued_bytes_.load(std::memory_order_acquire)) {
        high_water_queued_bytes_.store(new_bytes, std::memory_order_release);
    }

    queue_cv_.notify_one();
}

bool DecoderVideo::peekFrameTimestamp(int64_t& pts, AVRational& time_base) const
{
    std::lock_guard<std::mutex> lk(queue_mutex_);
    if (frames_.empty()) {
        return false;
    }

    pts = frames_.front().pts;
    time_base =
        (frames_.front().pts_time_base.num > 0 && frames_.front().pts_time_base.den > 0)
            ? frames_.front().pts_time_base
            : frames_.front().time_base;
    return true;
}

bool DecoderVideo::getCadenceHint(AVRational& nominal_frame_rate, bool& interlaced) const
{
    std::lock_guard<std::mutex> lk(queue_mutex_);

    if (!frames_.empty() &&
        frames_.front().nominal_frame_rate.num > 0 &&
        frames_.front().nominal_frame_rate.den > 0) {
        nominal_frame_rate = frames_.front().nominal_frame_rate;
        interlaced = frames_.front().interlaced;
        return true;
    }

    if (last_nominal_frame_rate_.num > 0 && last_nominal_frame_rate_.den > 0) {
        nominal_frame_rate = last_nominal_frame_rate_;
        interlaced = last_interlaced_;
        return true;
    }

    return false;
}

bool DecoderVideo::popFrame(VideoFrame& out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(queue_mutex_);
    if (!queue_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            return !frames_.empty() || !running_.load(std::memory_order_acquire);
        })) {
        return false;
    }

    if (frames_.empty()) {
        return false;
    }

    const size_t frame_bytes = frames_.front().buffer_size;
    out = std::move(frames_.front());
    frames_.pop_front();

    queue_depth_.store(frames_.size(), std::memory_order_release);

    const size_t prev_bytes = queued_bytes_.load(std::memory_order_acquire);
    queued_bytes_.store(prev_bytes >= frame_bytes ? prev_bytes - frame_bytes : 0u,
                        std::memory_order_release);

    return true;
}

void DecoderVideo::decodeLoop()
{
    if (!demuxer_ || !codec_ctx_ || stream_index_ < 0) {
        running_.store(false, std::memory_order_release);
        queue_cv_.notify_all();
        return;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        setLastError("Failed to allocate video decode frame.");
        running_.store(false, std::memory_order_release);
        queue_cv_.notify_all();
        return;
    }

    DemuxedPacket dpkt;
    bool decoder_flushed_for_generation_change = false;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (!demuxer_->popVideoPacket(dpkt, 100)) {
            continue;
        }

        if (!dpkt.pkt) {
            continue;
        }

        if (dpkt.stream_index != stream_index_) {
            continue;
        }

        if (dpkt.generation != opened_generation_) {
            if (!decoder_flushed_for_generation_change) {
                flushDecoder();
                decoder_flushed_for_generation_change = true;
                std::cerr << "[DecoderVideo] Dropping stale packet from generation "
                          << dpkt.generation
                          << " while bound to generation "
                          << opened_generation_
                          << "\n";
            }
            continue;
        }

        decoder_flushed_for_generation_change = false;

        if (waiting_for_start_keyframe_) {
            const bool isKeyPacket = (dpkt.pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (!isKeyPacket) {
                ++dropped_until_keyframe_;
                continue;
            }

            // Start from a clean decoder point. This is intentionally done only
            // during acquisition/re-acquisition; after lock, normal PTS
            // continuity is handled by the playout controller.
            flushDecoder();
            waiting_for_start_keyframe_ = false;
            std::cerr << "[DecoderVideo] Acquisition keyframe accepted. dropped_pre_key="
                      << dropped_until_keyframe_
                      << " pkt_pts=" << dpkt.pkt->pts
                      << " pkt_dts=" << dpkt.pkt->dts
                      << " generation=" << opened_generation_
                      << "\n";
        }

        const auto drainDecodedFrames = [&]() -> bool {
            while (!stop_requested_.load(std::memory_order_acquire)) {
                const int receive_ret = avcodec_receive_frame(codec_ctx_, frame);
                if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF) {
                    return true;
                }

                if (receive_ret < 0) {
                    setLastError(std::string("avcodec_receive_frame(video) failed: ") + ffErr(receive_ret));
                    return false;
                }

                VideoFrame out;
                if (copyFrame(frame, out)) {
                    updateCadenceEstimate(out);
                    pushFrame(std::move(out));
                }

                av_frame_unref(frame);
            }

            return false;
        };

        int ret = avcodec_send_packet(codec_ctx_, dpkt.pkt.get());
        if (ret == AVERROR(EAGAIN)) {
            // FFmpeg requires us to drain decoded frames, then retry the same
            // packet. Dropping this packet here causes intermittent video loss
            // under decoder back-pressure.
            if (!drainDecodedFrames()) {
                continue;
            }

            ret = avcodec_send_packet(codec_ctx_, dpkt.pkt.get());
        }

        if (ret < 0) {
            setLastError(std::string("avcodec_send_packet(video) failed: ") + ffErr(ret));
            continue;
        }

        drainDecodedFrames();
    }

    if (codec_ctx_) {
        avcodec_send_packet(codec_ctx_, nullptr);

        while (avcodec_receive_frame(codec_ctx_, frame) == 0) {
            VideoFrame out;
            if (copyFrame(frame, out)) {
                updateCadenceEstimate(out);
                pushFrame(std::move(out));
            }
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    running_.store(false, std::memory_order_release);
    queue_cv_.notify_all();
}