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
 * Small cooperative stop token shared by worker loops for deterministic sender/output shutdown.
 */

#pragma once

#include <atomic>

class StopToken
{
public:
    // Single-writer/multi-reader cooperative shutdown signal.
    void request_stop() { stop_.store(true, std::memory_order_release); }
    bool stop_requested() const { return stop_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> stop_{false};
};