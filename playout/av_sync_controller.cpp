/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: playout/av_sync_controller.cpp
 * Description: Implements the receiver A/V sync scheduler used before SDI playout.
 */

#include "playout/av_sync_controller.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/mathematics.h>
}

int64_t AvSyncController::ptsToUs(int64_t pts, AVRational tb)
{
    if (pts == AV_NOPTS_VALUE || tb.num <= 0 || tb.den <= 0) {
        return invalidTime();
    }
    return av_rescale_q(pts, tb, AVRational{1, 1000000});
}

int64_t AvSyncController::audioEndUs(const AudioFrame& frame, int64_t pts_us)
{
    if (pts_us == invalidTime() || frame.sample_rate <= 0 || frame.num_samples <= 0) {
        return invalidTime();
    }
    return pts_us + samplesToUs(frame.num_samples, frame.sample_rate);
}

int64_t AvSyncController::usToSamples(int64_t us, int sampleRate)
{
    if (sampleRate <= 0) {
        return 0;
    }
    return av_rescale_q(us, AVRational{1, 1000000}, AVRational{1, sampleRate});
}

int64_t AvSyncController::samplesToUs(int64_t samples, int sampleRate)
{
    if (sampleRate <= 0) {
        return 0;
    }
    return av_rescale_q(samples, AVRational{1, sampleRate}, AVRational{1, 1000000});
}

void AvSyncController::reset()
{
    video_q_.clear();
    audio_q_.clear();
    locked_ = false;
    media_origin_us_ = invalidTime();
    next_audio_sample_ = 0;
    last_video_pts_us_ = invalidTime();
    last_audio_pts_us_ = invalidTime();
    last_video_playout_us_ = invalidTime();
    last_audio_playout_us_ = invalidTime();
    anchor_drop_video_ = 0;
    anchor_drop_audio_ = 0;
    duplicate_video_drop_ = 0;
    duplicate_audio_drop_ = 0;
    silence_inserted_samples_ = 0;
    audio_trimmed_samples_ = 0;
}

bool AvSyncController::pushVideo(VideoFrame&& frame, int64_t pts_us)
{
    if (!frame.buffer || pts_us == invalidTime()) {
        return false;
    }
    VideoItem item;
    item.frame = std::move(frame);
    item.pts_us = pts_us;
    video_q_.push_back(std::move(item));
    while (video_q_.size() > 64) {
        video_q_.pop_front();
    }
    return true;
}

bool AvSyncController::pushAudio(AudioFrame&& frame, int64_t pts_us, int64_t end_us)
{
    if (!frame.buffer || frame.num_samples <= 0 || pts_us == invalidTime() || end_us == invalidTime()) {
        return false;
    }
    AudioItem item;
    item.frame = std::move(frame);
    item.pts_us = pts_us;
    item.end_us = end_us;
    audio_q_.push_back(std::move(item));
    while (audio_q_.size() > 512) {
        audio_q_.pop_front();
    }
    return true;
}

