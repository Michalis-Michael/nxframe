/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/decoder_audio.cpp
 * Description: Implements audio decoding, resampling and decoded AudioFrame queueing.
 */

#include "receiver/decoder_audio.h"

#include <chrono>
#include <cstring>
#include <iostream>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

namespace {

static std::string ffErrA(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), errnum);
    return std::string(buf);
}

static bool channelLayoutsEqual(const AVChannelLayout& a, const AVChannelLayout& b)
{
    return av_channel_layout_compare(&a, &b) == 0;
}

static int channelCountFromFrame(const AVFrame* frame)
{
    if (!frame) {
        return 0;
    }
    return frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 0;
}

static AVChannelLayout unspecifiedLayoutForChannels(int channels)
{
    AVChannelLayout layout{};
    if (channels > 0) {
        layout.order = AV_CHANNEL_ORDER_UNSPEC;
        layout.nb_channels = channels;
    }
    return layout;
}

static bool isS302MCodec(enum AVCodecID codec_id)
{
    return codec_id == AV_CODEC_ID_S302M;
}

static void forceDefaultS302MLayoutIfMissing(AVCodecContext* ctx)
{
    if (!ctx || !isS302MCodec(ctx->codec_id)) {
        return;
    }

    // FFmpeg's MPEG-TS demuxer can expose SMPTE 302M audio with incomplete
    // codec parameters at low analyze/probe settings, for example
    // "s302m, 0 channels". NxFrame creates one S302M stream per embedded
    // stereo pair, so a missing channel layout should be treated as a 2ch
    // pair instead of opening a 0-channel decoder and depending on later
    // frame side data to repair it.
    if (ctx->ch_layout.nb_channels <= 0) {
        av_channel_layout_uninit(&ctx->ch_layout);
        ctx->ch_layout = unspecifiedLayoutForChannels(2);
    }
    if (ctx->sample_rate <= 0) {
        ctx->sample_rate = 48000;
    }
}

static bool shouldPreserveRawChannelOrder(const AVFrame* src, int output_channels)
{
    if (!src) {
        return false;
    }

    const int input_channels = channelCountFromFrame(src);
    return input_channels > 0 && output_channels > 0 && input_channels == output_channels;
}

} // namespace

DecoderAudio::DecoderAudio() = default;

DecoderAudio::~DecoderAudio()
{
    stop();
}

void DecoderAudio::setLastError(const std::string& err)
{
    std::lock_guard<std::mutex> lk(err_mutex_);
    last_error_ = err;
}

bool DecoderAudio::init(DemuxerTS& demuxer, const Config& config)
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
    decoded_channels_.store(0, std::memory_order_release);
    bound_generation_.store(0, std::memory_order_release);
    next_output_pts_ = AV_NOPTS_VALUE;
    next_output_pts_valid_ = false;

    {
        std::lock_guard<std::mutex> lk(err_mutex_);
        last_error_.clear();
    }

    if (!openDecoder()) {
        return false;
    }

    if (stream_index_ < 0) {
        return true;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&DecoderAudio::decodeLoop, this);
    return true;
}

void DecoderAudio::resetResampler()
{
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }

    if (resample_state_.valid) {
        av_channel_layout_uninit(&resample_state_.in_ch_layout);
        av_channel_layout_uninit(&resample_state_.out_ch_layout);
        resample_state_ = ResampleSourceState();
    }
}

void DecoderAudio::closeDecoder()
{
    resetResampler();

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    codec_ = nullptr;
    stream_index_ = -1;
    snapshot_.reset();
    opened_generation_ = 0;
    bound_generation_.store(0, std::memory_order_release);
    decoded_channels_.store(0, std::memory_order_release);
    next_output_pts_ = AV_NOPTS_VALUE;
    next_output_pts_valid_ = false;
}

void DecoderAudio::stop()
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

std::string DecoderAudio::getLastError() const
{
    std::lock_guard<std::mutex> lk(err_mutex_);
    return last_error_;
}

