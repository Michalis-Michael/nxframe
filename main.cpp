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
 * NxFrame process entry point. The executable installs minimal signal handlers
 * and delegates all command-line dispatch and pipeline startup to the app layer.
 */

#include "app/nxframe_app.h"

#include <atomic>
#include <csignal>

namespace {

// Shared shutdown flag observed by sender/receiver pipeline loops.
std::atomic<bool> gShutdownRequested{false};

void handleSignal(int)
{
    // Keep the signal handler async-signal-safe: only publish the shutdown flag.
    gShutdownRequested.store(true, std::memory_order_release);
}

} // namespace

int main(int argc, char* argv[])
{
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // The app layer owns CLI parsing and mode-specific startup. Keeping main()
    // small prevents transport or device lifetime logic from leaking here.
    return runNxFrameApp(argc, argv, gShutdownRequested);
}
