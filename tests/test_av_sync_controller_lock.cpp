/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: tests/test_av_sync_controller_lock.cpp
 * Description: NxFrame regression and validation tests.
 */

#include "playout/av_sync_controller.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <utility>

static VideoFrame makeVideo(int64_t pts)
{
    VideoFrame f;
    f.buffer = make_shared_u8(4);
    f.buffer_size = 4;
    f.width = 2;
    f.height = 1;
    f.pts = pts;
    return f;
}

static AudioFrame makeAudio(int64_t pts, int samples)
{
    AudioFrame f;
    f.sample_rate = 48000;
    f.channels = 2;
    f.bytes_per_sample = 2;
    f.num_samples = samples;
    f.buffer_size = static_cast<size_t>(samples * f.channels * f.bytes_per_sample);
    f.buffer = make_shared_u8(f.buffer_size);
    std::memset(f.buffer.get(), 0, f.buffer_size);
    f.pts = pts;
    return f;
}

int main()
{
    AvSyncController sync;
    AvSyncController::ScheduledVideo sv;
    AvSyncController::ScheduledAudio sa;

    // Audio covers 0..20ms. Video at 40ms must not lock and should drop the
    // stale audio block instead of anchoring to the wrong media time.
    AudioFrame a0 = makeAudio(0, 960);
    assert(sync.pushAudio(std::move(a0), 0, 20000));
    assert(sync.pushVideo(makeVideo(40000), 40000));
    assert(!sync.tryLock(false, &sv, &sa));
    assert(sync.anchorDroppedAudio() == 1);

    // Add an audio block covering 40ms; now the first video can become the
    // media origin and the seeded audio/video pair should be coherent.
    AudioFrame a1 = makeAudio(1920, 960);
    assert(sync.pushAudio(std::move(a1), 40000, 60000));
    assert(sync.tryLock(false, &sv, &sa));
    assert(sync.locked());
    assert(sv.media_pts_us == 40000);
    assert(sa.frame.num_samples == 960);

    std::cout << "av sync initial lock test passed\n";
    return 0;
}