bool DecoderAudio::openDecoder()
{
    if (!demuxer_) {
        setLastError("DecoderAudio::openDecoder called without demuxer.");
        return false;
    }

    snapshot_ = demuxer_->snapshot();
    if (!snapshot_) {
        setLastError("No demux snapshot available for audio decoder.");
        return false;
    }

    stream_index_ =
        (config_.input_stream_index >= 0)
            ? config_.input_stream_index
            : snapshot_->primary_audio_stream_index;

    if (stream_index_ < 0) {
        std::cerr << "[DecoderAudio] No audio stream available. Audio decoder inactive.\n";
        return true;
    }

    std::map<int, DemuxerTS::CodecParametersPtr>::const_iterator cp_it =
        snapshot_->codecpar_by_stream.find(stream_index_);
    if (cp_it == snapshot_->codecpar_by_stream.end() || !cp_it->second) {
        setLastError("Missing audio codec parameters in demux snapshot for stream " +
                     std::to_string(stream_index_) + ".");
        snapshot_.reset();
        stream_index_ = -1;
        return false;
    }

    codec_ = avcodec_find_decoder(cp_it->second->codec_id);
    if (!codec_) {
        setLastError("No FFmpeg audio decoder for codec_id=" +
                     std::to_string(cp_it->second->codec_id));
        snapshot_.reset();
        stream_index_ = -1;
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        setLastError("Failed to allocate audio decoder context.");
        snapshot_.reset();
        stream_index_ = -1;
        return false;
    }

    int ret = avcodec_parameters_to_context(codec_ctx_, cp_it->second.get());
    if (ret < 0) {
        setLastError(std::string("avcodec_parameters_to_context(audio) failed: ") + ffErrA(ret));
        closeDecoder();
        return false;
    }

    forceDefaultS302MLayoutIfMissing(codec_ctx_);

    codec_ctx_->pkt_timebase = demuxer_->audioTimeBaseForStream(stream_index_);
    codec_ctx_->thread_count = config_.thread_count;

    ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (ret < 0) {
        setLastError(std::string("Failed to open audio decoder: ") + ffErrA(ret));
        closeDecoder();
        return false;
    }

    const int initial_channels = codec_ctx_->ch_layout.nb_channels;

    decoded_channels_.store(initial_channels, std::memory_order_release);

    opened_generation_ = snapshot_->generation;
    bound_generation_.store(opened_generation_, std::memory_order_release);

    const int effective_output_channels =
        (config_.output_channels > 0)
            ? config_.output_channels
            : initial_channels;

    std::cerr << "[DecoderAudio] Opened decoder stream_index=" << stream_index_
              << " codec=" << codec_->name
              << " input_channels=" << initial_channels
              << " output_channels=" << effective_output_channels
              << " generation=" << opened_generation_
              << "\n";

    return true;
}

void DecoderAudio::flushDecoder()
{
    if (!codec_ctx_) {
        return;
    }

    avcodec_flush_buffers(codec_ctx_);
    resetResampler();
    next_output_pts_ = AV_NOPTS_VALUE;
    next_output_pts_valid_ = false;
}

