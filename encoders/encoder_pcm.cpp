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
 * PCM/ST 302M audio encoder implementation. This module validates channel routing, builds fixed-size PCM packets, handles timestamp discontinuities, and drains encoded packets for MPEG-TS carriage.
 */

#include "encoder_pcm.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <array>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace {

static std::string joinChannels(const std::vector<int>& channels)
{
    std::ostringstream oss;
    for (size_t i = 0; i < channels.size(); ++i) {
        if (i) oss << ",";
        oss << channels[i];
    }
    return oss.str();
}

static AVChannelLayout defaultLayoutForChannels(int channels)
{
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    return layout;
}

static constexpr int kS302MPacketSamples = 480; // 10 ms at 48 kHz; independent from video FPS.

static int64_t pcmDiscontinuityThresholdSamples(int sampleRate)
{
    // Treat jumps larger than 100 ms as a real source discontinuity. Smaller
    // jitter should not reset the live audio packetizer.
    return std::max<int64_t>(kS302MPacketSamples * 4, static_cast<int64_t>(sampleRate) / 10);
}

static inline int32_t pcmS16LeToS32LeftJustified(const uint8_t* p)
{
    int16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return static_cast<int32_t>(v) << 16;
}

static inline int32_t pcmS24LeToS32LeftJustified(const uint8_t* p)
{
    int32_t v = static_cast<int32_t>(p[0]) |
                (static_cast<int32_t>(p[1]) << 8) |
                (static_cast<int32_t>(p[2]) << 16);
    if (v & 0x00800000) {
        v |= static_cast<int32_t>(0xFF000000);
    }

    // FFmpeg's s32 PCM path expects full-scale signed 32-bit samples.
    // Left-align packed 24-bit data so the valid AES/PCM payload bits are
    // preserved by the S302M packetizer. This is carriage/wrapping only;
    // Dolby E payload data is not decoded or re-encoded here.
    return v << 8;
}

static inline int32_t pcmS32ContainerToS32LeftJustified(const uint8_t* p)
{
    int32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    // DeckLink audio is already represented by NxFrame as a signed 32-bit
    // container with 24 valid MSBs. Do not shift or truncate this value.
    return v;
}

} // namespace

EncoderPCM::EncoderPCM(const json& preset)
    : audioConfig(preset.contains("audio") && preset["audio"].is_object() ? preset["audio"] : json::object())
{
    sample_rate = audioConfig.value("sample_rate", 48000);
    channels = audioConfig.value("channels", 2);
    bytes_per_sample = 2;
    configured_input_channels = audioConfig.value("input_channels", 0);
    codecName = audioConfig.value("codec", std::string("pcm"));
    mode = audioConfig.value("mode", std::string("pcm"));
}

EncoderPCM::~EncoderPCM()
{
    freePendingPackets();

    if (input_frame) {
        av_frame_free(&input_frame);
    }
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
}

bool EncoderPCM::parsePassthroughPairs()
{
    passthrough_pairs.clear();

    if (!audioConfig.contains("passthrough_pairs")) {
        return true;
    }
    if (!audioConfig["passthrough_pairs"].is_array()) {
        std::cerr << "[EncoderPCM] ERROR: audio.passthrough_pairs must be an array.\n";
        return false;
    }

    bool ok = true;
    for (const auto& entry : audioConfig["passthrough_pairs"]) {
        if (!entry.is_object()) {
            ok = false;
            continue;
        }
        if (!entry.contains("channels") || !entry["channels"].is_array() || entry["channels"].size() != 2) {
            ok = false;
            continue;
        }

        PassthroughPair p;
        p.type = entry.value("type", std::string("unknown"));
        p.chA = entry["channels"][0].get<int>();
        p.chB = entry["channels"][1].get<int>();

        if (p.chA < 1 || p.chB < 1 || p.chA == p.chB) {
            ok = false;
            continue;
        }

        passthrough_pairs.push_back(p);
    }

    return ok;
}

