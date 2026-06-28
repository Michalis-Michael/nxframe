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
 * DeckLink SDI output implementation. This module receives synchronized receiver frames, converts NxFrame internal video/audio buffers into DeckLink-compatible output buffers, schedules video frames, and writes embedded SDI audio with controlled playout cadence.
 */

#include "output/decklink_output.h"
#include "playout/av_sync_controller.h"
#include "playout/receiver_clock_policy.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <cstdlib>

#include <libavutil/imgutils.h>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace {


// Normalize both right-aligned and left-aligned 10-bit samples before
// packing to DeckLink v210. Receiver-side producers should normally deliver
// right-aligned YUV422P10LE, but this guard keeps playout robust.
static inline uint32_t normalize10Sample(uint16_t v)
{
    // AV_PIX_FMT_YUV422P10LE should normally be stored right-aligned as
    // 0..1023 in a 16-bit container. Some producer/conversion paths may
    // deliver left-aligned 10-bit samples, usually 0..65472. Normalize both
    // forms before packing to DeckLink v210.
    uint32_t sample = static_cast<uint32_t>(v);
    if (sample > 1023) {
        sample = (sample + 32) >> 6;
    }
    return static_cast<uint32_t>(std::max<uint32_t>(0, std::min<uint32_t>(1023, sample)));
}

static int v210RowBytes(int width)
{
    return ((width + 47) / 48) * 128;
}

// Small COM-compatible buffer wrapper used with CreateVideoFrameWithBuffer().
// The DeckLink frame owns the wrapper through AddRef/Release while NxFrame
// writes converted v210 data directly into the backing vector.
class OwnedDeckLinkVideoBuffer : public IDeckLinkVideoBuffer {
public:
    explicit OwnedDeckLinkVideoBuffer(size_t size)
        : data_(size), refs_(1)
    {
    }

    HRESULT QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (!ppv) {
            return E_INVALIDARG;
        }
        *ppv = nullptr;

        static const CFUUIDBytes kIID_IUnknown = IID_IUnknown;
        static const CFUUIDBytes kIID_VideoBuffer = IID_IDeckLinkVideoBuffer;