bool DecoderAudio::ensureResamplerForFrame(const AVFrame* src,
                                           int output_channels,
                                           bool& use_passthrough)
{
    use_passthrough = false;

    if (!src) {
        return false;
    }

    const int input_channels = channelCountFromFrame(src);
    if (input_channels <= 0 || output_channels <= 0 || src->sample_rate <= 0) {
        return false;
    }

    const AVSampleFormat in_fmt = static_cast<AVSampleFormat>(src->format);
    const AVSampleFormat out_fmt = config_.output_sample_fmt;
    const int in_rate = src->sample_rate;
    const int out_rate = config_.output_sample_rate;

    if (input_channels == output_channels &&
        in_fmt == out_fmt &&
        in_rate == out_rate) {
        resetResampler();
        use_passthrough = true;
        return true;
    }

    AVChannelLayout in_layout{};
    AVChannelLayout out_layout{};

    if (shouldPreserveRawChannelOrder(src, output_channels)) {
        in_layout = unspecifiedLayoutForChannels(input_channels);
        out_layout = unspecifiedLayoutForChannels(output_channels);
    } else if (src->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&in_layout, &src->ch_layout) < 0) {
            return false;
        }
        av_channel_layout_default(&out_layout, output_channels);
    } else {
        av_channel_layout_default(&in_layout, input_channels);
        av_channel_layout_default(&out_layout, output_channels);
    }

    const bool need_rebuild =
        !resample_state_.valid ||
        !channelLayoutsEqual(resample_state_.in_ch_layout, in_layout) ||
        !channelLayoutsEqual(resample_state_.out_ch_layout, out_layout) ||
        resample_state_.in_sample_fmt != in_fmt ||
        resample_state_.out_sample_fmt != out_fmt ||
        resample_state_.in_sample_rate != in_rate ||
        resample_state_.out_sample_rate != out_rate;

    if (!need_rebuild) {
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        return true;
    }

    resetResampler();

    SwrContext* swr = nullptr;
    int ret = swr_alloc_set_opts2(&swr,
                                  &out_layout,
                                  out_fmt,
                                  out_rate,
                                  &in_layout,
                                  in_fmt,
                                  in_rate,
                                  0,
                                  nullptr);
    if (ret < 0 || !swr) {
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        setLastError(std::string("swr_alloc_set_opts2 failed: ") + ffErrA(ret));
        return false;
    }

    ret = swr_init(swr);
    if (ret < 0) {
        swr_free(&swr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        setLastError(std::string("swr_init failed: ") + ffErrA(ret));
        return false;
    }

    swr_ctx_ = swr;
    resample_state_.in_ch_layout = in_layout;
    resample_state_.out_ch_layout = out_layout;
    resample_state_.in_sample_fmt = in_fmt;
    resample_state_.out_sample_fmt = out_fmt;
    resample_state_.in_sample_rate = in_rate;
    resample_state_.out_sample_rate = out_rate;
    resample_state_.valid = true;

    return true;
}