bool EncoderPCM::parseChannelMap()
{
    channel_map_input_to_output.clear();

    if (audioConfig.contains("channel_map")) {
        if (!audioConfig["channel_map"].is_array()) {
            std::cerr << "[EncoderPCM] ERROR: audio.channel_map must be an array when provided.\n";
            return false;
        }

        const auto& map = audioConfig["channel_map"];
        if (static_cast<int>(map.size()) != channels) {
            std::cerr << "[EncoderPCM] ERROR: audio.channel_map size (" << map.size()
                      << ") must match configured output channels (" << channels << ").\n";
            return false;
        }

        std::set<int> seen;
        for (const auto& entry : map) {
            if (!entry.is_number_integer()) {
                std::cerr << "[EncoderPCM] ERROR: audio.channel_map entries must be integers.\n";
                return false;
            }
            const int ch = entry.get<int>();
            if (ch < 1) {
                std::cerr << "[EncoderPCM] ERROR: audio.channel_map entries must be 1-based positive channels.\n";
                return false;
            }
            if (!seen.insert(ch).second) {
                std::cerr << "[EncoderPCM] ERROR: audio.channel_map contains duplicate input channel "
                          << ch << ".\n";
                return false;
            }
            channel_map_input_to_output.push_back(ch);
        }
    } else {
        for (int ch = 1; ch <= channels; ++ch) {
            channel_map_input_to_output.push_back(ch);
        }
    }

    if (configured_input_channels > 0) {
        for (size_t i = 0; i < channel_map_input_to_output.size(); ++i) {
            if (channel_map_input_to_output[i] > configured_input_channels) {
                std::cerr << "[EncoderPCM] ERROR: audio.channel_map references input channel "
                          << channel_map_input_to_output[i] << " beyond configured input_channels="
                          << configured_input_channels << ".\n";
                return false;
            }
        }
    }

    return true;
}

bool EncoderPCM::validateProtectedRouting() const
{
    if (passthrough_pairs.empty()) {
        return true;
    }

    if (sample_rate != 48000) {
        std::cerr << "[EncoderPCM] ERROR: passthrough_pairs require exact 48 kHz carriage.\n";
        return false;
    }

    if (channels < 2 || (channels % 2) != 0) {
        std::cerr << "[EncoderPCM] ERROR: passthrough_pairs require an even output channel count >= 2.\n";
        return false;
    }

    for (size_t i = 0; i < passthrough_pairs.size(); ++i) {
        const PassthroughPair& pair = passthrough_pairs[i];
        bool found = false;

        for (size_t out = 0; out + 1 < channel_map_input_to_output.size(); ++out) {
            if (channel_map_input_to_output[out] == pair.chA &&
                channel_map_input_to_output[out + 1] == pair.chB &&
                (out % 2) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            std::cerr << "[EncoderPCM] ERROR: passthrough pair ["
                      << pair.chA << ", " << pair.chB
                      << "] must appear contiguously and in order in audio.channel_map.\n";
            return false;
        }
    }

    return true;
}

bool EncoderPCM::initialize()
{
    if (channels <= 0 || sample_rate <= 0) {
        std::cerr << "[EncoderPCM] ERROR: invalid audio configuration. channels="
                  << channels << " sample_rate=" << sample_rate << "\n";
        return false;
    }

    if (!parsePassthroughPairs()) {
        std::cerr << "[EncoderPCM] WARNING: one or more passthrough_pairs entries were ignored due to validation failures.\n";
    }
    if (!parseChannelMap()) {
        return false;
    }
    if (!validateProtectedRouting()) {
        return false;
    }

    protected_passthrough_mode = !passthrough_pairs.empty();
    bits_per_raw_sample = audioConfig.value("bits_per_raw_sample", audioConfig.value("bit_depth", protected_passthrough_mode ? 20 : 16));
    if (bits_per_raw_sample != 16 && bits_per_raw_sample != 20 && bits_per_raw_sample != 24) {
        std::cerr << "[EncoderPCM] ERROR: audio.bits_per_raw_sample must be 16, 20 or 24 for S302M. configured="
                  << bits_per_raw_sample << "\n";
        return false;
    }
    if (protected_passthrough_mode && bits_per_raw_sample < 20) {
        std::cerr << "[EncoderPCM] ERROR: Dolby E/protected passthrough requires 20-bit or 24-bit S302M carriage.\n";
        return false;
    }
    bytes_per_sample = (bits_per_raw_sample > 16) ? 4 : 2;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_S302M);
    if (!codec) {
        std::cerr << "[EncoderPCM] ERROR: SMPTE 302M packetizer not available in this FFmpeg build.\n";
        return false;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "[EncoderPCM] ERROR: failed to allocate codec context.\n";
        return false;
    }

    codec_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    codec_ctx->codec_id = AV_CODEC_ID_S302M;
    codec_ctx->sample_fmt = (bits_per_raw_sample > 16) ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;
    codec_ctx->sample_rate = sample_rate;
    av_channel_layout_uninit(&codec_ctx->ch_layout);
    av_channel_layout_default(&codec_ctx->ch_layout, channels);
    codec_ctx->time_base = AVRational{1, sample_rate};
    codec_ctx->frame_size = 0;
    codec_ctx->bits_per_raw_sample = bits_per_raw_sample;
    codec_ctx->bit_rate = static_cast<int64_t>(sample_rate) * channels * bits_per_raw_sample;
    codec_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[EncoderPCM] ERROR: failed to open SMPTE 302M packetizer.\n";
        return false;
    }

    input_frame = av_frame_alloc();
    if (!input_frame) {
        std::cerr << "[EncoderPCM] ERROR: failed to allocate input frame.\n";
        return false;
    }

    logConfigurationSummary();
    return true;
}