        if (std::memcmp(&iid, &kIID_IUnknown, sizeof(REFIID)) == 0 ||
            std::memcmp(&iid, &kIID_VideoBuffer, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG AddRef() override { return ++refs_; }

    ULONG Release() override
    {
        const ULONG v = --refs_;
        if (v == 0) {
            delete this;
        }
        return v;
    }

    HRESULT GetBytes(void** buffer) override
    {
        if (!buffer) {
            return E_INVALIDARG;
        }
        *buffer = data_.empty() ? nullptr : data_.data();
        return *buffer ? S_OK : E_FAIL;
    }

    HRESULT StartAccess(BMDBufferAccessFlags) override { return S_OK; }
    HRESULT EndAccess(BMDBufferAccessFlags) override { return S_OK; }

private:
    std::vector<uint8_t> data_;
    std::atomic<ULONG> refs_;
};

static int64_t ptsToUs(int64_t pts, AVRational tb)
{
    if (pts == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0) {
        return std::numeric_limits<int64_t>::min();
    }
    return av_rescale_q(pts, tb, AVRational{1, 1000000});
}

static AVRational decklinkPlaybackFrameRate(const VideoFrame& f)
{
    AVRational fps = f.nominal_frame_rate;
    if ((fps.num <= 0 || fps.den <= 0) && f.pts_time_base.num > 0 && f.pts_time_base.den > 0) {
        fps = AVRational{f.pts_time_base.den, f.pts_time_base.num};
    }
    if ((fps.num <= 0 || fps.den <= 0) && f.time_base.num > 0 && f.time_base.den > 0) {
        fps = AVRational{f.time_base.den, f.time_base.num};
    }
    if (fps.num <= 0 || fps.den <= 0) {
        return AVRational{0, 1};
    }

    // DeckLink scheduled output schedules complete video frames. Some demuxers
    // report interlaced streams by field rate; normalize those to frame rate.
    if (f.interlaced) {
        if (av_cmp_q(fps, AVRational{50, 1}) == 0) return AVRational{25, 1};
        if (av_cmp_q(fps, AVRational{60000, 1001}) == 0) return AVRational{30000, 1001};
        if (av_cmp_q(fps, AVRational{60, 1}) == 0) return AVRational{30, 1};
    }
    return fps;
}

static std::string getDeckLinkName(IDeckLink* dev)
{
    if (!dev) {
        return "unknown";
    }

    const char* name = nullptr;
    if (dev->GetDisplayName(&name) != S_OK || !name) {
        return "unknown";
    }

    std::string out(name);
    free(const_cast<char*>(name));
    return out;
}

static VideoFrame cloneVideo(const VideoFrame& src)
{
    VideoFrame out = src;
    if (!src.data[0] || !src.data[1] || !src.data[2] || src.width <= 0 || src.height <= 0) {
        out.buffer.reset();
        out.buffer_size = 0;
        out.data[0] = out.data[1] = out.data[2] = nullptr;
        return out;
    }

    const int size = av_image_get_buffer_size(src.pix_fmt, src.width, src.height, 1);
    if (size <= 0) {
        out.buffer.reset();
        out.buffer_size = 0;
        out.data[0] = out.data[1] = out.data[2] = nullptr;
        return out;
    }

    out.buffer = make_shared_u8(static_cast<size_t>(size));
    out.buffer_size = static_cast<size_t>(size);

    uint8_t* dstData[4] = {nullptr, nullptr, nullptr, nullptr};
    int dstLinesize[4] = {0, 0, 0, 0};
    if (av_image_fill_arrays(dstData, dstLinesize, out.buffer.get(),
                             src.pix_fmt, src.width, src.height, 1) < 0) {
        out.buffer.reset();
        out.buffer_size = 0;
        out.data[0] = out.data[1] = out.data[2] = nullptr;
        return out;
    }

    av_image_copy(dstData, dstLinesize,
                  const_cast<const uint8_t**>(src.data), src.linesize,
                  src.pix_fmt, src.width, src.height);

    for (int i = 0; i < 3; ++i) {
        out.data[i] = dstData[i];
        out.linesize[i] = dstLinesize[i];
    }

    return out;
}

static AudioFrame cloneAudio(const AudioFrame& src)
{
    AudioFrame out = src;
    if (!src.buffer || src.buffer_size == 0) {
        out.buffer.reset();
        out.buffer_size = 0;
        return out;
    }

    out.buffer = make_shared_u8(src.buffer_size);
    out.buffer_size = src.buffer_size;
    std::memcpy(out.buffer.get(), src.buffer.get(), src.buffer_size);
    return out;
}

static VideoFrame makeBlackFrame(const VideoFrame& ref)
{
    VideoFrame b = ref;
    if (ref.width <= 0 || ref.height <= 0 || ref.pix_fmt != AV_PIX_FMT_YUV422P10LE) {
        b.buffer.reset();
        b.buffer_size = 0;
        b.data[0] = b.data[1] = b.data[2] = nullptr;
        return b;
    }

    const int size = av_image_get_buffer_size(ref.pix_fmt, ref.width, ref.height, 1);
    if (size <= 0) {
        b.buffer.reset();
        b.buffer_size = 0;
        b.data[0] = b.data[1] = b.data[2] = nullptr;
        return b;
    }

    b.buffer = make_shared_u8(static_cast<size_t>(size));
    b.buffer_size = static_cast<size_t>(size);

    uint8_t* dstData[4] = {nullptr, nullptr, nullptr, nullptr};
    int dstLinesize[4] = {0, 0, 0, 0};
    if (av_image_fill_arrays(dstData, dstLinesize, b.buffer.get(),
                             ref.pix_fmt, ref.width, ref.height, 1) < 0) {
        b.buffer.reset();
        b.buffer_size = 0;
        b.data[0] = b.data[1] = b.data[2] = nullptr;
        return b;
    }

    for (int i = 0; i < 3; ++i) {
        b.data[i] = dstData[i];
        b.linesize[i] = dstLinesize[i];
    }

    // YUV limited-range black: Y=64 (16 in 8-bit maps to 256 in 10-bit), Cb/Cr=512
    for (int y = 0; y < b.height; ++y) {
        uint16_t* yRow = reinterpret_cast<uint16_t*>(b.data[0] + y * b.linesize[0]);
        uint16_t* uRow = reinterpret_cast<uint16_t*>(b.data[1] + y * b.linesize[1]);
        uint16_t* vRow = reinterpret_cast<uint16_t*>(b.data[2] + y * b.linesize[2]);
        for (int x = 0; x < b.width; ++x) {
            yRow[x] = 256;
        }
        for (int x = 0; x < b.width / 2; ++x) {
            uRow[x] = 512;
            vRow[x] = 512;
        }
    }
    return b;
}

static AudioFrame makeSilence(int sampleRate, int channels, int bytesPerSample, int numSamples)
{
    AudioFrame s;
    s.sample_rate = sampleRate;
    s.channels = channels;
    s.bytes_per_sample = bytesPerSample;
    s.num_samples = numSamples;
    s.time_base = AVRational{1, sampleRate > 0 ? sampleRate : 48000};

    const size_t totalBytes =
        static_cast<size_t>(std::max(numSamples, 0)) *
        static_cast<size_t>(std::max(channels, 0)) *
        static_cast<size_t>(std::max(bytesPerSample, 0));

    s.buffer = make_shared_u8(totalBytes > 0 ? totalBytes : 1);
    s.buffer_size = totalBytes;
    if (totalBytes > 0) {
        std::memset(s.buffer.get(), 0, totalBytes);
    }
    return s;
}


static bool trimAudioFrameFrontToPts(AudioFrame& frame, int64_t targetPtsUs, int64_t& trimmedSamples)
{
    trimmedSamples = 0;
    if (!frame.buffer || frame.buffer_size == 0 || frame.num_samples <= 0 ||
        frame.sample_rate <= 0 || frame.channels <= 0 || frame.bytes_per_sample <= 0 ||
        frame.time_base.num <= 0 || frame.time_base.den <= 0 || frame.pts == AV_NOPTS_VALUE) {
        return false;
    }
    const int64_t framePtsUs = ptsToUs(frame.pts, frame.time_base);
    if (framePtsUs == std::numeric_limits<int64_t>::min()) {
        return false;
    }
    if (targetPtsUs <= framePtsUs) {
        return true;
    }
    const int64_t deltaUs = targetPtsUs - framePtsUs;
    int64_t samplesToTrim = av_rescale_q(deltaUs, AVRational{1, 1000000}, AVRational{1, frame.sample_rate});
    if (samplesToTrim <= 0) {
        return true;
    }
    if (samplesToTrim >= frame.num_samples) {
        return false;
    }
    const size_t bytesPerSampleFrame = static_cast<size_t>(frame.channels) * static_cast<size_t>(frame.bytes_per_sample);
    const size_t bytesToTrim = static_cast<size_t>(samplesToTrim) * bytesPerSampleFrame;
    const size_t remainingBytes = frame.buffer_size - bytesToTrim;
    std::shared_ptr<uint8_t> trimmed = make_shared_u8(remainingBytes > 0 ? remainingBytes : 1);
    if (remainingBytes > 0) {
        std::memcpy(trimmed.get(), frame.buffer.get() + bytesToTrim, remainingBytes);
    }
    frame.buffer = trimmed;
    frame.buffer_size = remainingBytes;
    frame.num_samples -= static_cast<int>(samplesToTrim);

    // Advance pts in sample-rate units to avoid time-base rounding
    // error when converting back to microseconds downstream. Previously pts was
    // advanced in its native time_base which could accumulate rounding drift when
    // the time_base doesn't divide the sample_rate evenly.
    const int64_t samplesInNativeTb = av_rescale_q(
        samplesToTrim,
        AVRational{1, frame.sample_rate},
        frame.time_base);
    frame.pts += samplesInNativeTb;

    // Verify the trimmed frame's pts converts back within 1-sample tolerance of
    // targetPtsUs. If not, the time bases are incompatible and the caller should
    // drop this frame rather than anchoring on a misaligned position.
    const int64_t trimmedPtsUs = ptsToUs(frame.pts, frame.time_base);
    if (trimmedPtsUs != std::numeric_limits<int64_t>::min()) {
        const int64_t oneSampleUs = av_rescale_q(1, AVRational{1, frame.sample_rate}, AVRational{1, 1000000});
        const int64_t alignError = std::llabs(trimmedPtsUs - targetPtsUs);
        if (alignError > oneSampleUs + 1) {
            // Time bases are incompatible; signal the caller to discard this frame.
            return false;
        }
    }

    trimmedSamples = samplesToTrim;
    return true;
}

static int64_t audioFrameEndPtsUs(const AudioFrame& frame)
{
    const int64_t ptsUs = ptsToUs(frame.pts, frame.time_base);
    if (ptsUs == std::numeric_limits<int64_t>::min() || frame.sample_rate <= 0 || frame.num_samples <= 0) {
        return std::numeric_limits<int64_t>::min();
    }
    return ptsUs + av_rescale_q(frame.num_samples, AVRational{1, frame.sample_rate}, AVRational{1, 1000000});
}


static int64_t videoFramePtsUs(const VideoFrame& frame)
{
    const AVRational tb = (frame.pts_time_base.num > 0 && frame.pts_time_base.den > 0)
        ? frame.pts_time_base
        : frame.time_base;
    return AvSyncController::ptsToUs(frame.pts, tb);
}

static int64_t audioFramePtsUs(const AudioFrame& frame)
{
    return AvSyncController::ptsToUs(frame.pts, frame.time_base);
}

static BMDTimeValue usToDeckLinkTicks(int64_t us, BMDTimeScale timeScale)
{
    if (timeScale <= 0) {
        return 0;
    }
    return av_rescale_q(us,
                        AVRational{1, 1000000},
                        AVRational{1, static_cast<int>(timeScale)});
}

static int64_t deckLinkTicksToAudioSamples(BMDTimeValue ticks,
                                           BMDTimeScale timeScale,
                                           int sampleRate)
{
    if (timeScale <= 0 || sampleRate <= 0) {
        return 0;
    }
    return av_rescale_q(ticks,
                        AVRational{1, static_cast<int>(timeScale)},
                        AVRational{1, sampleRate});
}

static bool refreshVideoFallbackTemplate(const VideoFrame& frame,
                                         VideoFrame& videoTemplate,
                                         bool& haveVideoTemplate)
{
    const bool needTemplate =
        !haveVideoTemplate ||
        !videoTemplate.buffer ||
        videoTemplate.width != frame.width ||
        videoTemplate.height != frame.height ||
        videoTemplate.pix_fmt != frame.pix_fmt ||
        videoTemplate.interlaced != frame.interlaced ||
        videoTemplate.tff != frame.tff;

    if (!needTemplate) {
        return haveVideoTemplate;
    }

    videoTemplate = cloneVideo(frame);
    haveVideoTemplate = static_cast<bool>(videoTemplate.buffer);
    return haveVideoTemplate;
}

static bool refreshSilentAudioTemplate(const AudioFrame& frame,
                                       AudioFrame& audioTemplate,
                                       bool& haveAudioTemplate)
{
    const bool needTemplate =
        !haveAudioTemplate ||
        audioTemplate.sample_rate != frame.sample_rate ||
        audioTemplate.channels != frame.channels ||
        audioTemplate.bytes_per_sample != frame.bytes_per_sample ||
        audioTemplate.num_samples != frame.num_samples;

    if (!needTemplate) {
        return haveAudioTemplate;
    }

    audioTemplate = makeSilence(frame.sample_rate,
                                frame.channels,
                                frame.bytes_per_sample,
                                std::max(frame.num_samples, 1));
    haveAudioTemplate = static_cast<bool>(audioTemplate.buffer);
    return haveAudioTemplate;
}

static bool elapsedMsAtLeast(std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point then,
                             int thresholdMs,
                             int minimumMs)
{
    return (now - then) >= std::chrono::milliseconds(std::max(minimumMs, thresholdMs));
}

static void logPlayoutConfig(const DeckLinkPlayoutConfig& config,
                             uint32_t videoPrerollFrames,
                             uint32_t maxVideoQueueFrames,
                             uint32_t maxAudioQueueSamples)
{
    std::cout << "[PLAY-DECKLINK] Playout config: video_preroll_frames=" << videoPrerollFrames
              << " max_video_queue_frames=" << maxVideoQueueFrames
              << " max_audio_queue_samples=" << maxAudioQueueSamples
              << " startup_anchor_timeout_ms=" << config.startupAnchorTimeoutMs
              << " source_loss_ms=" << config.sourceLossThresholdMs
              << " clock_policy=" << nxframe::receiver_clock_policy::modeName()
              << " media_clock=" << nxframe::receiver_clock_policy::mediaClockName()
              << " output_clock=" << nxframe::receiver_clock_policy::outputClockName()
              << " transport_clock=" << nxframe::receiver_clock_policy::transportClockName()
              << "\n";
}



struct DeckLinkFrameMetadataValues {
    bool has_colorspace = false;
    BMDColorspace colorspace = bmdColorspaceRec709;

    bool has_eotf = false;
    int64_t eotf = 0; // CTA-861: 0=traditional SDR, 2=PQ/ST2084, 3=HLG.

    bool has_mastering_display = false;
    double red_x = 0.0;
    double red_y = 0.0;
    double green_x = 0.0;
    double green_y = 0.0;
    double blue_x = 0.0;
    double blue_y = 0.0;
    double white_x = 0.0;
    double white_y = 0.0;
    double max_luminance = 0.0;
    double min_luminance = 0.0;

    bool has_content_light = false;
    double max_cll = 0.0;
    double max_fall = 0.0;

    SmpteTimecode timecode;

    bool hasHdrMetadata() const noexcept
    {
        return has_colorspace || has_eotf || has_mastering_display || has_content_light;
    }
};

static bool isHdrTransfer(AVColorTransferCharacteristic trc)
{
    return trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67;
}

static bool isBt2020Color(const VideoFrame& frame)
{
    return frame.color_primaries == AVCOL_PRI_BT2020 ||
           frame.colorspace == AVCOL_SPC_BT2020_NCL ||
           frame.colorspace == AVCOL_SPC_BT2020_CL;
}

static BMDColorspace deckLinkColorspaceForFrame(const VideoFrame& frame)
{
    if (isBt2020Color(frame)) {
        return bmdColorspaceRec2020;
    }
    if (frame.width > 0 && frame.height > 0 && frame.height < 720) {
        return bmdColorspaceRec601;
    }
    return bmdColorspaceRec709;
}

static BMDDynamicRange deckLinkDynamicRangeForFrame(const VideoFrame& frame)
{
    if (frame.color_trc == AVCOL_TRC_SMPTE2084) {
        return bmdDynamicRangeHDRStaticPQ;
    }
    if (frame.color_trc == AVCOL_TRC_ARIB_STD_B67) {
        return bmdDynamicRangeHDRStaticHLG;
    }
    return bmdDynamicRangeSDR;
}

static const char* deckLinkColorspaceLabel(BMDColorspace cs)
{
    switch (cs) {
        case bmdColorspaceRec2020: return "Rec.2020";
        case bmdColorspaceRec601:  return "Rec.601";
        case bmdColorspaceRec709:
        default:                   return "Rec.709";
    }
}

static const char* deckLinkDynamicRangeLabel(BMDDynamicRange dr)
{
    switch (dr) {
        case bmdDynamicRangeHDRStaticPQ:  return "PQ/ST2084";
        case bmdDynamicRangeHDRStaticHLG: return "HLG";
        case bmdDynamicRangeSDR:
        default:                         return "SDR";
    }
}

static DeckLinkFrameMetadataValues buildDeckLinkFrameMetadata(const VideoFrame& frame)
{
    DeckLinkFrameMetadataValues md;
    const bool wcg = isBt2020Color(frame);
    const bool hdr = isHdrTransfer(frame.color_trc) ||
                     frame.has_mastering_display ||
                     frame.has_content_light;

    // Do not mark a timecode-only wrapper as HDR metadata. Preserve the
    // existing WCG/HDR behavior only when the frame actually needs color/HDR
    // DeckLink metadata.
    if (wcg || hdr) {
        md.has_colorspace = true;
        md.colorspace = deckLinkColorspaceForFrame(frame);
    }

    if (frame.color_trc == AVCOL_TRC_SMPTE2084) {
        md.has_eotf = true;
        md.eotf = 2;
    } else if (frame.color_trc == AVCOL_TRC_ARIB_STD_B67) {
        md.has_eotf = true;
        md.eotf = 3;
    } else if (wcg || hdr) {
        md.has_eotf = true;
        md.eotf = 0;
    }

    if (frame.has_mastering_display && frame.mastering_display.has_primaries &&
        frame.mastering_display.has_luminance) {
        md.has_mastering_display = true;
        md.red_x = av_q2d(frame.mastering_display.display_primaries[0][0]);
        md.red_y = av_q2d(frame.mastering_display.display_primaries[0][1]);
        md.green_x = av_q2d(frame.mastering_display.display_primaries[1][0]);
        md.green_y = av_q2d(frame.mastering_display.display_primaries[1][1]);
        md.blue_x = av_q2d(frame.mastering_display.display_primaries[2][0]);
        md.blue_y = av_q2d(frame.mastering_display.display_primaries[2][1]);
        md.white_x = av_q2d(frame.mastering_display.white_point[0]);
        md.white_y = av_q2d(frame.mastering_display.white_point[1]);
        md.max_luminance = av_q2d(frame.mastering_display.max_luminance);
        md.min_luminance = av_q2d(frame.mastering_display.min_luminance);
    }

    if (frame.has_content_light) {
        md.has_content_light = true;
        md.max_cll = static_cast<double>(frame.content_light.MaxCLL);
        md.max_fall = static_cast<double>(frame.content_light.MaxFALL);
    }

    md.timecode = frame.metadata.timecode;
    return md;
}

static bool needsDeckLinkFrameMetadata(const VideoFrame& frame)
{
    return isBt2020Color(frame) ||
           isHdrTransfer(frame.color_trc) ||
           frame.has_mastering_display ||
           frame.has_content_light ||
           frame.metadata.hasTimecode();
}


class SimpleDeckLinkTimecode : public IDeckLinkTimecode {
public:
    explicit SimpleDeckLinkTimecode(const SmpteTimecode& tc)
        : tc_(tc), refs_(1)
    {
    }

    HRESULT QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        static const CFUUIDBytes kIID_IUnknown = IID_IUnknown;
        static const CFUUIDBytes kIID_Timecode = IID_IDeckLinkTimecode;
        if (std::memcmp(&iid, &kIID_IUnknown, sizeof(REFIID)) == 0 ||
            std::memcmp(&iid, &kIID_Timecode, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkTimecode*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG AddRef() override { return ++refs_; }

    ULONG Release() override
    {
        const ULONG v = --refs_;
        if (v == 0) delete this;
        return v;
    }

    BMDTimecodeBCD GetBCD(void) override
    {
        if (!tc_.valid) return 0;
        return ((static_cast<BMDTimecodeBCD>(tc_.hours / 10) << 28) |
                (static_cast<BMDTimecodeBCD>(tc_.hours % 10) << 24) |
                (static_cast<BMDTimecodeBCD>(tc_.minutes / 10) << 20) |
                (static_cast<BMDTimecodeBCD>(tc_.minutes % 10) << 16) |
                (static_cast<BMDTimecodeBCD>(tc_.seconds / 10) << 12) |
                (static_cast<BMDTimecodeBCD>(tc_.seconds % 10) << 8) |
                (static_cast<BMDTimecodeBCD>(tc_.frames / 10) << 4) |
                (static_cast<BMDTimecodeBCD>(tc_.frames % 10)));
    }

    HRESULT GetComponents(uint8_t* hours, uint8_t* minutes, uint8_t* seconds, uint8_t* frames) override
    {
        if (!hours || !minutes || !seconds || !frames || !tc_.valid) return E_FAIL;
        *hours = tc_.hours;
        *minutes = tc_.minutes;
        *seconds = tc_.seconds;
        *frames = tc_.frames;
        return S_OK;
    }

    HRESULT GetString(const char** timecode) override
    {
        if (!timecode || !tc_.valid) return E_FAIL;
        const std::string text = tc_.toString();
        char* copy = static_cast<char*>(std::malloc(text.size() + 1));
        if (!copy) return E_OUTOFMEMORY;
        std::memcpy(copy, text.c_str(), text.size() + 1);
        *timecode = copy;
        return S_OK;
    }

    BMDTimecodeFlags GetFlags(void) override
    {
        return static_cast<BMDTimecodeFlags>(tc_.flags);
    }

    HRESULT GetTimecodeUserBits(BMDTimecodeUserBits* userBits) override
    {
        if (!userBits || !tc_.has_user_bits) return E_FAIL;
        *userBits = static_cast<BMDTimecodeUserBits>(tc_.user_bits);
        return S_OK;
    }

private:
    SmpteTimecode tc_;
    std::atomic<ULONG> refs_;
};

class MetadataVideoFrame : public IDeckLinkVideoFrame, public IDeckLinkVideoFrameMetadataExtensions, public IDeckLinkVideoBuffer {
public:
    MetadataVideoFrame(IDeckLinkMutableVideoFrame* wrapped,
                       IDeckLinkVideoBuffer* buffer,
                       const DeckLinkFrameMetadataValues& metadata)
        : wrapped_(wrapped), buffer_(buffer), metadata_(metadata), refs_(1)
    {
        // Takes ownership of the caller's scheduled-playback reference.
        // Keep the SDK 15 video buffer alive while this metadata wrapper is scheduled.
        if (buffer_) {
            buffer_->AddRef();
        }
    }

    ~MetadataVideoFrame() override
    {
        if (buffer_) {
            buffer_->Release();
            buffer_ = nullptr;
        }
        if (wrapped_) {
            wrapped_->Release();
            wrapped_ = nullptr;
        }
    }

    IDeckLinkMutableVideoFrame* wrappedFrame() const { return wrapped_; }

    HRESULT QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (!ppv) {
            return E_INVALIDARG;
        }
        *ppv = nullptr;

        static const CFUUIDBytes kIID_IUnknown = IID_IUnknown;
        static const CFUUIDBytes kIID_VideoFrame = IID_IDeckLinkVideoFrame;
        static const CFUUIDBytes kIID_Metadata = IID_IDeckLinkVideoFrameMetadataExtensions;
        static const CFUUIDBytes kIID_VideoBuffer = IID_IDeckLinkVideoBuffer;

        if (std::memcmp(&iid, &kIID_IUnknown, sizeof(REFIID)) == 0 ||
            std::memcmp(&iid, &kIID_VideoFrame, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrame*>(this);
            AddRef();
            return S_OK;
        }
        if (std::memcmp(&iid, &kIID_Metadata, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrameMetadataExtensions*>(this);
            AddRef();
            return S_OK;
        }
        if (std::memcmp(&iid, &kIID_VideoBuffer, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG AddRef() override { return ++refs_; }

    ULONG Release() override
    {
        const ULONG v = --refs_;
        if (v == 0) {
            delete this;
        }
        return v;
    }

    long GetWidth() override { return wrapped_ ? wrapped_->GetWidth() : 0; }
    long GetHeight() override { return wrapped_ ? wrapped_->GetHeight() : 0; }
    long GetRowBytes() override { return wrapped_ ? wrapped_->GetRowBytes() : 0; }
    BMDPixelFormat GetPixelFormat() override { return wrapped_ ? wrapped_->GetPixelFormat() : bmdFormat10BitYUV; }
    BMDFrameFlags GetFlags() override
    {
        const BMDFrameFlags base = wrapped_ ? wrapped_->GetFlags() : bmdFrameFlagDefault;
        if (metadata_.hasHdrMetadata()) {
            return static_cast<BMDFrameFlags>(base | bmdFrameContainsHDRMetadata);
        }
        return base;
    }
    HRESULT GetTimecode(BMDTimecodeFormat format, IDeckLinkTimecode** timecode) override
    {
        if (!timecode) return E_INVALIDARG;
        *timecode = nullptr;
        if (metadata_.timecode.valid &&
            (format == bmdTimecodeRP188Any ||
             format == bmdTimecodeRP188VITC1 ||
             format == bmdTimecodeRP188VITC2 ||
             format == bmdTimecodeRP188LTC ||
             format == bmdTimecodeRP188HighFrameRate ||
             format == bmdTimecodeVITC ||
             format == bmdTimecodeVITCField2 ||
             format == bmdTimecodeSerial)) {
            *timecode = new SimpleDeckLinkTimecode(metadata_.timecode);
            return *timecode ? S_OK : E_OUTOFMEMORY;
        }
        return wrapped_ ? wrapped_->GetTimecode(format, timecode) : E_FAIL;
    }
    HRESULT GetAncillaryData(IDeckLinkVideoFrameAncillary** ancillary) override
    {
        return wrapped_ ? wrapped_->GetAncillaryData(ancillary) : E_FAIL;
    }

    HRESULT GetBytes(void** buffer) override
    {
        return buffer_ ? buffer_->GetBytes(buffer) : E_FAIL;
    }

    HRESULT StartAccess(BMDBufferAccessFlags flags) override
    {
        return buffer_ ? buffer_->StartAccess(flags) : E_FAIL;
    }

    HRESULT EndAccess(BMDBufferAccessFlags flags) override
    {
        return buffer_ ? buffer_->EndAccess(flags) : E_FAIL;
    }

    HRESULT GetInt(BMDDeckLinkFrameMetadataID metadataID, int64_t* value) override
    {
        if (!value) return E_INVALIDARG;
        switch (metadataID) {
            case bmdDeckLinkFrameMetadataColorspace:
                if (!metadata_.has_colorspace) return E_FAIL;
                *value = static_cast<int64_t>(metadata_.colorspace);
                return S_OK;
            case bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc:
                if (!metadata_.has_eotf) return E_FAIL;
                *value = metadata_.eotf;
                return S_OK;
            default:
                return E_FAIL;
        }
    }

    HRESULT GetFloat(BMDDeckLinkFrameMetadataID metadataID, double* value) override
    {
        if (!value) return E_INVALIDARG;
        switch (metadataID) {
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.red_x; return S_OK;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.red_y; return S_OK;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.green_x; return S_OK;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.green_y; return S_OK;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.blue_x; return S_OK;
            case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.blue_y; return S_OK;
            case bmdDeckLinkFrameMetadataHDRWhitePointX:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.white_x; return S_OK;
            case bmdDeckLinkFrameMetadataHDRWhitePointY:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.white_y; return S_OK;
            case bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.max_luminance; return S_OK;
            case bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance:
                if (!metadata_.has_mastering_display) return E_FAIL;
                *value = metadata_.min_luminance; return S_OK;
            case bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel:
                if (!metadata_.has_content_light) return E_FAIL;
                *value = metadata_.max_cll; return S_OK;
            case bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel:
                if (!metadata_.has_content_light) return E_FAIL;
                *value = metadata_.max_fall; return S_OK;
            default:
                return E_FAIL;
        }
    }

    HRESULT GetFlag(BMDDeckLinkFrameMetadataID, bool*) override { return E_FAIL; }
    HRESULT GetString(BMDDeckLinkFrameMetadataID, const char**) override { return E_FAIL; }
    HRESULT GetBytes(BMDDeckLinkFrameMetadataID, void*, uint32_t*) override { return E_FAIL; }

private:
    IDeckLinkMutableVideoFrame* wrapped_ = nullptr;
    IDeckLinkVideoBuffer* buffer_ = nullptr;
    DeckLinkFrameMetadataValues metadata_;
    std::atomic<ULONG> refs_;
};

// DeckLink invokes this callback from its playback thread. Keep this path
// short: report frame completion back to DeckLinkOutput and avoid heavy work.
class OutputCallback : public IDeckLinkVideoOutputCallback {
public:
    explicit OutputCallback(DeckLinkOutput* owner)
        : owner_(owner), refs_(1)
    {
    }

    HRESULT QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (!ppv) {
            return E_INVALIDARG;
        }
        *ppv = nullptr;

        static const CFUUIDBytes kIID_IUnknown = IID_IUnknown;
        static const CFUUIDBytes kIID_Callback = IID_IDeckLinkVideoOutputCallback;

        if (std::memcmp(&iid, &kIID_IUnknown, sizeof(REFIID)) == 0 ||
            std::memcmp(&iid, &kIID_Callback, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG AddRef() override { return ++refs_; }

    ULONG Release() override
    {
        const ULONG v = --refs_;
        if (v == 0) {
            delete this;
        }
        return v;
    }

    HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame* frame,
                                    BMDOutputFrameCompletionResult result) override
    {
        if (owner_) {
            owner_->onScheduledFrameCallbackBegin();
            owner_->onScheduledFrameCompleted(frame);
            if (result != bmdOutputFrameCompleted) {
                owner_->onScheduledFrameCompletionWarning();
            }
        }
        // Release the scheduled-playback reference AddRef'd in obtainPooledFrame().
        // The pool continues to hold its own separate create-time reference.
        if (frame) {
            frame->Release();
        }
        if (owner_) {
            owner_->onScheduledFrameCallbackEnd();
        }
        return S_OK;
    }

    HRESULT ScheduledPlaybackHasStopped() override
    {
        if (owner_) {
            owner_->onScheduledPlaybackStopped();
        }
        return S_OK;
    }

private:
    DeckLinkOutput* owner_;
    std::atomic<ULONG> refs_;
};

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DeckLinkOutput::DeckLinkOutput() = default;

DeckLinkOutput::~DeckLinkOutput()
{
    stop();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void DeckLinkOutput::setError(const std::string& msg)
{
    last_error_ = msg;
    std::cerr << "[DeckLinkOutput] " << msg << "\n";
}

// ---------------------------------------------------------------------------
// init / stop
// ---------------------------------------------------------------------------

bool DeckLinkOutput::init(int deviceIndex)
{
    stop();

    IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
    if (!it) {
        setError("No DeckLink iterator available.");
        return false;
    }

    int idx = 0;
    IDeckLink* dev = nullptr;
    while (it->Next(&dev) == S_OK) {
        if (idx == deviceIndex) {
            decklink_ = dev;
            device_name_ = getDeckLinkName(dev);
            break;
        }
        dev->Release();
        ++idx;
    }
    it->Release();

    if (!decklink_) {
        setError("Requested DeckLink output device not found.");
        return false;
    }

    if (decklink_->QueryInterface(IID_IDeckLinkOutput,
                                  reinterpret_cast<void**>(&decklink_output_)) != S_OK) {
        setError("Failed to query IDeckLinkOutput.");
        decklink_->Release();
        decklink_ = nullptr;
        return false;
    }

    // Optional: not every DeckLink device/driver exposes configuration.
    // When available, this lets us request Rec.2020 SDI output for WCG/HDR.
    decklink_->QueryInterface(IID_IDeckLinkConfiguration,
                              reinterpret_cast<void**>(&decklink_config_));

    initialized_.store(true, std::memory_order_release);
    std::cout << "[DeckLinkOutput] Initialized device[" << deviceIndex
              << "] \"" << device_name_ << "\"\n";
    return true;
}

void DeckLinkOutput::stop()
{
    if (decklink_output_) {
        resetHardwarePipeline(false);

        if (audio_enabled_.load(std::memory_order_acquire)) {
            decklink_output_->DisableAudioOutput();
            audio_enabled_.store(false, std::memory_order_release);
        }
        if (running_.load(std::memory_order_acquire)) {
            decklink_output_->DisableVideoOutput();
            running_.store(false, std::memory_order_release);
        }

        // Disabling the output may cause the driver to return any remaining
        // scheduled frames. Drain those callbacks before removing the callback
        // object and before dropping the pool's create-time frame references.
        waitForScheduledCallbacksDrained(250);

        if (callback_) {
            decklink_output_->SetScheduledFrameCompletionCallback(nullptr);
            callback_->Release();
            callback_ = nullptr;
        }

        releaseAllPooledFrames();
    }

    if (decklink_config_) {
        // Return the output to Rec.709/SDR when stopping where the driver supports it.
        decklink_config_->SetFlag(bmdDeckLinkConfigRec2020Output, false);
        decklink_config_->Release();
        decklink_config_ = nullptr;
    }

    if (decklink_output_) {
        decklink_output_->Release();
        decklink_output_ = nullptr;
    }
    if (decklink_) {
        decklink_->Release();
        decklink_ = nullptr;
    }

    if (initialized_.load(std::memory_order_acquire) && !device_name_.empty()) {
        std::cout << "[DeckLinkOutput] Stopped \"" << device_name_ << "\"\n";
    }

    initialized_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    audio_enabled_.store(false, std::memory_order_release);
    device_name_.clear();
    last_error_.clear();

    current_mode_ = bmdModeUnknown;
    current_width_ = 0;
    current_height_ = 0;
    current_interlaced_ = false;
    current_output_colorspace_ = bmdColorspaceRec709;
    current_output_dynamic_range_ = bmdDynamicRangeSDR;
    current_audio_sample_rate_ = 0;
    current_audio_channels_ = 0;
    current_audio_bps_ = 0;
    video_time_scale_ = 0;
    video_frame_duration_ = 0;
    next_video_time_ = 0;
    next_audio_time_ = 0;
    audio_preroll_active_ = false;
    playback_started_ = false;

    scheduled_video_frames_.store(0, std::memory_order_relaxed);
    active_frame_callbacks_.store(0, std::memory_order_relaxed);
    buffered_video_frames_.store(0, std::memory_order_relaxed);
    buffered_audio_samples_.store(0, std::memory_order_relaxed);
    dropped_video_frames_.store(0, std::memory_order_relaxed);
    dropped_audio_frames_.store(0, std::memory_order_relaxed);
    schedule_failures_.store(0, std::memory_order_relaxed);
    completion_warnings_.store(0, std::memory_order_relaxed);
    playback_stop_notified_.store(false, std::memory_order_relaxed);

    reference_supported_ = true;
    reference_locked_ = false;
}

// ---------------------------------------------------------------------------
// SDK callbacks
// ---------------------------------------------------------------------------

void DeckLinkOutput::onScheduledFrameCallbackBegin()
{
    active_frame_callbacks_.fetch_add(1, std::memory_order_acq_rel);
}

void DeckLinkOutput::onScheduledFrameCompleted(IDeckLinkVideoFrame* frame)
{
    // Called from the SDK's internal thread. A reset may advance the frame-pool
    // generation if callbacks do not drain in time. In that case, late
    // callbacks from the retired generation must not decrement the new
    // timeline's scheduled-frame counter.
    uint64_t frameGeneration = 0;
    {
        std::lock_guard<std::mutex> lk(hw_mutex_);
        frameGeneration = framePoolGenerationForFrame(frame);
        IDeckLinkVideoFrame* pooledFrame = frame;
        if (MetadataVideoFrame* metadataFrame = dynamic_cast<MetadataVideoFrame*>(frame)) {
            pooledFrame = metadataFrame->wrappedFrame();
        }
        markPooledFrameFree(pooledFrame);
    }

    if (frameGeneration == frame_pool_generation_.load(std::memory_order_acquire)) {
        const uint32_t prev = scheduled_video_frames_.load(std::memory_order_acquire);
        if (prev > 0) {
            scheduled_video_frames_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    playback_stop_cv_.notify_all();
}

void DeckLinkOutput::onScheduledFrameCallbackEnd()
{
    const uint32_t prev = active_frame_callbacks_.load(std::memory_order_acquire);
    if (prev > 0) {
        active_frame_callbacks_.fetch_sub(1, std::memory_order_acq_rel);
    }
    playback_stop_cv_.notify_all();
}

void DeckLinkOutput::onScheduledFrameCompletionWarning()
{
    completion_warnings_.fetch_add(1, std::memory_order_relaxed);
}

void DeckLinkOutput::onScheduledPlaybackStopped()
{
    playback_stop_notified_.store(true, std::memory_order_release);
    playback_stop_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool DeckLinkOutput::validateVideoFrame(const VideoFrame& f) const
{
    if (f.width <= 0 || f.height <= 0) return false;
    // v210 packs 6 luma samples per 4 words; width must be a multiple of 6.
    if (f.width % 6 != 0) return false;
    if (f.pix_fmt != AV_PIX_FMT_YUV422P10LE) return false;
    if (!f.data[0] || !f.data[1] || !f.data[2]) return false;
    if (f.linesize[0] < f.width * 2) return false;
    if (f.linesize[1] < (f.width / 2) * 2) return false;
    if (f.linesize[2] < (f.width / 2) * 2) return false;
    return true;
}

bool DeckLinkOutput::validateAudioFrame(const AudioFrame& f) const
{
    if (!f.buffer || f.buffer_size == 0) return false;
    if (f.sample_rate != 48000) return false;
    if (f.channels < 2 || f.channels > 16 || (f.channels & 1)) return false;
    if (f.bytes_per_sample != 2) return false;
    if (f.num_samples <= 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Mode / timing resolution
// ---------------------------------------------------------------------------

BMDDisplayMode DeckLinkOutput::resolveDisplayMode(const VideoFrame& f) const
{
    const AVRational playbackRate = decklinkPlaybackFrameRate(f);
    const int n = playbackRate.num;
    const int d = playbackRate.den;

    if (f.width == 1920 && f.height == 1080) {
        if (f.interlaced) {
            if ((n == 25 && d == 1) || (n == 50 && d == 1)) return bmdModeHD1080i50;
            if ((n == 30000 && d == 1001) || (n == 60000 && d == 1001)) return bmdModeHD1080i5994;
            if ((n == 30 && d == 1) || (n == 60 && d == 1)) return bmdModeHD1080i6000;
            return bmdModeUnknown;
        }
        if (n == 25 && d == 1) return bmdModeHD1080p25;
        if (n == 50 && d == 1) return bmdModeHD1080p50;
        if (n == 30000 && d == 1001) return bmdModeHD1080p2997;
        if (n == 30 && d == 1) return bmdModeHD1080p30;
        if (n == 60000 && d == 1001) return bmdModeHD1080p5994;
        if (n == 60 && d == 1) return bmdModeHD1080p6000;
    }

    if (f.width == 1280 && f.height == 720) {
        if (n == 50 && d == 1) return bmdModeHD720p50;
        if (n == 60000 && d == 1001) return bmdModeHD720p5994;
        if (n == 60 && d == 1) return bmdModeHD720p60;
    }

    return bmdModeUnknown;
}

bool DeckLinkOutput::resolveFrameTiming(const VideoFrame& f)
{
    const BMDDisplayMode mode = resolveDisplayMode(f);

    struct TimingEntry {
        BMDDisplayMode mode;
        BMDTimeScale timeScale;
        BMDTimeValue frameDuration;
    };

    static const TimingEntry table[] = {
        { bmdModeHD1080i50,   25000, 1000 },
        { bmdModeHD1080p25,   25000, 1000 },
        { bmdModeHD1080i5994, 30000, 1001 },
        { bmdModeHD1080i6000, 30000, 1000 },
        { bmdModeHD1080p2997, 30000, 1001 },
        { bmdModeHD1080p30,   30000, 1000 },
        { bmdModeHD1080p50,   50000, 1000 },
        { bmdModeHD1080p5994, 60000, 1001 },
        { bmdModeHD1080p6000, 60000, 1000 },
        { bmdModeHD720p50,    50000, 1000 },
        { bmdModeHD720p5994,  60000, 1001 },
        { bmdModeHD720p60,    60000, 1000 }
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].mode == mode) {
            video_time_scale_ = table[i].timeScale;
            video_frame_duration_ = table[i].frameDuration;
            return true;
        }
    }

    setError("Unsupported DeckLink timing for decoded video mode.");
    return false;
}

// ---------------------------------------------------------------------------
// Output configuration
// ---------------------------------------------------------------------------


bool DeckLinkOutput::configureDeckLinkColorOutput(const VideoFrame& frame)
{
    const BMDColorspace colorspace = deckLinkColorspaceForFrame(frame);
    const BMDDynamicRange dynamicRange = deckLinkDynamicRangeForFrame(frame);
    const bool rec2020 = (colorspace == bmdColorspaceRec2020);

    if (decklink_config_) {
        const HRESULT hr = decklink_config_->SetFlag(bmdDeckLinkConfigRec2020Output, rec2020);
        if (hr != S_OK && rec2020) {
            std::cerr << "[DeckLinkOutput] WARN: DeckLink device/driver rejected Rec.2020 output flag. "
                      << "Continuing with frame metadata only.\n";
        }
    } else if (rec2020) {
        std::cerr << "[DeckLinkOutput] WARN: DeckLink configuration interface unavailable; "
                  << "cannot set Rec.2020 output flag. Continuing with frame metadata only.\n";
    }

    current_output_colorspace_ = colorspace;
    current_output_dynamic_range_ = dynamicRange;

    std::cout << "[DeckLinkOutput] Color output "
              << "colorspace=" << deckLinkColorspaceLabel(colorspace)
              << " dynamic_range=" << deckLinkDynamicRangeLabel(dynamicRange)
              << " rec2020_flag=" << (rec2020 ? "on" : "off")
              << " frame_metadata=" << (needsDeckLinkFrameMetadata(frame) ? "yes" : "no")
              << "\n";

    return true;
}

// Select and enable the SDI output mode that matches the receiver video
// format. This is intentionally performed once the first validated frame is
// available, because the receiver determines the actual playout format.
bool DeckLinkOutput::configureVideoOutput(const VideoFrame& frame)
{
    if (!validateVideoFrame(frame)) {
        setError("Invalid decoded video frame.");
        return false;
    }

    const BMDDisplayMode mode = resolveDisplayMode(frame);
    if (mode == bmdModeUnknown) {
        setError("Unsupported decoded video mode.");
        return false;
    }

    const BMDColorspace requestedColorspace = deckLinkColorspaceForFrame(frame);
    const BMDDynamicRange requestedDynamicRange = deckLinkDynamicRangeForFrame(frame);

    if (running_.load(std::memory_order_acquire) &&
        current_mode_ == mode &&
        current_width_ == frame.width &&
        current_height_ == frame.height &&
        current_interlaced_ == frame.interlaced &&
        current_output_colorspace_ == requestedColorspace &&
        current_output_dynamic_range_ == requestedDynamicRange) {
        return true;
    }

    if (running_.load(std::memory_order_acquire)) {
        // Video format changed: tear down the whole pipeline.
        // audio_enabled_ will be reset here; callers must reset haveAudioConfigured.
        resetHardwarePipeline(false);
        if (audio_enabled_.load(std::memory_order_acquire)) {
            decklink_output_->DisableAudioOutput();
            audio_enabled_.store(false, std::memory_order_release);
        }
        decklink_output_->DisableVideoOutput();
        running_.store(false, std::memory_order_release);
        waitForScheduledCallbacksDrained(250);
        if (callback_) {
            decklink_output_->SetScheduledFrameCompletionCallback(nullptr);
            callback_->Release();
            callback_ = nullptr;
        }
        releaseAllPooledFrames();
    }

    if (!resolveFrameTiming(frame)) {
        return false;
    }

    if (!configureDeckLinkColorOutput(frame)) {
        return false;
    }

    if (decklink_output_->EnableVideoOutput(mode, bmdVideoOutputFlagDefault) != S_OK) {
        setError("EnableVideoOutput failed.");
        return false;
    }

    callback_ = new OutputCallback(this);
    if (decklink_output_->SetScheduledFrameCompletionCallback(callback_) != S_OK) {
        setError("SetScheduledFrameCompletionCallback failed.");
        callback_->Release();
        callback_ = nullptr;
        decklink_output_->DisableVideoOutput();
        return false;
    }

    current_mode_ = mode;
    current_width_ = frame.width;
    current_height_ = frame.height;
    current_interlaced_ = frame.interlaced;
    current_pixel_format_ = bmdFormat10BitYUV;

    {
        std::lock_guard<std::mutex> lk(hw_mutex_);
        next_video_time_ = 0;
        next_audio_time_ = 0;
        playback_started_ = false;
        expect_audio_at_start_ = false;
        scheduled_video_frames_.store(0, std::memory_order_relaxed);
        buffered_video_frames_.store(0, std::memory_order_relaxed);
    }

    updateReferenceStatus();
    const AVRational playbackRate = decklinkPlaybackFrameRate(frame);
    cadence_.configure(playbackRate, frame.interlaced, frame.width, frame.height);
    running_.store(true, std::memory_order_release);

    std::cout << "[DeckLinkOutput] Video enabled "
              << frame.width << "x" << frame.height
              << " mode=" << static_cast<int>(mode)
              << " interlaced=" << (frame.interlaced ? "yes" : "no")
              << " source_rate=" << frame.nominal_frame_rate.num << "/" << frame.nominal_frame_rate.den
              << " playback_rate=" << playbackRate.num << "/" << playbackRate.den
              << " audio_samples_per_frame=" << cadenceSamplesForFrameIndex(0)
              << " ts=" << video_time_scale_
              << " fd=" << video_frame_duration_
              << " ref=" << getReferenceStatusString()
              << "\n";

    return true;
}

bool DeckLinkOutput::configureAudioOutput(const AudioFrame& frame)
{
    // Must be called with hw_mutex_ held (or before the callback is active).
    if (!running_.load(std::memory_order_acquire)) {
        setError("configureAudioOutput called before video.");
        return false;
    }
    if (!validateAudioFrame(frame)) {
        setError("Invalid decoded audio frame.");
        return false;
    }

    if (audio_enabled_.load(std::memory_order_acquire) &&
        current_audio_sample_rate_ == frame.sample_rate &&
        current_audio_channels_ == frame.channels &&
        current_audio_bps_ == frame.bytes_per_sample) {
        return true;
    }

    if (audio_enabled_.load(std::memory_order_acquire)) {
        decklink_output_->DisableAudioOutput();
        audio_enabled_.store(false, std::memory_order_release);
        audio_preroll_active_ = false;
    }

    if (decklink_output_->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                            bmdAudioSampleType16bitInteger,
                                            frame.channels,
                                            bmdAudioOutputStreamContinuous) != S_OK) {
        setError("EnableAudioOutput failed.");
        return false;
    }

    current_audio_sample_rate_ = frame.sample_rate;
    current_audio_channels_ = frame.channels;
    current_audio_bps_ = frame.bytes_per_sample;

    audio_preroll_target_samples_ = std::max<uint32_t>(
        static_cast<uint32_t>(frame.num_samples),
        cadenceSamplesForPreroll(video_preroll_frames_));

    next_audio_time_ = 0;
    buffered_audio_samples_.store(0, std::memory_order_relaxed);
    audio_preroll_active_ = false;

    if (!playback_started_) {
        if (decklink_output_->BeginAudioPreroll() != S_OK) {
            setError("BeginAudioPreroll failed.");
            decklink_output_->DisableAudioOutput();
            return false;
        }
        audio_preroll_active_ = true;
    }

    audio_enabled_.store(true, std::memory_order_release);

    std::cout << "[DeckLinkOutput] Audio enabled "
              << frame.sample_rate << "Hz ch=" << frame.channels
              << " preroll_target=" << audio_preroll_target_samples_ << "\n";

    return true;
}

// ---------------------------------------------------------------------------
// Cadence helpers
// ---------------------------------------------------------------------------

uint32_t DeckLinkOutput::cadenceSamplesForFrameIndex(int64_t frameIndex) const
{
    if (cadence_.isConfigured()) {
        return static_cast<uint32_t>(std::max(1, cadence_.samplesForFrame(frameIndex)));
    }
    if (video_time_scale_ <= 0 || video_frame_duration_ <= 0 || current_audio_sample_rate_ <= 0) {
        return 1920;
    }
    return static_cast<uint32_t>(std::max<int64_t>(1,
        (static_cast<int64_t>(current_audio_sample_rate_) * video_frame_duration_) /
        video_time_scale_));
}

uint32_t DeckLinkOutput::cadenceSamplesForPreroll(uint32_t frameCount) const
{
    if (frameCount == 0) {
        return 0;
    }
    if (cadence_.isConfigured()) {
        return static_cast<uint32_t>(
            std::max<int64_t>(0, cadence_.sampleStartForFrame(frameCount)));
    }
    const uint64_t total =
        static_cast<uint64_t>(cadenceSamplesForFrameIndex(0)) *
        static_cast<uint64_t>(frameCount);
    return static_cast<uint32_t>(
        std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

// ---------------------------------------------------------------------------
// Pixel format conversion
// ---------------------------------------------------------------------------

// Final receiver-to-SDI conversion boundary. The receiver frame is planar
// YUV422P10LE; DeckLink SDI output expects packed v210, so this function writes
// directly into the DeckLink output frame buffer without an extra temporary
// v210 image.
bool DeckLinkOutput::convertYUV422P10ToV210(const VideoFrame& f,
                                            uint8_t* dst,
                                            int rowBytes)
{
    const uint16_t* yBase = reinterpret_cast<const uint16_t*>(f.data[0]);
    const uint16_t* uBase = reinterpret_cast<const uint16_t*>(f.data[1]);
    const uint16_t* vBase = reinterpret_cast<const uint16_t*>(f.data[2]);

    const int yStride = f.linesize[0] / 2;
    const int uStride = f.linesize[1] / 2;
    const int vStride = f.linesize[2] / 2;

    for (int y = 0; y < f.height; ++y) {
        const uint16_t* yRow = yBase + y * yStride;
        const uint16_t* uRow = uBase + y * uStride;
        const uint16_t* vRow = vBase + y * vStride;
        uint32_t* out = reinterpret_cast<uint32_t*>(dst + y * rowBytes);

        // Width is guaranteed to be a multiple of 6 by validateVideoFrame().
        for (int x = 0; x < f.width; x += 6) {
            const uint32_t U0 = normalize10Sample(uRow[x / 2 + 0]);
            const uint32_t Y0 = normalize10Sample(yRow[x + 0]);
            const uint32_t V0 = normalize10Sample(vRow[x / 2 + 0]);

            const uint32_t Y1 = normalize10Sample(yRow[x + 1]);
            const uint32_t U1 = normalize10Sample(uRow[x / 2 + 1]);
            const uint32_t Y2 = normalize10Sample(yRow[x + 2]);

            const uint32_t V1 = normalize10Sample(vRow[x / 2 + 1]);
            const uint32_t Y3 = normalize10Sample(yRow[x + 3]);
            const uint32_t U2 = normalize10Sample(uRow[x / 2 + 2]);

            const uint32_t Y4 = normalize10Sample(yRow[x + 4]);
            const uint32_t V2 = normalize10Sample(vRow[x / 2 + 2]);
            const uint32_t Y5 = normalize10Sample(yRow[x + 5]);

            out[0] = U0 | (Y0 << 10) | (V0 << 20);
            out[1] = Y1 | (U1 << 10) | (Y2 << 20);
            out[2] = V1 | (Y3 << 10) | (U2 << 20);
            out[3] = Y4 | (V2 << 10) | (Y5 << 20);
            out += 4;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Frame pool
// ---------------------------------------------------------------------------

IDeckLinkMutableVideoFrame* DeckLinkOutput::obtainPooledFrame()
{
    // Reuse only frames from the active pool generation. If a hardware reset
    // timed out while the DeckLink driver still owned scheduled frames, those
    // frames are quarantined in an older generation and must not be overwritten
    // by the new playout timeline. They may be released later once callbacks
    // return them, but they are never reused across the reset boundary.
    for (size_t i = 0; i < frame_pool_.size(); ++i) {
        if (!frame_pool_[i].in_use &&
            frame_pool_[i].generation == frame_pool_generation_.load(std::memory_order_acquire) &&
            frame_pool_[i].frame) {
            frame_pool_[i].in_use = true;
            frame_pool_[i].frame->AddRef(); // scheduled-playback ref
            return frame_pool_[i].frame;
        }
    }

    // SDK 15.x separates the frame object from its writable memory.
    // Own one explicit buffer per pooled frame and bind it with CreateVideoFrameWithBuffer().
    const int rowBytes = v210RowBytes(current_width_);
    const size_t bufferSize = static_cast<size_t>(rowBytes) * static_cast<size_t>(current_height_);
    IDeckLinkVideoBuffer* buffer = new OwnedDeckLinkVideoBuffer(bufferSize);

    IDeckLinkMutableVideoFrame* frame = nullptr;
    if (decklink_output_->CreateVideoFrameWithBuffer(current_width_,
                                                     current_height_,
                                                     rowBytes,
                                                     bmdFormat10BitYUV,
                                                     bmdFrameFlagDefault,
                                                     buffer,
                                                     &frame) != S_OK || !frame) {
        buffer->Release();
        setError("CreateVideoFrameWithBuffer failed.");
        return nullptr;
    }

    PooledFrame pooled;
    pooled.frame = frame;   // pool retains the create-time frame reference (refcount=1)
    pooled.buffer = buffer; // pool retains the create-time buffer reference
    pooled.in_use = true;
    pooled.generation = frame_pool_generation_.load(std::memory_order_acquire);
    frame_pool_.push_back(pooled);

    frame->AddRef(); // scheduled-playback reference (refcount=2)
    return frame;
}

IDeckLinkVideoBuffer* DeckLinkOutput::bufferForPooledFrame(IDeckLinkVideoFrame* frame)
{
    if (!frame) {
        return nullptr;
    }
    for (size_t i = 0; i < frame_pool_.size(); ++i) {
        if (frame_pool_[i].frame == frame) {
            return frame_pool_[i].buffer;
        }
    }
    return nullptr;
}

uint64_t DeckLinkOutput::framePoolGenerationForFrame(IDeckLinkVideoFrame* frame) const
{
    if (!frame) {
        return 0;
    }

    IDeckLinkVideoFrame* pooledFrame = frame;
    if (MetadataVideoFrame* metadataFrame = dynamic_cast<MetadataVideoFrame*>(frame)) {
        pooledFrame = metadataFrame->wrappedFrame();
    }

    for (size_t i = 0; i < frame_pool_.size(); ++i) {
        if (frame_pool_[i].frame == pooledFrame) {
            return frame_pool_[i].generation;
        }
    }
    return 0;
}

void DeckLinkOutput::markPooledFrameFree(IDeckLinkVideoFrame* frame)
{
    if (!frame) {
        return;
    }
    for (size_t i = 0; i < frame_pool_.size(); ++i) {
        if (frame_pool_[i].frame == frame) {
            frame_pool_[i].in_use = false;
            return;
        }
    }
}

void DeckLinkOutput::releasePooledFrame(IDeckLinkMutableVideoFrame* frame)
{
    if (!frame) {
        return;
    }
    markPooledFrameFree(frame);
    frame->Release(); // release the scheduled-playback ref we issued
}

void DeckLinkOutput::releaseAllPooledFrames()
{
    std::vector<PooledFrame> retained;
    retained.reserve(frame_pool_.size());

    for (size_t i = 0; i < frame_pool_.size(); ++i) {
        if (frame_pool_[i].in_use) {
            // The DeckLink driver may still own the scheduled-playback
            // reference. Do not release the pool/buffer references yet; keep
            // the object quarantined instead of risking reuse/release while the
            // driver can still complete it asynchronously.
            retained.push_back(frame_pool_[i]);
            continue;
        }

        if (frame_pool_[i].frame) {
            frame_pool_[i].frame->Release(); // release pool's create-time ref
            frame_pool_[i].frame = nullptr;
        }
        if (frame_pool_[i].buffer) {
            frame_pool_[i].buffer->Release(); // release pool's buffer ref
            frame_pool_[i].buffer = nullptr;
        }
    }

    if (!retained.empty()) {
        std::cerr << "[DeckLinkOutput] Warning: retained " << retained.size()
                  << " in-use pooled frame(s) during output teardown; "
                  << "they remain quarantined until callbacks return.\n";
    }

    frame_pool_.swap(retained);
}

void DeckLinkOutput::quarantineFramePoolAfterDrainTimeout()
{
    const uint64_t newGeneration = frame_pool_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

    size_t quarantined = 0;
    for (size_t i = 0; i < frame_pool_.size(); ++i) {
        if (frame_pool_[i].in_use) {
            ++quarantined;
        }
    }

    std::cerr << "[DeckLinkOutput] Warning: DeckLink frame-pool generation advanced to "
              << newGeneration << " after reset drain timeout; "
              << quarantined << " in-use frame(s) quarantined from reuse.\n";
}

bool DeckLinkOutput::waitForScheduledCallbacksDrained(uint32_t timeoutMs)
{
    auto drained = [this]() {
        return scheduled_video_frames_.load(std::memory_order_acquire) == 0 &&
               active_frame_callbacks_.load(std::memory_order_acquire) == 0;
    };

    if (drained()) {
        return true;
    }

    std::unique_lock<std::mutex> lk(playback_stop_mutex_);
    const bool ok = playback_stop_cv_.wait_for(
        lk,
        std::chrono::milliseconds(timeoutMs),
        drained);

    if (!ok) {
        std::cerr << "[DeckLinkOutput] Warning: timed out waiting for "
                  << scheduled_video_frames_.load(std::memory_order_acquire)
                  << " scheduled frame(s), "
                  << active_frame_callbacks_.load(std::memory_order_acquire)
                  << " active callback(s) during reset. Continuing shutdown.\n";
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Low-level scheduling
// ---------------------------------------------------------------------------

// Convert the selected source frame into a pooled DeckLink frame and schedule
// it against the hardware timeline. Frame-pool ownership is released only from
// the DeckLink completion callback.
bool DeckLinkOutput::scheduleVideoFrame(const VideoFrame& source,
                                        BMDTimeValue displayTime)
{
    IDeckLinkMutableVideoFrame* frame = obtainPooledFrame();
    if (!frame) {
        return false;
    }

    IDeckLinkVideoBuffer* buffer = bufferForPooledFrame(frame);
    if (!buffer) {
        releasePooledFrame(frame);
        setError("No DeckLink video buffer for pooled frame.");
        return false;
    }

    HRESULT accessHr = buffer->StartAccess(bmdBufferAccessWrite);
    if (accessHr != S_OK) {
        releasePooledFrame(frame);
        setError("DeckLink video buffer StartAccess(write) failed.");
        return false;
    }

    void* bytes = nullptr;
    if (buffer->GetBytes(&bytes) != S_OK || !bytes) {
        buffer->EndAccess(bmdBufferAccessWrite);
        releasePooledFrame(frame);
        setError("DeckLink video buffer GetBytes failed.");
        return false;
    }

    const bool converted = convertYUV422P10ToV210(source,
                                                 static_cast<uint8_t*>(bytes),
                                                 v210RowBytes(current_width_));
    buffer->EndAccess(bmdBufferAccessWrite);

    if (!converted) {
        releasePooledFrame(frame);
        return false;
    }

    IDeckLinkVideoFrame* frameToSchedule = frame;
    MetadataVideoFrame* metadataWrapper = nullptr;
    if (needsDeckLinkFrameMetadata(source)) {
        metadataWrapper = new MetadataVideoFrame(frame, buffer, buildDeckLinkFrameMetadata(source));
        frameToSchedule = metadataWrapper;
    }

    if (decklink_output_->ScheduleVideoFrame(frameToSchedule,
                                             displayTime,
                                             video_frame_duration_,
                                             video_time_scale_) != S_OK) {
        if (metadataWrapper) {
            markPooledFrameFree(frame);
            metadataWrapper->Release(); // also releases the scheduled-playback ref on frame
        } else {
            releasePooledFrame(frame);
        }
        schedule_failures_.fetch_add(1, std::memory_order_relaxed);
        setError("ScheduleVideoFrame failed.");
        return false;
    }

    next_video_time_ = displayTime + video_frame_duration_;
    scheduled_video_frames_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool DeckLinkOutput::scheduleAudioSamples(const AudioFrame& source,
                                          BMDTimeValue streamTime,
                                          AudioFrame* leftoverOut)
{
    if (leftoverOut) {
        *leftoverOut = AudioFrame{};
    }

    uint32_t written = 0;
    if (decklink_output_->ScheduleAudioSamples(source.buffer.get(),
                                               static_cast<uint32_t>(source.num_samples),
                                               streamTime,
                                               static_cast<BMDTimeScale>(source.sample_rate),
                                               &written) != S_OK) {
        schedule_failures_.fetch_add(1, std::memory_order_relaxed);
        setError("ScheduleAudioSamples failed.");
        return false;
    }

    if (written > 0) {
        next_audio_time_ = streamTime + static_cast<BMDTimeValue>(written);
    } else if (source.num_samples > 0) {
        // The DeckLink SDK can legally accept zero samples when the scheduled
        // audio buffer is full. Treat that as output backpressure, not as
        // forward progress; otherwise scheduleAudioFrameChain() can spin forever
        // on the same audio block and starve video scheduling. For live output,
        // dropping this block is safer than blocking the SDI playout thread.
        dropped_audio_frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (written < static_cast<uint32_t>(source.num_samples) && leftoverOut) {
        const uint32_t remaining = static_cast<uint32_t>(source.num_samples) - written;
        const size_t bytesPerSampleFrame =
            static_cast<size_t>(source.channels) *
            static_cast<size_t>(source.bytes_per_sample);
        const size_t offsetBytes = static_cast<size_t>(written) * bytesPerSampleFrame;
        const size_t remainingBytes = static_cast<size_t>(remaining) * bytesPerSampleFrame;

        AudioFrame leftover;
        leftover.sample_rate = source.sample_rate;
        leftover.channels = source.channels;
        leftover.bytes_per_sample = source.bytes_per_sample;
        leftover.num_samples = static_cast<int>(remaining);
        leftover.time_base = source.time_base;
        leftover.pts = source.pts + static_cast<int64_t>(written);
        leftover.buffer_size = remainingBytes;
        leftover.buffer = make_shared_u8(std::max<size_t>(remainingBytes, 1));
        if (remainingBytes > 0) {
            std::memcpy(leftover.buffer.get(),
                        source.buffer.get() + offsetBytes,
                        remainingBytes);
        }
        *leftoverOut = std::move(leftover);
    }

    return true;
}

// Drain an AudioFrame fully, handling partial writes from the SDK.
// startStreamTime is the initial stream position; subsequent leftovers
// use next_audio_time_ which scheduleAudioSamples keeps updated.
// Break receiver audio into DeckLink scheduling chunks while preserving the
// video-derived sample cadence. This keeps embedded SDI audio aligned with the
// scheduled video timeline.
bool DeckLinkOutput::scheduleAudioFrameChain(const AudioFrame& source,
                                             BMDTimeValue startStreamTime)
{
    AudioFrame current = source;
    BMDTimeValue streamTime = startStreamTime;
    while (current.buffer && current.num_samples > 0) {
        AudioFrame leftover;
        if (!scheduleAudioSamples(current, streamTime, &leftover)) {
            return false;
        }
        if (!leftover.buffer || leftover.num_samples <= 0) {
            break;
        }
        // next_audio_time_ was updated by scheduleAudioSamples for the leftover.
        streamTime = next_audio_time_;
        current = std::move(leftover);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Playback lifecycle
// ---------------------------------------------------------------------------

bool DeckLinkOutput::startPlaybackIfReady(bool require_audio_preroll)
{
    if (playback_started_) {
        return true;
    }

    if (scheduled_video_frames_.load(std::memory_order_acquire) < video_preroll_frames_) {
        return true;
    }

    if (require_audio_preroll) {
        if (!audio_enabled_.load(std::memory_order_acquire)) {
            return true;
        }

        uint32_t buffered = 0;
        if (decklink_output_->GetBufferedAudioSampleFrameCount(&buffered) == S_OK) {
            if (buffered < audio_preroll_target_samples_) {
                return true;
            }
        }

        if (audio_preroll_active_) {
            if (decklink_output_->EndAudioPreroll() != S_OK) {
                setError("EndAudioPreroll failed.");
                return false;
            }
            audio_preroll_active_ = false;
        }
    }

    if (decklink_output_->StartScheduledPlayback(0, video_time_scale_, 1.0) != S_OK) {
        setError("StartScheduledPlayback failed.");
        return false;
    }

    playback_started_ = true;

    // After playback starts, synchronise next_audio_time_ to the
    // hardware stream clock position so that subsequent audio scheduling uses the
    // real stream cursor rather than the preroll accumulation value. Without this,
    // any drift between preroll sample count and actual hardware position causes
    // all subsequent audio to be misaligned vs video.
    if (audio_enabled_.load(std::memory_order_acquire) && current_audio_sample_rate_ > 0 &&
        video_time_scale_ > 0) {
        BMDTimeValue streamTime = 0;
        double speed = 0.0;
        if (decklink_output_->GetScheduledStreamTime(video_time_scale_, &streamTime, &speed) == S_OK) {
            const int64_t syncedAudioTime = av_rescale_q(
                streamTime,
                AVRational{1, static_cast<int>(video_time_scale_)},
                AVRational{1, current_audio_sample_rate_});
            // Only advance next_audio_time_; never rewind it, to avoid
            // re-scheduling samples already handed to the SDK.
            if (syncedAudioTime > next_audio_time_) {
                next_audio_time_ = syncedAudioTime;
                std::cout << "[DeckLinkOutput] next_audio_time_ synced to hardware clock: "
                          << next_audio_time_ << "\n";
            }
        }
    }

    updateReferenceStatus();
    std::cout << "[DeckLinkOutput] Playback started. ref=" << getReferenceStatusString() << "\n";
    return true;
}

// Reset DeckLink playback state after underrun, source loss, or mode changes.
// The function drains callbacks before reusing pooled frames so the SDK never
// receives a buffer that NxFrame has already recycled.
bool DeckLinkOutput::resetHardwarePipeline(bool restartPreroll)
{
    if (playback_started_) {
        playback_stop_notified_.store(false, std::memory_order_release);

        // Shutdown must never hang the receiver. Some DeckLink drivers do not
        // deliver ScheduledPlaybackHasStopped quickly when Ctrl-C arrives while
        // audio/video are queued. Request stop, then continue with buffer
        // flushing and output disable below. The callback may still arrive
        // later, but NxFrame shutdown no longer depends on it.
        BMDTimeValue actualStopTime = 0;
        const HRESULT stopHr = decklink_output_->StopScheduledPlayback(
            0,
            &actualStopTime,
            video_time_scale_ > 0 ? video_time_scale_ : 25000);
        if (stopHr != S_OK) {
            std::cerr << "[DeckLinkOutput] Warning: StopScheduledPlayback returned hr=0x"
                      << std::hex << stopHr << std::dec << "\n";
        }

        playback_started_ = false;
        expect_audio_at_start_ = false;
        playback_stop_notified_.store(false, std::memory_order_relaxed);
    }

    // Give the DeckLink callback thread a bounded chance to return scheduled
    // frame references before pooled frames are made reusable. If the driver
    // does not drain in time, advance the pool generation and quarantine any
    // in-flight frames instead of marking them free and risking overwrite while
    // the DeckLink hardware still owns them.
    const bool callbacksDrained = waitForScheduledCallbacksDrained(250);
    if (!callbacksDrained) {
        quarantineFramePoolAfterDrainTimeout();
    }

    if (audio_enabled_.load(std::memory_order_acquire)) {
        decklink_output_->FlushBufferedAudioSamples();
    }

    next_video_time_ = 0;
    next_audio_time_ = 0;
    scheduled_video_frames_.store(0, std::memory_order_relaxed);
    active_frame_callbacks_.store(0, std::memory_order_relaxed);
    buffered_video_frames_.store(0, std::memory_order_relaxed);
    buffered_audio_samples_.store(0, std::memory_order_relaxed);
    audio_preroll_active_ = false;

    if (callbacksDrained) {
        for (size_t i = 0; i < frame_pool_.size(); ++i) {
            if (frame_pool_[i].generation == frame_pool_generation_.load(std::memory_order_acquire)) {
                frame_pool_[i].in_use = false;
            }
        }
    }

    if (audio_enabled_.load(std::memory_order_acquire) && restartPreroll) {
        if (decklink_output_->BeginAudioPreroll() != S_OK) {
            setError("BeginAudioPreroll failed on reset.");
            return false;
        }
        audio_preroll_active_ = true;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Clock / status queries
// ---------------------------------------------------------------------------

DeckLinkOutput::OutputClock DeckLinkOutput::queryOutputClock() const
{
    OutputClock c;
    if (!decklink_output_ || video_time_scale_ <= 0) {
        return c;
    }

    if (!playback_started_) {
        c.valid = true;
        c.video_ticks = 0;
        c.audio_samples = 0;
        c.speed = 0.0;
        return c;
    }

    BMDTimeValue streamTime = 0;
    double speed = 0.0;
    if (decklink_output_->GetScheduledStreamTime(video_time_scale_, &streamTime, &speed) != S_OK) {
        return c;
    }

    c.valid = true;
    c.video_ticks = streamTime;
    c.speed = speed;

    if (current_audio_sample_rate_ > 0) {
        c.audio_samples = av_rescale_q(
            streamTime,
            AVRational{1, static_cast<int>(video_time_scale_)},
            AVRational{1, current_audio_sample_rate_});
    }

    return c;
}

bool DeckLinkOutput::updateReferenceStatus()
{
    if (!decklink_output_) {
        return false;
    }

    BMDReferenceStatus st = bmdReferenceUnlocked;
    if (decklink_output_->GetReferenceStatus(&st) != S_OK) {
        reference_supported_ = false;
        reference_locked_ = false;
        return false;
    }

    reference_supported_ = !(st & bmdReferenceNotSupportedByHardware);
    reference_locked_ = (st & bmdReferenceLocked) != 0;
    return true;
}

std::string DeckLinkOutput::getReferenceStatusString() const
{
    if (!reference_supported_) return "not_supported";
    return reference_locked_ ? "locked" : "internal";
}

void DeckLinkOutput::refreshBufferedCounts()
{
    if (!decklink_output_) {
        return;
    }

    uint32_t videoCount = 0;
    if (decklink_output_->GetBufferedVideoFrameCount(&videoCount) == S_OK) {
        buffered_video_frames_.store(videoCount, std::memory_order_release);
    }

    if (audio_enabled_.load(std::memory_order_acquire)) {
        uint32_t audioCount = 0;
        if (decklink_output_->GetBufferedAudioSampleFrameCount(&audioCount) == S_OK) {
            buffered_audio_samples_.store(audioCount, std::memory_order_release);
        }
    }
}

// ---------------------------------------------------------------------------
// Main playout loop
// ---------------------------------------------------------------------------

// Main receiver playout loop. It pulls synchronized audio/video from the
// receiver, maintains black fallback during source loss, and feeds DeckLink
// with enough preroll to keep hardware playback stable.
int DeckLinkOutput::runPlayout(Receiver& receiver,
                               const std::atomic<bool>& stopFlag,
                               const DeckLinkPlayoutConfig& config)
{
    video_preroll_frames_ = config.videoPrerollFrames;
    max_video_queue_frames_ = config.maxVideoQueueFrames;
    max_audio_queue_samples_ = config.maxAudioQueueSamples;

    logPlayoutConfig(config,
                     video_preroll_frames_,
                     max_video_queue_frames_,
                     max_audio_queue_samples_);

    AvSyncController sync;

    VideoFrame videoTemplate;
    bool haveVideoTemplate = false;
    AudioFrame audioTemplate;
    bool haveAudioTemplate = false;

    bool haveAudioConfigured = false;
    bool sawAnyAudio = false;
    bool loggedAudioStartupWait = false;
    uint64_t lastSourceGeneration = receiver.sourceGeneration();

    // Master-clock policy is defined in playout/receiver_clock_policy.h.
    // Keep this loop PTS-clocked and DeckLink-scheduled: transport arrival
    // time may wake the loop, but it must never place media on the SDI timeline.
    const int64_t mediaBackwardResetUs = nxframe::receiver_clock_policy::backwardPtsResetUs();
    const int64_t mediaForwardResetUs  = nxframe::receiver_clock_policy::forwardPtsResetUs();
    int64_t lastAcceptedVideoPtsUs = AvSyncController::invalidTime();
    int64_t lastAcceptedAudioPtsUs = AvSyncController::invalidTime();

    uint64_t displayedVideo = 0;
    uint64_t playedAudioFrames = 0;
    uint64_t playedAudioSamples = 0;
    uint64_t hardResyncEvents = 0;
    uint64_t lateVideoDrops = 0;
    uint64_t liveTimelineCatchups = 0;
    uint64_t fallbackVideoFrames = 0;
    uint64_t fallbackAudioFrames = 0;

    // DeckLink scheduled playback uses a hardware stream clock. In live mode the
    // receiver can occasionally start with only a small preroll. If the hardware
    // clock outruns our media-relative timeline, permanently dropping all future
    // frames as "late" freezes output because live PTS advances at the same
    // speed as the hardware clock. Keep a small output-time offset and move the
    // media timeline forward when this happens. This preserves live recovery
    // without rewriting incoming PTS.
    BMDTimeValue outputVideoOffsetTicks = 0;
    int64_t outputAudioOffsetSamples = 0;

    auto lastMediaAt = std::chrono::steady_clock::now();
    auto lastLog = lastMediaAt;
    auto anchorWaitStartedAt = lastMediaAt;
    bool sourceLossActive = false;

    auto resetTimeline = [&](const char* reason) {
        sync.reset();
        outputVideoOffsetTicks = 0;
        outputAudioOffsetSamples = 0;
        lastAcceptedVideoPtsUs = AvSyncController::invalidTime();
        lastAcceptedAudioPtsUs = AvSyncController::invalidTime();
        sawAnyAudio = false;
        haveAudioConfigured = false;
        haveAudioTemplate = false;
        audioTemplate = AudioFrame{};
        loggedAudioStartupWait = false;
        ++hardResyncEvents;
        receiver.requestAudioCursorReset();

        // A source-generation reset must not keep the old embedded-audio output
        // path alive. If the next source is video-only, stale audio preroll would
        // otherwise block StartScheduledPlayback() forever because the playout
        // loop would still require audio preroll from the previous source.
        resetHardwarePipeline(false);
        if (audio_enabled_.load(std::memory_order_acquire)) {
            decklink_output_->DisableAudioOutput();
            audio_enabled_.store(false, std::memory_order_release);
        }
        current_audio_sample_rate_ = 0;
        current_audio_channels_ = 0;
        current_audio_bps_ = 0;
        next_audio_time_ = 0;
        audio_preroll_active_ = false;
        expect_audio_at_start_ = false;
        buffered_audio_samples_.store(0, std::memory_order_release);

        anchorWaitStartedAt = std::chrono::steady_clock::now();
        std::cout << "[PLAY-DECKLINK] timeline reset reason=" << reason
                  << " audio_output=reset"
                  << " clock_policy=" << nxframe::receiver_clock_policy::modeName()
                  << "\n";
    };

    auto refreshOutputAudioOffset = [&]() {
        outputAudioOffsetSamples = deckLinkTicksToAudioSamples(outputVideoOffsetTicks,
                                                               video_time_scale_,
                                                               current_audio_sample_rate_);
    };

    auto isPtsDiscontinuity = [&](int64_t previousUs, int64_t currentUs) -> bool {
        if (previousUs == AvSyncController::invalidTime() ||
            currentUs == AvSyncController::invalidTime()) {
            return false;
        }

        const int64_t deltaUs = currentUs - previousUs;
        if (deltaUs < -mediaBackwardResetUs) {
            return true;
        }
        if (deltaUs > mediaForwardResetUs) {
            return true;
        }
        return false;
    };

    auto scheduleSeededVideo = [&](const AvSyncController::ScheduledVideo& sv) -> bool {
        if (!sv.frame.buffer) {
            return true;
        }
        const BMDTimeValue displayTime =
            usToDeckLinkTicks(sv.playout_time_us, video_time_scale_) + outputVideoOffsetTicks;
        if (!scheduleVideoFrame(sv.frame, displayTime)) {
            return false;
        }
        ++displayedVideo;
        return true;
    };

    auto scheduleSeededAudio = [&](const AvSyncController::ScheduledAudio& sa) -> bool {
        if (!sa.frame.buffer || sa.frame.num_samples <= 0 || !audio_enabled_.load(std::memory_order_acquire)) {
            return true;
        }
        if (!scheduleAudioFrameChain(sa.frame, static_cast<BMDTimeValue>(sa.playout_sample + outputAudioOffsetSamples))) {
            return false;
        }
        ++playedAudioFrames;
        playedAudioSamples += static_cast<uint64_t>(std::max(sa.frame.num_samples, 0));
        return true;
    };

    while (!stopFlag.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        const bool allowFallback =
            elapsedMsAtLeast(now, lastMediaAt, config.blackFallbackThresholdMs, 100);
        bool progressed = false;

        if (receiver.sourceGeneration() != lastSourceGeneration) {
            lastSourceGeneration = receiver.sourceGeneration();
            sourceLossActive = false;
            resetTimeline("source_generation");
        }

        VideoFrame vf;
        while (receiver.popVideoFrame(vf, 0)) {
            progressed = true;
            lastMediaAt = now;
            sourceLossActive = false;

            const bool audioWasEnabled = audio_enabled_.load(std::memory_order_acquire);
            if (!configureVideoOutput(vf)) {
                return -1;
            }
            if (audioWasEnabled && !audio_enabled_.load(std::memory_order_acquire)) {
                resetTimeline("video_format_change");
            }

            const int64_t ptsUs = videoFramePtsUs(vf);

            // Keep one fallback/template frame, but do not clone every decoded
            // video frame before handing it to the AV-sync queue. DecoderVideo
            // already gives us an owned frame buffer, so the previous code paid
            // for a second full 1080i/p 10-bit 4:2:2 frame copy on every frame.
            // On live SDI playout this extra memory bandwidth can starve the
            // DeckLink scheduler just enough to drain the hardware queue, causing
            // repeated live timeline catch-up events and visibly uneven output.
            refreshVideoFallbackTemplate(vf, videoTemplate, haveVideoTemplate);

            if (vf.buffer && ptsUs != AvSyncController::invalidTime()) {
                if (isPtsDiscontinuity(lastAcceptedVideoPtsUs, ptsUs)) {
                    std::cout << "[PLAY-DECKLINK] video PTS discontinuity: previous_us="
                              << lastAcceptedVideoPtsUs
                              << " current_us=" << ptsUs
                              << " delta_ms=" << (ptsUs - lastAcceptedVideoPtsUs) / 1000.0
                              << " -> hard timeline reset\n";
                    resetTimeline("video_pts_discontinuity");
                }
                lastAcceptedVideoPtsUs = ptsUs;
                sync.pushVideo(std::move(vf), ptsUs);
            }
        }

        AudioFrame af;
        while (receiver.popAudioFrame(af, 0)) {
            progressed = true;
            lastMediaAt = now;
            sourceLossActive = false;
            sawAnyAudio = true;

            const int64_t ptsUs = audioFramePtsUs(af);
            const int64_t endUs = AvSyncController::audioEndUs(af, ptsUs);

            // Receiver::popAudioFrame() gives us owned audio memory. Move it
            // directly into the AV-sync controller instead of cloning every
            // 10 ms audio block. Keep only a lightweight silent template for
            // audio-output configuration/fallback, updating it only when the
            // audio format changes. This reduces allocation/copy churn in the
            // live SDI playout loop without changing timing decisions.
            refreshSilentAudioTemplate(af, audioTemplate, haveAudioTemplate);

            if (af.buffer && ptsUs != AvSyncController::invalidTime() && endUs != AvSyncController::invalidTime()) {
                if (isPtsDiscontinuity(lastAcceptedAudioPtsUs, ptsUs)) {
                    std::cout << "[PLAY-DECKLINK] audio PTS discontinuity: previous_us="
                              << lastAcceptedAudioPtsUs
                              << " current_us=" << ptsUs
                              << " delta_ms=" << (ptsUs - lastAcceptedAudioPtsUs) / 1000.0
                              << " -> hard timeline reset\n";
                    resetTimeline("audio_pts_discontinuity");
                }
                lastAcceptedAudioPtsUs = ptsUs;
                sync.pushAudio(std::move(af), ptsUs, endUs);
            }
        }

        if (!running_.load(std::memory_order_acquire)) {
            if (!progressed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(hw_mutex_);
            refreshBufferedCounts();

            if (!haveAudioConfigured && sync.queuedAudio() > 0) {
                // Use the saved template because AvSyncController intentionally
                // owns queued frames and does not expose mutable front access.
                if (haveAudioTemplate && !configureAudioOutput(audioTemplate)) {
                    return -1;
                }
                haveAudioConfigured = haveAudioTemplate;
            }

            if (!sync.locked()) {
                const Receiver::AudioRoutingState audioRoutingState = receiver.getAudioRoutingState();
                const bool audioExpectedForAnchor =
                    audioRoutingState.audio_chain_ready ||
                    sawAnyAudio ||
                    haveAudioConfigured ||
                    haveAudioTemplate ||
                    sync.queuedAudio() > 0;
                const bool startupAnchorTimedOut =
                    elapsedMsAtLeast(now, anchorWaitStartedAt, config.startupAnchorTimeoutMs, 100);
                const bool videoOnlyTimeout = startupAnchorTimedOut && !audioExpectedForAnchor;

                if (startupAnchorTimedOut && audioExpectedForAnchor && !loggedAudioStartupWait) {
                    std::cout << "[PLAY-DECKLINK] startup anchor timeout reached, "
                              << "but receiver audio is expected/ready; waiting for audio overlap "
                              << "instead of starting video-only. "
                              << "audio_chain_ready=" << (audioRoutingState.audio_chain_ready ? "yes" : "no")
                              << " staged_audio=" << sync.queuedAudio()
                              << " saw_audio=" << (sawAnyAudio ? "yes" : "no")
                              << "\n";
                    loggedAudioStartupWait = true;
                }

                AvSyncController::ScheduledVideo seededVideo;
                AvSyncController::ScheduledAudio seededAudio;
                if (sync.tryLock(videoOnlyTimeout, &seededVideo, &seededAudio)) {
                    loggedAudioStartupWait = false;
                    if (!scheduleSeededVideo(seededVideo)) {
                        return -1;
                    }
                    if (!scheduleSeededAudio(seededAudio)) {
                        return -1;
                    }
                    std::cout << "[PLAY-DECKLINK] anchor established media_origin_us=" << sync.mediaOriginUs()
                              << " anchor_rule=" << (seededAudio.frame.buffer ? "first_overlap_continuous_audio" : "video_only_timeout")
                              << " seeded_audio_samples=" << seededAudio.frame.num_samples
                              << " seeded_audio_trim=" << seededAudio.trimmed_samples
                              << " output_offset_ticks=" << outputVideoOffsetTicks
                              << " audio_offset_samples=" << outputAudioOffsetSamples
                              << "\n";
                    refreshBufferedCounts();
                }
            }

            if (sync.locked()) {
                // Schedule video by media PTS translated into the DeckLink video
                // time scale. Drop only frames that are already late according to
                // the hardware playback clock.
                while (buffered_video_frames_.load(std::memory_order_acquire) < max_video_queue_frames_) {
                    AvSyncController::ScheduledVideo sv;
                    if (!sync.popVideo(sv)) {
                        break;
                    }

                    const BMDTimeValue relativeDisplayTime =
                        usToDeckLinkTicks(sv.playout_time_us, video_time_scale_);
                    BMDTimeValue displayTime = relativeDisplayTime + outputVideoOffsetTicks;

                    OutputClock clock = queryOutputClock();
                    if (clock.valid && playback_started_ && displayTime + video_frame_duration_ < clock.video_ticks) {
                        ++lateVideoDrops;

                        // Live catch-up: if no video is currently queued in the hardware,
                        // do not drop forever. Move the output timeline just ahead of the
                        // DeckLink clock and schedule this decoded frame as the new live edge.
                        if (buffered_video_frames_.load(std::memory_order_acquire) == 0) {
                            const BMDTimeValue guardTicks = std::max<BMDTimeValue>(
                                video_frame_duration_,
                                static_cast<BMDTimeValue>(video_preroll_frames_) * video_frame_duration_);
                            const BMDTimeValue targetDisplayTime = clock.video_ticks + guardTicks;
                            outputVideoOffsetTicks = targetDisplayTime - relativeDisplayTime;
                            refreshOutputAudioOffset();
                            displayTime = targetDisplayTime;
                            ++liveTimelineCatchups;
                            std::cout << "[PLAY-DECKLINK] live timeline catch-up: clock=" << clock.video_ticks
                                      << " relative=" << relativeDisplayTime
                                      << " new_display=" << displayTime
                                      << " output_offset_ticks=" << outputVideoOffsetTicks
                                      << " audio_offset_samples=" << outputAudioOffsetSamples
                                      << " late_drops=" << lateVideoDrops
                                      << "\n";
                        } else {
                            dropped_video_frames_.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                    }

                    if (!scheduleVideoFrame(sv.frame, displayTime)) {
                        break;
                    }
                    ++displayedVideo;
                    refreshBufferedCounts();
                }

                // Audio is PTS-positioned, but emitted as one continuous sample
                // stream. The horizon is derived from the video already handed to
                // DeckLink, plus one frame, so audio cannot outrun video.
                while (audio_enabled_.load(std::memory_order_acquire) &&
                       buffered_audio_samples_.load(std::memory_order_acquire) < max_audio_queue_samples_) {
                    // popAudio() expects a media-relative sample horizon, not the
                    // absolute DeckLink stream sample clock. Remove the live output
                    // offset before converting the scheduled video horizon to samples;
                    // otherwise every catch-up/output offset lets audio run farther
                    // ahead of video and destabilises receiver playout.
                    const int64_t horizonVideoTicks = next_video_time_ + video_frame_duration_;
                    const int64_t mediaHorizonVideoTicks = std::max<int64_t>(
                        0,
                        horizonVideoTicks - static_cast<int64_t>(outputVideoOffsetTicks));
                    const int64_t horizonSample = deckLinkTicksToAudioSamples(mediaHorizonVideoTicks,
                                                                                  video_time_scale_,
                                                                                  current_audio_sample_rate_);

                    AvSyncController::ScheduledAudio sa;
                    if (!sync.popAudio(horizonSample,
                                       current_audio_sample_rate_,
                                       current_audio_channels_,
                                       current_audio_bps_,
                                       sa)) {
                        break;
                    }

                    if (!scheduleAudioFrameChain(sa.frame, static_cast<BMDTimeValue>(sa.playout_sample + outputAudioOffsetSamples))) {
                        return -1;
                    }
                    ++playedAudioFrames;
                    playedAudioSamples += static_cast<uint64_t>(std::max(sa.frame.num_samples, 0));
                    refreshBufferedCounts();
                }

                if (allowFallback && haveVideoTemplate && sync.queuedVideo() == 0 &&
                    buffered_video_frames_.load(std::memory_order_acquire) < max_video_queue_frames_) {
                    VideoFrame black = makeBlackFrame(videoTemplate);
                    if (black.buffer && scheduleVideoFrame(black, next_video_time_)) {
                        ++displayedVideo;
                        ++fallbackVideoFrames;
                        refreshBufferedCounts();
                    }
                }

                if (allowFallback && audio_enabled_.load(std::memory_order_acquire) && haveAudioTemplate &&
                    sync.queuedAudio() == 0 &&
                    buffered_audio_samples_.load(std::memory_order_acquire) < cadenceSamplesForPreroll(2)) {
                    const int64_t frameIndex = (video_frame_duration_ > 0) ? (next_video_time_ / video_frame_duration_) : 0;
                    AudioFrame silence = makeSilence(current_audio_sample_rate_,
                                                     current_audio_channels_,
                                                     current_audio_bps_,
                                                     static_cast<int>(cadenceSamplesForFrameIndex(frameIndex)));
                    if (silence.buffer && silence.num_samples > 0) {
                        if (!scheduleAudioFrameChain(silence, next_audio_time_)) {
                            return -1;
                        }
                        ++playedAudioFrames;
                        ++fallbackAudioFrames;
                        playedAudioSamples += static_cast<uint64_t>(std::max(silence.num_samples, 0));
                        refreshBufferedCounts();
                    }
                }

                if (!startPlaybackIfReady(audio_enabled_.load(std::memory_order_acquire))) {
                    return -1;
                }
            }
        }

        if (!sourceLossActive && elapsedMsAtLeast(now, lastMediaAt, config.sourceLossThresholdMs, 100)) {
            sourceLossActive = true;
            std::cout << "[PLAY-DECKLINK] source gap detected, fallback enabled.\n";
        }

        if (elapsedMsAtLeast(now, lastLog, config.logStatusIntervalMs, 250)) {
            const OutputClock c = queryOutputClock();
            const bool udpStatsAvailable = receiver.isUdpTransport();
            const bool rtpStatsAvailable = receiver.isRtpTransport();
            UDPInput::Diagnostics udpStats;
            if (udpStatsAvailable) {
                udpStats = receiver.udpDiagnostics();
            }
            const int64_t scheduledAudioUs = (current_audio_sample_rate_ > 0)
                ? AvSyncController::samplesToUs(next_audio_time_, current_audio_sample_rate_)
                : 0;
            const int64_t scheduledVideoUs = (video_time_scale_ > 0)
                ? av_rescale_q(next_video_time_, AVRational{1, static_cast<int>(video_time_scale_)}, AVRational{1, 1000000})
                : 0;
            std::cout << "[PLAY-DECKLINK] state=" << (sync.locked() ? "timeline_locked" : "waiting_anchor")
                      << " vQ=" << buffered_video_frames_.load(std::memory_order_acquire)
                      << " aQ=" << buffered_audio_samples_.load(std::memory_order_acquire)
                      << " staged_video=" << sync.queuedVideo()
                      << " staged_audio=" << sync.queuedAudio()
                      << " displayed=" << displayedVideo
                      << " played_af=" << playedAudioFrames
                      << " played_as=" << playedAudioSamples
                      << " media_origin_us=" << sync.mediaOriginUs()
                      << " media_clock=" << nxframe::receiver_clock_policy::mediaClockName()
                      << " output_clock=" << nxframe::receiver_clock_policy::outputClockName()
                      << " av_sched_ms=" << (scheduledAudioUs - scheduledVideoUs) / 1000.0
                      << " silence_samp=" << sync.silenceInsertedSamples()
                      << " trim_samp=" << sync.audioTrimmedSamples()
                      << " late_v_drop=" << lateVideoDrops
                      << " live_catchup=" << liveTimelineCatchups
                      << " out_off_v=" << outputVideoOffsetTicks
                      << " out_off_a=" << outputAudioOffsetSamples
                      << " fallback_v=" << fallbackVideoFrames
                      << " fallback_a=" << fallbackAudioFrames
                      << " dropped_v=" << dropped_video_frames_.load(std::memory_order_acquire)
                      << " dropped_a=" << dropped_audio_frames_.load(std::memory_order_acquire)
                      << " anchor_drop_v=" << sync.anchorDroppedVideo()
                      << " anchor_drop_a=" << sync.anchorDroppedAudio()
                      << " dup_v=" << sync.duplicateVideoDropped()
                      << " dup_a=" << sync.duplicateAudioDropped()
                      << " hard_resync_total=" << hardResyncEvents
                      << " rx_soft_loss=" << receiver.softTransportLossCount()
                      << " rx_hard_loss=" << receiver.hardTransportLossCount();
            if (udpStatsAvailable) {
                std::cout << " udp_drop=" << udpStats.dropped_packets
                          << " udp_rcvbuf=" << udpStats.actual_rcvbuf
                          << " udp_ts_cc=" << udpStats.ts_continuity_errors
                          << " udp_ts_sync=" << udpStats.ts_sync_errors;
                if (rtpStatsAvailable) {
                    std::cout << " rtp_gap=" << udpStats.rtp_sequence_gaps
                              << " rtp_ooo=" << udpStats.rtp_out_of_order
                              << " rtp_dup=" << udpStats.rtp_duplicates
                              << " rtp_bad=" << udpStats.rtp_malformed
                              << " rtp_srcchg=" << udpStats.rtp_source_changes;
                }
            }
            std::cout << " rx_queue_av_delta_ms=";
            if (receiver.hasReceiverQueueAvDelta()) {
                std::cout << receiver.receiverQueueAvDeltaMs();
            } else {
                std::cout << "na";
            }
            std::cout << " clk_v=" << (c.valid ? c.video_ticks : -1)
                      << " clk_a=" << (c.valid ? c.audio_samples : -1)
                      << " next_v=" << next_video_time_
                      << " next_a=" << next_audio_time_
                      << " ref=" << getReferenceStatusString()
                      << "\n";
            lastLog = now;
        }

        if (!progressed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    return 0;
}