bool AvSyncController::tryLock(bool allowVideoOnly, ScheduledVideo* seededVideo, ScheduledAudio* seededAudio)
{
    if (seededVideo) *seededVideo = ScheduledVideo{};
    if (seededAudio) *seededAudio = ScheduledAudio{};
    if (locked_) {
        return true;
    }

    while (!video_q_.empty() && !audio_q_.empty()) {
        VideoItem& v = video_q_.front();
        AudioItem& a = audio_q_.front();

        if (v.pts_us < a.pts_us) {
            video_q_.pop_front();
            ++anchor_drop_video_;
            continue;
        }
        if (v.pts_us >= a.end_us) {
            audio_q_.pop_front();
            ++anchor_drop_audio_;
            continue;
        }

        media_origin_us_ = v.pts_us;
        next_audio_sample_ = 0;
        locked_ = true;

        if (seededVideo) {
            seededVideo->frame = std::move(v.frame);
            seededVideo->media_pts_us = v.pts_us;
            seededVideo->playout_time_us = 0;
        }
        last_video_pts_us_ = v.pts_us;
        last_video_playout_us_ = 0;
        video_q_.pop_front();

        AudioFrame audio = std::move(a.frame);
        int64_t desiredSample = usToSamples(a.pts_us - media_origin_us_, audio.sample_rate);
        int64_t trimmed = 0;
        if (desiredSample < 0) {
            trimmed = -desiredSample;
            if (trimmed >= audio.num_samples) {
                audio_q_.pop_front();
                ++anchor_drop_audio_;
                locked_ = false;
                media_origin_us_ = invalidTime();
                continue;
            }
            trimAudioFront(audio, trimmed);
            desiredSample = 0;
        }

        if (seededAudio && audio.buffer && audio.num_samples > 0) {
            seededAudio->frame = std::move(audio);
            seededAudio->media_pts_us = media_origin_us_;
            seededAudio->playout_sample = 0;
            seededAudio->trimmed_samples = trimmed;
            seededAudio->silence = false;
        }
        audio_q_.pop_front();
        next_audio_sample_ = (seededAudio && seededAudio->frame.num_samples > 0) ? seededAudio->frame.num_samples : 0;
        last_audio_pts_us_ = media_origin_us_;
        last_audio_playout_us_ = 0;
        audio_trimmed_samples_ += static_cast<uint64_t>(std::max<int64_t>(0, trimmed));
        return true;
    }

    if (allowVideoOnly && !video_q_.empty()) {
        VideoItem v = std::move(video_q_.front());
        video_q_.pop_front();
        media_origin_us_ = v.pts_us;
        next_audio_sample_ = 0;
        locked_ = true;
        if (seededVideo) {
            seededVideo->frame = std::move(v.frame);
            seededVideo->media_pts_us = v.pts_us;
            seededVideo->playout_time_us = 0;
        }
        last_video_pts_us_ = v.pts_us;
        last_video_playout_us_ = 0;
        return true;
    }

    return false;
}

bool AvSyncController::popVideo(ScheduledVideo& out)
{
    out = ScheduledVideo{};
    if (!locked_) {
        return false;
    }

    while (!video_q_.empty()) {
        VideoItem item = std::move(video_q_.front());
        video_q_.pop_front();
        if (item.pts_us <= last_video_pts_us_) {
            ++duplicate_video_drop_;
            continue;
        }
        const int64_t rel = item.pts_us - media_origin_us_;
        if (rel < 0) {
            ++duplicate_video_drop_;
            continue;
        }
        out.frame = std::move(item.frame);
        out.media_pts_us = item.pts_us;
        out.playout_time_us = rel;
        last_video_pts_us_ = item.pts_us;
        last_video_playout_us_ = rel;
        return true;
    }
    return false;
}