void EncoderPCM::logConfigurationSummary() const
{
    std::cout << "[EncoderPCM] Initialized SMPTE 302M audio packetizer: "
              << channels << " ch @ " << sample_rate
              << " Hz, input s16/s24/s32 auto -> TS s302m"
              << " output=" << (codec_ctx && codec_ctx->sample_fmt == AV_SAMPLE_FMT_S32 ? "s32" : "s16")
              << "/" << bits_per_raw_sample << "-bit";
    std::cout << "\n";
    std::cout << "[EncoderPCM] Output channel map (output<-input): "
              << joinChannels(channel_map_input_to_output) << "\n";
    std::cout << "[EncoderPCM] S302M packet cadence: "
              << kS302MPacketSamples << " samples/packet (10 ms at 48 kHz), video-rate independent\n";

    if (!passthrough_pairs.empty()) {
        for (size_t i = 0; i < passthrough_pairs.size(); ++i) {
            std::cout << "[EncoderPCM] Protected passthrough pair "
                      << passthrough_pairs[i].chA << "-" << passthrough_pairs[i].chB
                      << " type=" << passthrough_pairs[i].type
                      << " bit_depth=" << bits_per_raw_sample
                      << " (bit-transparent S302M carriage only; no Dolby E decode/encode, no SRC, no remix)\n";
        }
    }
}

bool EncoderPCM::ensureInputFrameCapacity(int nb_samples)
{
    if (!input_frame || !codec_ctx || nb_samples <= 0) {
        return false;
    }

    const bool needRealloc =
        !input_frame->buf[0] ||
        input_frame->format != codec_ctx->sample_fmt ||
        av_channel_layout_compare(&input_frame->ch_layout, &codec_ctx->ch_layout) != 0 ||
        input_frame->sample_rate != codec_ctx->sample_rate ||
        input_frame->nb_samples < nb_samples;

    if (needRealloc) {
        av_frame_unref(input_frame);

        input_frame->nb_samples = nb_samples;
        input_frame->format = codec_ctx->sample_fmt;
        input_frame->sample_rate = codec_ctx->sample_rate;

        av_channel_layout_uninit(&input_frame->ch_layout);
        if (av_channel_layout_copy(&input_frame->ch_layout, &codec_ctx->ch_layout) < 0) {
            std::cerr << "[EncoderPCM] ERROR: failed to copy channel layout into input frame.\n";
            return false;
        }

        const int ret = av_frame_get_buffer(input_frame, 0);
        if (ret < 0) {
            char err[256];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "[EncoderPCM] ERROR: failed to allocate input frame buffer: "
                      << err << "\n";
            return false;
        }
    } else {
        if (av_frame_make_writable(input_frame) < 0) {
            std::cerr << "[EncoderPCM] ERROR: input frame not writable.\n";
            return false;
        }
        input_frame->nb_samples = nb_samples;
    }

    return true;
}


