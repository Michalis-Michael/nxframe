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
 * AAC encoder implementation. This module converts incoming AudioFrame data to the codec sample format, buffers complete AAC frames, handles timestamp discontinuities, and drains encoded audio packets.
 */

#include "encoder_aac.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

#include "../stage_timing.h"

namespace {

static std::string ffErrStrAac(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), errnum);
    return std::string(buf);
}

static AVChannelLayout defaultLayoutForChannels(int ch)
{
    AVChannelLayout layout{};
    if (ch > 0) {
        av_channel_layout_default(&layout, ch);
    }
    return layout;
}

static int channelCountFromLayout(const AVChannelLayout& layout)
{
    return layout.nb_channels > 0 ? layout.nb_channels : 0;
}

static std::string normalizeLowerAac(std::string v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] >= 'A' && v[i] <= 'Z') {
            v[i] = static_cast<char>(v[i] - 'A' + 'a');
        }
    }
    return v;
}

static bool isMpeg2AacAlias(const std::string& v)
{
    const std::string c = normalizeLowerAac(v);
    return c == "mpeg2" || c == "mpeg-2" || c == "mpeg2_aac_lc" ||
           c == "mpeg-2_aac_lc" || c == "aac_lc_mpeg2" || c == "aac-lc-mpeg2";
}

static bool isMpeg4AacAlias(const std::string& v)
{
    const std::string c = normalizeLowerAac(v);
    return c == "mpeg4" || c == "mpeg-4" || c == "mpeg4_aac_lc" ||
           c == "mpeg-4_aac_lc" || c == "aac_lc_mpeg4" || c == "aac-lc-mpeg4" ||
           c == "aac" || c == "aac_lc" || c == "aac-lc";
}

static int parseAacOutputChannelsFromConfig(const json& audioConfig)
{
    if (audioConfig.contains("channels")) {
        const auto& channelsNode = audioConfig["channels"];
        if (channelsNode.is_array()) {
            return static_cast<int>(channelsNode.size());
        }
        if (channelsNode.is_number_integer()) {
            return channelsNode.get<int>();
        }
    }
    return 2;
}

static inline int16_t aacReadPcmAsS16(const uint8_t* p, int bytesPerSample)
{
    if (!p) {
        return 0;
    }

    if (bytesPerSample == 2) {
        int16_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    if (bytesPerSample == 3) {
        int32_t v = static_cast<int32_t>(p[0]) |
                    (static_cast<int32_t>(p[1]) << 8) |
                    (static_cast<int32_t>(p[2]) << 16);
        if (v & 0x00800000) {
            v |= static_cast<int32_t>(0xFF000000);
        }
        // 24-bit packed PCM -> keep the top 16 audio bits for AAC-LC.
        return static_cast<int16_t>(v >> 8);
    }

    if (bytesPerSample == 4) {
        int32_t v = 0;
        std::memcpy(&v, p, sizeof(v));
        // DeckLink bmdAudioSampleType32bitInteger is used by NxFrame as
        // s32/24-valid with the valid audio bits in the MSBs.  Feed AAC with
        // normal signed 16-bit PCM, not with the protected/raw 32-bit carrier.
        return static_cast<int16_t>(v >> 16);
    }

    return 0;
}

} // namespace

EncoderAAC::EncoderAAC(const json& preset)
    : audioConfig(preset.contains("audio") && preset["audio"].is_object() ? preset["audio"] : json::object()),
      sample_rate(audioConfig.value("sample_rate", 48000)),
      channels(parseAacOutputChannelsFromConfig(audioConfig)),
      bitrate(audioConfig.value("bitrate", 128000)),
      profile(audioConfig.value("profile", std::string("aac_low"))),
      codec_name(audioConfig.value("codec", std::string("aac_lc_mpeg4"))),
      standard(audioConfig.value("standard", std::string())),
      transport(audioConfig.value("transport", std::string("adts"))),
      codec_id(AV_CODEC_ID_AAC),
      codec_ctx(nullptr),
      input_frame(nullptr),
      output_frame(nullptr),
      encode_frame(nullptr),
      swr_ctx(nullptr),
      fifo(nullptr),
      last_input_fmt(AV_SAMPLE_FMT_NONE),
      last_input_rate(0),
      last_input_channels(0),
      next_pts(0),
      next_pts_initialized(false),
      expected_input_next_pts(0),
      input_pts_initialized(false),
      discontinuity_count(0),
      configured_input_channels(audioConfig.value("input_channels", 0))
{
    parseChannelMap();
}

EncoderAAC::~EncoderAAC()
{
    freePendingPackets();

    if (fifo) {
        av_audio_fifo_free(fifo);
        fifo = nullptr;
    }

    if (input_frame) {
        av_frame_free(&input_frame);
    }
    if (output_frame) {
        av_frame_free(&output_frame);
    }
    if (encode_frame) {
        av_frame_free(&encode_frame);
    }
    if (swr_ctx) {
        swr_free(&swr_ctx);
    }
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
}

