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
 * Encoder manager declarations. EncoderManager is the sender-facing codec coordinator that owns video/audio encoder instances and hides codec-specific details from the application pipeline.
 */

#ifndef ENCODER_MANAGER_H
#define ENCODER_MANAGER_H

#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "../core/packet_item.h"
#include "../core/frame.h"
#include "encoder_x264.h"
#include "encoder_x265.h"
#include "encoder_aac.h"
#include "encoder_pcm.h"

using json = nlohmann::json;

// Preset-driven facade used by the sender pipeline. It owns exactly one video
// encoder type and zero or more audio legs selected from the JSON preset.
class EncoderManager {
public:
    static std::unique_ptr<EncoderManager> createEncoder(const std::string& presetPath);

    explicit EncoderManager(const json& presetJson);
    ~EncoderManager() = default;

    EncoderManager(const EncoderManager&) = delete;
    EncoderManager& operator=(const EncoderManager&) = delete;

    bool initialize();
    bool initializeAudio();

    // Video entry points. The VideoFrame overload is preferred because it carries
    // buffer ownership, timing, colorimetry, and HDR metadata.
    std::vector<AVPacketPtr> encodeFrameZeroCopyPackets(const std::shared_ptr<uint8_t>& inputBuf,
                                                        size_t inputBytes,
                                                        int64_t pts);

    std::vector<AVPacketPtr> encodeVideoFramePackets(const VideoFrame& vf);

    AVPacketPtr encodeFrameZeroCopy(const std::shared_ptr<uint8_t>& inputBuf,
                                    size_t inputBytes,
                                    int64_t pts);

    std::vector<AVPacketPtr> encodeFramePackets(uint8_t* inputFrame, int64_t pts);

    AVPacketPtr encodeFrame(uint8_t* inputFrame, int64_t pts);

    // Audio entry points. Audio codecs may buffer internally until a complete
    // codec/transport packet is available.
    AVPacket* encodeAudioFrame(const AudioFrame& frame);
    AVPacket* drainAudioPacket();

    std::vector<AVPacket*> flushAudio();
    std::vector<AVPacketPtr> flushVideo();
    bool audioOutputMayBuffer() const;

    void requestVideoKeyFrame();
    uint8_t* getBlackFrame() const;
    AVCodecContext* getCodecContext() const;
    AVCodecContext* getVideoCodecContext() const;
    AVCodecContext* getAudioCodecContext() const;
    std::vector<AVCodecContext*> getAudioCodecContexts() const;

private:
    // One configured audio output leg. A leg is AAC or PCM/ST 302M, never both.
    struct AudioLeg {
        std::unique_ptr<EncoderAAC> aac;
        std::unique_ptr<EncoderPCM> pcm;
    };

    void enqueueAudioPacket(AVPacket* pkt, size_t legIndex);
    AVPacket* popPendingAudioPacket();

    std::unique_ptr<EncoderX264> encoderX264;
    std::unique_ptr<EncoderX265> encoderX265;
    std::vector<AudioLeg> audioLegs;
    std::deque<AVPacket*> pendingAudioPackets;

    std::string codecType;
    int width = 0;
    int height = 0;
    int framerate = 0;
};

#endif // ENCODER_MANAGER_H