bool DecoderAudio::convertFrame(const AVFrame* src, AudioFrame& out)
{
    if (!src || !codec_ctx_) {
        return false;
    }

    const int input_channels = channelCountFromFrame(src);
    const int output_channels =
        (config_.output_channels > 0) ? config_.output_channels : input_channels;

    if (input_channels < 1 || output_channels < 1 || src->nb_samples <= 0) {
        return false;
    }

    bool use_passthrough = false;
    if (!ensureResamplerForFrame(src, output_channels, use_passthrough)) {
        return false;
    }

    const int bytes_per_sample = av_get_bytes_per_sample(config_.output_sample_fmt);
    if (bytes_per_sample <= 0) {
        return false;
    }

    const int64_t src_pts =
        (src->best_effort_timestamp != AV_NOPTS_VALUE)
            ? src->best_effort_timestamp
            : src->pts;

    int64_t rescaled_src_pts = AV_NOPTS_VALUE;
    if (src_pts != AV_NOPTS_VALUE &&
        codec_ctx_->pkt_timebase.num > 0 &&
        codec_ctx_->pkt_timebase.den > 0) {
        rescaled_src_pts = av_rescale_q(src_pts,
                                        codec_ctx_->pkt_timebase,
                                        AVRational{1, config_.output_sample_rate});
    }

    int converted = 0;
    out = AudioFrame();

    if (use_passthrough) {
        converted = src->nb_samples;
        const size_t total_bytes =
            static_cast<size_t>(converted) *
            static_cast<size_t>(output_channels) *
            static_cast<size_t>(bytes_per_sample);

        out.buffer = make_shared_u8(total_bytes > 0 ? total_bytes : 1);
        out.buffer_size = total_bytes;
        if (total_bytes > 0) {
            std::memcpy(out.buffer.get(), src->extended_data[0], total_bytes);
        }
    } else {
        const int max_out_samples = av_rescale_rnd(
            swr_get_delay(swr_ctx_, src->sample_rate) + src->nb_samples,
            config_.output_sample_rate,
            src->sample_rate,
            AV_ROUND_UP);

        if (max_out_samples <= 0) {
            return false;
        }

        uint8_t** out_data = nullptr;
        int out_linesize = 0;

        const int alloc_ret = av_samples_alloc_array_and_samples(&out_data,
                                                                 &out_linesize,
                                                                 output_channels,
                                                                 max_out_samples,
                                                                 config_.output_sample_fmt,
                                                                 1);
        if (alloc_ret < 0) {
            setLastError(std::string("av_samples_alloc_array_and_samples failed: ") + ffErrA(alloc_ret));
            return false;
        }

        converted = swr_convert(swr_ctx_,
                                out_data,
                                max_out_samples,
                                const_cast<const uint8_t**>(src->extended_data),
                                src->nb_samples);
        if (converted < 0) {
            setLastError(std::string("swr_convert failed: ") + ffErrA(converted));
            if (out_data) {
                av_freep(&out_data[0]);
            }
            av_freep(&out_data);
            return false;
        }

        const size_t total_bytes =
            static_cast<size_t>(converted) *
            static_cast<size_t>(output_channels) *
            static_cast<size_t>(bytes_per_sample);

        out.buffer = make_shared_u8(total_bytes > 0 ? total_bytes : 1);
        out.buffer_size = total_bytes;
        if (total_bytes > 0) {
            std::memcpy(out.buffer.get(), out_data[0], total_bytes);
        }

        if (out_data) {
            av_freep(&out_data[0]);
        }
        av_freep(&out_data);
    }

    out.sample_rate = config_.output_sample_rate;
    out.channels = output_channels;
    out.bytes_per_sample = bytes_per_sample;
    out.num_samples = converted;
    out.time_base = AVRational{1, config_.output_sample_rate};

    if (next_output_pts_valid_) {
        if (rescaled_src_pts != AV_NOPTS_VALUE) {
            const int64_t discontinuity_threshold =
                std::max<int64_t>(static_cast<int64_t>(config_.output_sample_rate / 10),
                                  static_cast<int64_t>(converted * 2));
            if (std::llabs(rescaled_src_pts - next_output_pts_) > discontinuity_threshold) {
                next_output_pts_ = rescaled_src_pts;
            }
        }
        out.pts = next_output_pts_;
    } else if (rescaled_src_pts != AV_NOPTS_VALUE) {
        out.pts = rescaled_src_pts;
        next_output_pts_ = rescaled_src_pts;
        next_output_pts_valid_ = true;
    } else {
        out.pts = AV_NOPTS_VALUE;
    }

    if (out.pts != AV_NOPTS_VALUE) {
        next_output_pts_ = out.pts + static_cast<int64_t>(converted);
        next_output_pts_valid_ = true;
    } else {
        next_output_pts_ = AV_NOPTS_VALUE;
        next_output_pts_valid_ = false;
    }

    decoded_channels_.store(input_channels, std::memory_order_release);
    return true;
}

void DecoderAudio::pushFrame(AudioFrame&& out)
{
    const size_t out_bytes = out.buffer_size;

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

    frames_.push_back(std::move(out));
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

bool DecoderAudio::popFrame(AudioFrame& out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(queue_mutex_);

    if (timeout_ms <= 0) {
        if (frames_.empty()) {
            return false;
        }
    } else {
        const bool ready = queue_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this]() {
            return !frames_.empty() || !running_.load(std::memory_order_acquire) ||
                   stop_requested_.load(std::memory_order_acquire);
        });

        if (!ready || frames_.empty()) {
            return false;
        }
    }

    out = std::move(frames_.front());
    frames_.pop_front();
    queue_depth_.store(frames_.size(), std::memory_order_release);

    const size_t prev_bytes = queued_bytes_.load(std::memory_order_acquire);
    queued_bytes_.store(prev_bytes >= out.buffer_size ? prev_bytes - out.buffer_size : 0u,
                        std::memory_order_release);
    return true;
}

