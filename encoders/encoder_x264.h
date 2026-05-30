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
 * H.264 encoder declarations. EncoderX264 provides the primary low-latency broadcast contribution path using FFmpeg/libx264 and the NxFrame YUV422P10LE internal video bus.
 */

#ifndef ENCODER_X264_H
#define ENCODER_X264_H

#include <nlohmann/json.hpp>

#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>
#include <memory>
#include <vector>

#include "../core/packet_item.h"
#include "../core/frame.h"

struct SwsContext;
struct X264RuntimeState;

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
}

using json = nlohmann::json;

class EncoderX264 {
public:
    explicit EncoderX264(const json& presetJson);
    ~EncoderX264();

    bool initialize();

    // Preferred zero-copy-oriented video path. The input buffer is wrapped by
    // AVBufferRef so FFmpeg keeps the shared owner alive while x264 consumes it.
    std::vector<AVPacketPtr> encodeFrameZeroCopyPackets(const std::shared_ptr<uint8_t>& inputBuf,
                                                        size_t inputBytes,
                                                        int64_t pts);

    std::vector<AVPacketPtr> encodeVideoFramePackets(const VideoFrame& vf);

    AVPacketPtr encodeFrameZeroCopy(const std::shared_ptr<uint8_t>& inputBuf,
                                    size_t inputBytes,
                                    int64_t pts);

    // Legacy copy fallback. Keep for compatibility and tests; the sender should
    // normally use encodeVideoFramePackets().
    std::vector<AVPacketPtr> encodeFramePackets(uint8_t* inputFrame, int64_t pts);

    AVPacketPtr encodeFrame(uint8_t* inputFrame, int64_t pts);

    uint8_t* getBlackFrame() const;
    AVCodecContext* getCodecContext() const;
    AVPacketPtr acquirePacket();
    void requestKeyFrame();

    std::vector<AVPacketPtr> flush();

private:
    void startFpsTracking();
    void allocateBlackFrame();
    bool validateRequestedPixelFormat(const AVCodec* codec) const;
    bool validateEncoderConfiguration(const std::string& rateControl,
                                      int keyint,
                                      int minKeyint) const;
    void logEffectiveEncoderConfiguration(const std::string& rateControl,
                                          const std::string& x264Params) const;
    bool prepareConvertedFrame(AVFrame* srcFrame, AVFrame** outFrame);
    bool sendFrameAndReceivePackets(AVFrame* inFrame, std::vector<AVPacketPtr>& out);
    bool receiveAvailablePackets(std::vector<AVPacketPtr>& out);
    void applyVideoFrameMetadata(AVFrame* dst, const VideoFrame& src) const;
    bool attachHdrSideData(AVFrame* dst, const VideoFrame& src) const;

    // FFmpeg codec state. zc_frame is only a wrapper; it does not own input
    // image memory unless an AVBufferRef owner is attached for the submitted frame.
    AVCodecContext* codec_ctx = nullptr;
    AVFrame*        frame     = nullptr; // copy path frame
    AVFrame*        zc_frame  = nullptr; // zero-copy wrapper frame

    // Preset values
    int width = 0;
    int height = 0;
    int bitrate = 0;         // bps
    int framerate = 0;
    int gop_size = 0;
    int max_b_frames = 0;
    int crf = 0;
    int vbv_maxrate = 0;      // bps
    int vbv_bufsize = 0;      // bps

    std::string preset;
    std::string tune;
    std::string profile;

    json additional_options;

    // Fallback black frame in the internal planar YUV422P10LE bus format.
    uint8_t* blackFrameYUV = nullptr;

    // PTS/keyframe tracking shared by zero-copy and fallback paths.
    int64_t frame_counter = 0;
    std::atomic<bool> force_next_keyframe_{false};

    // FPS Tracking
    std::atomic<int> fps_frame_count{0};
    AVPacketPool packetPool{32};
    std::unique_ptr<X264RuntimeState> runtime_;
    std::chrono::time_point<std::chrono::high_resolution_clock> fps_start_time;
};

#endif // ENCODER_X264_H