void EncoderAAC::freePendingPackets()
{
    while (!pending_packets.empty()) {
        AVPacket* pkt = pending_packets.front();
        pending_packets.pop_front();
        av_packet_free(&pkt);
    }
}

bool EncoderAAC::parseChannelMap()
{
    channel_map_input_to_output.clear();

    if (audioConfig.contains("channel_map")) {
        if (!audioConfig["channel_map"].is_array()) {
            std::cerr << "[EncoderAAC] ERROR: audio.channel_map must be an array when provided.\n";
            return false;
        }

        const auto& map = audioConfig["channel_map"];
        if (static_cast<int>(map.size()) != channels) {
            std::cerr << "[EncoderAAC] ERROR: audio.channel_map size (" << map.size()
                      << ") must match configured output channels (" << channels << ").\n";
            return false;
        }

        std::set<int> seen;
        for (const auto& entry : map) {
            if (!entry.is_number_integer()) {
                std::cerr << "[EncoderAAC] ERROR: audio.channel_map entries must be integers.\n";
                return false;
            }
            const int ch = entry.get<int>();
            if (ch < 1) {
                std::cerr << "[EncoderAAC] ERROR: audio.channel_map entries must be 1-based positive channels.\n";
                return false;
            }
            if (!seen.insert(ch).second) {
                std::cerr << "[EncoderAAC] ERROR: audio.channel_map contains duplicate input channel "
                          << ch << ".\n";
                return false;
            }
            channel_map_input_to_output.push_back(ch);
        }
    } else if (audioConfig.contains("channels") && audioConfig["channels"].is_array()) {
        const auto& map = audioConfig["channels"];
        if (static_cast<int>(map.size()) != channels) {
            std::cerr << "[EncoderAAC] ERROR: audio.channels array size does not match configured output channels.\n";
            return false;
        }

        std::set<int> seen;
        for (const auto& entry : map) {
            if (!entry.is_number_integer()) {
                std::cerr << "[EncoderAAC] ERROR: audio.channels array entries must be integers.\n";
                return false;
            }
            const int ch = entry.get<int>();
            if (ch < 1) {
                std::cerr << "[EncoderAAC] ERROR: audio.channels array entries must be 1-based positive channels.\n";
                return false;
            }
            if (!seen.insert(ch).second) {
                std::cerr << "[EncoderAAC] ERROR: audio.channels array contains duplicate input channel "
                          << ch << ".\n";
                return false;
            }
            channel_map_input_to_output.push_back(ch);
        }
    } else {
        // DeckLink capture normally provides a full embedded-audio bus, e.g.
        // 16 channels of s32/24-valid PCM.  AAC should encode the selected
        // program pair, not downmix all embedded channels.  Without an explicit
        // map, default to the first N input channels, matching EncoderPCM.
        for (int ch = 1; ch <= channels; ++ch) {
            channel_map_input_to_output.push_back(ch);
        }
    }

    if (configured_input_channels > 0) {
        for (int ch : channel_map_input_to_output) {
            if (ch > configured_input_channels) {
                std::cerr << "[EncoderAAC] ERROR: audio channel map references input channel "
                          << ch << " beyond configured input_channels=" << configured_input_channels << ".\n";
                return false;
            }
        }
    }

    return true;
}