int64_t EncoderPCM::normalizeInputPtsToSampleClock(const AudioFrame& frame) const
{
    if (frame.pts == AV_NOPTS_VALUE) {
        return AV_NOPTS_VALUE;
    }

    AVRational srcTb = frame.time_base;
    if (srcTb.num <= 0 || srcTb.den <= 0) {
        srcTb = AVRational{1, frame.sample_rate > 0 ? frame.sample_rate : sample_rate};
    }

    const AVRational dstTb{1, sample_rate > 0 ? sample_rate : 48000};
    return av_rescale_q(frame.pts, srcTb, dstTb);
}

bool EncoderPCM::appendSelectedPcmToFifo(const AudioFrame& frame)
{
    if (!codec_ctx || !frame.buffer || frame.buffer_size == 0) {
        return false;
    }

    const int in_rate = frame.sample_rate > 0 ? frame.sample_rate : sample_rate;
    if (in_rate != sample_rate) {
        std::cerr << "[EncoderPCM] ERROR: PCM mode currently requires exact sample-rate match. input="
                  << in_rate << " configured=" << sample_rate << "\n";
        return false;
    }

    if (sample_rate != 48000) {
        std::cerr << "[EncoderPCM] ERROR: S302M fixed packetizer currently requires 48 kHz PCM. configured="
                  << sample_rate << "\n";
        return false;
    }

    const int inChannels = frame.channels;
    const int inBytesPerSample = frame.bytes_per_sample;
    const int inSamples = frame.num_samples;
    const int validBits = frame.valid_bits_per_sample > 0
        ? frame.valid_bits_per_sample
        : inBytesPerSample * 8;

    if (inChannels <= 0 || inBytesPerSample <= 0 || inSamples <= 0) {
        std::cerr << "[EncoderPCM] ERROR: invalid AudioFrame metadata: channels=" << inChannels
                  << " bytes_per_sample=" << inBytesPerSample
                  << " num_samples=" << inSamples << "\n";
        return false;
    }

    if (inBytesPerSample != 2 && inBytesPerSample != 3 && inBytesPerSample != 4) {
        std::cerr << "[EncoderPCM] ERROR: unsupported PCM input bytes/sample="
                  << inBytesPerSample << "\n";
        return false;
    }

    const size_t expectedBytes = static_cast<size_t>(inSamples) *
                                 static_cast<size_t>(inChannels) *
                                 static_cast<size_t>(inBytesPerSample);
    if (frame.buffer_size < expectedBytes) {
        std::cerr << "[EncoderPCM] ERROR: AudioFrame buffer too small for metadata. buffer="
                  << frame.buffer_size << " expected=" << expectedBytes << "\n";
        return false;
    }

    for (size_t i = 0; i < channel_map_input_to_output.size(); ++i) {
        if (channel_map_input_to_output[i] > inChannels) {
            std::cerr << "[EncoderPCM] ERROR: output channel " << (i + 1)
                      << " maps from input channel " << channel_map_input_to_output[i]
                      << " but incoming frame only has " << inChannels << " channels.\n";
            return false;
        }
    }

    const int64_t sampleClockPts = normalizeInputPtsToSampleClock(frame);
    if (!handleInputPtsDiscontinuity(sampleClockPts, inSamples)) {
        return false;
    }

    const uint8_t* pcmData = frame.buffer.get();
    for (int n = 0; n < inSamples; ++n) {
        for (int outCh = 0; outCh < channels; ++outCh) {
            const int inCh = channel_map_input_to_output[outCh] - 1;
            const size_t srcIndex = static_cast<size_t>(n) *
                                    static_cast<size_t>(inChannels) +
                                    static_cast<size_t>(inCh);
            const uint8_t* src = pcmData + srcIndex * static_cast<size_t>(inBytesPerSample);

            if (protected_passthrough_mode && (inBytesPerSample == 2 || validBits <= 16)) {
                std::cerr << "[EncoderPCM] ERROR: protected/Dolby E passthrough received non-bit-transparent PCM input. "
                          << "Configure DeckLink input as s32/24-valid.\n";
                return false;
            }

            int32_t sample = 0;
            if (inBytesPerSample == 2) {
                sample = pcmS16LeToS32LeftJustified(src);
            } else if (inBytesPerSample == 3) {
                sample = pcmS24LeToS32LeftJustified(src);
            } else {
                sample = pcmS32ContainerToS32LeftJustified(src);
            }

            pcm_fifo_.push_back(sample);
        }
    }

    return true;
}

