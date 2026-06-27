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
 * DeckLink SDI output declarations. The class owns DeckLink output device setup, callback handling, playout configuration, frame pooling, SDI audio scheduling, and receiver-to-SDI lifecycle control.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "DeckLinkAPI.h"
#include "DeckLinkAPIConfiguration.h"
#include "output/sdi_audio_cadence.h"
#include "receiver/receiver.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

// Receiver SDI playout policy. These presets trade startup buffer depth
// against output stability and end-to-end latency.
enum class DeckLinkLatencyPolicy {
    Safe,
    Normal,
    LowLatency
};

// Runtime SDI playout tuning loaded from presets. Keep these values at the
// output-manager boundary so DeckLinkOutput can remain focused on scheduling.
struct DeckLinkPlayoutConfig {
    DeckLinkLatencyPolicy latencyPolicy = DeckLinkLatencyPolicy::Normal;

    uint32_t videoPrerollFrames   = 4;
    uint32_t maxVideoQueueFrames  = 8;
    uint32_t maxAudioQueueSamples = 48000;

    int startupAnchorTimeoutMs    = 750;
    int sourceLossThresholdMs     = 250;
    int blackFallbackThresholdMs  = 200;
    int logStatusIntervalMs       = 5000;
};

// Owns the receiver-to-SDI output path: device setup, frame pool, callback
// tracking, v210 packing, embedded audio scheduling, and playout recovery.
class DeckLinkOutput {
public:
    DeckLinkOutput();
    ~DeckLinkOutput();

    DeckLinkOutput(const DeckLinkOutput&) = delete;
    DeckLinkOutput& operator=(const DeckLinkOutput&) = delete;

    bool init(int deviceIndex);
    void stop();

    int runPlayout(Receiver& receiver,
                   const std::atomic<bool>& stopFlag,
                   const DeckLinkPlayoutConfig& config);

    std::string getLastError() const { return last_error_; }
    std::string getDeviceName() const { return device_name_; }
    bool isInitialized() const { return initialized_.load(std::memory_order_acquire); }
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    uint64_t droppedVideoFrames() const { return dropped_video_frames_.load(std::memory_order_acquire); }
    uint64_t droppedAudioFrames() const { return dropped_audio_frames_.load(std::memory_order_acquire); }

    void onScheduledFrameCallbackBegin();
    void onScheduledFrameCompleted(IDeckLinkVideoFrame* frame);
    void onScheduledFrameCallbackEnd();
    void onScheduledFrameCompletionWarning();
    void onScheduledPlaybackStopped();

private:
    // DeckLink output frame plus its backing buffer. The frame remains in use
    // until the SDK completion callback releases it back to the pool.
    struct PooledFrame {
        IDeckLinkMutableVideoFrame* frame = nullptr;
        IDeckLinkVideoBuffer* buffer = nullptr;
        bool in_use = false;
        uint64_t generation = 0;
    };

    struct OutputClock {
        bool valid = false;
        BMDTimeValue video_ticks = 0;
        BMDTimeValue audio_samples = 0;
        double speed = 0.0;
    };

    bool configureVideoOutput(const VideoFrame& frame);
    bool configureAudioOutput(const AudioFrame& frame);
    bool configureDeckLinkColorOutput(const VideoFrame& frame);
    uint32_t cadenceSamplesForFrameIndex(int64_t frameIndex) const;
    uint32_t cadenceSamplesForPreroll(uint32_t frameCount) const;
    bool resolveFrameTiming(const VideoFrame& frame);
    BMDDisplayMode resolveDisplayMode(const VideoFrame& frame) const;
    bool validateVideoFrame(const VideoFrame& frame) const;
    bool validateAudioFrame(const AudioFrame& frame) const;

    bool convertYUV422P10ToV210(const VideoFrame& frame,
                                uint8_t* dst,
                                int dstRowBytes);

    IDeckLinkMutableVideoFrame* obtainPooledFrame();
    IDeckLinkVideoBuffer* bufferForPooledFrame(IDeckLinkVideoFrame* frame);
    uint64_t framePoolGenerationForFrame(IDeckLinkVideoFrame* frame) const;
    void markPooledFrameFree(IDeckLinkVideoFrame* frame);
    void releasePooledFrame(IDeckLinkMutableVideoFrame* frame);
    void releaseAllPooledFrames();
    void quarantineFramePoolAfterDrainTimeout();
    bool waitForScheduledCallbacksDrained(uint32_t timeoutMs);

    bool scheduleVideoFrame(const VideoFrame& source, BMDTimeValue displayTime);
    bool scheduleAudioSamples(const AudioFrame& source, BMDTimeValue streamTime, AudioFrame* leftoverOut = nullptr);
    bool scheduleAudioFrameChain(const AudioFrame& source, BMDTimeValue startStreamTime);
    bool startPlaybackIfReady(bool require_audio_preroll);
    bool resetHardwarePipeline(bool restartPreroll);

    OutputClock queryOutputClock() const;
    bool updateReferenceStatus();
    std::string getReferenceStatusString() const;
    void refreshBufferedCounts();
    void setError(const std::string& msg);

private:
    IDeckLink* decklink_ = nullptr;
    IDeckLinkOutput* decklink_output_ = nullptr;
    IDeckLinkConfiguration* decklink_config_ = nullptr;
    IDeckLinkVideoOutputCallback* callback_ = nullptr;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> audio_enabled_{false};

    std::string device_name_;
    std::string last_error_;

    BMDDisplayMode current_mode_ = bmdModeUnknown;
    BMDPixelFormat current_pixel_format_ = bmdFormat10BitYUV;
    int current_width_ = 0;
    int current_height_ = 0;
    bool current_interlaced_ = false;
    BMDColorspace current_output_colorspace_ = bmdColorspaceRec709;
    BMDDynamicRange current_output_dynamic_range_ = bmdDynamicRangeSDR;

    int current_audio_sample_rate_ = 0;
    int current_audio_channels_ = 0;
    int current_audio_bps_ = 0;

    BMDTimeScale video_time_scale_ = 0;
    BMDTimeValue video_frame_duration_ = 0;

    uint32_t video_preroll_frames_ = 4;
    uint32_t max_video_queue_frames_ = 8;
    uint32_t max_audio_queue_samples_ = 48000;
    uint32_t audio_preroll_target_samples_ = 0;

    mutable std::mutex hw_mutex_;
    mutable std::mutex playback_stop_mutex_;
    std::condition_variable playback_stop_cv_;

    BMDTimeValue next_video_time_ = 0;
    BMDTimeValue next_audio_time_ = 0;
    bool audio_preroll_active_ = false;
    bool playback_started_ = false;
    bool expect_audio_at_start_ = false;

    std::vector<PooledFrame> frame_pool_;
    std::atomic<uint64_t> frame_pool_generation_{1};
    SdiAudioCadence cadence_;

    std::atomic<uint32_t> scheduled_video_frames_{0};
    std::atomic<uint32_t> active_frame_callbacks_{0};
    std::atomic<uint32_t> buffered_video_frames_{0};
    std::atomic<uint32_t> buffered_audio_samples_{0};

    std::atomic<uint64_t> dropped_video_frames_{0};
    std::atomic<uint64_t> dropped_audio_frames_{0};
    std::atomic<uint64_t> schedule_failures_{0};
    std::atomic<uint64_t> completion_warnings_{0};
    std::atomic<bool> playback_stop_notified_{false};

    bool reference_supported_ = true;
    bool reference_locked_ = false;
};