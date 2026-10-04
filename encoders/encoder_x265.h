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
#include <limits>
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

// HEVC encoder with the same public shape as EncoderX264. Encode/flush calls
// are serialized by the owner; only requestKeyFrame is safe across threads.
class EncoderX265 {
public:
    explicit EncoderX265(const json& presetJson);
    ~EncoderX265();

    bool initialize();

    // Shared input is retained through AVBufferRef; callers must not mutate it
    // until all references are released. Conversion writes directly to output.
    std::vector<AVPacketPtr> encodeFrameZeroCopyPackets(const std::shared_ptr<uint8_t>& inputBuf,
                                                        size_t inputBytes,
                                                        int64_t pts);

    std::vector<AVPacketPtr> encodeVideoFramePackets(const VideoFrame& vf);
    // Clears prior output and reuses its capacity. Calls remain serialized.
    void encodeVideoFramePackets(const VideoFrame& vf, std::vector<AVPacketPtr>& out);

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
    bool prepareInputFrame(AVFrame* src, int64_t pts, bool forceKeyframe, AVFrame** out);
    bool applyVideoFrameMetadata(AVFrame* dst, const VideoFrame& src) const;
    bool attachHdrSideData(AVFrame* dst, const VideoFrame& src) const;
    bool ensureConvertedFrame();
    bool copyColorMetadata(AVFrame* dst, const AVFrame* src) const;
    bool submitFrame(AVFrame* in);
    bool drainPackets();
    void reportPipelineDiagnostics(int64_t submittedPts,
                                   const std::vector<AVPacketPtr>& out);
    AVPacketPtr popPendingPacket();
    void appendPendingPacket(AVPacketPtr pkt);
    std::vector<AVPacketPtr> collectAllPendingPackets();
    void collectAllPendingPackets(std::vector<AVPacketPtr>& out);

private:
    // Working buffers: shared inputs are ref-counted; raw compatibility inputs
    // are copied only when no format conversion is required.
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* copy_input_frame_ = nullptr;   // encoder-owned input-format frame
    AVFrame* zc_input_frame_ = nullptr;     // wrapper around caller buffer
    AVPacket* receive_packet_ = nullptr;   // reusable receive scratch, not shared with output
    AVFrame* converted_frame_ = nullptr;    // optional output-format frame
    SwsContext* sws_ctx_ = nullptr;

    bool preset_valid_ = false;
    bool initialized_ = false;
    bool flushed_ = false;
    bool failed_ = false;

    size_t input_bytes_ = 0;
    int width_ = 0;
    int height_ = 0;
    int bitrate_ = 0;         // bps
    int framerate_ = 0;
    int gop_size_ = 0;
    int keyint_min_ = 0;
    int max_b_frames_ = 0;
    double crf_ = -1;
    int vbv_maxrate_ = 0;     // bps
    int vbv_bufsize_ = 0;     // bits
    int thread_count_ = 0;
    int rc_lookahead_ = -1;
    int slices_ = -1;
    int refs_ = -1;
    int level_idc_ = 0;

    bool interlaced_ = false;
    bool closed_gop_ = false;
    // Legacy preset flag retained only so old presets fail soft while migrating.
    // It no longer changes x265 threading/lookahead or enforces same-call output.
    bool legacy_single_frame_requested_ = false;

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

    // Diagnostic association: the first packet drained after a successful
    // non-flush send. Its PTS can differ from the submitted PTS due to buffering.
    bool diagnosticSubmission_=false;
    uint64_t diagnosticSubmitWallNs_=0;
    int64_t diagnosticSubmitCpuNs_=-1;
    int64_t diagnosticSubmittedPts_=AV_NOPTS_VALUE;

    // Same diagnostic model used by EncoderX264: measure the codec's actual
    // submitted-PTS to first-output-PTS depth without constraining it. For HEVC
    // a deeper fixed pipeline is acceptable; stability/cadence is the target.
    uint64_t pipeline_diag_submissions_ = 0;
    uint64_t pipeline_diag_with_output_ = 0;
    uint64_t pipeline_diag_no_output_ = 0;
    uint64_t pipeline_diag_packets_ = 0;
    int64_t pipeline_diag_lag_sum_ = 0;
    int64_t pipeline_diag_lag_min_ = std::numeric_limits<int64_t>::max();
    int64_t pipeline_diag_lag_max_ = std::numeric_limits<int64_t>::min();
    int64_t pipeline_diag_last_lag_ = 0;
    bool pipeline_diag_have_last_lag_ = false;
    uint64_t pipeline_diag_lag_changes_ = 0;
    int64_t pipeline_diag_latest_submitted_pts_ = AV_NOPTS_VALUE;
    int64_t pipeline_diag_latest_output_pts_ = AV_NOPTS_VALUE;
    std::chrono::steady_clock::time_point pipeline_diag_last_report_{};

    AVPacketPool packetPool_{32};
    std::deque<AVPacketPtr> pending_packets_;
};

#endif // ENCODER_X265_H