bool EncoderAAC::parseAacMode()
{
    codec_name = normalizeLowerAac(codec_name);
    profile = normalizeLowerAac(profile);
    standard = normalizeLowerAac(standard);
    transport = normalizeLowerAac(transport);

    if (standard.empty()) {
        if (isMpeg2AacAlias(codec_name)) {
            standard = "mpeg2";
        } else {
            standard = "mpeg4";
        }
    } else if (isMpeg2AacAlias(standard)) {
        standard = "mpeg2";
    } else if (isMpeg4AacAlias(standard)) {
        standard = "mpeg4";
    }

    if (!isMpeg2AacAlias(standard) && !isMpeg4AacAlias(standard)) {
        std::cerr << "[EncoderAAC] ERROR: unsupported AAC standard='" << standard
                  << "'. Use mpeg4 or mpeg2.\n";
        return false;
    }

    if (profile.empty() || profile == "aac" || profile == "aac_lc" || profile == "aac-lc") {
        profile = "aac_low";
    }

    if (profile != "aac_low") {
        std::cerr << "[EncoderAAC] ERROR: current AAC path supports AAC-LC only. configured profile='"
                  << profile << "'.\n";
        return false;
    }

    if (sample_rate != 48000) {
        std::cerr << "[EncoderAAC] ERROR: SDI AAC path currently supports 48 kHz only. configured="
                  << sample_rate << "\n";
        return false;
    }

    if (channels <= 0 || channels > 8) {
        std::cerr << "[EncoderAAC] ERROR: invalid AAC channel count " << channels << ".\n";
        return false;
    }

    if (bitrate <= 0) {
        std::cerr << "[EncoderAAC] ERROR: invalid AAC bitrate " << bitrate << ".\n";
        return false;
    }

    // FFmpeg's MPEG-TS muxer handles AAC-LC carriage from AV_CODEC_ID_AAC.
    // NxFrame currently carries AAC-LC in MPEG-TS using ADTS.
    //
    // standard=mpeg2 maps to the classic ISO/IEC 13818-7 ADTS AAC-LC
    // contribution workflow.
    //
    // standard=mpeg4 is accepted as an MPEG-4 AAC-LC preset/profile alias while
    // still using ADTS carriage in MPEG-TS for the current transport path. True
    // LATM/LOAS MPEG-4 AAC-in-TS signalling can be added later as a dedicated
    // transport mode if a downstream receiver specifically requires it.
    codec_id = AV_CODEC_ID_AAC;
    if (transport.empty()) {
        transport = "adts";
    }

    if (transport != "adts") {
        std::cerr << "[EncoderAAC] ERROR: current AAC path supports AAC-LC "
                  << "transport='adts' only. requested transport='" << transport
                  << "'. LATM/LOAS can be added as a separate later milestone.\n";
        return false;
    }

    return true;
}

void EncoderAAC::logConfigurationSummary(const char* encoderName) const
{
    const char* isoLabel = (standard == "mpeg2")
        ? "ISO/IEC 13818-7"
        : "ISO/IEC 14496-3";

    std::cout << "[EncoderAAC] Initialized AAC-LC encoder: "
              << channels << " ch @ " << sample_rate << " Hz"
              << " bitrate=" << bitrate
              << " standard=" << standard
              << " iso=" << isoLabel
              << " transport=" << transport
              << " encoder=" << (encoderName ? encoderName : "unknown") << "\n";

    if (!channel_map_input_to_output.empty()) {
        std::cout << "[EncoderAAC] Output channel map (output<-input): ";
        for (size_t i = 0; i < channel_map_input_to_output.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << channel_map_input_to_output[i];
        }
        std::cout << "\n";
    }
}

bool EncoderAAC::initialize()
{
    if (!parseAacMode()) {
        return false;
    }
    if (!parseChannelMap()) {
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libfdk_aac");
    if (!codec) {
        codec = avcodec_find_encoder(codec_id);
    }
    if (!codec) {
        std::cerr << "[EncoderAAC] ERROR: AAC encoder not found.\n";
        return false;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "[EncoderAAC] ERROR: Failed to allocate codec context.\n";
        return false;
    }

    codec_ctx->sample_rate = sample_rate;
    av_channel_layout_uninit(&codec_ctx->ch_layout);
    av_channel_layout_default(&codec_ctx->ch_layout, channels);

    const AVSampleFormat* sample_fmts = nullptr;
    int nb_sample_fmts = 0;

    if (avcodec_get_supported_config(codec_ctx,
                                    codec,
                                    AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                    0,
                                    reinterpret_cast<const void**>(&sample_fmts),
                                    &nb_sample_fmts) >= 0 &&
        sample_fmts && nb_sample_fmts > 0) {
        codec_ctx->sample_fmt = sample_fmts[0];
    } else {
        codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }
    codec_ctx->bit_rate = bitrate;
    codec_ctx->time_base = AVRational{1, sample_rate};
    codec_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    codec_ctx->profile = AV_PROFILE_AAC_LOW;

    const int ret = avcodec_open2(codec_ctx, codec, nullptr);
    if (ret < 0) {
        std::cerr << "[EncoderAAC] ERROR: Failed to open AAC encoder: "
                  << ffErrStrAac(ret) << "\n";
        return false;
    }

    logConfigurationSummary(codec ? codec->name : "unknown");

    const int frame_size = (codec_ctx->frame_size > 0) ? codec_ctx->frame_size : 1024;
    fifo = av_audio_fifo_alloc(codec_ctx->sample_fmt,
                               codec_ctx->ch_layout.nb_channels,
                               frame_size * 16);
    if (!fifo) {
        std::cerr << "[EncoderAAC] ERROR: Failed to allocate audio FIFO.\n";
        return false;
    }

    input_frame = av_frame_alloc();
    output_frame = av_frame_alloc();
    encode_frame = av_frame_alloc();
    if (!input_frame || !output_frame || !encode_frame) {
        std::cerr << "[EncoderAAC] ERROR: Failed to allocate audio frames.\n";
        return false;
    }

    if (!ensureEncodeFrame()) {
        return false;
    }

    next_pts = 0;
    next_pts_initialized = false;
    expected_input_next_pts = 0;
    input_pts_initialized = false;
    return true;
}

bool EncoderAAC::setupResampler(AVSampleFormat in_fmt, int in_rate, int in_channels)
{
    if (in_fmt == last_input_fmt &&
        in_rate == last_input_rate &&
        in_channels == last_input_channels &&
        swr_ctx) {
        return true;
    }

    if (swr_ctx) {
        swr_free(&swr_ctx);
        swr_ctx = nullptr;
    }

    AVChannelLayout in_layout = defaultLayoutForChannels(in_channels);
    AVChannelLayout out_layout{};
    if (codec_ctx->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&out_layout, &codec_ctx->ch_layout);
    } else {
        av_channel_layout_default(&out_layout, channels);
    }

    SwrContext* swr = nullptr;
    const int ret = swr_alloc_set_opts2(
        &swr,
        &out_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
        &in_layout,  in_fmt,                in_rate,
        0, nullptr
    );

    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (ret < 0 || !swr) {
        std::cerr << "[EncoderAAC] ERROR: Failed to allocate resampler: "
                  << ffErrStrAac(ret) << "\n";
        return false;
    }

    const int init_ret = swr_init(swr);
    if (init_ret < 0) {
        std::cerr << "[EncoderAAC] ERROR: Failed to initialize resampler: "
                  << ffErrStrAac(init_ret) << "\n";
        swr_free(&swr);
        return false;
    }

    swr_ctx = swr;
    last_input_fmt = in_fmt;
    last_input_rate = in_rate;
    last_input_channels = in_channels;
    return true;
}

