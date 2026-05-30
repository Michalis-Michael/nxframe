/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: playout/receiver_clock_policy.h
 * Description: Documents the receiver clock policy used by SDI playout.
 */

#pragma once

#include <cstdint>

namespace nxframe {
namespace receiver_clock_policy {

// Receiver master-clock / A/V-sync policy for SDI playout.
//
// This policy is intentionally centralized and named because receiver timing is
// one of the most sensitive parts of the appliance. Keep behavior changes here
// reviewable and explicit; do not let transport arrival timing leak into media
// scheduling decisions.
//
// Sender side:
//   * The SDI capture timeline is the media source clock.
//   * Encoded video PTS and audio PTS must describe that captured timeline.
//   * MPEG-TS/SRT/UDP/RTP transport is only a delivery mechanism.
//
// Receiver side:
//   * Decoded media PTS is the A/V alignment timeline.
//   * DeckLink scheduled playback is the SDI output hardware clock.
//   * Transport packet arrival time is never used as the media clock.
//   * The playout loop absorbs normal jitter with bounded buffering.
//   * A real source discontinuity resets and re-anchors the SDI timeline.
//
// Operational consequence:
//   * Small jitter must not cause re-anchoring.
//   * Large PTS jumps must not be hidden by stretching silence/black.
//   * After reconnect/restart, wait for a new anchor and resume cleanly.

static constexpr const char* modeName()
{
    return "pts_media_clock_decklink_output_clock";
}

static constexpr const char* mediaClockName()
{
    return "decoded_pts";
}

static constexpr const char* outputClockName()
{
    return "decklink_scheduled";
}

static constexpr const char* transportClockName()
{
    return "not_media_clock";
}

static constexpr int64_t backwardPtsResetUs()
{
    return 100000; // 100 ms backward means new media timeline.
}

static constexpr int64_t forwardPtsResetUs()
{
    return 2000000; // 2 s forward means source discontinuity/gap.
}

} // namespace receiver_clock_policy
} // namespace nxframe
