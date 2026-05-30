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
 * HEVC encoder declarations. EncoderX265 provides an HEVC-compatible encoder path using FFmpeg/libx265 while keeping the same manager-facing API used by the sender pipeline.
 */

#ifndef ENCODER_X265_H
#define ENCODER_X265_H

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../core/packet_item.h"
#include "../core/frame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

using json = nlohmann::json;

// HEVC encoder with the same public shape as EncoderX264. Unlike the x264
// path, this implementation intentionally copies into encoder-owned frames.
class EncoderX265 {
public:
    explicit EncoderX265(const json& presetJson);
    ~EncoderX265();

    bool initialize();

    // NxFrame keeps a zero-copy-oriented call shape, but for libx265 this path
    // intentionally copies into encoder-owned frames to avoid retaining caller
    // buffers across lookahead / reference buffering.
    std::vector<AVPacketPtr> encodeFrameZeroCopyPackets(const std::shared_ptr<uint8_t>& inputBuf,
                                                        size_t inputBytes,
                                                        int64_t pts);

    std::vector<AVPacketPtr> encodeVideoFramePackets(const VideoFrame& vf);

    AVPacketPtr encodeFrameZeroCopy(const std::shared_ptr<uint8_t>& inputBuf,
                                    size_t inputBytes,
                                    int64_t pts);

    std::vector<AVPacketPtr> encodeFramePackets(uint8_t* inputFrame, int64_t pts);

    AVPacketPtr encodeFrame(uint8_t* inputFrame, int64_t pts);

    std::vector<AVPacketPtr> flush();

    uint8_t* getBlackFrame() const;
    AVCodecContext* getCodecContext() const;
    AVPacketPtr acquirePacket();
    void requestKeyFrame();

private:
    bool parsePreset(const json& presetJson);
    bool configureCodecContext(const AVCodec* codec);
    bool validateRequestedPixelFormat(const AVCodec* codec) const;
    bool allocateWorkingFrames();
    void allocateBlackFrame();

    bool fillFramePointersForContiguousInternalBus(AVFrame* f, uint8_t* base) const;
    bool copyIntoInputFrame(AVFrame* src, int64_t pts, bool forceKeyframe, AVFrame** out);
    void applyVideoFrameMetadata(AVFrame* dst, const VideoFrame& src) const;
    bool attachHdrSideData(AVFrame* dst, const VideoFrame& src) const;
    bool ensureConvertedFrame();
    bool copyColorMetadata(AVFrame* dst, const AVFrame* src) const;
    bool submitFrame(AVFrame* in);
    bool drainPackets();
    AVPacketPtr popPendingPacket();
    void appendPendingPacket(AVPacketPtr pkt);
    std::vector<AVPacketPtr> collectAllPendingPackets();

private:
    // FFmpeg codec state and working frames. copy_input_frame_ owns the pixels
    // submitted to x265 so lookahead/reference buffering cannot outlive caller memory.
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* copy_input_frame_ = nullptr;   // encoder-owned input-format frame
    AVFrame* zc_input_frame_ = nullptr;     // wrapper around caller buffer
    AVFrame* converted_frame_ = nullptr;    // optional output-format frame
    SwsContext* sws_ctx_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int bitrate_ = 0;         // bps
    int framerate_ = 0;
    int gop_size_ = 0;
    int keyint_min_ = 0;
    int max_b_frames_ = 0;
    int crf_ = -1;
    int vbv_maxrate_ = 0;     // bps
    int vbv_bufsize_ = 0;     // bps
    int thread_count_ = 0;
    int rc_lookahead_ = -1;
    int slices_ = -1;
    int refs_ = -1;
    int level_idc_ = 0;

    bool interlaced_ = false;
    bool top_field_first_ = true;
    bool closed_gop_ = false;

    AVPixelFormat input_fmt_ = AV_PIX_FMT_YUV422P10LE;
    AVPixelFormat output_fmt_ = AV_PIX_FMT_YUV422P10LE;
    int output_bit_depth_ = 10;
    std::string output_chroma_ = "422";

    std::string preset_ = "medium";
    std::string tune_;
    std::string profile_;
    std::string rate_control_ = "cbr";

    json additional_options_;

    AVColorPrimaries color_primaries_ = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic color_trc_ = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace colorspace_ = AVCOL_SPC_UNSPECIFIED;
    AVColorRange color_range_ = AVCOL_RANGE_UNSPECIFIED;
    AVChromaLocation chroma_location_ = AVCHROMA_LOC_UNSPECIFIED;

    bool has_mastering_display_ = false;
    AVMasteringDisplayMetadata mastering_display_{};
    bool has_content_light_ = false;
    AVContentLightMetadata content_light_{};
    std::string x265_master_display_;
    std::string x265_max_cll_;

    uint8_t* blackFrameYUV_ = nullptr;      // internal-bus YUV422P10LE fallback

    int64_t frame_counter_ = 0;
    std::atomic<bool> force_next_keyframe_{false};

    std::atomic<int> fps_frame_count_{0};
    AVPacketPool packetPool_{32};
    std::deque<AVPacketPtr> pending_packets_;
    std::chrono::time_point<std::chrono::high_resolution_clock> fps_start_time_;
};

#endif // ENCODER_X265_H