bool EncoderAAC::ensureResampleFrameCapacity(int nb_samples)
{
    if (!output_frame || !codec_ctx || nb_samples <= 0) {
        return false;
    }

    const bool needRealloc =
        !output_frame->buf[0] ||
        output_frame->format != codec_ctx->sample_fmt ||
        av_channel_layout_compare(&output_frame->ch_layout, &codec_ctx->ch_layout) != 0 ||
        output_frame->sample_rate != codec_ctx->sample_rate ||
        output_frame->nb_samples < nb_samples;

    if (needRealloc) {
        av_frame_unref(output_frame);
        output_frame->nb_samples = nb_samples;
        output_frame->format = codec_ctx->sample_fmt;
        output_frame->sample_rate = codec_ctx->sample_rate;

        av_channel_layout_uninit(&output_frame->ch_layout);
        av_channel_layout_copy(&output_frame->ch_layout, &codec_ctx->ch_layout);

        if (av_frame_get_buffer(output_frame, 0) < 0) {
            std::cerr << "[EncoderAAC] ERROR: Failed to allocate resample frame buffer.\n";
            return false;
        }
    } else {
        if (av_frame_make_writable(output_frame) < 0) {
            std::cerr << "[EncoderAAC] ERROR: Resample frame not writable.\n";
            return false;
        }
        output_frame->nb_samples = nb_samples;
    }

    output_frame->format = codec_ctx->sample_fmt;
    output_frame->sample_rate = codec_ctx->sample_rate;
    av_channel_layout_uninit(&output_frame->ch_layout);
    av_channel_layout_copy(&output_frame->ch_layout, &codec_ctx->ch_layout);
    return true;
}

bool EncoderAAC::ensureEncodeFrame()
{
    if (!encode_frame || !codec_ctx) {
        return false;
    }

    const int frame_size = (codec_ctx->frame_size > 0) ? codec_ctx->frame_size : 1024;
    const bool needRealloc =
        !encode_frame->buf[0] ||
        encode_frame->format != codec_ctx->sample_fmt ||
        av_channel_layout_compare(&encode_frame->ch_layout, &codec_ctx->ch_layout) != 0 ||
        encode_frame->sample_rate != codec_ctx->sample_rate ||
        encode_frame->nb_samples != frame_size;

    if (needRealloc) {
        av_frame_unref(encode_frame);
        encode_frame->nb_samples = frame_size;
        encode_frame->format = codec_ctx->sample_fmt;
        encode_frame->sample_rate = codec_ctx->sample_rate;

        av_channel_layout_uninit(&encode_frame->ch_layout);
        av_channel_layout_copy(&encode_frame->ch_layout, &codec_ctx->ch_layout);

        if (av_frame_get_buffer(encode_frame, 0) < 0) {
            std::cerr << "[EncoderAAC] ERROR: Failed to allocate encode frame buffer.\n";
            return false;
        }
    } else {
        if (av_frame_make_writable(encode_frame) < 0) {
            std::cerr << "[EncoderAAC] ERROR: Encode frame not writable.\n";
            return false;
        }
    }

    return true;
}