void DecoderAudio::decodeLoop()
{
    if (!demuxer_ || stream_index_ < 0 || !codec_ctx_) {
        running_.store(false, std::memory_order_release);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        setLastError("Failed to allocate FFmpeg audio decode packet/frame.");
        running_.store(false, std::memory_order_release);
        return;
    }

    while (!stop_requested_.load(std::memory_order_acquire)) {
        DemuxedPacket in;

        // CRITICAL FIX:
        // Each DecoderAudio instance owns exactly one audio stream.
        // DemuxerTS stores audio packets in per-stream queues, so the decoder
        // must pop from its own stream queue. Using popAudioPacket() here only
        // returns the primary stream and breaks multi-S302M audio: secondary
        // decoder threads starve, the packer emits gaps/silence, and SDI audio
        // becomes intermittent with drifting A/V sync.
        if (!demuxer_->popAudioPacketForStream(stream_index_, in, 100)) {
            continue;
        }

        if (in.stream_index != stream_index_) {
            // Defensive only. popAudioPacketForStream() should already guarantee this.
            continue;
        }

        if (in.generation != opened_generation_) {
            std::cerr << "[DecoderAudio] Drop packet from old generation=" << in.generation
                      << " expected=" << opened_generation_ << " stream=" << stream_index_
                      << "\n";
            continue;
        }

        av_packet_unref(pkt);
        if (!in.pkt) {
            continue;
        }
        const int ref_ret = av_packet_ref(pkt, in.pkt.get());
        if (ref_ret < 0) {
            setLastError(std::string("Audio packet ref failed: ") + ffErrA(ref_ret));
            continue;
        }
        pkt->time_base = in.time_base;

        const int send_ret = avcodec_send_packet(codec_ctx_, pkt);
        if (send_ret == AVERROR(EAGAIN)) {
            while (true) {
                const int recv_ret = avcodec_receive_frame(codec_ctx_, frame);
                if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                    break;
                }
                if (recv_ret < 0) {
                    setLastError(std::string("Audio decode receive failed: ") + ffErrA(recv_ret));
                    stop_requested_.store(true, std::memory_order_release);
                    break;
                }

                AudioFrame out;
                if (convertFrame(frame, out)) {
                    pushFrame(std::move(out));
                }
                av_frame_unref(frame);
            }

            const int retry_ret = avcodec_send_packet(codec_ctx_, pkt);
            if (retry_ret < 0 && retry_ret != AVERROR(EAGAIN)) {
                setLastError(std::string("Audio decode send retry failed: ") + ffErrA(retry_ret));
                stop_requested_.store(true, std::memory_order_release);
                break;
            }
        } else if (send_ret == AVERROR_INVALIDDATA) {
            std::cerr << "[DecoderAudio] Invalid audio packet skipped on stream "
                      << stream_index_ << "\n";
            continue;
        } else if (send_ret < 0) {
            setLastError(std::string("Audio decode send failed: ") + ffErrA(send_ret));
            stop_requested_.store(true, std::memory_order_release);
            break;
        }

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const int recv_ret = avcodec_receive_frame(codec_ctx_, frame);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                break;
            }
            if (recv_ret < 0) {
                setLastError(std::string("Audio decode receive failed: ") + ffErrA(recv_ret));
                stop_requested_.store(true, std::memory_order_release);
                break;
            }

            AudioFrame out;
            if (convertFrame(frame, out)) {
                pushFrame(std::move(out));
            }
            av_frame_unref(frame);
        }
    }

    avcodec_send_packet(codec_ctx_, nullptr);
    while (avcodec_receive_frame(codec_ctx_, frame) == 0) {
        AudioFrame out;
        if (convertFrame(frame, out)) {
            pushFrame(std::move(out));
        }
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    running_.store(false, std::memory_order_release);
    queue_cv_.notify_all();
}