bool AvSyncController::popAudio(int64_t horizon_sample,
                                int sampleRate,
                                int channels,
                                int bytesPerSample,
                                ScheduledAudio& out)
{
    out = ScheduledAudio{};
    if (!locked_ || sampleRate <= 0 || channels <= 0 || bytesPerSample <= 0) {
        return false;
    }

    if (horizon_sample >= 0 && next_audio_sample_ >= horizon_sample) {
        return false;
    }

    while (!audio_q_.empty()) {
        AudioItem item = std::move(audio_q_.front());
        audio_q_.pop_front();

        if (item.frame.sample_rate != sampleRate || item.frame.channels != channels || item.frame.bytes_per_sample != bytesPerSample) {
            ++duplicate_audio_drop_;
            continue;
        }

        int64_t desired = usToSamples(item.pts_us - media_origin_us_, sampleRate);
        if (desired + item.frame.num_samples <= next_audio_sample_) {
            ++duplicate_audio_drop_;
            continue;
        }

        if (desired > next_audio_sample_) {
            const int64_t gap = desired - next_audio_sample_;
            if (horizon_sample >= 0 && next_audio_sample_ + gap > horizon_sample) {
                audio_q_.push_front(std::move(item));
                return false;
            }
            const int64_t cappedGap = std::min<int64_t>(gap, 48000); // avoid giant one-shot silence blocks
            out.frame = makeSilence(sampleRate, channels, bytesPerSample, static_cast<int>(cappedGap));
            out.media_pts_us = media_origin_us_ + samplesToUs(next_audio_sample_, sampleRate);
            out.playout_sample = next_audio_sample_;
            out.inserted_silence_samples = cappedGap;
            out.silence = true;
            next_audio_sample_ += cappedGap;
            last_audio_playout_us_ = samplesToUs(out.playout_sample, sampleRate);
            silence_inserted_samples_ += static_cast<uint64_t>(cappedGap);
            // Put the real audio back; it will be handled after the silence gap.
            audio_q_.push_front(std::move(item));
            return true;
        }

        int64_t trimmed = 0;
        if (desired < next_audio_sample_) {
            trimmed = next_audio_sample_ - desired;
            if (trimmed >= item.frame.num_samples) {
                ++duplicate_audio_drop_;
                continue;
            }
            trimAudioFront(item.frame, trimmed);
            desired = next_audio_sample_;
            audio_trimmed_samples_ += static_cast<uint64_t>(trimmed);
        }

        if (horizon_sample >= 0 && desired + item.frame.num_samples > horizon_sample) {
            audio_q_.push_front(std::move(item));
            return false;
        }

        out.frame = std::move(item.frame);
        out.media_pts_us = media_origin_us_ + samplesToUs(desired, sampleRate);
        out.playout_sample = desired;
        out.trimmed_samples = trimmed;
        out.silence = false;
        next_audio_sample_ = desired + out.frame.num_samples;
        last_audio_pts_us_ = out.media_pts_us;
        last_audio_playout_us_ = samplesToUs(out.playout_sample, sampleRate);
        return true;
    }

    return false;
}

bool AvSyncController::trimAudioFront(AudioFrame& frame, int64_t samplesToTrim)
{
    if (!frame.buffer || samplesToTrim <= 0 || samplesToTrim >= frame.num_samples ||
        frame.channels <= 0 || frame.bytes_per_sample <= 0) {
        return false;
    }
    const size_t bytesPerFrame = static_cast<size_t>(frame.channels) * static_cast<size_t>(frame.bytes_per_sample);
    const size_t bytesToTrim = static_cast<size_t>(samplesToTrim) * bytesPerFrame;
    if (bytesToTrim >= frame.buffer_size) {
        return false;
    }
    const size_t remainingBytes = frame.buffer_size - bytesToTrim;
    std::shared_ptr<uint8_t> b = make_shared_u8(std::max<size_t>(remainingBytes, 1));
    std::memcpy(b.get(), frame.buffer.get() + bytesToTrim, remainingBytes);
    frame.buffer = b;
    frame.buffer_size = remainingBytes;
    frame.num_samples -= static_cast<int>(samplesToTrim);
    if (frame.time_base.num > 0 && frame.time_base.den > 0 && frame.sample_rate > 0 && frame.pts != AV_NOPTS_VALUE) {
        frame.pts += av_rescale_q(samplesToTrim, AVRational{1, frame.sample_rate}, frame.time_base);
    }
    return true;
}

AudioFrame AvSyncController::makeSilence(int sampleRate, int channels, int bytesPerSample, int numSamples)
{
    AudioFrame s;
    s.sample_rate = sampleRate;
    s.channels = channels;
    s.bytes_per_sample = bytesPerSample;
    s.num_samples = std::max(0, numSamples);
    s.time_base = AVRational{1, sampleRate > 0 ? sampleRate : 48000};
    const size_t total = static_cast<size_t>(s.num_samples) * static_cast<size_t>(std::max(0, channels)) * static_cast<size_t>(std::max(0, bytesPerSample));
    s.buffer = make_shared_u8(std::max<size_t>(total, 1));
    s.buffer_size = total;
    if (total > 0) {
        std::memset(s.buffer.get(), 0, total);
    }
    return s;
}
