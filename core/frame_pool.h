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
 * Shared byte-buffer pool used by hot-path media stages to reduce allocation churn while preserving shared ownership semantics.
 */

#pragma once

#include <vector>
#include <memory>
#include <mutex>
#include <cstddef>
#include <cstdint>

// Small shared buffer pool. Returned shared_ptr instances either recycle a preallocated block or own a temporary fallback allocation.
class SharedBufferPool
{
public:
    SharedBufferPool() = default;
    SharedBufferPool(size_t blockSize, size_t blockCount) { reset(blockSize, blockCount); }

    // Replace the whole pool state atomically from the caller perspective; outstanding buffers keep their old state alive.
    void reset(size_t blockSize, size_t blockCount)
    {
        auto st = std::make_shared<State>();
        st->blockSize = blockSize;

        if (blockSize > 0 && blockCount > 0) {
            st->storage.resize(blockCount);
            st->freeList.reserve(blockCount);
            for (size_t i = 0; i < blockCount; ++i) {
                st->storage[i].reset(new uint8_t[blockSize]);
                st->freeList.push_back(i);
            }
        }

        std::lock_guard<std::mutex> lk(m_mtx);
        m_state = std::move(st);
    }

    // Acquire a block. If the pool is exhausted, allocate a one-off block rather than blocking the media callback.
    std::shared_ptr<uint8_t> acquire()
    {
        std::shared_ptr<State> st;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            st = m_state;
        }

        if (!st || st->blockSize == 0) {
            return {};
        }

        size_t idx = static_cast<size_t>(-1);
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            if (!st->freeList.empty()) {
                idx = st->freeList.back();
                st->freeList.pop_back();
            }
        }

        if (idx == static_cast<size_t>(-1)) {
            return std::shared_ptr<uint8_t>(new uint8_t[st->blockSize], std::default_delete<uint8_t[]>());
        }

        uint8_t* ptr = st->storage[idx].get();
        return std::shared_ptr<uint8_t>(ptr, [st, idx](uint8_t*) {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->freeList.push_back(idx);
        });
    }

    size_t blockSize() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_state ? m_state->blockSize : 0;
    }

private:
    struct State
    {
        size_t blockSize = 0;
        std::vector<std::unique_ptr<uint8_t[]>> storage;
        std::vector<size_t> freeList;
        std::mutex mtx;
    };

    mutable std::mutex m_mtx;
    std::shared_ptr<State> m_state;
};