bool EncoderAAC::ensureFifoCapacity(int additional_samples)
{
    if (!fifo || additional_samples <= 0) {
        return true;
    }

    const int needed = av_audio_fifo_size(fifo) + additional_samples;
    if (needed <= av_audio_fifo_space(fifo)) {
        return true;
    }

    return av_audio_fifo_realloc(fifo, needed + std::max(codec_ctx->frame_size, 1024)) >= 0;
}

int64_t EncoderAAC::normalizeInputPtsToOutputClock(const AudioFrame& frame, int in_rate) const
{
    if (frame.pts == AV_NOPTS_VALUE) {
        return AV_NOPTS_VALUE;
    }

    AVRational src_tb = frame.time_base;
    if (src_tb.num <= 0 || src_tb.den <= 0) {
        src_tb = AVRational{1, in_rate > 0 ? in_rate : sample_rate};
    }

    return av_rescale_q(frame.pts, src_tb, AVRational{1, sample_rate});
}

bool EncoderAAC::submitAudioFrame(const AudioFrame& frame)
{
    if (!frame.buffer || frame.buffer_size == 0 || !codec_ctx) {
        return false;
    }

    const int inRate = frame.sample_rate > 0 ? frame.sample_rate : sample_rate;
    const int inChannels = frame.channels > 0 ? frame.channels : configured_input_channels;
    const int bytesPerSample = frame.bytes_per_sample > 0 ? frame.bytes_per_sample : 2;
    const int validBits = frame.valid_bits_per_sample > 0
        ? frame.valid_bits_per_sample
        : bytesPerSample * 8;

    if (inChannels <= 0 || bytesPerSample <= 0) {
        std::cerr << "[EncoderAAC] ERROR: invalid AudioFrame metadata channels="
                  << inChannels << " bytes_per_sample=" << bytesPerSample << "\n";
        return false;
    }

    int samples = frame.num_samples;
    if (samples <= 0) {
        const size_t denom = static_cast<size_t>(inChannels) *
                             static_cast<size_t>(bytesPerSample);
        if (denom == 0 || (frame.buffer_size % denom) != 0) {
            std::cerr << "[EncoderAAC] ERROR: AudioFrame buffer size does not match metadata size="
                      << frame.buffer_size << " channels=" << inChannels
                      << " bytes/sample=" << bytesPerSample << "\n";
            return false;
        }
        samples = static_cast<int>(frame.buffer_size / denom);
    }

    const int64_t ptsInOutputRate = normalizeInputPtsToOutputClock(frame, inRate);
    return submitPCMWithFormat(frame.buffer.get(),
                               static_cast<int>(frame.buffer_size),
                               ptsInOutputRate,
                               inRate,
                               inChannels,
                               bytesPerSample,
                               validBits,
                               samples);
}