void EncoderPCM::resetBufferedAudioState(int64_t restartPts, const char* reason)
{
    pcm_fifo_.clear();
    freePendingPackets();
    next_audio_pts_ = (restartPts == AV_NOPTS_VALUE) ? 0 : restartPts;
    next_audio_pts_initialized_ = true;
    ++discontinuity_count_;

    std::cerr << "[EncoderPCM] Audio PTS discontinuity: reset buffered PCM state"
              << " reason=" << (reason ? reason : "unknown")
              << " restart_pts=" << next_audio_pts_
              << " count=" << discontinuity_count_ << "\n";
}

bool EncoderPCM::handleInputPtsDiscontinuity(int64_t pts, int incomingSamples)
{
    if (incomingSamples <= 0) {
        return false;
    }

    if (pts == AV_NOPTS_VALUE) {
        if (!next_audio_pts_initialized_) {
            next_audio_pts_ = 0;
            next_audio_pts_initialized_ = true;
        }
        return true;
    }

    if (!next_audio_pts_initialized_) {
        next_audio_pts_ = pts;
        next_audio_pts_initialized_ = true;
        return true;
    }

    const int64_t fifoSamples = channels > 0
        ? static_cast<int64_t>(pcm_fifo_.size() / static_cast<size_t>(channels))
        : 0;
    const int64_t expectedInputPts = next_audio_pts_ + fifoSamples;
    const int64_t delta = pts - expectedInputPts;
    const int64_t threshold = pcmDiscontinuityThresholdSamples(sample_rate);

    if (std::llabs(delta) > threshold) {
        std::cerr << "[EncoderPCM] WARNING: incoming PCM PTS jump detected"
                  << " pts=" << pts
                  << " expected=" << expectedInputPts
                  << " delta=" << delta
                  << " threshold=" << threshold << " samples\n";
        resetBufferedAudioState(pts, "input-pts-jump");
    }

    return true;
}

// Build one fixed-cadence PCM frame from the internal FIFO. ST 302M-style
// carriage should not depend on arbitrary capture callback sample counts.
bool EncoderPCM::buildFixedPacketFrame()
{
    if (!codec_ctx || channels <= 0) {
        return false;
    }

    const size_t needed = static_cast<size_t>(kS302MPacketSamples) * static_cast<size_t>(channels);
    if (pcm_fifo_.size() < needed) {
        return false;
    }

    if (!ensureInputFrameCapacity(kS302MPacketSamples)) {
        return false;
    }

    if (!input_frame->data[0]) {
        std::cerr << "[EncoderPCM] ERROR: input frame data[0] is null after allocation.\n";
        return false;
    }

    const int neededSamples = kS302MPacketSamples * channels;
    if (codec_ctx->sample_fmt == AV_SAMPLE_FMT_S32) {
        int32_t* dst = reinterpret_cast<int32_t*>(input_frame->data[0]);
        const int dstStrideSamples = input_frame->linesize[0] / static_cast<int>(sizeof(int32_t));
        if (dstStrideSamples < neededSamples) {
            std::cerr << "[EncoderPCM] ERROR: destination s32 audio buffer too small.\n";
            return false;
        }
        for (int i = 0; i < neededSamples; ++i) {
            dst[i] = pcm_fifo_[static_cast<size_t>(i)];
        }
    } else {
        int16_t* dst = reinterpret_cast<int16_t*>(input_frame->data[0]);
        const int dstStrideSamples = input_frame->linesize[0] / static_cast<int>(sizeof(int16_t));
        if (dstStrideSamples < neededSamples) {
            std::cerr << "[EncoderPCM] ERROR: destination s16 audio buffer too small.\n";
            return false;
        }
        for (int i = 0; i < neededSamples; ++i) {
            dst[i] = static_cast<int16_t>(pcm_fifo_[static_cast<size_t>(i)] >> 16);
        }
    }

    input_frame->pts = next_audio_pts_;
    input_frame->nb_samples = kS302MPacketSamples;
    return true;
}

