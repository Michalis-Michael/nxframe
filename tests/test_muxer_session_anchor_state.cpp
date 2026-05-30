/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: tests/test_muxer_session_anchor_state.cpp
 * Description: NxFrame regression and validation tests.
 */

#include "output/muxer_ts.h"

#include <cassert>
#include <iostream>

int main()
{
    MuxerTS muxer;
    assert(!muxer.isVideoSessionAnchored());

    const uint64_t initialSession = muxer.getSessionId();
    muxer.resetTimestampState("unit-test");
    assert(muxer.getSessionId() == initialSession + 1u);
    assert(!muxer.isVideoSessionAnchored());

    std::cout << "muxer session anchor state test passed\n";
    return 0;
}