// Normalize caller PCM into the configured AAC sample format. This function may
// resample, remap channels, and append to FIFO before a codec frame is available.
bool EncoderAAC::submitPCMWithFormat(const uint8_t* pcmData, int dataSizeBytes, int64_t pts_in_output_rate, int in_rate,
                                     int in_channels, int bytes_per_sample,
                                     int valid_bits_per_sample, int num_samples)
{
    static stage_timing::StageStats& submitPcmStat = stage_timing::get("aac_submit_pcm");
    stage_timing::ScopedTimer _timer(submitPcmStat);

    if (!pcmData || dataSizeBytes <= 0 || !codec_ctx) {
        return false;
    }

    if (in_rate <= 0) {
        in_rate = sample_rate;
    }

    if (in_channels <= 0 || bytes_per_sample <= 0) {
        std::cerr << "[EncoderAAC] ERROR: invalid explicit PCM format channels="
                  << in_channels << " bytes_per_sample=" << bytes_per_sample << "\n";
        return false;
    }

    if (num_samples <= 0) {
        const int denom = in_channels * bytes_per_sample;
        if (denom <= 0 || dataSizeBytes % denom != 0) {
            std::cerr << "[EncoderAAC] ERROR: PCM buffer size does not match explicit format size="
                      << dataSizeBytes << " channels=" << in_channels
                      << " bytes/sample=" << bytes_per_sample << "\n";
            return false;
        }
        num_samples = dataSizeBytes / denom;
    }

    const int64_t expectedBytes = static_cast<int64_t>(num_samples) *
                                  static_cast<int64_t>(in_channels) *
                                  static_cast<int64_t>(bytes_per_sample);
    if (expectedBytes <= 0 || expectedBytes > dataSizeBytes) {
        std::cerr << "[EncoderAAC] ERROR: explicit PCM format exceeds buffer size expected="
                  << expectedBytes << " actual=" << dataSizeBytes << "\n";
        return false;
    }

    // AAC is lossy PCM encoding, not protected bitstream carriage.
    // Normalize the selected embedded SDI channels to ordinary signed 16-bit
    // PCM before resampling/encoding.  This avoids feeding libfdk_aac with the
    // raw DeckLink s32/24-valid carrier used for S302M/Dolby-E passthrough.
    const AVSampleFormat resample_in_fmt = AV_SAMPLE_FMT_S16;

    const int in_nb_samples = (num_samples > 0)
        ? num_samples
        : dataSizeBytes / (in_channels * bytes_per_sample);

    if (!handleInputPtsDiscontinuity(pts_in_output_rate, in_nb_samples, sample_rate)) {
        return false;
    }

    int selected_in_channels = channels;
    selected_input_buf.resize(static_cast<size_t>(in_nb_samples) *
                              static_cast<size_t>(channels) *
                              sizeof(int16_t));

    int16_t* selected_s16 = reinterpret_cast<int16_t*>(selected_input_buf.data());
    for (int sample = 0; sample < in_nb_samples; ++sample) {
        for (int outCh = 0; outCh < channels; ++outCh) {
            const int inCh1 = channel_map_input_to_output.empty()
                ? (outCh + 1)
                : channel_map_input_to_output[outCh];
            if (inCh1 < 1 || inCh1 > in_channels) {
                std::cerr << "[EncoderAAC] ERROR: mapped input channel " << inCh1
                          << " is not present in incoming buffer with "
                          << in_channels << " channels.\n";
                return false;
            }

            const uint8_t* src = pcmData +
                static_cast<size_t>(sample * in_channels + (inCh1 - 1)) *
                static_cast<size_t>(bytes_per_sample);
            selected_s16[static_cast<size_t>(sample * channels + outCh)] =
                aacReadPcmAsS16(src, bytes_per_sample);
        }
    }

    av_frame_unref(input_frame);
    input_frame->nb_samples = in_nb_samples;
    input_frame->format = resample_in_fmt;
    input_frame->sample_rate = in_rate;
    av_channel_layout_uninit(&input_frame->ch_layout);
    av_channel_layout_default(&input_frame->ch_layout, selected_in_channels);

    const uint8_t* src_ptr = selected_input_buf.data();

    if (!setupResampler(resample_in_fmt, in_rate, selected_in_channels)) {
        return false;
    }

    if (av_samples_fill_arrays(input_frame->data,
                               input_frame->linesize,
                               src_ptr,
                               selected_in_channels,
                               in_nb_samples,
                               resample_in_fmt,
                               1) < 0) {
        std::cerr << "[EncoderAAC] ERROR: av_samples_fill_arrays failed.\n";
        return false;
    }

    const int max_out = swr_get_out_samples(swr_ctx, in_nb_samples);
    if (max_out < 0) {
        std::cerr << "[EncoderAAC] ERROR: swr_get_out_samples failed.\n";
        return false;
    }

    if (!ensureResampleFrameCapacity(max_out)) {
        return false;
    }

    {
        static stage_timing::StageStats& aacResampleStat = stage_timing::get("aac_resample");
        stage_timing::ScopedTimer _timer_swr(aacResampleStat);

        const int converted = swr_convert(
            swr_ctx,
            output_frame->data, max_out,
            const_cast<const uint8_t**>(input_frame->data), input_frame->nb_samples
        );
        if (converted < 0) {
            std::cerr << "[EncoderAAC] ERROR: swr_convert failed.\n";
            return false;
        }
        output_frame->nb_samples = converted;
    }

    if (!ensureFifoCapacity(output_frame->nb_samples)) {
        std::cerr << "[EncoderAAC] ERROR: FIFO realloc failed.\n";
        return false;
    }

    {
        static stage_timing::StageStats& aacFifoWriteStat = stage_timing::get("aac_fifo_write");
        stage_timing::ScopedTimer _timer_fifo(aacFifoWriteStat);

        const int written = av_audio_fifo_write(
            fifo,
            reinterpret_cast<void**>(output_frame->data),
            output_frame->nb_samples
        );
        if (written != output_frame->nb_samples) {
            std::cerr << "[EncoderAAC] ERROR: FIFO write failed.\n";
            return false;
        }
    }

    const int frame_size = (codec_ctx->frame_size > 0) ? codec_ctx->frame_size : 1024;
    while (av_audio_fifo_size(fifo) >= frame_size) {
        static stage_timing::StageStats& aacFifoDrainStat = stage_timing::get("aac_fifo_drain");
        stage_timing::ScopedTimer _timer_drain(aacFifoDrainStat);

        if (!sendOneFrameToEncoder(frame_size, false)) {
            return false;
        }
    }

    return true;
}

