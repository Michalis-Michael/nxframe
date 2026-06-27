/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: playout/av_sync_controller.h
 * Description: Declares the receiver A/V sync scheduler and media-clock policy interface.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <limits>

#include "core/frame.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

// Receiver-side media scheduler. It aligns decoded video and audio by media
// PTS and produces a playout decision for the SDI output layer.
class AvSyncController
{
public:
    struct VideoItem {
        VideoFrame frame;
        int64_t pts_us = invalidTime();
    };

    struct AudioItem {
        AudioFrame frame;
        int64_t pts_us = invalidTime();
        int64_t end_us = invalidTime();
    };

    struct ScheduledVideo {
        VideoFrame frame;
        int64_t media_pts_us = invalidTime();
        int64_t playout_time_us = 0;
    };

    struct ScheduledAudio {
        AudioFrame frame;
        int64_t media_pts_us = invalidTime();
        int64_t playout_sample = 0;
        int64_t inserted_silence_samples = 0;
        int64_t trimmed_samples = 0;
        bool silence = false;
    };

    static int64_t invalidTime()
    {
        return std::numeric_limits<int64_t>::min();
    }

    static int64_t ptsToUs(int64_t pts, AVRational tb);
    static int64_t audioEndUs(const AudioFrame& frame, int64_t pts_us);
    static int64_t usToSamples(int64_t us, int sampleRate);
    static int64_t samplesToUs(int64_t samples, int sampleRate);

    void reset();

    bool pushVideo(VideoFrame&& frame, int64_t pts_us);
    bool pushAudio(AudioFrame&& frame, int64_t pts_us, int64_t end_us);

    bool locked() const noexcept { return locked_; }
    int64_t mediaOriginUs() const noexcept { return media_origin_us_; }
    int64_t nextAudioSample() const noexcept { return next_audio_sample_; }
    int64_t lastVideoPtsUs() const noexcept { return last_video_pts_us_; }
    int64_t lastAudioPtsUs() const noexcept { return last_audio_pts_us_; }
    int64_t lastScheduledVideoUs() const noexcept { return last_video_playout_us_; }
    int64_t lastScheduledAudioUs() const noexcept { return last_audio_playout_us_; }

    size_t queuedVideo() const noexcept { return video_q_.size(); }
    size_t queuedAudio() const noexcept { return audio_q_.size(); }

    uint64_t anchorDroppedVideo() const noexcept { return anchor_drop_video_; }
    uint64_t anchorDroppedAudio() const noexcept { return anchor_drop_audio_; }
    uint64_t duplicateVideoDropped() const noexcept { return duplicate_video_drop_; }
    uint64_t duplicateAudioDropped() const noexcept { return duplicate_audio_drop_; }
    uint64_t silenceInsertedSamples() const noexcept { return silence_inserted_samples_; }
    uint64_t audioTrimmedSamples() const noexcept { return audio_trimmed_samples_; }

    // Locks the media timeline to the first video frame whose PTS is covered by
    // a decoded audio block. If audio is absent and allowVideoOnly is true, the
    // first video frame becomes the origin.
    bool tryLock(bool allowVideoOnly, ScheduledVideo* seededVideo, ScheduledAudio* seededAudio);

    bool popVideo(ScheduledVideo& out);

    // Produces continuous SDI audio. PTS chooses the desired placement; the
    // controller inserts silence for positive gaps and trims overlaps. The
    // caller supplies an output horizon in audio samples so audio cannot run too
    // far ahead of scheduled video.
    bool popAudio(int64_t horizon_sample, int sampleRate, int channels, int bytesPerSample, ScheduledAudio& out);

private:
    static bool trimAudioFront(AudioFrame& frame, int64_t samplesToTrim);
    static AudioFrame makeSilence(int sampleRate, int channels, int bytesPerSample, int numSamples);

    std::deque<VideoItem> video_q_;
    std::deque<AudioItem> audio_q_;

    bool locked_ = false;
    int64_t media_origin_us_ = invalidTime();
    int64_t next_audio_sample_ = 0;
    int64_t last_video_pts_us_ = invalidTime();
    int64_t last_audio_pts_us_ = invalidTime();
    int64_t last_video_playout_us_ = invalidTime();
    int64_t last_audio_playout_us_ = invalidTime();

    uint64_t anchor_drop_video_ = 0;
    uint64_t anchor_drop_audio_ = 0;
    uint64_t duplicate_video_drop_ = 0;
    uint64_t duplicate_audio_drop_ = 0;
    uint64_t silence_inserted_samples_ = 0;
    uint64_t audio_trimmed_samples_ = 0;
};