bool EncoderPCM::encodeOneFixedPacket()
{
    if (!buildFixedPacketFrame()) {
        return false;
    }

    const int ret = avcodec_send_frame(codec_ctx, input_frame);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "[EncoderPCM] ERROR: avcodec_send_frame failed: " << err << "\n";
        return false;
    }

    const size_t consumed = static_cast<size_t>(kS302MPacketSamples) * static_cast<size_t>(channels);
    for (size_t i = 0; i < consumed; ++i) {
        pcm_fifo_.pop_front();
    }
    next_audio_pts_ += kS302MPacketSamples;

    drainEncoderPackets();
    return true;
}

void EncoderPCM::drainEncoderPackets()
{
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            std::cerr << "[EncoderPCM] ERROR: failed to allocate packet.\n";
            return;
        }

        const int ret = avcodec_receive_packet(codec_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            return;
        }
        if (ret < 0) {
            char err[256];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "[EncoderPCM] ERROR: avcodec_receive_packet failed: " << err << "\n";
            av_packet_free(&pkt);
            return;
        }

        pending_packets.push_back(pkt);
    }
}

AVPacket* EncoderPCM::popPendingPacket()
{
    if (pending_packets.empty()) {
        return nullptr;
    }

    AVPacket* pkt = pending_packets.front();
    pending_packets.pop_front();
    return pkt;
}

void EncoderPCM::freePendingPackets()
{
    while (!pending_packets.empty()) {
        AVPacket* pkt = pending_packets.front();
        pending_packets.pop_front();
        av_packet_free(&pkt);
    }
}

AVPacket* EncoderPCM::receivePacket()
{
    if (!codec_ctx) {
        std::cerr << "[EncoderPCM] ERROR: codec context not initialized.\n";
        return nullptr;
    }

    if (AVPacket* pending = popPendingPacket()) {
        return pending;
    }

    if (pcm_fifo_.size() >= static_cast<size_t>(kS302MPacketSamples) * static_cast<size_t>(channels)) {
        if (!encodeOneFixedPacket()) {
            return nullptr;
        }
    }

    return popPendingPacket();
}

// Public PCM submit entry point. Incoming samples are buffered and emitted only
// when a complete fixed-size transport packet can be built.
AVPacket* EncoderPCM::encodeAudioFrame(const AudioFrame& frame)
{
    if (!codec_ctx) {
        std::cerr << "[EncoderPCM] ERROR: codec context not initialized.\n";
        return nullptr;
    }

    if (AVPacket* pending = popPendingPacket()) {
        return pending;
    }

    if (frame.buffer && frame.buffer_size > 0) {
        if (!appendSelectedPcmToFifo(frame)) {
            return nullptr;
        }
    }

    return receivePacket();
}

std::vector<AVPacket*> EncoderPCM::flush()
{
    std::vector<AVPacket*> out;

    if (codec_ctx) {
        const int ret = avcodec_send_frame(codec_ctx, nullptr);
        if (ret < 0 && ret != AVERROR_EOF) {
            char err[256];
            av_strerror(ret, err, sizeof(err));
            std::cerr << "[EncoderPCM] WARNING: avcodec_send_frame(flush) failed: " << err << "\n";
        }
        drainEncoderPackets();
    }

    while (!pending_packets.empty()) {
        out.push_back(popPendingPacket());
    }
    return out;
}

AVCodecContext* EncoderPCM::getCodecContext() const
{
    return codec_ctx;
}