void EncoderAAC::resetBufferedAudioState(int64_t restartPts, const char* reason)
{
    if (fifo) {
        av_audio_fifo_reset(fifo);
    }
    if (swr_ctx) {
        swr_free(&swr_ctx);
        swr_ctx = nullptr;
        last_input_fmt = AV_SAMPLE_FMT_NONE;
        last_input_rate = 0;
        last_input_channels = 0;
    }
    freePendingPackets();

    next_pts = (restartPts == AV_NOPTS_VALUE) ? 0 : restartPts;
    next_pts_initialized = true;
    expected_input_next_pts = next_pts;
    input_pts_initialized = true;
    ++discontinuity_count;

    std::cerr << "[EncoderAAC] Audio PTS discontinuity: reset buffered AAC state"
              << " reason=" << (reason ? reason : "unknown")
              << " restart_pts=" << next_pts
              << " count=" << discontinuity_count << "\n";
}

bool EncoderAAC::handleInputPtsDiscontinuity(int64_t pts, int incomingSamples, int in_rate)
{
    if (incomingSamples <= 0) {
        return false;
    }

    if (in_rate <= 0) {
        in_rate = sample_rate;
    }

    const int64_t incomingSamplesOutRate = (in_rate == sample_rate)
        ? static_cast<int64_t>(incomingSamples)
        : av_rescale_q(incomingSamples, AVRational{1, in_rate}, AVRational{1, sample_rate});

    // AAC is frame-rate independent.  DeckLink audio may arrive in video-paced
    // chunks such as 1920 samples at 25 fps, 960 samples at 50 fps, or an
    // alternating cadence for 29.97/59.94 workflows.  AAC-LC still emits fixed
    // 1024-sample access units.  Therefore this function must not reset the
    // encoder simply because an input chunk PTS does not line up with the AAC
    // output cursor.
    const int64_t ptsInOutputRate = (pts == AV_NOPTS_VALUE)
        ? AV_NOPTS_VALUE
        : ((in_rate == sample_rate)
            ? pts
            : av_rescale_q(pts, AVRational{1, in_rate}, AVRational{1, sample_rate}));

    if (!next_pts_initialized) {
        // Anchor the encoded AAC timeline to the first SDI audio timestamp when
        // available.  If DeckLink/other inputs do not provide one, start from 0.
        next_pts = (ptsInOutputRate == AV_NOPTS_VALUE) ? 0 : ptsInOutputRate;
        next_pts_initialized = true;
    }

    if (!input_pts_initialized) {
        expected_input_next_pts = (ptsInOutputRate == AV_NOPTS_VALUE)
            ? (next_pts + incomingSamplesOutRate)
            : (ptsInOutputRate + incomingSamplesOutRate);
        input_pts_initialized = true;
        return true;
    }

    if (ptsInOutputRate == AV_NOPTS_VALUE) {
        expected_input_next_pts += incomingSamplesOutRate;
        return true;
    }

    const int64_t delta = ptsInOutputRate - expected_input_next_pts;

    // Only treat very large source timeline jumps as informational for now.
    // Do not reset the AAC encoder here: repeated resets cause libfdk_aac to
    // report "Queue input is backward in time" and produce non-monotonic TS
    // packets.  A dedicated signal-loss/session-reset path can be added later.
    const int64_t warnThreshold = std::max<int64_t>(sample_rate * 2,
                                                   incomingSamplesOutRate * 8);
    if (std::llabs(delta) > warnThreshold) {
        if ((discontinuity_count++ % 25) == 0) {
            std::cerr << "[EncoderAAC] WARNING: large incoming PCM PTS jump observed"
                      << " pts=" << ptsInOutputRate
                      << " expected=" << expected_input_next_pts
                      << " delta=" << delta
                      << " threshold=" << warnThreshold
                      << " samples; keeping monotonic AAC output timeline\n";
        }
    }

    // Follow the source PTS for the next expected input chunk, but never move
    // the AAC output cursor backwards.
    expected_input_next_pts = ptsInOutputRate + incomingSamplesOutRate;
    return true;
}

bool EncoderAAC::sendOneFrameToEncoder(int samples_to_read, bool pad_with_silence)
{
    static stage_timing::StageStats& sendOneFrameStat = stage_timing::get("aac_send_one_frame");
    stage_timing::ScopedTimer _timer(sendOneFrameStat);

    if (!codec_ctx || !fifo || !encode_frame) {
        return false;
    }

    const int frame_size = (codec_ctx->frame_size > 0) ? codec_ctx->frame_size : 1024;
    if (samples_to_read <= 0 || samples_to_read > frame_size) {
        return false;
    }
    if (av_audio_fifo_size(fifo) < samples_to_read) {
        return false;
    }

    if (!ensureEncodeFrame()) {
        return false;
    }

    if (av_audio_fifo_read(fifo,
                           reinterpret_cast<void**>(encode_frame->data),
                           samples_to_read) != samples_to_read) {
        std::cerr << "[EncoderAAC] ERROR: FIFO read failed.\n";
        return false;
    }

    if (pad_with_silence && samples_to_read < frame_size) {
        const int silence_samples = frame_size - samples_to_read;
        if (silence_samples > 0) {
            av_samples_set_silence(encode_frame->data,
                                   samples_to_read,
                                   silence_samples,
                                   codec_ctx->ch_layout.nb_channels,
                                   codec_ctx->sample_fmt);
        }
    }

    encode_frame->pts = next_pts;
    next_pts += frame_size;

    {
        static stage_timing::StageStats& aacSendFrameStat = stage_timing::get("aac_send_frame");
        stage_timing::ScopedTimer _timer_send(aacSendFrameStat);

        const int ret = avcodec_send_frame(codec_ctx, encode_frame);
        if (ret < 0) {
            std::cerr << "[EncoderAAC] ERROR: avcodec_send_frame failed: "
                      << ffErrStrAac(ret) << "\n";
            return false;
        }
    }

    drainEncoderPackets();
    return true;
}

