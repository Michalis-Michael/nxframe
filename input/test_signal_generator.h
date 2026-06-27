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
 * Synthetic input source used by the sender pipeline for development,
 * validation, and explicitly enabled fallback operation. It generates video in
 * the same planar 10-bit 4:2:2 format used by the DeckLink path so downstream
 * encoder behavior remains comparable during lab tests.
 */

#pragma once

#include <cstdint>

// TestSignalGenerator produces deterministic audio/video without depending on
// SDI hardware. It is intentionally small and stateless from the caller's point
// of view: the caller owns the output buffers, while the generator only keeps
// tone phase between audio calls.
class TestSignalGenerator {
public:
    // Default source format: 1920x1080 at 50 fps, 48 kHz audio.
    TestSignalGenerator();
    TestSignalGenerator(int w, int h, int fps, int sr = 48000);

    // Generate a planar YUV422P10LE test frame into caller-provided storage.
    //
    // Expected buffer layout:
    //   [Y 16-bit plane][U 16-bit plane][V 16-bit plane]
    //
    // Each stored sample carries a 10-bit value in a 16-bit little-endian word.
    // If strideBytes is 0, the output is tightly packed. If strideBytes is
    // positive, it is used as the Y-plane row stride and UV planes use half of
    // that row stride by convention. The caller must allocate enough memory for
    // the selected layout.
    void generateFrame(uint8_t* buffer, int width, int height, int strideBytes);

    // Generate interleaved signed 16-bit PCM.
    //
    // Channel 1 carries a 1 kHz tone and channel 2 carries an 800 Hz tone. For
    // channel counts above stereo, remaining channels are filled with the left
    // tone so the output stays deterministic for tests.
    void generateAudioFrame(int16_t* buffer, int numChannels, int numSamples);

    // Update the generated video timing/size used by callers that track source
    // properties through the generator instance.
    void setVideoFormat(int w, int h, int fps);

    // Update the generated audio sample rate used for tone phase increments.
    void setAudioFormat(int sampleRate);

private:
    int  width_;
    int  height_;
    int  fps_;
    int  sampleRate_;

    double phaseL_; // Left-channel oscillator phase in radians.
    double phaseR_; // Right-channel oscillator phase in radians.
};
