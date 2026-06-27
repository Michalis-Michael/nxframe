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
 * SDI audio cadence helper. This header-only helper calculates the number of 48 kHz audio samples that should be scheduled for each video frame rate, including fractional NTSC-style cadence patterns.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

extern "C" {
#include <libavutil/rational.h>
#include <libavutil/avutil.h>
}

#include "core/frame.h"

// Calculates 48 kHz SDI audio sample cadence per video frame. Fractional
// frame rates use repeating patterns so long-term audio/video alignment remains
// exact enough for DeckLink scheduled playback.
class SdiAudioCadence
{
public:
    bool configure(const VideoFrame& frame)
    {
        AVRational fps = frame.nominal_frame_rate;
        if ((fps.num <= 0 || fps.den <= 0) && frame.time_base.num > 0 && frame.time_base.den > 0) {
            fps = AVRational{frame.time_base.den, frame.time_base.num};
        }
        return configure(fps, frame.interlaced, frame.width, frame.height);
    }

    bool configure(const AVRational& nominalFrameRate, bool interlaced, int width, int height)
    {
        return configureCommon(width, height, interlaced, nominalFrameRate);
    }

    bool isConfigured() const noexcept { return configured_; }
    AVRational frameTimeBase() const noexcept { return frame_time_base_; }
    AVRational nominalFrameRate() const noexcept { return nominal_frame_rate_; }

    // Number of audio samples to schedule for the requested video frame.
    int samplesForFrame(int64_t frameIndex) const
    {
        if (!configured_ || pattern_.empty()) {
            return default_samples_per_frame_;
        }
        const int64_t idx = frameIndex >= 0 ? frameIndex : 0;
        return pattern_[static_cast<size_t>(idx % static_cast<int64_t>(pattern_.size()))];
    }

    // Cumulative audio sample position at the beginning of a video frame.
    int64_t sampleStartForFrame(int64_t frameIndex) const
    {
        if (!configured_ || frameIndex <= 0 || pattern_.empty()) {
            return 0;
        }

        const int64_t cycle = static_cast<int64_t>(pattern_.size());
        int64_t total = 0;
        const int64_t fullCycles = frameIndex / cycle;
        int64_t cycleSum = 0;
        for (size_t i = 0; i < pattern_.size(); ++i) {
            cycleSum += pattern_[i];
        }
        total += fullCycles * cycleSum;
        const int64_t remainder = frameIndex % cycle;
        for (int64_t i = 0; i < remainder; ++i) {
            total += pattern_[static_cast<size_t>(i)];
        }
        return total;
    }

    int defaultSamplesPerFrame() const noexcept { return default_samples_per_frame_; }

private:
    static bool rateEquals(const AVRational& a, int num, int den)
    {
        if (a.num <= 0 || a.den <= 0) {
            return false;
        }
        return av_cmp_q(a, AVRational{num, den}) == 0;
    }

    bool configureCommon(int width, int height, bool interlaced, const AVRational& fps)
    {
        configured_ = false;
        pattern_.clear();

        if (fps.num <= 0 || fps.den <= 0 || width <= 0 || height <= 0) {
            return false;
        }

        nominal_frame_rate_ = fps;
        frame_time_base_ = AVRational{fps.den, fps.num};

        if (rateEquals(fps, 25, 1)) {
            pattern_ = {1920};
        } else if (rateEquals(fps, 50, 1)) {
            pattern_ = {960};
        } else if (rateEquals(fps, 30, 1)) {
            pattern_ = {1600};
        } else if (rateEquals(fps, 30000, 1001)) {
            pattern_ = {1602, 1601, 1602, 1601, 1602};
        } else if (rateEquals(fps, 24, 1)) {
            pattern_ = {2000};
        } else if (rateEquals(fps, 24000, 1001)) {
            pattern_ = {2002, 2002, 2002, 2002, 2002, 2002, 2001, 2002, 2002, 2002, 2002, 2002};
        } else if (rateEquals(fps, 60, 1)) {
            pattern_ = {800};
        } else if (rateEquals(fps, 60000, 1001)) {
            pattern_ = {801, 801, 801, 801, 800};
        } else {
            const double samples = 48000.0 * static_cast<double>(fps.den) / static_cast<double>(fps.num);
            const int rounded = std::max(1, static_cast<int>(std::llround(samples)));
            pattern_ = {rounded};
        }

        default_samples_per_frame_ = pattern_.empty() ? 1920 : pattern_.front();
        configured_ = !pattern_.empty();
        (void)interlaced;
        return configured_;
    }

    bool configured_ = false;
    AVRational nominal_frame_rate_{0, 1};
    AVRational frame_time_base_{1, 25};
    std::vector<int> pattern_;
    int default_samples_per_frame_ = 1920;
};