void EncoderAAC::drainEncoderPackets()
{
    static stage_timing::StageStats& receivePacketsStat = stage_timing::get("aac_receive_packets");
    stage_timing::ScopedTimer _timer(receivePacketsStat);

    while (true) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            return;
        }

        const int ret = avcodec_receive_packet(codec_ctx, pkt);
        if (ret == 0) {
            if (pkt->duration <= 0) {
                pkt->duration = (codec_ctx->frame_size > 0) ? codec_ctx->frame_size : 1024;
            }
            pending_packets.push_back(pkt);
            continue;
        }

        av_packet_free(&pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }

        std::cerr << "[EncoderAAC] ERROR: avcodec_receive_packet failed: "
                  << ffErrStrAac(ret) << "\n";
        break;
    }
}

AVPacket* EncoderAAC::popPendingPacket()
{
    if (pending_packets.empty()) {
        return nullptr;
    }

    AVPacket* pkt = pending_packets.front();
    pending_packets.pop_front();
    return pkt;
}

AVPacket* EncoderAAC::receivePacket()
{
    static stage_timing::StageStats& receivePacketStat = stage_timing::get("aac_receive_packet");
    stage_timing::ScopedTimer _timer(receivePacketStat);
    return popPendingPacket();
}

// Public audio submit entry point. Returns one packet when available; additional
// packets, if produced, remain queued for receivePacket()/drain handling.
AVPacket* EncoderAAC::encodeAudioFrame(const AudioFrame& frame)
{
    static stage_timing::StageStats& encodeAudioFrameStat = stage_timing::get("aac_encode_audio_frame");
    stage_timing::ScopedTimer _timer(encodeAudioFrameStat);

    if (!frame.buffer || frame.buffer_size == 0) {
        return receivePacket();
    }

    if (!submitAudioFrame(frame)) {
        return nullptr;
    }

    return receivePacket();
}

std::vector<AVPacket*> EncoderAAC::flush()
{
    static stage_timing::StageStats& aacFlushStat = stage_timing::get("aac_flush");
    stage_timing::ScopedTimer _timer(aacFlushStat);

    std::vector<AVPacket*> out;

    while (AVPacket* pkt = popPendingPacket()) {
        out.push_back(pkt);
    }

    if (!codec_ctx) {
        return out;
    }

    if (swr_ctx) {
        const int delayed = swr_get_out_samples(swr_ctx, 0);
        if (delayed > 0 && ensureResampleFrameCapacity(delayed) && ensureFifoCapacity(delayed)) {
            static stage_timing::StageStats& aacFlushResampleStat = stage_timing::get("aac_flush_resample");
            stage_timing::ScopedTimer _timer_flush_swr(aacFlushResampleStat);

            const int got = swr_convert(swr_ctx, output_frame->data, delayed, nullptr, 0);
            if (got > 0) {
                output_frame->nb_samples = got;
                av_audio_fifo_write(fifo, reinterpret_cast<void**>(output_frame->data), got);
            }
        }
    }

    const int frame_size = (codec_ctx->frame_size > 0) ? codec_ctx->frame_size : 1024;
    while (av_audio_fifo_size(fifo) >= frame_size) {
        if (!sendOneFrameToEncoder(frame_size, false)) {
            break;
        }
        while (AVPacket* pkt = popPendingPacket()) {
            out.push_back(pkt);
        }
    }

    const int leftover = av_audio_fifo_size(fifo);
    if (leftover > 0) {
        if (sendOneFrameToEncoder(leftover, true)) {
            while (AVPacket* pkt = popPendingPacket()) {
                out.push_back(pkt);
            }
        }
    }

    avcodec_send_frame(codec_ctx, nullptr);
    drainEncoderPackets();

    while (AVPacket* pkt = popPendingPacket()) {
        out.push_back(pkt);
    }

    return out;
}

AVCodecContext* EncoderAAC::getCodecContext() const
{
    return codec_ctx;
}