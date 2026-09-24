/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * Presentation-timestamp keyed metadata association across buffering video
 * encoders. This keeps timecode/caption metadata attached to the encoded
 * picture that originated from the same input frame.
 */

#pragma once

#include "core/metadata.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

namespace nxframe {

class FrameMetadataTracker
{
public:
    void remember(int64_t pts, const FrameMetadata& metadata)
    {
        pending_[pts] = metadata;
    }

    bool take(int64_t pts, FrameMetadata& metadata)
    {
        const auto it = pending_.find(pts);
        if (it == pending_.end()) {
            return false;
        }

        metadata = std::move(it->second);
        pending_.erase(it);
        return true;
    }

    void clear() noexcept
    {
        pending_.clear();
    }

    size_t size() const noexcept
    {
        return pending_.size();
    }

private:
    std::map<int64_t, FrameMetadata> pending_;
};

} // namespace nxframe
